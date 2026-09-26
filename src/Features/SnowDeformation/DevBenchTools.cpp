#include "Features/SnowDeformation.h"

#include "Features/PerformanceOverlay.h"
#include "Globals.h"
#include "Menu.h"
#include "State.h"

#ifdef DEVBENCH_BRIDGE_ENABLED
#	include <DevBenchAPI.h>
#endif

// Read-only DevBench tools for perf loops. The shader-type table is the F10
// overlay's: CPU wall time between draw submissions attributed to the drawing
// shader type, smoothed 0.95/0.05, and only updated while the overlay shows.

#ifdef DEVBENCH_BRIDGE_ENABLED
namespace
{
	void ShaderTypesTool(void*, const char*, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			auto* state = globals::state;
			const bool live = globals::menu && globals::menu->overlayVisible && globals::features::performanceOverlay.loaded && globals::features::performanceOverlay.IsOverlayVisible();
			json types = json::array();
			float total = 0.0f;
			{
				auto lock = state->Lock();
				State::ForEachShaderTypeWithMetrics([&](auto a_type, int, float a_drawCalls, float a_frameTime, float a_percent, float a_costPerCall) {
					types.push_back({ { "type", std::string(magic_enum::enum_name(a_type)) }, { "drawCalls", a_drawCalls }, { "cpuMs", a_frameTime }, { "percent", a_percent }, { "cpuMsPerCall", a_costPerCall } });
				});
				total = state->smoothFrameTimePerType[magic_enum::enum_integer(RE::BSShader::Type::Total)];
			}
			out = { { "live", live }, { "totalCpuMs", total }, { "frame_count", state->frameCountAtomic.load(std::memory_order_relaxed) }, { "types", types } };
		} catch (const std::exception& e) {
			out = { { "error", e.what() } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}

	void CpuCensusTool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			if (a_argsJson && *a_argsJson) {
				const json args = json::parse(a_argsJson);
				if (args.contains("contactRowMarks") && args["contactRowMarks"].is_boolean())
					globals::features::snowDeformation.debugContactRowMarks = args["contactRowMarks"].get<bool>();
				if (args.contains("markSettleMode") && args["markSettleMode"].is_number_integer())
					globals::features::snowDeformation.trenchMarkSettleMode = std::clamp(args["markSettleMode"].get<int>(), 0, 2);
			}
			globals::features::snowDeformation.cpuCensusArmedUntil = globals::state->frameCount + 120;
			const auto& s = globals::features::snowDeformation.cpuShown;
			json spans = json::array();
			for (uint32_t i = 0; i < SnowDeformation::CpuCensus::kSpanCount; i++)
				spans.push_back({ { "name", SnowDeformation::CpuCensus::kSpanNames[i] }, { "ms", s.spanMs[i] }, { "calls", s.spanCalls[i] } });
			const auto& cc = SnowDeformation::cpuCensus;
			json stampers = json::array();
			for (uint32_t i = 0; i < cc.lastStampSourceCount; i++) {
				const auto* form = RE::TESForm::LookupByID(cc.lastStampSources[i].formID);
				stampers.push_back({ { "formId", std::format("0x{:08X}", cc.lastStampSources[i].formID) }, { "name", form && form->GetName() ? form->GetName() : "" }, { "stamps", cc.lastStampSources[i].stamps } });
			}
			json storePaths = json::object();
			for (uint32_t i = 0; i < SnowDeformation::CpuCensus::kStorePathCount; i++)
				storePaths[SnowDeformation::CpuCensus::kStorePathNames[i]] = { { "calls", cc.storeCalls[i] }, { "raises", cc.storeRaises[i] } };
			out = { { "spans", spans }, { "storePaths", storePaths },
				{ "stampsTotal", cc.stampsTotal }, { "carveStampsTotal", cc.carveStampsTotal }, { "rowsMarkedTotal", cc.rowsMarkedTotal }, { "contactRectsTotal", cc.contactRectsTotal }, { "contactRowMarks", globals::features::snowDeformation.debugContactRowMarks },
				{ "markSettleMode", globals::features::snowDeformation.trenchMarkSettleMode }, { "settledSkipsTotal", cc.settledSkipsTotal },
				{ "stampersThisFrame", cc.stampSourceCount }, { "lastStampers", stampers }, { "lastStampersFrame", cc.lastStampSourceFrame },
				{ "hookMs", s.hookMs }, { "hookCalls", s.hookCalls }, { "actorMs", s.actorMs }, { "actors", s.actors },
				{ "landMs", s.landMs }, { "landCalls", s.landCalls }, { "depthMs", s.depthMs }, { "depthCalls", s.depthCalls },
				{ "traverseMs", s.traverseMs }, { "traverseCalls", s.traverseCalls },
				{ "frame_count", globals::state->frameCountAtomic.load(std::memory_order_relaxed) } };
		} catch (const std::exception& e) {
			out = { { "error", e.what() } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}

	void ShellTool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			const json args = a_argsJson && *a_argsJson ? json::parse(a_argsJson) : json::object();
			out = globals::features::snowDeformation.DevBenchShellArgs(args);
		} catch (const std::exception& e) {
			out = { { "error", e.what() } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}

	void VolumeTool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			const json args = a_argsJson && *a_argsJson ? json::parse(a_argsJson) : json::object();
			out = globals::features::snowDeformation.DevBenchVolumeArgs(args);
		} catch (const std::exception& e) {
			out = { { "error", e.what() } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}

	void TrenchProbeTool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			const json args = a_argsJson && *a_argsJson ? json::parse(a_argsJson) : json::object();
			json depths = json::array();
			for (const auto& p : args.value("points", json::array()))
				depths.push_back({ { "x", p.at(0) }, { "y", p.at(1) }, { "stored", globals::features::snowDeformation.ProbeTrenchStore(p.at(0).get<float>(), p.at(1).get<float>()) } });
			out = { { "points", depths } };
		} catch (const std::exception& e) {
			out = { { "error", e.what() } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}
}
#endif

json SnowDeformation::DevBenchShellArgs(const json& a_args)
{
	if (a_args.contains("shellKill") && a_args["shellKill"].is_number_integer())
		shellKillMask = a_args["shellKill"].get<uint32_t>();
	if (a_args.contains("shellSplitTiming") && a_args["shellSplitTiming"].is_boolean())
		shellSplitTiming = a_args["shellSplitTiming"].get<bool>();
	if (a_args.contains("shellPrepass") && a_args["shellPrepass"].is_boolean())
		shellDepthPrepassDisabled = !a_args["shellPrepass"].get<bool>();
	if (a_args.contains("shellBake") && a_args["shellBake"].is_boolean())
		shellVertexBakeDisabled = !a_args["shellBake"].get<bool>();
	if (a_args.contains("shellStats") && a_args["shellStats"].is_boolean())
		shellPipelineStatsEnabled = a_args["shellStats"].get<bool>();
	if (a_args.contains("shellVisibleCull") && a_args["shellVisibleCull"].is_boolean())
		shellVisibleCull = a_args["shellVisibleCull"].get<bool>();
	if (a_args.contains("shellPrepassLastFrame") && a_args["shellPrepassLastFrame"].is_boolean())
		shellPrepassLastFrame = a_args["shellPrepassLastFrame"].get<bool>();
	if (a_args.contains("shellMainCullAll") && a_args["shellMainCullAll"].is_boolean())
		shellMainCullAll = a_args["shellMainCullAll"].get<bool>();
	if (a_args.contains("effectCostProbe") && a_args["effectCostProbe"].is_boolean() && a_args["effectCostProbe"].get<bool>())
		StartEffectCostProbe();
	if (a_args.contains("coneSkip") && a_args["coneSkip"].is_boolean())
		coneSkipDisabled = !a_args["coneSkip"].get<bool>();
	if (a_args.contains("shadowAtlasLive") && a_args["shadowAtlasLive"].is_boolean())
		shadowAtlasLiveReadOff = !a_args["shadowAtlasLive"].get<bool>();
	if (a_args.contains("pointShadowLive") && a_args["pointShadowLive"].is_boolean())
		pointShadowLiveReadOff = !a_args["pointShadowLive"].get<bool>();
	if (a_args.contains("casterFullGrid") && a_args["casterFullGrid"].is_boolean())
		shellCasterFullGrid = a_args["casterFullGrid"].get<bool>();
	if (a_args.contains("hullTrust") && a_args["hullTrust"].is_boolean())
		shellHullTrustOff = !a_args["hullTrust"].get<bool>();
	if (a_args.contains("gatherEveryScroll") && a_args["gatherEveryScroll"].is_boolean())
		exclusionGatherEveryScroll = a_args["gatherEveryScroll"].get<bool>();
	if (a_args.contains("hiZCopyPath") && a_args["hiZCopyPath"].is_boolean())
		hiZCopyPath = a_args["hiZCopyPath"].get<bool>();
	if (a_args.contains("patchSplit") && a_args["patchSplit"].is_boolean())
		shellPatchSplit = a_args["patchSplit"].get<bool>();
	if (a_args.contains("prepassExit") && a_args["prepassExit"].is_boolean())
		shellPrepassExitOff = !a_args["prepassExit"].get<bool>();
	if (a_args.contains("skinPrepassExit") && a_args["skinPrepassExit"].is_boolean())
		staticsPrepassExitOff = !a_args["skinPrepassExit"].get<bool>();
	if (a_args.contains("tessTargetPx") && a_args["tessTargetPx"].is_number())
		shellTessTargetPx = std::clamp(a_args["tessTargetPx"].get<float>(), 0.0f, 64.0f);
	if (a_args.contains("churnParallax") && a_args["churnParallax"].is_boolean())
		shellChurnParallaxOff = !a_args["churnParallax"].get<bool>();
	if (a_args.contains("tessDiagonalUnflip") && a_args["tessDiagonalUnflip"].is_boolean())
		shellTessDiagonalUnflip = a_args["tessDiagonalUnflip"].get<bool>();
	if (a_args.contains("lodDebugView") && a_args["lodDebugView"].is_number_integer())
		lodDebugView = std::clamp(a_args["lodDebugView"].get<int>(), 0, 4);
	if (a_args.contains("projSnowTint") && a_args["projSnowTint"].is_boolean())
		debugProjSnowView = a_args["projSnowTint"].get<bool>();
	static constexpr const char* kPresetNames[4] = { "low", "medium", "high", "ultra" };
	static constexpr const char* kGateNames[3] = { "snowy", "unknown", "bare" };
	if (a_args.contains("qualityPreset") && a_args["qualityPreset"].is_string()) {
		const auto wanted = a_args["qualityPreset"].get<std::string>();
		for (int i = 0; i < 4; i++)
			if (wanted == kPresetNames[i])
				ApplyQualityPreset(i);
	}
	const int preset = MatchingQualityPreset();
	json effectCosts = json::array();
	for (uint32_t i = 0; i < EffectCostProbe::kEffects && effectCost.done; i++)
		effectCosts.push_back({ { "effect", EffectCostProbe::kNames[i] }, { "ms", effectCost.withMs[i] - effectCost.withoutMs[i] }, { "withMs", effectCost.withMs[i] } });
	json lodHist = json::array();
	for (uint32_t v : lodHistData)
		lodHist.push_back(v);
	const auto& s = shellStatsLast;
	return { { "shellKill", shellKillMask }, { "shellSplitTiming", shellSplitTiming }, { "shellPrepass", !shellDepthPrepassDisabled },
		{ "shellBake", !shellVertexBakeDisabled }, { "shellStats", shellPipelineStatsEnabled }, { "shellVisibleCull", shellVisibleCull }, { "shellPrepassLastFrame", shellPrepassLastFrame }, { "shellMainCullAll", shellMainCullAll },
		{ "tessellation", settings.Tessellation },
		{ "coneSkip", !coneSkipDisabled }, { "conePasses", { { "coarse", conePassesCoarse }, { "fine", conePassesFine } } },
		{ "shadowAtlasLive", !shadowAtlasLiveReadOff }, { "shadowAtlasCopyActive", shadowAtlasLiveReadOff && shadowCopyWantedFrames > 0 }, { "shadowAtlasCopyHeld", shadowAtlasCopyTex != nullptr },
		{ "pointShadowLive", !pointShadowLiveReadOff }, { "pointShadowCopyHeld", pointShadowAtlasCopyTex != nullptr },
		{ "casterFullGrid", shellCasterFullGrid }, { "casterIndices", shellGridCasterIndexCount }, { "hullTrust", !shellHullTrustOff }, { "gatherEveryScroll", exclusionGatherEveryScroll }, { "exclusionCount", statExclusionCount }, { "exclusionWalk", { { "slot", exclusionGatherSlot }, { "slots", exclusionGatherSlots } } }, { "hiZCopyPath", hiZCopyPath }, { "patchSplit", shellPatchSplit }, { "patchSplitActive", shellPatchSplitThisFrame }, { "patchCounts", { { "flat", shellPatchFlatCount }, { "tessellated", shellPatchTessCount } } },
		{ "tessDiagonalUnflip", shellTessDiagonalUnflip }, { "lodDebugView", lodDebugView }, { "projSnowTint", debugProjSnowView }, { "lodHistogram", lodHist }, { "prepassExit", !shellPrepassExitOff }, { "skinPrepassExit", !staticsPrepassExitOff }, { "tessTargetPx", shellTessTargetPx }, { "churnParallax", !shellChurnParallaxOff }, { "tessViewportHalf", { lastShellCBData ? lastShellCBData->TessScreen.y : 0.0f, lastShellCBData ? lastShellCBData->TessScreen.z : 0.0f } },
		{ "qualityPreset", preset < 0 ? "custom" : kPresetNames[preset] },
		{ "gates", { { "shell", kGateNames[std::min(shellSnowVerdict, 2u)] }, { "shellBareFrames", shellGateBareFrames }, { "deform", kGateNames[std::min(deformSnowVerdict, 2u)] }, { "deformBareFrames", deformGateBareFrames }, { "groundSuspended", GroundShellsSuspended() }, { "captured", capturedStatics.size() }, { "heightMapValid", heightMapValid } } },
		{ "effectCost", { { "running", effectCost.running }, { "done", effectCost.done }, { "step", effectCost.step }, { "costs", effectCosts } } },
		{ "stats", { { "valid", s.valid }, { "vs", s.vsInvocations }, { "hs", s.hsInvocations }, { "ds", s.dsInvocations },
					   { "ps", s.psInvocations }, { "primitives", s.rasterizedPrimitives }, { "samplesPassed", s.samplesPassed } } } };
}

json SnowDeformation::DevBenchVolumeArgs(const json& a_args)
{
	if (a_args.contains("memorySeconds") && a_args["memorySeconds"].is_number())
		voxelMemorySeconds = std::clamp(a_args["memorySeconds"].get<float>(), 0.5f, 30.0f);
	if (a_args.contains("showRings") && a_args["showRings"].is_boolean())
		showVolumeRings = a_args["showRings"].get<bool>();
	if (a_args.contains("forceRebuild") && a_args["forceRebuild"].is_boolean())
		debugForceVolumeRebuild = a_args["forceRebuild"].get<bool>();
	if (a_args.contains("stats") && a_args["stats"].is_boolean())
		voxelPipelineStatsEnabled = a_args["stats"].get<bool>();
	json levels = json::array();
	for (uint L = 0; L < VoxelLevelsLive(); L++) {
		const auto& lv = voxelLevels[L];
		levels.push_back({ { "level", L }, { "valid", lv.valid }, { "rebuilds", lv.rebuilds }, { "voxelSize", VoxelSizeForLevel(L) },
			{ "dims", { lv.dims.x, lv.dims.y, lv.dims.z } }, { "origin", { lv.origin.x, lv.origin.y, lv.origin.z } },
			{ "stableRebuilds", lv.stableRebuilds }, { "skipped", lv.skipped } });
	}
	return { { "volumeSnow", settings.VolumeSnow }, { "shadersFailed", voxelShadersFailed }, { "memorySeconds", voxelMemorySeconds },
		{ "showRings", showVolumeRings }, { "occupancyL0", voxelOccupancyValid ? json(voxelOccupancy) : json(nullptr) },
		{ "forceRebuild", debugForceVolumeRebuild }, { "idleSkipped", voxelIdleSkipped }, { "idleBlockers", voxelIdleBlockers }, { "skipRate", voxelSkipRate },
		{ "stats", { { "enabled", voxelPipelineStatsEnabled }, { "valid", voxelStatsLast.valid }, { "vs", voxelStatsLast.vsInvocations }, { "ps", voxelStatsLast.psInvocations },
					   { "primitives", voxelStatsLast.rasterizedPrimitives }, { "samplesPassed", voxelStatsLast.samplesPassed } } },
		{ "dirtyBricks", settings.VolumeDirtyBricks }, { "staggeredCapture", settings.VolumeStaggeredCapture }, { "ringShape", settings.VolumeRingShape },
		{ "capturedStatics", capturedStatics.size() }, { "levels", levels } };
}

void SnowDeformation::InstallDevBenchTools()
{
#ifdef DEVBENCH_BRIDGE_ENABLED
	auto* dvb = DevBenchAPI::GetDevBenchInterface001();
	if (!dvb)
		return;
	static constexpr const char* shaderTypesDesc =
		R"({"description":"The F10 performance overlay's shader-type table: per BSShader type, smoothed draw calls and CPU wall time (ms) between draw submissions attributed to that type - NOT GPU time. live=false means the overlay is hidden and the numbers are stale (toggle it with F10).","readOnly":true,"inputSchema":{"type":"object","properties":{}}})";
	dvb->RegisterTool("snowdeformation.shadertypes", shaderTypesDesc, &ShaderTypesTool, nullptr);
	static constexpr const char* cpuCensusDesc =
		R"({"description":"Snow Deformation CPU census: 30-frame averages (ms, rdtsc calibrated to QPC) of named CPU spans - Prepass and DrawShell whole, DrawShell's consecutive segments - plus the capture hook, actor/land stamping, depth and traversal counters. Stamps: stampsTotal/carveStampsTotal/rowsMarkedTotal are cumulative (read deltas); lastStampers lists the actors that emitted stamps in the latest frame that had any (props and spells not attributed). Arg contactRowMarks (bool) toggles the runtime A/B for contact-draw row marking; markSettleMode (0 off, 1 quantized keys, 2 hysteresis) picks how settled sources stop marking.","inputSchema":{"type":"object","properties":{"contactRowMarks":{"type":"boolean"},"markSettleMode":{"type":"integer"}}}})";
	dvb->RegisterTool("snowdeformation.cpucensus", cpuCensusDesc, &CpuCensusTool, nullptr);
	static constexpr const char* trenchProbeDesc =
		R"({"description":"Persistent trench store depth (0-1, bilinear) at world positions in the active worldspace. points: [[x,y],...].","readOnly":true,"inputSchema":{"type":"object","properties":{"points":{"type":"array","items":{"type":"array","items":{"type":"number"}}}},"required":["points"]}})";
	dvb->RegisterTool("snowdeformation.trenchprobe", trenchProbeDesc, &TrenchProbeTool, nullptr);
	static constexpr const char* shellDesc =
		R"({"description":"Landscape shell measurement switches (runtime-only). shellKill: bitmask zeroing PS features before the ShellCB upload (1 parallax, 2 normal map, 4 RMAOS, 8 crisp shadows, 16 screen-space shadows, 32 SSS remarch, 64 glints, 128 point lights, 256 skylighting, 512 albedo texture; the statics shell shares the CB). shellSplitTiming: Shell row split into Shell.Setup/Prepass/Fill/Main. shellPrepass, shellBake: the depth prepass and the vertex bake. shellVisibleCull: the shading pass tessellates only patches that won a prepass pixel. shellPrepassLastFrame: the prepass tessellates only last frame's visible patches (exact only for a still camera and scene). shellMainCullAll: the shading draw culls every patch (VS+HS cost only, breaks the image). coneSkip: false runs every cone pass (A/B; conePasses reports the counts run). shadowAtlasLive: false copies the sun atlas every frame instead of sampling it in place (A/B, default true; shadowAtlasCopyHeld says whether the copy texture exists). pointShadowLive: the same for the local lights' shadow maps (row Shadows - Local Atlas Copy when off). casterFullGrid: true casts sun shadows from the whole grid instead of the inner lattice (A/B). hullTrust: false makes the shading pass's hull re-run the prepass culls (A/B). gatherEveryScroll: true re-walks the loaded references for exclusion sources on every window scroll instead of once a second (A/B; the census span DrawShell.ExclusionGather is the cost). hiZCopyPath: true builds the skin-cull Hi-Z chain through scratch copies again (A/B). patchSplit: false draws the shading pass's whole grid through the tessellator again instead of the visible flat patches with a plain VS (A/B; patchCounts reports the two lists). tessDiagonalUnflip: true restores the tessellated patches' old, land-crossing split (A/B). prepassExit: false runs the depth prepass's alpha cut on fragments already hidden behind the scene again (A/B). skinPrepassExit: false does the same for the object skins' prepass (A/B). tessTargetPx: the shell's deformation tessellation keeps no more segments than an edge's projected length holds at this many pixels each (default 16; 0 = distance factor alone); tessViewportHalf reports the render viewport half size. churnParallax: the churn and berm clods leave the geometry and are marched in the shading pass (default on; false = the old geometric churn). lodDebugView: 0-4 selects Distant Snow & LOD > Debug View (1 = Depth Delta Heatmap, whose 4 bands x 8 buckets histogram comes back as lodHistogram). qualityPreset: low|medium|high|ultra applies that preset (the menu buttons' code); the reply names the preset the settings match. effectCostProbe: true starts the effect-cost probe (each shading effect off in turn, paired with an all-on read, ~50 s); results in effectCost. shellStats: D3D11 pipeline statistics around the shell draws, returned in stats (read two frames late). No args = read state. gates (read-only): the shell and deformation snow-presence verdicts (snowy / unknown / bare), their bare-frame counts, groundSuspended, the captured-statics count and heightMapValid.","inputSchema":{"type":"object","properties":{"shellKill":{"type":"integer"},"shellSplitTiming":{"type":"boolean"},"shellPrepass":{"type":"boolean"},"shellBake":{"type":"boolean"},"shellStats":{"type":"boolean"},"shellVisibleCull":{"type":"boolean"},"shellPrepassLastFrame":{"type":"boolean"},"shellMainCullAll":{"type":"boolean"},"effectCostProbe":{"type":"boolean"},"qualityPreset":{"type":"string"},"coneSkip":{"type":"boolean"},"shadowAtlasLive":{"type":"boolean"},"pointShadowLive":{"type":"boolean"},"casterFullGrid":{"type":"boolean"},"hullTrust":{"type":"boolean"},"gatherEveryScroll":{"type":"boolean"},"hiZCopyPath":{"type":"boolean"},"patchSplit":{"type":"boolean"},"tessDiagonalUnflip":{"type":"boolean"},"prepassExit":{"type":"boolean"},"skinPrepassExit":{"type":"boolean"},"tessTargetPx":{"type":"number"},"churnParallax":{"type":"boolean"},"lodDebugView":{"type":"integer"}}}})";
	dvb->RegisterTool("snowdeformation.shell", shellDesc, &ShellTool, nullptr);
	static constexpr const char* volumeDesc =
		R"({"description":"Volume snow measurement switches (runtime-only, not serialised). memorySeconds: Volume Memory, 0.5-30 s. showRings: Colour Rings. forceRebuild: rebuild every ring on its cadence (the idle gate's A/B; false = gate on). stats: pipeline statistics + occlusion around the volume draw, returned in stats (PS invocations over samples passed = marches per written pixel; read two frames late). Returns the volume's state: shader failure, level-0 occupancy count, capture count, and per level valid/rebuilds/voxel size/dims/origin. No args = read state.","inputSchema":{"type":"object","properties":{"memorySeconds":{"type":"number"},"showRings":{"type":"boolean"},"forceRebuild":{"type":"boolean"},"stats":{"type":"boolean"}}}})";
	dvb->RegisterTool("snowdeformation.volume", volumeDesc, &VolumeTool, nullptr);
	logger::info("[SNOW DEFORMATION] DevBench tools registered: snowdeformation.shadertypes, snowdeformation.cpucensus");
#endif
}
