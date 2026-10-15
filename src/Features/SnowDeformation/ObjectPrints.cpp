// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Features/SnowDeformation/CoSave.h"
#include "Globals.h"

// Snow Trenches on Objects, the data half: bodies' contacts on captured
// objects' tops, written into a world-anchored print map beside the trench
// map, refilled on the trenches' own refill, kept in a store when they leave
// the window and written to the co-save. Design and decisions:
// MD plans\OBJECT-TOP-TRACKS-PLAN.md. Kernels: the tail of
// DeformationUpdateCS.hlsl.
//
// Co-save record 'SNOP', version 1:
//   chunk { u32 tileCount, u16 tileDim, u16 pad, f32 texel }
//   tile  { u32 worldspace, i32 x, i32 y, f32 clockRel, u32 entries }
//         followed by entries x { u16 index, u32 value }
// index = slice * tileTexels + row * tileDim + column; value = the map's own
// packing. clockRel is relative to this session's decay clock (<= 0).

namespace
{
	uint64_t PrintTileId(int32_t a_x, int32_t a_y)
	{
		return ((uint64_t)(uint32_t)a_x << 32) | (uint32_t)a_y;
	}

	int32_t PrintTileX(uint64_t a_id)
	{
		return (int32_t)(uint32_t)(a_id >> 32);
	}

	int32_t PrintTileY(uint64_t a_id)
	{
		return (int32_t)(uint32_t)(a_id & 0xFFFFFFFFu);
	}

	int32_t FloorDiv(int32_t a_v, int32_t a_d)
	{
		return a_v >= 0 ? a_v / a_d : -((-a_v + a_d - 1) / a_d);
	}

	// Intersection of two (x0, y0, w, h) rects; false when empty.
	bool Overlap(const DirectX::XMINT4& a_a, const DirectX::XMINT4& a_b, DirectX::XMINT4& o_r)
	{
		const int32_t x0 = std::max(a_a.x, a_b.x);
		const int32_t y0 = std::max(a_a.y, a_b.y);
		const int32_t x1 = std::min(a_a.x + a_a.z, a_b.x + a_b.z);
		const int32_t y1 = std::min(a_a.y + a_a.w, a_b.y + a_b.w);
		o_r = { x0, y0, x1 - x0, y1 - y0 };
		return x1 > x0 && y1 > y0;
	}

	DirectX::XMINT4 TileRect(int32_t a_tx, int32_t a_ty, int32_t a_tileDim)
	{
		return { a_tx * a_tileDim, a_ty * a_tileDim, a_tileDim, a_tileDim };
	}

	bool AnyPrint(const std::vector<uint32_t>& a_texels)
	{
		return std::any_of(a_texels.begin(), a_texels.end(), [](uint32_t v) { return (v & 1023u) != 0u; });
	}
}

ID3D11ComputeShader* SnowDeformation::GetPrintStampCS()
{
	if (!printStampCS)
		printStampCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "PrintStampCS"));
	return printStampCS;
}

ID3D11ComputeShader* SnowDeformation::GetPrintRingCS()
{
	if (!printRingCS)
		printRingCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "PrintRingCS"));
	return printRingCS;
}

ID3D11ComputeShader* SnowDeformation::GetPrintRefillCS()
{
	if (!printRefillCS)
		printRefillCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "PrintRefillCS"));
	return printRefillCS;
}

ID3D11ComputeShader* SnowDeformation::GetPrintViewCS()
{
	if (!printViewCS)
		printViewCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "PrintViewCS"));
	return printViewCS;
}

bool SnowDeformation::EnsurePrintShaders()
{
	if (printCSFailed)
		return false;
	if (!GetPrintStampCS() || !GetPrintRingCS() || !GetPrintRefillCS()) {
		printCSFailed = true;
		logger::error("[SNOW DEFORMATION] print map kernels failed to compile; Snow Trenches on Objects disabled");
		return false;
	}
	return true;
}

void SnowDeformation::ReleaseObjectPrintShaders()
{
	for (auto** cs : { &printStampCS, &printRingCS, &printRefillCS, &printViewCS }) {
		if (*cs)
			(*cs)->Release();
		*cs = nullptr;
	}
	printCSFailed = false;
}

