#include "Features/SnowDeformation.h"

#include "Deferred.h"
#include "Features/Skylighting.h"
#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include <future>

// Landscape snow (LANDSCAPE-SNOW-PLAN Phase 1): the game's own landscape
// draws, raised onto the ground shell's surface. After the game's
// SetupGeometry a landscape draw is moved onto LandscapeSnow.hlsl, which
// reruns CS's landscape VS per vertex: near draws through the hull and
// domain stages, the rest through a raising VS. RestoreGeometry puts the
// pipeline back. See CODE-NOTES.md (LandscapeSnow.cpp).

namespace
{
	enum : uint32_t
	{
		kLsMain,
		kLsLightingOther,
		kLsShadowSun,
		kLsShadowLocal,
		kLsDepth,
		kLsUtilityOther,
		kLsSkyOcclusion,
		kLsCategories
	};
	constexpr const char* kLsNames[kLsCategories] = { "mainDeferred", "lightingOther", "shadowSun", "shadowLocal", "depthPrepass", "utilityOther", "skyOcclusion" };
	constexpr uint32_t kLsMaxTargets = 8;
	constexpr uint32_t kLsSlots = 6;
	constexpr uint32_t kLsMaxDraws = 1024;
	// Every SRV slot the surface reads, t0..t59.
	constexpr uint32_t kLsSRVCount = 60;

	bool LsCameraPass(uint32_t a_cat) { return a_cat == kLsMain || a_cat == kLsDepth; }
	bool LsShadowPass(uint32_t a_cat) { return a_cat == kLsShadowSun || a_cat == kLsShadowLocal || a_cat == kLsSkyOcclusion; }

	struct LsConfig
	{
		bool attach[kLsCategories]{ true, true, true, true, true, true, true };
		bool tessellate = true;
		bool gate = true;
		// LandscapeSnow.hlsl's content-gate checks: 1 map tiles, 2 class borders, 4 water, 8 object lift, 16 exclusions.
		int gateMask = 31;
		bool cull = true;
		bool measure = false;
		bool stats = false;
		float maxFactor = 64.0f;
		float guard = 0.05f;
		float lift = 512.0f;
		// Camera passes put the raised surface this far under the ground shell while it draws, so the shell wins the depth test.
		float shellSink = 2.0f;
		// Past these the base lattice outruns the land's own triangles: the
		// raising VS alone, no hull.
		float tessReach = 2600.0f;
		float shadowTessReach = 2000.0f;
	};

	struct alignas(16) LsCBData
	{
		float geometry[4];
		float tess[4];
		float cull[4];
	};
	STATIC_ASSERT_ALIGNAS_16(LsCBData);

	struct LsShaders
	{
		winrt::com_ptr<ID3D11VertexShader> vsRaise;
		winrt::com_ptr<ID3D11VertexShader> vs;
		winrt::com_ptr<ID3D11HullShader> hs;
		winrt::com_ptr<ID3D11DomainShader> ds;
		std::string defines;
		double compileMs = 0.0;
	};

	struct LsEntry
	{
		int state = 0;  // 1 compiling, 2 ready, 3 failed
		std::future<LsShaders> pending;
		LsShaders shaders;
		uint64_t tessAttaches = 0;
		uint64_t raiseAttaches = 0;
	};

	struct LsSlot
	{
		bool open = false;
		bool pendingRead = false;
		winrt::com_ptr<ID3D11Query> disjoint;
		std::vector<winrt::com_ptr<ID3D11Query>> ts;
		std::vector<winrt::com_ptr<ID3D11Query>> stats;
		std::array<uint8_t, kLsMaxDraws> cat{};
		std::array<uint8_t, kLsMaxDraws> hasStats{};
		std::array<uint8_t, kLsMaxDraws> target{};
		uint32_t drawCount = 0;
		uint32_t overflow = 0;
		uint32_t draws[kLsCategories]{};
		uint32_t tess[kLsCategories]{};
		uint32_t raise[kLsCategories]{};
		uint64_t tris[kLsCategories]{};
		uint32_t targetDraws[kLsMaxTargets]{};
		uint64_t targetTris[kLsMaxTargets]{};
	};

	struct LsTarget
	{
		void* texture = nullptr;
		uint32_t slice = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t arraySize = 0;
		uint32_t category = 0;
	};

	struct LsTotals
	{
		uint64_t frames = 0;
		uint64_t lost = 0;
		uint64_t disjoint = 0;
		uint64_t overflow = 0;
		double gpuMs[kLsCategories]{};
		uint64_t draws[kLsCategories]{};
		uint64_t tess[kLsCategories]{};
		uint64_t raise[kLsCategories]{};
		uint64_t tris[kLsCategories]{};
		uint64_t statFrames = 0;
		uint64_t ia[kLsCategories]{};
		uint64_t vs[kLsCategories]{};
		uint64_t hs[kLsCategories]{};
		uint64_t ds[kLsCategories]{};
		uint64_t cinv[kLsCategories]{};
		uint64_t cprim[kLsCategories]{};
		double targetMs[kLsMaxTargets]{};
		uint64_t targetDraws[kLsMaxTargets]{};
		uint64_t targetTris[kLsMaxTargets]{};
	};

