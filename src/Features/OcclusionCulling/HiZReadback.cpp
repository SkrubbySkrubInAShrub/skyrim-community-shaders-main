#include "HiZReadback.h"

#include "Features/OcclusionCulling/HiZCull.h"
#include "Utils/D3D.h"

namespace HiZReadback
{
	namespace
	{
		// Copies in flight. The GPU is typically one to two frames behind the render thread, so
		// three keeps a copy ready to collect every frame without ever blocking on one.
		constexpr std::uint32_t kInFlight = 3;
		// Published snapshots. More than kInFlight, so a slot is never rewritten while a cull
		// thread from an earlier frame could still be reading it.
		constexpr std::uint32_t kSlots = 6;

		struct Pending
		{
			winrt::com_ptr<ID3D11Texture2D> staging;
			bool                            busy = false;
			std::uint32_t                   frame = 0;
			DirectX::XMMATRIX               viewProj = DirectX::XMMatrixIdentity();
			RE::NiPoint3                    posAdjust{};
			float                           validWidth = 0.0f;
			float                           validHeight = 0.0f;
			float                           texelPixels = 4.0f;
			float                           projScale = 0.0f;
			std::uint32_t                   mipCount = 0;
			std::uint32_t                   texWidth = 0;
			std::uint32_t                   texHeight = 0;
		};

		std::array<Pending, kInFlight>      g_pending{};
		std::uint32_t                       g_writeIndex = 0;
		std::array<Snapshot, kSlots>        g_slots{};
		std::uint32_t                       g_slotIndex = 0;
		std::atomic<const Snapshot*>        g_current{ nullptr };
		std::atomic<std::uint32_t>          g_age{ 0 };

		// The staging chain mirrors the pyramid, so a size change (resolution, dynamic-res
		// reallocation) has to rebuild it.
		std::uint32_t g_texWidth = 0;
		std::uint32_t g_texHeight = 0;
		std::uint32_t g_texMips = 0;

		bool EnsureStaging(ID3D11Device* a_device, ID3D11Texture2D* a_source)
		{
			D3D11_TEXTURE2D_DESC src{};
			a_source->GetDesc(&src);

			if (g_texWidth == src.Width && g_texHeight == src.Height && g_texMips == src.MipLevels && g_pending[0].staging)
				return true;

			for (auto& p : g_pending) {
				p.staging = nullptr;
				p.busy = false;
			}
			g_current.store(nullptr, std::memory_order_release);

			D3D11_TEXTURE2D_DESC td = src;
			td.Usage = D3D11_USAGE_STAGING;
			td.BindFlags = 0;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			td.MiscFlags = 0;

			for (std::uint32_t i = 0; i < kInFlight; ++i) {
				winrt::com_ptr<ID3D11Texture2D> tex;
				if (FAILED(a_device->CreateTexture2D(&td, nullptr, tex.put()))) {
					logger::error("[OcclusionCulling][hiz] staging texture create failed ({}x{}, {} mips)", td.Width, td.Height, td.MipLevels);
					for (auto& p : g_pending)
						p.staging = nullptr;
					return false;
				}
				Util::SetResourceName(tex.get(), "OcclusionCulling::HiZReadback %u", i);
				g_pending[i].staging = tex;
				g_pending[i].busy = false;
			}

			g_texWidth = src.Width;
			g_texHeight = src.Height;
			g_texMips = src.MipLevels;
			logger::info("[OcclusionCulling][hiz] readback chain {}x{} x{} mips, {} in flight", src.Width, src.Height, src.MipLevels, kInFlight);
			return true;
		}

		void Submit(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx, const HiZPyramid& a_pyramid, float a_projScale)
		{
			if (!a_device || !a_ctx || !a_pyramid.IsValid())
				return;

			auto* srv = a_pyramid.GetSRV();
			if (!srv)
				return;
			winrt::com_ptr<ID3D11Resource> res;
			srv->GetResource(res.put());
			auto tex = res.try_as<ID3D11Texture2D>();
			if (!tex)
				return;

			if (!EnsureStaging(a_device, tex.get()))
				return;

			// Never wait for a slot: if every copy is still in flight the frame simply does not
			// capture one, and the cull keeps using the previous snapshot.
			Pending* slot = nullptr;
			for (std::uint32_t i = 0; i < kInFlight; ++i) {
				auto& p = g_pending[(g_writeIndex + i) % kInFlight];
				if (!p.busy) {
					slot = &p;
					g_writeIndex = (g_writeIndex + i + 1) % kInFlight;
					break;
				}
			}
			if (!slot)
				return;

			// The pyramid describes the depth of THIS frame's camera, so stamp it now, through the
			// same reconstruction the CPU tests will project with.
			RE::NiCamera* cam = RE::Main::WorldRootCamera();
			if (!cam)
				return;
			HiZCull::CameraViewProj(cam, slot->viewProj, slot->posAdjust);

			a_ctx->CopyResource(slot->staging.get(), tex.get());

			D3D11_TEXTURE2D_DESC td{};
			tex->GetDesc(&td);
			slot->busy = true;
			slot->frame = HiZCull::CurrentFrame();
			slot->validWidth = (float)a_pyramid.GetWidth();
			slot->validHeight = (float)a_pyramid.GetHeight();
			slot->texelPixels = a_pyramid.GetTexelPixels();
			slot->projScale = a_projScale;
			slot->mipCount = std::min<std::uint32_t>(a_pyramid.GetMipCount(), Snapshot::kMaxMips);
			slot->texWidth = td.Width;
			slot->texHeight = td.Height;
		}