void SnowDeformation::CreatePrintResources(uint32_t a_dim)
{
	auto device = globals::d3d::device;

	delete printMap;
	printMap = nullptr;
	D3D11_TEXTURE2D_DESC mapDesc{};
	mapDesc.Width = a_dim;
	mapDesc.Height = a_dim;
	mapDesc.MipLevels = 1;
	mapDesc.ArraySize = 2;
	mapDesc.Format = DXGI_FORMAT_R32_UINT;
	mapDesc.SampleDesc.Count = 1;
	mapDesc.Usage = D3D11_USAGE_DEFAULT;
	mapDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	printMap = new Texture2D(mapDesc, "SnowDeformation::PrintMap");
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = mapDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray = { 0, 1, 0, 2 };
	printMap->CreateSRV(srvDesc);
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = mapDesc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
	uavDesc.Texture2DArray = { 0, 0, 2 };
	printMap->CreateUAV(uavDesc);
	printMapDim = a_dim;

	if (!printFrameCB)
		printFrameCB = new ConstantBuffer(ConstantBufferDesc<PrintFrameCB>(), "SnowDeformation::PrintFrameCB");

	if (!printBlockBuffer) {
		D3D11_BUFFER_DESC blockDesc{};
		blockDesc.ByteWidth = kPrintBlockCap * sizeof(uint32_t);
		blockDesc.Usage = D3D11_USAGE_DYNAMIC;
		blockDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		blockDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		blockDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		blockDesc.StructureByteStride = sizeof(uint32_t);
		DX::ThrowIfFailed(device->CreateBuffer(&blockDesc, nullptr, printBlockBuffer.put()));
		Util::SetResourceName(printBlockBuffer.get(), "SnowDeformation::PrintBlocks");
		D3D11_SHADER_RESOURCE_VIEW_DESC blockSrvDesc{};
		blockSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
		blockSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		blockSrvDesc.Buffer.NumElements = kPrintBlockCap;
		DX::ThrowIfFailed(device->CreateShaderResourceView(printBlockBuffer.get(), &blockSrvDesc, printBlockSRV.put()));
		Util::SetResourceName(printBlockSRV.get(), "SnowDeformation::PrintBlocks SRV");
	}

	if (!printWriteBuffer) {
		// Sized for the largest map: (2048 / tile + 1)^2 flags.
		constexpr uint32_t maxRow = 2048u / kPrintTileDim + 1u;
		D3D11_BUFFER_DESC writeDesc{};
		writeDesc.ByteWidth = maxRow * maxRow * sizeof(uint32_t);
		writeDesc.Usage = D3D11_USAGE_DEFAULT;
		writeDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		writeDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		DX::ThrowIfFailed(device->CreateBuffer(&writeDesc, nullptr, printWriteBuffer.put()));
		Util::SetResourceName(printWriteBuffer.get(), "SnowDeformation::PrintWrites");
		D3D11_UNORDERED_ACCESS_VIEW_DESC writeUavDesc{};
		writeUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		writeUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		writeUavDesc.Buffer.NumElements = maxRow * maxRow;
		writeUavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(printWriteBuffer.get(), &writeUavDesc, printWriteUAV.put()));
		Util::SetResourceName(printWriteUAV.get(), "SnowDeformation::PrintWrites UAV");

		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = writeDesc.ByteWidth;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (auto& slot : printWriteReadbacks) {
			DX::ThrowIfFailed(device->CreateBuffer(&stagingDesc, nullptr, slot.staging.put()));
			Util::SetResourceName(slot.staging.get(), "SnowDeformation::PrintWritesStaging");
		}
	}

	if (!printStaging[0].texture) {
		D3D11_TEXTURE2D_DESC stagingDesc{};
		stagingDesc.Width = kPrintTileDim;
		stagingDesc.Height = kPrintTileDim;
		stagingDesc.MipLevels = 1;
		stagingDesc.ArraySize = 2;
		stagingDesc.Format = DXGI_FORMAT_R32_UINT;
		stagingDesc.SampleDesc.Count = 1;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (auto& slot : printStaging) {
			DX::ThrowIfFailed(device->CreateTexture2D(&stagingDesc, nullptr, slot.texture.put()));
			Util::SetResourceName(slot.texture.get(), "SnowDeformation::PrintMirrorStaging");
		}
	}

	printBlockSeen.assign((size_t)(a_dim / 8) * (a_dim / 8), 0);
	// The new map is uncleared: the next update clears it and refills it from
	// the store. Prints written since their last mirror are lost with the old one.
	printWindowValid = false;
	printLive.clear();
}

bool SnowDeformation::DecayPrintTile(PrintStoreTile& a_tile, float a_clock)
{
	const int drop = (int)std::floor((a_clock - a_tile.clock) * 1023.0f);
	if (drop <= 0)
		return AnyPrint(a_tile.texels);
	// Advanced only by what was applied, so a fraction is never discarded.
	a_tile.clock += (float)drop / 1023.0f;
	bool any = false;
	for (auto& v : a_tile.texels) {
		const uint32_t depth = v & 1023u;
		if (depth <= (uint32_t)drop) {
			v = 0u;
		} else {
			v -= (uint32_t)drop;
			any = true;
		}
	}
	return any;
}