	enum class LsMode : uint8_t
	{
		kNone,
		kTess,
		kRaise
	};

	struct LsState
	{
		LsConfig cfg;
		std::unordered_map<uint64_t, LsEntry> entries;
		std::unique_ptr<ConstantBuffer> drawCB;
		std::unique_ptr<ConstantBuffer> shellCB;
		LsSlot slots[kLsSlots];
		uint32_t slotIndex = 0;
		uint32_t lastFrame = UINT32_MAX;
		LsTotals totals;
		int curDraw = -1;
		LsTarget targets[kLsMaxTargets];
		uint32_t targetCount = 0;

		// This frame's surface inputs (Refresh); valid while cbFrame is the frame.
		uint32_t cbFrame = UINT32_MAX;
		uint32_t cbCategory = UINT32_MAX;
		bool cbValid = false;
		ID3D11ShaderResourceView* srvs[kLsSRVCount]{};
		ID3D11SamplerState* sampler = nullptr;
		float4 liveRect{};

		// The draw in flight.
		LsMode mode = LsMode::kNone;
		winrt::com_ptr<ID3D11VertexShader> savedVS;
		ID3D11ShaderResourceView* savedVSSRVs[kLsSRVCount]{};
		ID3D11Buffer* savedVSCB[2]{};
		ID3D11SamplerState* savedVSSampler = nullptr;
	};

	LsState ls;
	// Outside LsState: its alignas(16) would pad the struct (C4324).
	SnowDeformation::ShellCB lsCB{};
	std::mutex lsMutex;

	LsShaders LsCompile(bool a_lighting, std::string a_defines)
	{
		LsShaders out;
		out.defines = a_defines;
		const auto t0 = std::chrono::steady_clock::now();
		std::vector<std::pair<std::string, std::string>> parsed;
		std::istringstream ss(a_defines);
		std::string tok;
		while (ss >> tok) {
			const auto eq = tok.find('=');
			parsed.emplace_back(tok.substr(0, eq), eq == std::string::npos ? std::string() : tok.substr(eq + 1));
		}
		if (a_lighting)
			parsed.emplace_back("LS_LIGHTING", "");
		std::vector<std::pair<const char*, const char*>> defs;
		for (const auto& [k, v] : parsed)
			defs.emplace_back(k.c_str(), v.c_str());
		auto& snow = globals::features::snowDeformation;
		const wchar_t* path = L"Data\\Shaders\\SnowDeformation\\LandscapeSnow.hlsl";
		out.vsRaise.attach(static_cast<ID3D11VertexShader*>(snow.CompileSnowShader(path, defs, "vs_5_0", "LS_VSRaise")));
		out.vs.attach(static_cast<ID3D11VertexShader*>(snow.CompileSnowShader(path, defs, "vs_5_0", "LS_VS")));
		out.hs.attach(static_cast<ID3D11HullShader*>(snow.CompileSnowShader(path, defs, "hs_5_0", "LS_HS")));
		out.ds.attach(static_cast<ID3D11DomainShader*>(snow.CompileSnowShader(path, defs, "ds_5_0", "LS_DS")));
		out.compileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		return out;
	}

	void LsDetach()
	{
		auto* context = globals::d3d::context;
		if (ls.mode == LsMode::kTess) {
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ID3D11ShaderResourceView* none[kLsSRVCount]{};
			context->HSSetShaderResources(0, kLsSRVCount, none);
			context->DSSetShaderResources(0, kLsSRVCount, none);
		} else if (ls.mode == LsMode::kRaise) {
			// The game's state cache believes its own VS bindings are still in place.
			context->VSSetShaderResources(0, kLsSRVCount, ls.savedVSSRVs);
			context->VSSetConstantBuffers(11, 1, &ls.savedVSCB[0]);
			context->VSSetConstantBuffers(13, 1, &ls.savedVSCB[1]);
			context->VSSetSamplers(0, 1, &ls.savedVSSampler);
			for (auto*& srv : ls.savedVSSRVs) {
				if (srv)
					srv->Release();
				srv = nullptr;
			}
			for (auto*& cb : ls.savedVSCB) {
				if (cb)
					cb->Release();
				cb = nullptr;
			}
			if (ls.savedVSSampler)
				ls.savedVSSampler->Release();
			ls.savedVSSampler = nullptr;
		}
		context->VSSetShader(ls.savedVS.get(), nullptr, 0);
		ls.savedVS = nullptr;
		ls.mode = LsMode::kNone;
	}

