#pragma once

#include <DirectXMath.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "HiZPyramid.h"

/**
 * @brief Copies the GPU Hi-Z pyramid back to system memory for CPU culling.
 *
 * A depth buffer cannot be read on the frame it is written without stalling the pipeline, so the
 * copy is queued and collected a few frames later with MAP_FLAG_DO_NOT_WAIT. Every snapshot
 * therefore describes an older camera and is stamped with the view-projection that produced it;
 * \ref HiZCull projects into that frame rather than the current one. How stale the snapshot is
 * depends entirely on how early the copy is queued, which is why \ref Deferred::BuildHiZ runs in
 * the prepass rather than at present.
 */
namespace HiZReadback
{
	/** @brief One captured pyramid plus the camera state that produced it. */
	struct Snapshot
	{
		static constexpr std::uint32_t kMaxMips = 8;

		std::vector<float> texels;  ///< every mip packed back to back, padded dimensions

		std::array<std::uint32_t, kMaxMips> mipOffset{};  ///< index into texels of each mip
		std::array<std::uint32_t, kMaxMips> mipPitch{};   ///< row stride of each mip, in floats
		std::uint32_t                       mipCount = 0;

		float validWidth = 0.0f;   ///< level-0 texels actually covered by the rendered image
		float validHeight = 0.0f;  ///< (the texture itself is padded past this; padding reads as far)
		float texelPixels = 4.0f;  ///< nominal screen pixels per level-0 texel
		float projScale = 0.0f;    ///< screenH / (2 * |frustum.fTop|), as the grass cull uses

		DirectX::XMMATRIX viewProj = DirectX::XMMatrixIdentity();
		float             posAdjust[3]{};  ///< world origin viewProj is relative to
		std::uint32_t     frame = 0;

		/** @brief The farthest depth over a 3x3 texel block at @p level, clamped to valid texels. */
		float TileMax(int a_level, int a_x0, int a_y0, int a_validW, int a_validH) const;
	};

	/** @brief Queues this frame's pyramid and collects an older one. Render thread, once a frame. */
	void Update(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx, const HiZPyramid& a_pyramid);

	/**
	 * @brief The snapshot the cull threads test against, or nullptr when none is ready.
	 *
	 * The returned pointer stays valid for the frame: slots are recycled round-robin and there
	 * are more of them than there are frames of latency.
	 */
	const Snapshot* Current();

	/** @brief Per-frame counters for the settings UI and the logs. */
	struct Stats
	{
		std::uint32_t age;  ///< frames between capture and publication
	};
	Stats GetStats();
}