void SnowDeformation::EnforcePrintStoreCap()
{
	if (printStore.size() <= kPrintStoreCap)
		return;
	// Another worldspace first, then the furthest from the window.
	const float centreX = ((float)printWindowMin.x + 0.5f * (float)printMapDim) * kPrintTexel;
	const float centreY = ((float)printWindowMin.y + 0.5f * (float)printMapDim) * kPrintTexel;
	std::vector<std::pair<float, TrenchTileKey>> ranked;
	ranked.reserve(printStore.size());
	for (const auto& [key, tile] : printStore) {
		float rank = std::numeric_limits<float>::max();
		if (key.worldspace == printWorldspace) {
			const float dx = ((float)key.x + 0.5f) * kPrintTileDim * kPrintTexel - centreX;
			const float dy = ((float)key.y + 0.5f) * kPrintTileDim * kPrintTexel - centreY;
			rank = dx * dx + dy * dy;
		}
		ranked.emplace_back(rank, key);
	}
	std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	size_t evicted = 0;
	for (const auto& [rank, key] : ranked) {
		if (printStore.size() <= kPrintStoreCap)
			break;
		printStore.erase(key);
		evicted++;
	}
	logger::info("[SNOW DEFORMATION] print store over its cap: evicted {} tiles, {} remain", evicted, printStore.size());
}

bool SnowDeformation::QueuePrintMirror(ID3D11DeviceContext* a_context, int32_t a_tx, int32_t a_ty, float a_clock)
{
	auto slot = std::find_if(printStaging.begin(), printStaging.end(), [](const PrintStagingSlot& s) { return !s.busy; });
	if (slot == printStaging.end())
		return false;
	slot->key = { printWorldspace, a_tx, a_ty };
	slot->windowMin = printWindowMin;
	slot->dim = printMapDim;
	slot->clock = a_clock;
	slot->frame = printFrame;
	slot->generation = printQueueGeneration;
	slot->busy = true;
	// A tile is one contiguous block of the map: its origin is a multiple of
	// the tile and the dim a multiple of that.
	const int32_t mask = (int32_t)printMapDim - 1;
	const UINT px = (UINT)((a_tx * kPrintTileDim) & mask);
	const UINT py = (UINT)((a_ty * kPrintTileDim) & mask);
	const D3D11_BOX box{ px, py, 0, px + kPrintTileDim, py + kPrintTileDim, 1 };
	for (UINT s = 0; s < 2; s++)
		a_context->CopySubresourceRegion(slot->texture.get(), D3D11CalcSubresource(0, s, 1), 0, 0, 0,
			printMap->resource.get(), D3D11CalcSubresource(0, s, 1), &box);
	return true;
}

void SnowDeformation::PollPrintMirrors(ID3D11DeviceContext* a_context)
{
	std::vector<uint32_t> texels(2 * (size_t)kPrintTileTexels);
	for (auto& slot : printStaging) {
		if (!slot.busy || printFrame < slot.frame + 2)
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped[2]{};
		if (FAILED(a_context->Map(slot.texture.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped[0])))
			continue;
		if (FAILED(a_context->Map(slot.texture.get(), 1, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped[1]))) {
			a_context->Unmap(slot.texture.get(), 0);
			continue;
		}
		for (int s = 0; s < 2; s++)
			for (int j = 0; j < kPrintTileDim; j++)
				std::memcpy(texels.data() + (size_t)s * kPrintTileTexels + (size_t)j * kPrintTileDim,
					static_cast<const uint8_t*>(mapped[s].pData) + (size_t)j * mapped[s].RowPitch, kPrintTileDim * sizeof(uint32_t));
		a_context->Unmap(slot.texture.get(), 1);
		a_context->Unmap(slot.texture.get(), 0);
		slot.busy = false;

		// Texels outside the window the copy was taken in hold other ground.
		const DirectX::XMINT4 window = { slot.windowMin.x, slot.windowMin.y, (int32_t)slot.dim, (int32_t)slot.dim };
		DirectX::XMINT4 valid{};
		if (!Overlap(TileRect(slot.key.x, slot.key.y, kPrintTileDim), window, valid))
			continue;
		const int32_t lx0 = valid.x - slot.key.x * kPrintTileDim;
		const int32_t ly0 = valid.y - slot.key.y * kPrintTileDim;

		std::scoped_lock lock(printStoreMutex);
		if (slot.generation != printStoreGeneration.load(std::memory_order_relaxed))
			continue;
		auto it = printStore.find(slot.key);
		if (it == printStore.end()) {
			bool any = false;
			for (int s = 0; s < 2 && !any; s++)
				for (int j = ly0; j < ly0 + valid.w && !any; j++)
					for (int i = lx0; i < lx0 + valid.z && !any; i++)
						any = (texels[(size_t)s * kPrintTileTexels + (size_t)j * kPrintTileDim + i] & 1023u) != 0u;
			if (!any)
				continue;
			it = printStore.emplace(slot.key, PrintStoreTile{ std::vector<uint32_t>(2 * (size_t)kPrintTileTexels, 0u), slot.clock }).first;
		} else {
			DecayPrintTile(it->second, slot.clock);
		}
		for (int s = 0; s < 2; s++)
			for (int j = ly0; j < ly0 + valid.w; j++)
				for (int i = lx0; i < lx0 + valid.z; i++) {
					const size_t index = (size_t)s * kPrintTileTexels + (size_t)j * kPrintTileDim + i;
					it->second.texels[index] = texels[index];
				}
		it->second.clock = slot.clock;
		if (!AnyPrint(it->second.texels))
			printStore.erase(it);
		else
			EnforcePrintStoreCap();
	}
}

