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
}
#endif

void SnowDeformation::InstallDevBenchTools()
{
#ifdef DEVBENCH_BRIDGE_ENABLED
	auto* dvb = DevBenchAPI::GetDevBenchInterface001();
	if (!dvb)
		return;
	static constexpr const char* shaderTypesDesc =
		R"({"description":"The F10 performance overlay's shader-type table: per BSShader type, smoothed draw calls and CPU wall time (ms) between draw submissions attributed to that type - NOT GPU time. live=false means the overlay is hidden and the numbers are stale (toggle it with F10).","readOnly":true,"inputSchema":{"type":"object","properties":{}}})";
	dvb->RegisterTool("snowdeformation.shadertypes", shaderTypesDesc, &ShaderTypesTool, nullptr);
	logger::info("[SNOW DEFORMATION] DevBench tool registered: snowdeformation.shadertypes");
#endif
}
