#pragma once

// Stutter hunts: samples the render thread's stack while armed and reports
// where long frames spent their time against normal frames (DevBench
// snowdeformation.hitchsampler).
namespace SnowHitch
{
	/** @brief Once a frame on the render thread: a frame boundary, and the thread to watch. */
	void OnFrame();
	/** @brief Starts sampling every a_intervalUs microseconds; false when it cannot. */
	bool Start(uint32_t a_intervalUs);
	/** @brief Stops sampling and reports frames over a_thresholdMs against the rest, a_top rows a table. */
	json Report(float a_thresholdMs, uint32_t a_top);
}