void SnowDeformation::PollPrintWrites(ID3D11DeviceContext* a_context)
{
	for (auto& slot : printWriteReadbacks) {
		if (!slot.busy || printFrame < slot.frame + 1)
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(slot.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
			continue;
		slot.busy = false;
		// Flags from before the last clear name tiles the clear emptied; a clear's
		// own frame stamps after it.
		if (slot.frame >= printLastClearFrame && slot.worldspace == printWorldspace) {
			const auto* flags = static_cast<const uint32_t*>(mapped.pData);
			const DirectX::XMINT4 window = { printWindowMin.x, printWindowMin.y, (int32_t)printMapDim, (int32_t)printMapDim };
			for (uint32_t row = 0; row < slot.rowTiles; row++)
				for (uint32_t col = 0; col < slot.rowTiles; col++) {
					if (flags[row * slot.rowTiles + col] == 0u)
						continue;
					const int32_t tx = slot.tileOrigin.x + (int32_t)col;
					const int32_t ty = slot.tileOrigin.y + (int32_t)row;
					DirectX::XMINT4 inside{};
					if (!Overlap(TileRect(tx, ty, kPrintTileDim), window, inside))
						continue;
					auto& live = printLive[PrintTileId(tx, ty)];
					live.refillSince = 0.0f;
					live.dirty = true;
				}
		}
		a_context->Unmap(slot.staging.get(), 0);
	}
}

uint32_t SnowDeformation::InjectPrintRect(ID3D11DeviceContext* a_context, const DirectX::XMINT4& a_rect, float a_clock)
{
	if (a_rect.z <= 0 || a_rect.w <= 0)
		return 0;
	const int32_t mask = (int32_t)printMapDim - 1;
	const int32_t tx0 = FloorDiv(a_rect.x, kPrintTileDim);
	const int32_t ty0 = FloorDiv(a_rect.y, kPrintTileDim);
	const int32_t tx1 = FloorDiv(a_rect.x + a_rect.z - 1, kPrintTileDim);
	const int32_t ty1 = FloorDiv(a_rect.y + a_rect.w - 1, kPrintTileDim);
	uint32_t injected = 0;
	std::scoped_lock lock(printStoreMutex);
	if (printStore.empty())
		return 0;
	for (int32_t ty = ty0; ty <= ty1; ty++) {
		for (int32_t tx = tx0; tx <= tx1; tx++) {
			auto it = printStore.find({ printWorldspace, tx, ty });
			if (it == printStore.end())
				continue;
			if (!DecayPrintTile(it->second, a_clock)) {
				printStore.erase(it);
				continue;
			}
			DirectX::XMINT4 box{};
			if (!Overlap(TileRect(tx, ty, kPrintTileDim), a_rect, box))
				continue;
			const int32_t lx = box.x - tx * kPrintTileDim;
			const int32_t ly = box.y - ty * kPrintTileDim;
			const UINT px = (UINT)(box.x & mask);
			const UINT py = (UINT)(box.y & mask);
			const D3D11_BOX dst{ px, py, 0, px + (UINT)box.z, py + (UINT)box.w, 1 };
			for (UINT s = 0; s < 2; s++) {
				const uint32_t* src = it->second.texels.data() + (size_t)s * kPrintTileTexels + (size_t)ly * kPrintTileDim + lx;
				a_context->UpdateSubresource(printMap->resource.get(), D3D11CalcSubresource(0, s, 1), &dst, src, kPrintTileDim * sizeof(uint32_t), 0);
			}
			printLive[PrintTileId(tx, ty)].refillSince = 0.0f;
			injected++;
		}
	}
	return injected;
}

void SnowDeformation::UpdateObjectPrints(ID3D11DeviceContext* a_context, const PerFrame& a_frame, bool a_trenchPassRan)
{
	if (!EnsurePrintShaders())
		return;
	const uint32_t wantDim = settings.ObjectTrenchResolution >= 2048u ? 2048u : (settings.ObjectTrenchResolution >= 1024u ? 1024u : 512u);
	settings.ObjectTrenchResolution = wantDim;
	if (wantDim != printMapDim)
		CreatePrintResources(wantDim);
	if (!printMap)
		return;
	printFrame++;
	printStampBlocksLast = printRefillBlocksLast = printMirrorsLast = printInjectsLast = 0;

	float clock;
	{
		std::scoped_lock lock(trenchStoreMutex);
		clock = trenchDecayClock;
	}
	const bool persist = settings.PersistTrenches;
	{
		std::scoped_lock lock(printStoreMutex);
		printClockSnapshot = clock;
		if (!persist && !printStore.empty()) {
			logger::info("[SNOW DEFORMATION] print store CLEARED by the Remember Trenches toggle, {} tiles dropped", printStore.size());
			printStore.clear();
		}
	}

	// A load or revert replaced the store: copies and flags in flight describe
	// the timeline before it.
	const bool reinject = printReinjectRequested.exchange(false, std::memory_order_acq_rel);
	printQueueGeneration = printStoreGeneration.load(std::memory_order_acquire);
	if (reinject) {
		for (auto& slot : printStaging)
			slot.busy = false;
		for (auto& slot : printWriteReadbacks)
			slot.busy = false;
	}

	ID3D11UnorderedAccessView* nullUAV[3] = {};
	// Copies into and out of the map (mirror, inject) go with no UAV bound.
	a_context->CSSetUnorderedAccessViews(0, 3, nullUAV, nullptr);
	PollPrintWrites(a_context);
	PollPrintMirrors(a_context);

	const int32_t dim = (int32_t)printMapDim;
	const DirectX::XMINT2 newMin = { (int32_t)std::floor(contactCenter.x / kPrintTexel) - dim / 2,
		(int32_t)std::floor(contactCenter.y / kPrintTexel) - dim / 2 };
	const DirectX::XMINT4 newWindow = { newMin.x, newMin.y, dim, dim };
	const uint32_t worldspace = activeWorldspace.load(std::memory_order_acquire);
	const int32_t dx = newMin.x - printWindowMin.x;
	const int32_t dy = newMin.y - printWindowMin.y;
	const bool fullClear = reinject || !printWindowValid || worldspace != printWorldspace || std::abs(dx) + std::abs(dy) >= dim / 2;
	const bool moved = dx != 0 || dy != 0;

	// Tiles about to lose texels: a dirty one is mirrored before the clear
	// below reaches it. A reinject replaces the map from a store that already
	// says what it should hold, so nothing is mirrored over it.
	if (printWindowValid && (fullClear || moved)) {
		for (auto it = printLive.begin(); it != printLive.end();) {
			const int32_t tx = PrintTileX(it->first);
			const int32_t ty = PrintTileY(it->first);
			const auto rect = TileRect(tx, ty, kPrintTileDim);
			DirectX::XMINT4 inside{};
			const bool overlaps = !fullClear && Overlap(rect, newWindow, inside);
			const bool keepsAll = overlaps && inside.z == kPrintTileDim && inside.w == kPrintTileDim;
			if (!keepsAll && it->second.dirty && persist && !reinject && QueuePrintMirror(a_context, tx, ty, clock)) {
				it->second.dirty = false;
				printMirrorsLast++;
			}
			if (overlaps)
				++it;
			else
				it = printLive.erase(it);
		}
	}

	PrintFrameCB cb{};
	cb.WindowMin = newMin;
	uint32_t ringTotal = 0;
	if (fullClear) {
		const UINT zero[4] = {};
		a_context->ClearUnorderedAccessViewUint(printMap->uav.get(), zero);
		printLive.clear();
		printLastClearFrame = printFrame;
	} else if (moved) {
		uint32_t n = 0;
		if (dx != 0)
			cb.RingRects[n++] = { dx > 0 ? newMin.x + dim - dx : newMin.x, newMin.y, std::abs(dx), dim };
		if (dy != 0)
			cb.RingRects[n++] = { dx > 0 ? newMin.x : newMin.x - dx, dy > 0 ? newMin.y + dim - dy : newMin.y, dim - std::abs(dx), std::abs(dy) };
		cb.RingRectCount = n;
		for (uint32_t i = 0; i < n; i++)
			ringTotal += (uint32_t)(cb.RingRects[i].z * cb.RingRects[i].w);
		cb.RingTotal = ringTotal;
	}

	// The trench refill, in the map's 1023rds.
	uint32_t quanta = 0;
	if (a_trenchPassRan && a_frame.RefillAmount > 0.0f) {
		printRefillBank = std::min(printRefillBank + a_frame.RefillAmount, 1.0f);
		quanta = (uint32_t)std::floor(printRefillBank * 1023.0f);
		printRefillBank -= (float)quanta / 1023.0f;
	}
	cb.RefillQuanta = quanta;
	printFrameCB->Update(cb);

	ID3D11Buffer* cbs[2] = { perFrame->CB(), printFrameCB->CB() };
	a_context->CSSetConstantBuffers(0, 2, cbs);
	ID3D11UnorderedAccessView* mapUAV = printMap->uav.get();

	if (ringTotal > 0) {
		a_context->CSSetUnorderedAccessViews(0, 1, &mapUAV, nullptr);
		a_context->CSSetShader(printRingCS, nullptr, 0);
		a_context->Dispatch((ringTotal + 63) / 64, 1, 1);
		a_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
	}
	printWindowMin = newMin;
	printWindowValid = true;
	printWorldspace = worldspace;

	if (persist) {
		if (fullClear) {
			printInjectsLast += InjectPrintRect(a_context, newWindow, clock);
		} else {
			for (uint32_t i = 0; i < cb.RingRectCount; i++)
				printInjectsLast += InjectPrintRect(a_context, cb.RingRects[i], clock);
		}
	}

	// Physical 8x8 blocks over a world rect clipped to the window, each once.
	const int32_t blockMask = dim / 8 - 1;
	std::vector<uint32_t> touched;
	auto addBlocks = [&](DirectX::XMINT4 a_rect) {
		DirectX::XMINT4 clipped{};
		if (!Overlap(a_rect, newWindow, clipped))
			return;
		const int32_t bx0 = FloorDiv(clipped.x, 8);
		const int32_t by0 = FloorDiv(clipped.y, 8);
		const int32_t bx1 = FloorDiv(clipped.x + clipped.z - 1, 8);
		const int32_t by1 = FloorDiv(clipped.y + clipped.w - 1, 8);
		for (int32_t by = by0; by <= by1; by++)
			for (int32_t bx = bx0; bx <= bx1; bx++) {
				if (printBlockScratch.size() >= kPrintBlockCap)
					return;
				const uint32_t px = (uint32_t)(bx & blockMask);
				const uint32_t py = (uint32_t)(by & blockMask);
				const uint32_t seen = py * (uint32_t)(dim / 8) + px;
				if (printBlockSeen[seen])
					continue;
				printBlockSeen[seen] = 1;
				touched.push_back(seen);
				printBlockScratch.push_back(px | (py << 16));
			}
	};
	auto uploadBlocks = [&]() {
		for (uint32_t seen : touched)
			printBlockSeen[seen] = 0;
		touched.clear();
		if (printBlockScratch.empty())
			return false;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(printBlockBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return false;
		std::memcpy(mapped.pData, printBlockScratch.data(), printBlockScratch.size() * sizeof(uint32_t));
		a_context->Unmap(printBlockBuffer.get(), 0);
		return true;
	};
	ID3D11ShaderResourceView* blockSRV = printBlockSRV.get();

	if (quanta > 0 && !printLive.empty()) {
		printBlockScratch.clear();
		for (const auto& [id, live] : printLive)
			addBlocks(TileRect(PrintTileX(id), PrintTileY(id), kPrintTileDim));
		const uint32_t blocks = (uint32_t)printBlockScratch.size();
		if (uploadBlocks()) {
			a_context->CSSetShaderResources(2, 1, &blockSRV);
			a_context->CSSetUnorderedAccessViews(0, 1, &mapUAV, nullptr);
			a_context->CSSetShader(printRefillCS, nullptr, 0);
			a_context->Dispatch(blocks, 1, 1);
			a_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
			printRefillBlocksLast = blocks;
		}
		const float refill = (float)quanta / 1023.0f;
		for (auto it = printLive.begin(); it != printLive.end();) {
			it->second.refillSince += refill;
			// A full depth refilled since the last write: nothing left in it.
			if (it->second.refillSince >= 1.0f && !it->second.dirty)
				it = printLive.erase(it);
			else
				++it;
		}
	}

	ID3D11ShaderResourceView* topSRVs[2] = {
		heightTopRaw[heightCurrent] ? heightTopRaw[heightCurrent]->srv.get() : nullptr,
		heightTop2Raw[heightCurrent] ? heightTop2Raw[heightCurrent]->srv.get() : nullptr
	};
	ID3D11ShaderResourceView* contactSRVs[3] = {
		(contactHeight && a_frame.ContactDim > 0.5f) ? contactHeight->srv.get() : nullptr,
		shellTerrainTexture ? shellTerrainTexture->srv.get() : nullptr,
		(a_frame.HasRoadRaster > 0.5f && heightSkinDepth) ? heightSkinDepth->srv.get() : nullptr
	};
	// The object gate needs this frame's contacts and a finished raster; an
	// unbound raster reads 0, which is a top at sea level.
	const bool fieldReady = a_trenchPassRan && contactSRVs[0] && contactSRVs[1] && heightMapValid && topSRVs[0] && topSRVs[1];

	if (fieldReady) {
		printBlockScratch.clear();
		// The stamp reads the contact field 2x2 around each texel centre.
		const float pad = 2.0f * kPrintTexel;
		for (const auto& actor : contactActors) {
			const int32_t x0 = (int32_t)std::floor((actor.minX - pad) / kPrintTexel);
			const int32_t y0 = (int32_t)std::floor((actor.minY - pad) / kPrintTexel);
			const int32_t x1 = (int32_t)std::floor((actor.maxX + pad) / kPrintTexel);
			const int32_t y1 = (int32_t)std::floor((actor.maxY + pad) / kPrintTexel);
			addBlocks({ x0, y0, x1 - x0 + 1, y1 - y0 + 1 });
		}
		const uint32_t blocks = (uint32_t)printBlockScratch.size();
		auto readback = std::find_if(printWriteReadbacks.begin(), printWriteReadbacks.end(), [](const PrintWriteReadback& r) { return !r.busy; });
		if (blocks > 0 && readback != printWriteReadbacks.end() && uploadBlocks()) {
			const UINT zero[4] = {};
			a_context->ClearUnorderedAccessViewUint(printWriteUAV.get(), zero);
			a_context->CSSetShaderResources(2, 1, &blockSRV);
			a_context->CSSetShaderResources(7, 3, contactSRVs);
			a_context->CSSetShaderResources(10, 2, topSRVs);
			ID3D11UnorderedAccessView* stampUAVs[2] = { mapUAV, printWriteUAV.get() };
			a_context->CSSetUnorderedAccessViews(0, 2, stampUAVs, nullptr);
			a_context->CSSetShader(printStampCS, nullptr, 0);
			globals::profiler->BeginPass("SnowDeformation::Objects - Trench Prints");
			a_context->Dispatch(blocks, 1, 1);
			globals::profiler->EndPass();
			a_context->CSSetUnorderedAccessViews(0, 2, nullUAV, nullptr);
			a_context->CopyResource(readback->staging.get(), printWriteBuffer.get());
			readback->tileOrigin = { FloorDiv(newMin.x, kPrintTileDim), FloorDiv(newMin.y, kPrintTileDim) };
			readback->rowTiles = printMapDim / kPrintTileDim + 1;
			readback->worldspace = worldspace;
			readback->frame = printFrame;
			readback->busy = true;
			printStampBlocksLast = blocks;
		} else {
			if (blocks > 0 && readback == printWriteReadbacks.end())
				logger::debug("[SNOW DEFORMATION] print stamp skipped: every write-flag readback is in flight");
			for (uint32_t seen : touched)
				printBlockSeen[seen] = 0;
			touched.clear();
		}
	}
	if (!printStampBlocksLast)
		globals::profiler->MarkPassSkipped("SnowDeformation::Objects - Trench Prints");

	// Rolling mirror: keeps the store, and so the save, current with what the
	// map holds.
	if (persist) {
		uint32_t queued = 0;
		for (auto& [id, live] : printLive) {
			if (queued >= kPrintMirrorPerFrame)
				break;
			if (!live.dirty)
				continue;
			if (!QueuePrintMirror(a_context, PrintTileX(id), PrintTileY(id), clock))
				break;
			live.dirty = false;
			queued++;
		}
		printMirrorsLast += queued;
	}

	if (debugContactView && debugObjectPrintView && contactViewUAV) {
		if (auto* viewCS = GetPrintViewCS()) {
			ID3D11ShaderResourceView* mapSRV = printMap->srv.get();
			a_context->CSSetShaderResources(7, 3, contactSRVs);
			a_context->CSSetShaderResources(10, 2, topSRVs);
			a_context->CSSetShaderResources(12, 1, &mapSRV);
			ID3D11UnorderedAccessView* viewUAV = contactViewUAV.get();
			a_context->CSSetUnorderedAccessViews(2, 1, &viewUAV, nullptr);
			a_context->CSSetShader(viewCS, nullptr, 0);
			a_context->Dispatch(kContactViewPx / 8, 2 * kContactViewPx / 8, 1);
		}
	}

	{
		std::scoped_lock lock(printStoreMutex);
		printStoreTilesLast = printStore.size();
	}

	a_context->CSSetShader(nullptr, nullptr, 0);
	ID3D11ShaderResourceView* nullSRVs[11] = {};
	a_context->CSSetShaderResources(2, 11, nullSRVs);
	a_context->CSSetUnorderedAccessViews(0, 3, nullUAV, nullptr);
	ID3D11Buffer* nullCBs[2] = {};
	a_context->CSSetConstantBuffers(0, 2, nullCBs);
}

void SnowDeformation::SavePrintStore(const SKSE::SerializationInterface* a_intfc)
{
	std::scoped_lock lock(printStoreMutex);
	if (!a_intfc->OpenRecord(kPrintRecord, kPrintRecordVersion)) {
		logger::error("[SNOW DEFORMATION] Could not open the print co-save record");
		return;
	}
	// Written even when empty, so a load can tell "no prints" from "no data".
	uint32_t tileCount = 0;
	if (settings.PersistTrenches)
		for (const auto& [key, tile] : printStore)
			tileCount += AnyPrint(tile.texels) ? 1u : 0u;
	const uint16_t tileDim = (uint16_t)kPrintTileDim;
	const uint16_t pad = 0;
	const float texel = kPrintTexel;
	a_intfc->WriteRecordData(&tileCount, sizeof(tileCount));
	a_intfc->WriteRecordData(&tileDim, sizeof(tileDim));
	a_intfc->WriteRecordData(&pad, sizeof(pad));
	a_intfc->WriteRecordData(&texel, sizeof(texel));
	if (tileCount == 0) {
		logger::info("[SNOW DEFORMATION] print co-save: no tiles");
		return;
	}

	size_t entriesTotal = 0;
	for (const auto& [key, tile] : printStore) {
		if (!AnyPrint(tile.texels))
			continue;
		uint32_t entries = 0;
		for (uint32_t v : tile.texels)
			entries += (v & 1023u) ? 1u : 0u;
		const float clockRel = std::min(tile.clock - printClockSnapshot, 0.0f);
		a_intfc->WriteRecordData(&key.worldspace, sizeof(key.worldspace));
		a_intfc->WriteRecordData(&key.x, sizeof(key.x));
		a_intfc->WriteRecordData(&key.y, sizeof(key.y));
		a_intfc->WriteRecordData(&clockRel, sizeof(clockRel));
		a_intfc->WriteRecordData(&entries, sizeof(entries));
		for (size_t i = 0; i < tile.texels.size(); i++) {
			const uint32_t v = tile.texels[i];
			if ((v & 1023u) == 0u)
				continue;
			const uint16_t index = (uint16_t)i;
			a_intfc->WriteRecordData(&index, sizeof(index));
			a_intfc->WriteRecordData(&v, sizeof(v));
		}
		entriesTotal += entries;
	}
	logger::info("[SNOW DEFORMATION] print co-save: {} tiles, {} prints, {} KB", tileCount, entriesTotal, (tileCount * 20 + entriesTotal * 6) / 1024);
}

void SnowDeformation::LoadPrintStore(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t)
{
	if (a_version != kPrintRecordVersion) {
		logger::warn("[SNOW DEFORMATION] print co-save is version {}, this build reads {}; stored prints dropped", a_version, kPrintRecordVersion);
		return;
	}
	uint32_t tileCount = 0;
	uint16_t tileDim = 0;
	uint16_t pad = 0;
	float texel = 0.0f;
	if (a_intfc->ReadRecordData(&tileCount, sizeof(tileCount)) != sizeof(tileCount) ||
		a_intfc->ReadRecordData(&tileDim, sizeof(tileDim)) != sizeof(tileDim) ||
		a_intfc->ReadRecordData(&pad, sizeof(pad)) != sizeof(pad) ||
		a_intfc->ReadRecordData(&texel, sizeof(texel)) != sizeof(texel)) {
		logger::warn("[SNOW DEFORMATION] print co-save header would not read; nothing restored");
		return;
	}
	if (tileDim != (uint16_t)kPrintTileDim || std::abs(texel - kPrintTexel) > 0.01f) {
		logger::warn("[SNOW DEFORMATION] print co-save was written at {} texels of {} units, this build uses {} of {}; dropped",
			tileDim, texel, kPrintTileDim, kPrintTexel);
		return;
	}

	std::scoped_lock lock(printStoreMutex);
	printStore.clear();
	printStoreGeneration.fetch_add(1, std::memory_order_acq_rel);
	uint32_t restored = 0;
	const uint32_t tileValues = 2u * (uint32_t)kPrintTileTexels;
	for (uint32_t i = 0; i < tileCount; i++) {
		TrenchTileKey key{};
		float clockRel = 0.0f;
		uint32_t entries = 0;
		if (a_intfc->ReadRecordData(&key.worldspace, sizeof(key.worldspace)) != sizeof(key.worldspace) ||
			a_intfc->ReadRecordData(&key.x, sizeof(key.x)) != sizeof(key.x) ||
			a_intfc->ReadRecordData(&key.y, sizeof(key.y)) != sizeof(key.y) ||
			a_intfc->ReadRecordData(&clockRel, sizeof(clockRel)) != sizeof(clockRel) ||
			a_intfc->ReadRecordData(&entries, sizeof(entries)) != sizeof(entries))
			break;
		if (entries > tileValues) {
			logger::warn("[SNOW DEFORMATION] print co-save tile {} claims {} prints; the rest is dropped", i, entries);
			break;
		}
		PrintStoreTile tile{ std::vector<uint32_t>(tileValues, 0u), std::min(clockRel, 0.0f) };
		bool intact = true;
		for (uint32_t e = 0; e < entries && intact; e++) {
			uint16_t index = 0;
			uint32_t value = 0;
			intact = a_intfc->ReadRecordData(&index, sizeof(index)) == sizeof(index) &&
			         a_intfc->ReadRecordData(&value, sizeof(value)) == sizeof(value) && index < tileValues;
			if (intact)
				tile.texels[index] = value;
		}
		if (!intact) {
			logger::warn("[SNOW DEFORMATION] print co-save tile {} would not read; the rest is dropped", i);
			break;
		}
		printStore.insert_or_assign(key, std::move(tile));
		restored++;
	}
	// The next update replaces the map with what was just read.
	printReinjectRequested.store(true, std::memory_order_release);
	logger::info("[SNOW DEFORMATION] print co-save restored {} of {} tiles (active worldspace {:08X})",
		restored, tileCount, activeWorldspace.load(std::memory_order_acquire));
}

void SnowDeformation::RegisterPrintCoSave()
{
	CoSave::GetSingleton()->Register(
		kPrintRecord, kPrintRecordVersion,
		[this](const SKSE::SerializationInterface* a_intfc) { SavePrintStore(a_intfc); },
		[this](const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length) { LoadPrintStore(a_intfc, a_version, a_length); },
		[this]() {
			// Before a load and on a new game: the map must not carry one
			// timeline's prints into the next.
			std::scoped_lock lock(printStoreMutex);
			if (!printStore.empty())
				logger::info("[SNOW DEFORMATION] print store CLEARED by the co-save revert, {} tiles dropped", printStore.size());
			printStore.clear();
			printStoreGeneration.fetch_add(1, std::memory_order_acq_rel);
			printReinjectRequested.store(true, std::memory_order_release);
		});
}
