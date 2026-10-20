#pragma once

// Runtime-only measurement switches set through DevBench's
// snowdeformation.shell (its thread writes, the render thread reads).
namespace SnowDevSwitches
{
	// decalOverlaySplit: the Decals - Overlay row's segments time under their
	// own names instead of sharing the row's, to see which part costs.
	inline std::atomic<bool> decalOverlaySplit{ false };

	inline const char* DecalOverlayRow(const char* a_part)
	{
		return decalOverlaySplit.load(std::memory_order_relaxed) ? a_part : "SnowDeformation::Decals - Overlay";
	}

	// heightMapSplit: the same for the Objects - Height Maps row (layer 1,
	// ghost merge, fine, far, peel, peel merge, combine, cones, sky).
	inline std::atomic<bool> heightMapSplit{ false };

	inline const char* HeightMapRow(const char* a_part)
	{
		return heightMapSplit.load(std::memory_order_relaxed) ? a_part : "SnowDeformation::Objects - Height Maps";
	}
}