	bool LsRead(LsSlot& a_slot)
	{
		auto* context = globals::d3d::context;
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
		if (!a_slot.disjoint || context->GetData(a_slot.disjoint.get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			return false;
		double ms[kLsCategories]{};
		double tms[kLsMaxTargets]{};
		uint64_t ia[kLsCategories]{}, vs[kLsCategories]{}, hs[kLsCategories]{}, ds[kLsCategories]{}, cinv[kLsCategories]{}, cprim[kLsCategories]{};
		bool anyStats = false;
		for (uint32_t i = 0; i < a_slot.drawCount; i++) {
			UINT64 a = 0, b = 0;
			if (context->GetData(a_slot.ts[2 * i].get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
				context->GetData(a_slot.ts[2 * i + 1].get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				return false;
			const uint32_t c = a_slot.cat[i];
			if (b > a && dj.Frequency) {
				ms[c] += double(b - a) * 1000.0 / double(dj.Frequency);
				if (a_slot.target[i] < kLsMaxTargets)
					tms[a_slot.target[i]] += double(b - a) * 1000.0 / double(dj.Frequency);
			}
			if (a_slot.hasStats[i]) {
				D3D11_QUERY_DATA_PIPELINE_STATISTICS ps{};
				if (context->GetData(a_slot.stats[i].get(), &ps, sizeof(ps), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
					anyStats = true;
					ia[c] += ps.IAPrimitives;
					vs[c] += ps.VSInvocations;
					hs[c] += ps.HSInvocations;
					ds[c] += ps.DSInvocations;
					cinv[c] += ps.CInvocations;
					cprim[c] += ps.CPrimitives;
				}
			}
		}
		std::lock_guard lock(lsMutex);
		auto& t = ls.totals;
		if (dj.Disjoint) {
			t.disjoint++;
			return true;
		}
		t.frames++;
		t.overflow += a_slot.overflow;
		if (anyStats)
			t.statFrames++;
		for (uint32_t k = 0; k < kLsMaxTargets; k++) {
			t.targetMs[k] += tms[k];
			t.targetDraws[k] += a_slot.targetDraws[k];
			t.targetTris[k] += a_slot.targetTris[k];
		}
		for (uint32_t c = 0; c < kLsCategories; c++) {
			t.gpuMs[c] += ms[c];
			t.draws[c] += a_slot.draws[c];
			t.tess[c] += a_slot.tess[c];
			t.raise[c] += a_slot.raise[c];
			t.tris[c] += a_slot.tris[c];
			t.ia[c] += ia[c];
			t.vs[c] += vs[c];
			t.hs[c] += hs[c];
			t.ds[c] += ds[c];
			t.cinv[c] += cinv[c];
			t.cprim[c] += cprim[c];
		}
		return true;
	}

	void LsNewFrame(uint32_t a_frame)
	{
		ls.lastFrame = a_frame;
		auto* context = globals::d3d::context;
		auto& cur = ls.slots[ls.slotIndex];
		if (cur.open) {
			context->End(cur.disjoint.get());
			cur.open = false;
			cur.pendingRead = true;
		}
		ls.slotIndex = (ls.slotIndex + 1) % kLsSlots;
		auto& next = ls.slots[ls.slotIndex];
		if (next.pendingRead) {
			if (!LsRead(next)) {
				std::lock_guard lock(lsMutex);
				ls.totals.lost++;
			}
			next.pendingRead = false;
		}
		next.drawCount = 0;
		next.overflow = 0;
		std::fill(std::begin(next.draws), std::end(next.draws), 0u);
		std::fill(std::begin(next.tess), std::end(next.tess), 0u);
		std::fill(std::begin(next.raise), std::end(next.raise), 0u);
		std::fill(std::begin(next.tris), std::end(next.tris), 0ull);
		std::fill(std::begin(next.targetDraws), std::end(next.targetDraws), 0u);
		std::fill(std::begin(next.targetTris), std::end(next.targetTris), 0ull);
		if (!ls.cfg.measure)
			return;
		if (!next.disjoint) {
			D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			globals::d3d::device->CreateQuery(&qd, next.disjoint.put());
		}
		if (next.disjoint) {
			context->Begin(next.disjoint.get());
			next.open = true;
		}
	}

	// The depth target a Utility draw writes, folded into the per-target table.
	uint32_t LsTargetIndex(uint32_t a_cat)
	{
		auto* context = globals::d3d::context;
		ID3D11DepthStencilView* dsv = nullptr;
		context->OMGetRenderTargets(0, nullptr, &dsv);
		if (!dsv)
			return kLsMaxTargets;
		D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
		dsv->GetDesc(&dd);
		ID3D11Resource* res = nullptr;
		dsv->GetResource(&res);
		const uint32_t sliceIdx = dd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY ? dd.Texture2DArray.FirstArraySlice : 0;
		uint32_t index = kLsMaxTargets;
		for (uint32_t k = 0; k < ls.targetCount; k++)
			if (ls.targets[k].texture == res && ls.targets[k].slice == sliceIdx && ls.targets[k].category == a_cat)
				index = k;
		if (index == kLsMaxTargets && ls.targetCount < kLsMaxTargets) {
			auto& tg = ls.targets[ls.targetCount];
			tg.texture = res;
			tg.slice = sliceIdx;
			tg.category = a_cat;
			D3D11_TEXTURE2D_DESC td{};
			if (auto* tex = static_cast<ID3D11Texture2D*>(res))
				tex->GetDesc(&td);
			tg.width = td.Width;
			tg.height = td.Height;
			tg.arraySize = td.ArraySize;
			index = ls.targetCount++;
		}
		if (res)
			res->Release();
		dsv->Release();
		return index;
	}

	struct LS_Lighting_RestoreGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
		{
			globals::features::snowDeformation.LandscapeSnowRestore();
			func(a_shader, a_pass, a_flags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct LS_Utility_SetupGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
		{
			func(a_shader, a_pass, a_flags);
			auto& snow = globals::features::snowDeformation;
			if (snow.loaded)
				snow.LandscapeSnowSetup(a_pass, false);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct LS_Utility_RestoreGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
		{
			globals::features::snowDeformation.LandscapeSnowRestore();
			func(a_shader, a_pass, a_flags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void SnowDeformation::InstallLandscapeSnow()
{
	// Lighting's SetupGeometry side rides SD_BSLightingShader_SetupGeometry (Statics.cpp).
	logger::info("[SNOW DEFORMATION] Landscape snow: hooking Lighting RestoreGeometry, Utility Setup/RestoreGeometry");
	stl::write_vfunc<0x7, LS_Lighting_RestoreGeometry>(RE::VTABLE_BSLightingShader[0]);
	stl::write_vfunc<0x6, LS_Utility_SetupGeometry>(RE::VTABLE_BSUtilityShader[0]);
	stl::write_vfunc<0x7, LS_Utility_RestoreGeometry>(RE::VTABLE_BSUtilityShader[0]);
}

bool SnowDeformation::LandscapeSnowRefresh()
{
	ls.cbValid = false;
	if (!lastShellCBData || !shellTerrainTexture || !shellTerrainTexture->srv || !GetDeformationSRV() || !heightTopFiltered || !heightBottomFiltered ||
		!heightTopRaw[heightCurrent] || !heightSkinDepth || !shellSnowSampler || !ShellFootprintHasSnow())
		return false;

	// DrawShellEarlyPrepass's recipe: last frame's snapshot, placed for this
	// frame's camera and paired with the fields as they stand now.
	auto& fb = globals::game::frameBufferCached;
	ShellCB cb = *lastShellCBData;
	cb.CameraViewProj = fb.GetCameraViewProj();
	cb.CameraViewProjUnjittered = fb.GetCameraViewProjUnjittered();
	cb.CameraPreviousViewProjUnjittered = fb.GetCameraPreviousViewProjUnjittered();
	cb.CameraView = fb.GetCameraView();
	cb.CameraPosAdjust = fb.GetCameraPosAdjust();
	cb.CameraPreviousPosAdjust = fb.GetCameraPreviousPosAdjust();
	RefreshShellGridPlacement(cb);
	cb.SeamRampInv = 0.0f;
	int seamMinX, seamMinY, seamMaxX, seamMaxY;
	if (LoadedCellSquare(seamMinX, seamMinY, seamMaxX, seamMaxY))
		SetShellSeamBounds(cb, seamMinX, seamMinY, seamMaxX, seamMaxY);
	cb.ShellDebugData = 0;
	cb.ShellLODDebug = 0;
	cb.DeformInvWorldSize = 1.0f / deformWorldSize;
	cb.ObjectHeightCenter = heightWindowCenter;
	cb.ObjectHeightHalfExtent = ObjectRasterHalfExtent();
	cb.BermBakeActive = (!shellBermBakeDisabled && bermFieldTexture) ? 1.0f : 0.0f;
	cb.UndulationFieldWindow = { undulationFieldCenter.x, undulationFieldCenter.y, 1.0f / kUndulationFieldHalfExtent,
		(undulationFieldValid && !shellUndulationBakeDisabled) ? 1.0f : 0.0f };
	cb.ExclusionFieldWindow = { exclusionFieldCenter.x, exclusionFieldCenter.y, 1.0f / kExclusionFieldHalfExtent,
		(exclusionFieldValid && !shellDistantExclusionsDisabled) ? 1.0f : 0.0f };
	const auto mapDim = [](const Texture2D* a_tex) { return (a_tex && a_tex->srv) ? float(a_tex->desc.Width) : 0.0f; };
	cb.FieldDims = { mapDim(deformationTextures[0]), mapDim(bermFieldTexture), mapDim(heightTopFiltered), mapDim(exclusionFieldTexture) };
	cb.TessScreen = { std::max(shellTessTargetPx, 0.0f), shellDrawViewport.Width * 0.5f, shellDrawViewport.Height * 0.5f, 0.0f };

	if (!ls.shellCB)
		ls.shellCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ShellCB>(), "SnowDeformation::LandscapeShellCB");
	ls.shellCB->Update(cb);
	lsCB = cb;

	auto& s = ls.srvs;
	std::fill(std::begin(s), std::end(s), nullptr);
	s[0] = shellTerrainTexture->srv.get();
	s[1] = GetDeformationSRV();
	s[4] = heightTopFiltered->srv.get();
	s[5] = heightBottomFiltered->srv.get();
	s[8] = shellSnowHeightSRV.get();
	s[11] = heightTopRaw[heightCurrent]->srv.get();
	s[12] = heightSkinDepth->srv.get();
	s[13] = (shellFineValid && shellTerrainFine) ? shellTerrainFine->srv.get() : nullptr;
	s[14] = GetBermFieldSRV();
	s[15] = GetExclusionFieldSRV();
	s[19] = waterNearTexture && waterNearTexture->srv ? waterNearTexture->srv.get() : nullptr;
	s[27] = ShellWaterSRV();
	s[28] = waterHeightTexture ? waterHeightTexture->srv.get() : nullptr;
	s[29] = GetUndulationFieldSRV();
	s[kExtentFieldSlot] = GetExtentFieldSRV();
	s[kUntouchedTilesSlot] = untouchedTilesSRV.get();
	s[kTerrainWetSlot] = TerrainWetSRV(cb.CompactLook.x > 0.5f);
	s[kTerrainPlainSlot] = TerrainPlainSRV(cb);
	ls.sampler = shellSnowSampler.get();

	// Where ShellEdgeFade can be above zero: the seam square, within the grid's span.
	const float2 spanLo = cb.GridOrigin;
	const float2 spanHi = { cb.GridOrigin.x + 2.0f * cb.WarpedHalfSpan, cb.GridOrigin.y + 2.0f * cb.WarpedHalfSpan };
	ls.liveRect = { spanLo.x, spanLo.y, spanHi.x, spanHi.y };
	if (cb.SeamRampInv > 0.0f)
		ls.liveRect = { std::max(spanLo.x, cb.SeamBounds.x), std::max(spanLo.y, cb.SeamBounds.y), std::min(spanHi.x, cb.SeamBounds.z), std::min(spanHi.y, cb.SeamBounds.w) };
	ls.cbValid = true;
	return true;
}

void SnowDeformation::LandscapeSnowSetup(RE::BSRenderPass* a_pass, bool a_lighting)
{
	if (ls.mode != LsMode::kNone)
		LsDetach();
	ls.curDraw = -1;
	const auto& cfg = ls.cfg;
	const bool raiseOn = landscapeSnow && settings.EnableSnowDeformation && !SnowShadersPending(1);
	if (!raiseOn && !cfg.measure)
		return;
	if (!a_pass || !a_pass->shaderProperty || !a_pass->geometry)
		return;
	if (!a_pass->shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape))
		return;
	auto* state = globals::state;
	auto* shader = state->currentShader;
	if (!shader)
		return;
	auto* context = globals::d3d::context;

	uint32_t cat;
	if (a_lighting) {
		cat = globals::deferred->deferredPass ? kLsMain : kLsLightingOther;
	} else {
		using U = SIE::ShaderCache::UtilityShaderFlags;
		const uint32_t raw = state->currentPixelDescriptor | state->currentVertexDescriptor;
		const auto& sky = globals::features::skylighting;
		// Skylighting's occlusion map draws through RenderDepth (the 512x512 target).
		if (sky.loaded && sky.inOcclusion)
			cat = kLsSkyOcclusion;
		else if (raw & uint32_t(U::RenderShadowmap))
			cat = (raw & uint32_t(U::RenderShadowmapPb)) ? kLsShadowLocal : kLsShadowSun;
		else if (raw & uint32_t(U::RenderDepth))
			cat = kLsDepth;
		else
			cat = kLsUtilityOther;
	}

	const uint32_t frame = state->frameCountAtomic.load(std::memory_order_relaxed);
	if (frame != ls.lastFrame)
		LsNewFrame(frame);
	auto& slot = ls.slots[ls.slotIndex];
	slot.draws[cat]++;
	auto* triShape = a_pass->geometry->AsTriShape();
	if (triShape)
		slot.tris[cat] += triShape->GetTrishapeRuntimeData().triangleCount;

	LsMode want = LsMode::kNone;
	if (raiseOn && cfg.attach[cat]) {
		want = LsMode::kRaise;
		// Terrain Blending's near prepass binds its own VS after this hook.
		if (cat == kLsDepth) {
			const auto& tb = globals::features::terrainBlending;
			if (tb.loaded && tb.renderDepth && tb.renderTerrainDepth)
				want = LsMode::kNone;
		}
		// The raise works in model space less a translate.
		const auto& world = a_pass->geometry->world;
		if (std::abs(world.scale - 1.0f) > 1e-4f)
			want = LsMode::kNone;
		for (int r = 0; r < 3 && want != LsMode::kNone; r++)
			for (int c = 0; c < 3; c++)
				if (std::abs(world.rotate.entry[r][c] - (r == c ? 1.0f : 0.0f)) > 1e-4f)
					want = LsMode::kNone;
		if (want != LsMode::kNone && (ls.cbFrame != frame || ls.cbCategory != cat)) {
			ls.cbFrame = frame;
			ls.cbCategory = cat;
			LandscapeSnowRefresh();
		}
		if (!ls.cbValid)
			want = LsMode::kNone;
	}
	float2 boundXY{};
	float boundRadius = 0.0f;
	if (want != LsMode::kNone) {
		const auto& b = a_pass->geometry->worldBound;
		boundXY = { b.center.x, b.center.y };
		boundRadius = b.radius;
		const auto& r = ls.liveRect;
		if (boundXY.x + boundRadius < r.x || boundXY.y + boundRadius < r.y || boundXY.x - boundRadius > r.z || boundXY.y - boundRadius > r.w)
			want = LsMode::kNone;
	}
	if (want == LsMode::kRaise && cfg.tessellate && (LsCameraPass(cat) || LsShadowPass(cat))) {
		const float reach = LsCameraPass(cat) ? cfg.tessReach : cfg.shadowTessReach;
		const auto& cam = lsCB.CameraPosAdjust;
		const float dx = boundXY.x - cam.x;
		const float dy = boundXY.y - cam.y;
		if (std::sqrt(dx * dx + dy * dy) - boundRadius < reach)
			want = LsMode::kTess;
	}
	LsEntry* entry = nullptr;
	if (want != LsMode::kNone) {
		const uint64_t key = (uint64_t(a_lighting ? 1 : 2) << 32) | state->modifiedVertexDescriptor;
		entry = &ls.entries[key];
		if (entry->state == 0) {
			entry->pending = std::async(std::launch::async, LsCompile, a_lighting, SIE::ShaderCache::GetDefinesString(*shader, state->modifiedVertexDescriptor));
			entry->state = 1;
		}
		if (entry->state == 1 && entry->pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			entry->shaders = entry->pending.get();
			const auto& sh = entry->shaders;
			entry->state = (sh.vsRaise && sh.vs && sh.hs && sh.ds) ? 2 : 3;
			if (entry->state == 2)
				logger::info("[SNOW DEFORMATION] Landscape snow {} {:X}: ready in {:.0f} ms", a_lighting ? "Lighting" : "Utility", state->modifiedVertexDescriptor, sh.compileMs);
			else
				logger::warn("[SNOW DEFORMATION] Landscape snow {} {:X}: shader compile FAILED, those draws stay unraised; defines: {}", a_lighting ? "Lighting" : "Utility", state->modifiedVertexDescriptor, sh.defines);
		}
		if (entry->state != 2)
			want = LsMode::kNone;
	}

	if (want != LsMode::kNone) {
		if (!ls.drawCB)
			ls.drawCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<LsCBData>(), "SnowDeformation::LandscapeSnowCB");
		const auto& t = a_pass->geometry->world.translate;
		const auto& grid = lsCB.GridOrigin;
		const bool parity = LsCameraPass(cat);
		const float sink = (parity && !GroundShellHidden()) ? std::max(cfg.shellSink, 0.0f) : 0.0f;
		LsCBData data{
			{ float(double(t.x) - double(grid.x)), float(double(t.y) - double(grid.y)), t.z, std::max(cfg.lift, 0.0f) },
			{ parity ? 1.0f : 0.0f, std::clamp(cfg.maxFactor, 1.0f, 64.0f), cfg.gate ? float(std::clamp(cfg.gateMask, 0, 31)) : 0.0f, cfg.cull ? 1.0f : 0.0f },
			{ std::max(cfg.guard, 0.0f), sink, 0.0f, 0.0f }
		};
		ls.drawCB->Update(data);
		ID3D11Buffer* own[2] = { ls.drawCB->CB(), ls.shellCB->CB() };

		ID3D11VertexShader* prevVS = nullptr;
		context->VSGetShader(&prevVS, nullptr, nullptr);
		ls.savedVS.attach(prevVS);
		if (want == LsMode::kTess) {
			ID3D11Buffer* cbs[13]{};
			context->VSGetConstantBuffers(0, 13, cbs);
			context->HSSetConstantBuffers(0, 13, cbs);
			context->DSSetConstantBuffers(0, 13, cbs);
			for (auto* b : cbs)
				if (b)
					b->Release();
			ID3D11Buffer* shared[3] = { state->permutationCB->CB(), state->sharedDataCB->CB(), state->featureDataCB->CB() };
			context->HSSetConstantBuffers(4, 3, shared);
			context->DSSetConstantBuffers(4, 3, shared);
			context->HSSetConstantBuffers(11, 1, &own[0]);
			context->DSSetConstantBuffers(11, 1, &own[0]);
			context->HSSetConstantBuffers(13, 1, &own[1]);
			context->DSSetConstantBuffers(13, 1, &own[1]);
			context->HSSetShaderResources(0, kLsSRVCount, ls.srvs);
			context->DSSetShaderResources(0, kLsSRVCount, ls.srvs);
			context->HSSetSamplers(0, 1, &ls.sampler);
			context->DSSetSamplers(0, 1, &ls.sampler);
			context->VSSetShader(entry->shaders.vs.get(), nullptr, 0);
			context->HSSetShader(entry->shaders.hs.get(), nullptr, 0);
			context->DSSetShader(entry->shaders.ds.get(), nullptr, 0);
			// The draw re-sends TRIANGLELIST only when the cached topology differs.
			globals::game::shadowState->GetRuntimeData().topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			globals::game::stateUpdateFlags->reset(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
			slot.tess[cat]++;
			entry->tessAttaches++;
		} else {
			context->VSGetShaderResources(0, kLsSRVCount, ls.savedVSSRVs);
			context->VSGetConstantBuffers(11, 1, &ls.savedVSCB[0]);
			context->VSGetConstantBuffers(13, 1, &ls.savedVSCB[1]);
			context->VSGetSamplers(0, 1, &ls.savedVSSampler);
			context->VSSetShaderResources(0, kLsSRVCount, ls.srvs);
			context->VSSetConstantBuffers(11, 1, &own[0]);
			context->VSSetConstantBuffers(13, 1, &own[1]);
			context->VSSetSamplers(0, 1, &ls.sampler);
			context->VSSetShader(entry->shaders.vsRaise.get(), nullptr, 0);
			slot.raise[cat]++;
			entry->raiseAttaches++;
		}
		ls.mode = want;
	}

	if (!cfg.measure || !slot.open)
		return;
	uint32_t targetIndex = kLsMaxTargets;
	if (!a_lighting) {
		targetIndex = LsTargetIndex(cat);
		if (targetIndex < kLsMaxTargets) {
			slot.targetDraws[targetIndex]++;
			if (triShape)
				slot.targetTris[targetIndex] += triShape->GetTrishapeRuntimeData().triangleCount;
		}
	}
	if (slot.drawCount >= kLsMaxDraws) {
		slot.overflow++;
		return;
	}
	const uint32_t i = slot.drawCount;
	auto* device = globals::d3d::device;
	while (slot.ts.size() < 2 * (i + 1)) {
		D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP, 0 };
		winrt::com_ptr<ID3D11Query> q;
		device->CreateQuery(&qd, q.put());
		slot.ts.push_back(q);
	}
	if (cfg.stats) {
		while (slot.stats.size() < i + 1) {
			D3D11_QUERY_DESC qd{ D3D11_QUERY_PIPELINE_STATISTICS, 0 };
			winrt::com_ptr<ID3D11Query> q;
			device->CreateQuery(&qd, q.put());
			slot.stats.push_back(q);
		}
	}
	if (!slot.ts[2 * i] || !slot.ts[2 * i + 1])
		return;
	slot.cat[i] = uint8_t(cat);
	slot.target[i] = uint8_t(targetIndex);
	slot.hasStats[i] = cfg.stats && slot.stats.size() > i && slot.stats[i] ? 1 : 0;
	context->End(slot.ts[2 * i].get());
	if (slot.hasStats[i])
		context->Begin(slot.stats[i].get());
	slot.drawCount++;
	ls.curDraw = int(i);
}

void SnowDeformation::LandscapeSnowRestore()
{
	if (ls.curDraw >= 0) {
		auto* context = globals::d3d::context;
		auto& slot = ls.slots[ls.slotIndex];
		const uint32_t i = uint32_t(ls.curDraw);
		context->End(slot.ts[2 * i + 1].get());
		if (slot.hasStats[i])
			context->End(slot.stats[i].get());
		ls.curDraw = -1;
	}
	if (ls.mode != LsMode::kNone)
		LsDetach();
}

std::string SnowDeformation::LandscapeSnowStatus() const
{
	uint32_t ready = 0, compiling = 0, failed = 0;
	uint64_t tess = 0, raise = 0;
	for (const auto& [key, e] : ls.entries) {
		ready += e.state == 2 ? 1 : 0;
		compiling += e.state == 1 ? 1 : 0;
		failed += e.state == 3 ? 1 : 0;
		tess += e.tessAttaches;
		raise += e.raiseAttaches;
	}
	return std::format("shader sets: {} ready, {} compiling, {} failed | draws raised so far: {} tessellated, {} vertex-only{}", ready, compiling, failed, tess, raise,
		ls.cbValid ? "" : " | waiting for the shell's fields (no snow in the footprint, or not drawn yet)");
}

json SnowDeformation::LandscapeSnowTool(const json& a_args)
{
	auto& c = ls.cfg;
	const auto rdb = [&](const char* k, bool& v) {
		if (a_args.contains(k) && a_args[k].is_boolean())
			v = a_args[k].get<bool>();
	};
	const auto rdf = [&](const char* k, float& v) {
		if (a_args.contains(k) && a_args[k].is_number())
			v = a_args[k].get<float>();
	};
	for (uint32_t i = 0; i < kLsCategories; i++)
		rdb(kLsNames[i], c.attach[i]);
	rdb("landscapeSnow", landscapeSnow);
	rdb("hideShell", landscapeHideShell);
	rdb("tessellate", c.tessellate);
	rdb("gate", c.gate);
	rdb("cull", c.cull);
	rdb("measure", c.measure);
	rdb("stats", c.stats);
	rdf("maxFactor", c.maxFactor);
	rdf("guard", c.guard);
	rdf("lift", c.lift);
	rdf("tessReach", c.tessReach);
	rdf("shadowTessReach", c.shadowTessReach);
	rdf("shellSink", c.shellSink);
	if (a_args.contains("gateMask") && a_args["gateMask"].is_number_integer())
		c.gateMask = std::clamp(a_args["gateMask"].get<int>(), 0, 31);
	c.maxFactor = std::clamp(c.maxFactor, 1.0f, 64.0f);
	if (a_args.contains("recompile") && a_args["recompile"].is_boolean() && a_args["recompile"].get<bool>()) {
		for (auto it = ls.entries.begin(); it != ls.entries.end();)
			it = it->second.state == 1 ? std::next(it) : ls.entries.erase(it);
		Util::ClearShaderCompileFailures();
	}

	json out;
	{
		std::lock_guard lock(lsMutex);
		if (a_args.contains("reset") && a_args["reset"].is_boolean() && a_args["reset"].get<bool>())
			ls.totals = {};
		const auto& t = ls.totals;
		const double f = t.frames ? double(t.frames) : 1.0;
		const double sf = t.statFrames ? double(t.statFrames) : 1.0;
		json cats = json::object();
		for (uint32_t i = 0; i < kLsCategories; i++) {
			cats[kLsNames[i]] = { { "gpuMs", t.gpuMs[i] / f }, { "draws", t.draws[i] / f }, { "tessellated", t.tess[i] / f }, { "raised", t.raise[i] / f }, { "baseTris", t.tris[i] / f },
				{ "stats", { { "iaPrims", t.ia[i] / sf }, { "vs", t.vs[i] / sf }, { "hs", t.hs[i] / sf }, { "ds", t.ds[i] / sf }, { "cInvocations", t.cinv[i] / sf }, { "cPrimitives", t.cprim[i] / sf } } } };
		}
		json targets = json::array();
		for (uint32_t k = 0; k < ls.targetCount; k++) {
			const auto& tg = ls.targets[k];
			targets.push_back({ { "category", kLsNames[tg.category] }, { "texture", std::format("{:X}", reinterpret_cast<uintptr_t>(tg.texture)) }, { "slice", tg.slice },
				{ "size", std::format("{}x{}x{}", tg.width, tg.height, tg.arraySize) }, { "gpuMs", t.targetMs[k] / f }, { "draws", t.targetDraws[k] / f }, { "baseTris", t.targetTris[k] / f } });
		}
		out["targets"] = targets;
		out["totals"] = { { "frames", t.frames }, { "statFrames", t.statFrames }, { "lost", t.lost }, { "disjoint", t.disjoint }, { "overflow", t.overflow }, { "categories", cats } };
	}
	json attach = json::object();
	for (uint32_t i = 0; i < kLsCategories; i++)
		attach[kLsNames[i]] = c.attach[i];
	out["config"] = { { "landscapeSnow", landscapeSnow }, { "hideShell", landscapeHideShell }, { "attach", attach }, { "tessellate", c.tessellate }, { "gate", c.gate }, { "cull", c.cull },
		{ "measure", c.measure }, { "stats", c.stats }, { "maxFactor", c.maxFactor }, { "guard", c.guard }, { "lift", c.lift }, { "tessReach", c.tessReach }, { "shadowTessReach", c.shadowTessReach }, { "gateMask", c.gateMask }, { "shellSink", c.shellSink } };
	json entries = json::array();
	for (const auto& [key, e] : ls.entries)
		entries.push_back({ { "type", (key >> 32) == 1 ? "Lighting" : "Utility" }, { "descriptor", std::format("{:X}", uint32_t(key)) }, { "state", e.state },
			{ "tessAttaches", e.tessAttaches }, { "raiseAttaches", e.raiseAttaches }, { "compileMs", e.shaders.compileMs }, { "defines", e.shaders.defines } });
	out["shaders"] = entries;
	out["surface"] = { { "valid", ls.cbValid }, { "gridOrigin", { lsCB.GridOrigin.x, lsCB.GridOrigin.y } },
		{ "liveRect", { ls.liveRect.x, ls.liveRect.y, ls.liveRect.z, ls.liveRect.w } }, { "camera", { lsCB.CameraPosAdjust.x, lsCB.CameraPosAdjust.y, lsCB.CameraPosAdjust.z } } };
	out["status"] = LandscapeSnowStatus();
	return out;
}
