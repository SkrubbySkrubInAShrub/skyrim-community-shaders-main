#pragma once

#include "Feature.h"

/**
 * @brief CPU occlusion culling for the main view, against the GPU's Hi-Z depth pyramid.
 *
 * Hooks the engine's scene-graph cull walk and skips objects that are provably hidden, so the
 * draw is never issued. Worth more under DXVK than on the native driver, which pays less per
 * draw on the render thread.
 *
 * The buffer is built by \ref Deferred::BuildHiZ and copied back by \ref HiZReadback; this
 * feature only reads it. Three culls, each independently switchable: occlusion, small objects,
 * and small shadow casters.
 */
struct OcclusionCulling : public Feature
{
	static OcclusionCulling* GetSingleton()
	{
		static OcclusionCulling singleton;
		return &singleton;
	}

	struct Settings
	{
		/// Master gate. Every cull in this feature is behind it, so toggling it is a true A/B.
		bool EnableOcclusionTesting = false;

		// --- occlusion ---------------------------------------------------------------------

		/// NDC-z tolerance on the depth comparison. Only ever makes culling less likely.
		float HiZOcclusionBias = 0.0005f;
		/// Camera travel since the snapshot is the only thing that can reveal hidden geometry.
		/// Past this the snapshot is refused outright.
		float HiZMaxCameraMotion = 1024.0f;
		/// Do not occlusion-test bounds below this world radius; the draw saved is not worth it.
		float ObjectTestMinRadius = 0.0f;
		/// Occlusion-test distant-tree LOD instance groups. Off by default: a net cost at open
		/// venues, where the groups are large and rarely fully hidden.
		bool CullTreeLOD = false;

		/// Consecutive frames an object must read hidden before it is culled; 1 disables the
		/// hysteresis. Culling on a single reading lets distant LOD flicker.
		std::uint32_t HideFrames = 2;

		// --- small objects ------------------------------------------------------------------

		/// Drop objects whose bounding sphere is smaller than (near + slope * distance). Off by
		/// default: a slight net loss where clutter is sparse, since anything small enough to
		/// qualify was already cheap to draw.
		bool  CullSmallObjects = false;
		float ObjectCullNearRadius = 8.0f;
		float ObjectCullDistSlope = 0.004f;

		// --- small shadow casters -----------------------------------------------------------

		/// The same rule on the sun's caster gather, where a rejection removes the caster from
		/// every cascade. Screen-space shadows regenerate the near-field contact shadows those
		/// casters would have contributed.
		bool  CullSmallShadows = true;
		float ShadowCullNearRadius = 32.0f;
		float ShadowCullDistSlope = 0.012f;
	};

	Settings settings;

	virtual std::string GetName() override { return "Occlusion Culling"; }
	virtual std::string GetShortName() override { return "OcclusionCulling"; }

	/** @brief Installs the cull-walk hooks. */
	virtual void PostPostLoad() override;

	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	/** @brief Pushes the current settings into the cull module. */
	void SyncSettings();

	/** @brief True when the feature is loaded and testing is enabled. */
	bool IsActive() const;
};
