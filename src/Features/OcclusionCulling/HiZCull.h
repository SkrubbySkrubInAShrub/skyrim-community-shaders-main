#pragma once

#include <DirectXMath.h>

#include <cstdint>

namespace RE
{
	class NiAVObject;
	class NiCamera;
	class BSMultiBoundAABB;
	class NiPoint3;
}

/**
 * @brief CPU occlusion culling against the GPU's Hi-Z depth pyramid.
 *
 * The scene-graph cull walk asks this whether an object can be skipped. The pyramid is built by
 * \ref Deferred::BuildHiZ from the opaque depth buffer and copied to system memory by
 * \ref HiZReadback; nothing here rasterizes.
 *
 * Also hosts the size culls, which are pure distance arithmetic and need no buffer: small
 * objects fold into \ref TestObject, small shadow casters into \ref TestShadowCasterSmall.
 */
namespace HiZCull
{
	// --- settings, pushed from OcclusionCulling::SyncSettings -------------------------------

	extern bool  EnableOcclusionTesting;  ///< master gate; every cull here is behind it
	extern float HiZOcclusionBias;        ///< NDC-z tolerance on the depth comparison
	extern float HiZMaxCameraMotion;      ///< refuse a snapshot once the camera has travelled this far
	extern float ObjectTestMinRadius;     ///< do not occlusion-test bounds smaller than this

	extern bool  CullSmallObjects;
	extern float ObjectCullNearRadius;
	extern float ObjectCullDistSlope;

	extern bool  CullSmallShadows;
	extern float ShadowCullNearRadius;
	extern float ShadowCullDistSlope;

	extern bool CullTreeLODGroups;  ///< occlusion-test distant-tree LOD instance groups

	/// Consecutive frames an object must read hidden before it is culled; 1 disables the
	/// hysteresis. See \ref Stabilize for why a single reading is not enough.
	extern std::uint32_t HideFrames;

	// --- frame lifecycle --------------------------------------------------------------------

	/**
	 * @brief Advances the frame index every occlusion decision is keyed on.
	 *
	 * Exactly once per rendered frame, from the present hook. Deliberately not
	 * BSGraphics::State::frameCount, which does not advance on this runtime.
	 */
	void BeginFrame();

	/** @brief The frame index set by \ref BeginFrame. */
	std::uint32_t CurrentFrame();

	/** @brief Latches the camera for this cull pass. Returns true if testing should happen. */
	bool BeginCull(const RE::NiCamera* a_camera);

	/**
	 * @brief Camera-relative view-projection for @p a_camera, plus the origin it is relative to.
	 *
	 * Shared with the readback so a captured buffer is stamped with its camera using the same
	 * reconstruction the tests project with; two derivations that disagreed would cull along
	 * silhouette edges.
	 */
	void CameraViewProj(RE::NiCamera* a_camera, DirectX::XMMATRIX& a_outViewProj, RE::NiPoint3& a_outPosAdjust);

	// --- the culls --------------------------------------------------------------------------

	/**
	 * @brief Occlusion query for a scene object.
	 * @return true if it may be visible, false if provably hidden.
	 */
	bool TestObject(RE::NiAVObject* a_object);

	/** @brief Occlusion query for a multibound container (rooms, cells, building shells). */
	bool TestMultiBound(void* a_multiBound);

	/** @brief Occlusion query for one distant-tree LOD instance group. */
	bool TestInstanceGroup(RE::BSMultiBoundAABB* a_aabb);

	/**
	 * @brief The same rule on the sun's caster gather. Returns true to keep.
	 *
	 * A rejection removes the caster from every cascade.
	 */
	bool TestShadowCasterSmall(RE::NiAVObject* a_object);

	// --- camera identification --------------------------------------------------------------

	/** @brief True when @p a_camera is the main render camera driving the scene lists. */
	bool IsSceneListCamera(const RE::NiCamera* a_camera);

	/** @brief True when @p a_camera is the sun's shadow-caster gather camera. */
	bool IsSunGatherCamera(const RE::NiCamera* a_camera);

	/** @brief Per-frame counters for the settings UI. */
	struct Stats
	{
		std::uint32_t tested;
		std::uint32_t culled;
		std::uint32_t culledSphere;
		std::uint32_t culledAABB;
		std::uint32_t flips;     ///< verdict changed from the previous frame
		std::uint32_t deferred;  ///< hidden, but not yet for long enough to cull
		std::uint32_t smallObjects;
		std::uint32_t smallShadows;
		std::uint32_t snapshotAge;
	};
	Stats GetStats();
}