		void Poll(ID3D11DeviceContext* a_ctx)
		{
			if (!a_ctx)
				return;

			// Oldest first, so the published snapshot is always the freshest COMPLETE one and the
			// queue cannot deadlock behind a copy nobody collects.
			Pending* oldest = nullptr;
			for (auto& p : g_pending) {
				if (!p.busy)
					continue;
				if (!oldest || p.frame < oldest->frame)
					oldest = &p;
			}
			if (!oldest)
				return;

			Snapshot& dst = g_slots[g_slotIndex];

			std::size_t total = 0;
			std::array<std::uint32_t, Snapshot::kMaxMips> offs{}, pitches{};
			for (std::uint32_t m = 0; m < oldest->mipCount; ++m) {
				const std::uint32_t w = std::max(1u, oldest->texWidth >> m);
				const std::uint32_t h = std::max(1u, oldest->texHeight >> m);
				offs[m] = (std::uint32_t)total;
				pitches[m] = w;
				total += static_cast<std::size_t>(w) * h;
			}
			if (dst.texels.size() != total)
				dst.texels.resize(total);

			for (std::uint32_t m = 0; m < oldest->mipCount; ++m) {
				const UINT sub = D3D11CalcSubresource(m, 0, oldest->mipCount);
				D3D11_MAPPED_SUBRESOURCE mapped{};
				const HRESULT hr = a_ctx->Map(oldest->staging.get(), sub, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
				if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
					// Still on the GPU. Unmap whatever succeeded and try again next frame; the copy
					// stays queued, so nothing is lost and nothing blocks.
					for (std::uint32_t k = 0; k < m; ++k)
						a_ctx->Unmap(oldest->staging.get(), D3D11CalcSubresource(k, 0, oldest->mipCount));
					return;
				}
				if (FAILED(hr)) {
					for (std::uint32_t k = 0; k < m; ++k)
						a_ctx->Unmap(oldest->staging.get(), D3D11CalcSubresource(k, 0, oldest->mipCount));
					oldest->busy = false;
					return;
				}

				const std::uint32_t w = std::max(1u, oldest->texWidth >> m);
				const std::uint32_t h = std::max(1u, oldest->texHeight >> m);
				auto*               out = dst.texels.data() + offs[m];
				const auto*         src = static_cast<const std::uint8_t*>(mapped.pData);
				for (std::uint32_t y = 0; y < h; ++y)
					std::memcpy(out + static_cast<std::size_t>(y) * w, src + static_cast<std::size_t>(y) * mapped.RowPitch, w * sizeof(float));
				a_ctx->Unmap(oldest->staging.get(), sub);
			}

			dst.mipOffset = offs;
			dst.mipPitch = pitches;
			dst.mipCount = oldest->mipCount;
			dst.validWidth = oldest->validWidth;
			dst.validHeight = oldest->validHeight;
			dst.texelPixels = oldest->texelPixels;
			dst.projScale = oldest->projScale;
			dst.viewProj = oldest->viewProj;
			dst.posAdjust[0] = oldest->posAdjust.x;
			dst.posAdjust[1] = oldest->posAdjust.y;
			dst.posAdjust[2] = oldest->posAdjust.z;
			dst.frame = oldest->frame;

			g_current.store(&dst, std::memory_order_release);
			g_slotIndex = (g_slotIndex + 1) % kSlots;
			oldest->busy = false;
			g_age.store(HiZCull::CurrentFrame() - oldest->frame, std::memory_order_relaxed);
		}
	}

	float Snapshot::TileMax(int a_level, int a_x0, int a_y0, int a_validW, int a_validH) const
	{
		const std::uint32_t off = mipOffset[a_level];
		const std::uint32_t pitch = mipPitch[a_level];
		float               m = 0.0f;
		for (int y = 0; y < 3; ++y) {
			const int ty = std::clamp(a_y0 + y, 0, a_validH - 1);
			const float* row = texels.data() + off + static_cast<std::size_t>(ty) * pitch;
			for (int x = 0; x < 3; ++x) {
				const int tx = std::clamp(a_x0 + x, 0, a_validW - 1);
				m = std::max(m, row[tx]);
			}
		}
		return m;
	}

	void Update(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx, const HiZPyramid& a_pyramid)
	{
		if (!a_device || !a_ctx || !a_pyramid.IsValid())
			return;

		RE::NiCamera* cam = RE::Main::WorldRootCamera();
		if (!cam)
			return;

		// projScale converts a world radius at a given distance into screen pixels, and has to
		// match what the pyramid's texels mean. Same expression the grass cull uses.
		const auto& vf = cam->GetRuntimeData2().viewFrustum;
		const auto [screenW, screenH] = globals::game::renderer->GetScreenSize();
		const float top = std::abs(vf.fTop);
		if (top < 1e-6f)
			return;

		Submit(a_device, a_ctx, a_pyramid, screenH / (2.0f * top));
		Poll(a_ctx);
	}

	const Snapshot* Current()
	{
		return g_current.load(std::memory_order_acquire);
	}

	Stats GetStats()
	{
		return Stats{ g_age.load(std::memory_order_relaxed) };
	}
}
