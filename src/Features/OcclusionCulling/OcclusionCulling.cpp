#include "OcclusionCulling.h"

#include "HiZCull.h"
#include "HiZReadback.h"

#include "Globals.h"
#include "Utils/UI.h"

#include <RE/B/BSCullingProcess.h>
#include <RE/B/BSMultiBound.h>
#include <RE/B/BSParabolicCullingProcess.h>
#include <RE/N/NiAVObject.h>
#include <RE/N/NiCamera.h>

#include <imgui.h>

#define I18N_KEY_PREFIX "feature.occlusion_culling."

namespace
{
	// Mirror the ENGINE's own cull side effect when we skip an object: clear its kAccumulated
	// flag exactly like BSCullingProcess does on a frustum cull (gated on recurseToGeometry +
	// updateAccumulateFlag). Without this, downstream consumers can read a STALE accumulated bit
	// from a previous frame on an object we culled -- our culling has to be a strict superset of
	// vanilla culling's effects, not just its verdicts.
	inline void MarkCulledLikeEngine(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object)
	{
		auto* bsp = static_cast<RE::BSCullingProcess*>(a_self);
		if (bsp->recurseToGeometry && a_self->updateAccumulateFlag)
			a_object->GetFlags().reset(RE::NiAVObject::Flag::kAccumulated);
	}

	// BSCullingProcess::Process1 (NiCullingProcess vtable index 0x16): the per-object processing
	// and recursion driver. Skipping the original call drops the object and its whole subtree.
	template <class HookT>
	void Process1_Impl(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object, std::int32_t a_arg2)
	{
		// Gate on the camera, not on a per-process marker. The main view runs several concurrent
		// cull processes and only whichever held the marker would be tested, so a walk's verdict
		// would depend on thread scheduling. The camera check covers every main-view walk while
		// still excluding shadow, reflection, cubemap and first-person culls.
		const bool isSceneList = HiZCull::IsSceneListCamera(a_self->camera);
		if (isSceneList && a_object && !HiZCull::TestObject(a_object)) {
			MarkCulledLikeEngine(a_self, a_object);
			return;
		}

		// SUN SHADOW CASTER culling: the stage-1 caster pre-gather runs through this same body
		// with the dir light's gather camera, so a rejection here removes the caster from EVERY
		// cascade (the lists rebuild each frame, there is no cross-frame caching). The guards
		// mirror the engine's own testable subset: zero-radius and kAlwaysDraw(0x800)/0x1000
		// objects are never tested, nor are actors.
		if (!isSceneList && a_object && HiZCull::IsSunGatherCamera(a_self->camera)) {
			const auto fl = a_object->GetFlags().underlying();
			if (a_object->worldBound.radius > 0.0f && !(fl & 0x800) && !(fl & 0x1000)) {
				auto* ref = a_object->GetUserData();
				if (!ref || ref->formType != RE::FormType::ActorCharacter) {
					if (!HiZCull::TestShadowCasterSmall(a_object)) {
						MarkCulledLikeEngine(a_self, a_object);
						return;
					}
				}
			}
		}

		HookT::func(a_self, a_object, a_arg2);
	}

	struct Process1_Hook
	{
		static void thunk(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object, std::int32_t a_arg2)
		{
			Process1_Impl<Process1_Hook>(a_self, a_object, a_arg2);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Same hook for BSParabolicCullingProcess, which OVERRIDES Process1/Process2 with its own
	// bodies. The main-scene subtree culls run on the global parabolic process, so hooking only
	// BSCullingProcess never sees them. Separate structs keep each body's original pointer.
	struct PProcess1_Hook
	{
		static void thunk(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object, std::int32_t a_arg2)
		{
			Process1_Impl<PProcess1_Hook>(a_self, a_object, a_arg2);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Process2 (vtable 0x17) is the top-level cull entry. Latch the camera for this pass.
	template <class HookT>
	void Process2_Impl(RE::NiCullingProcess* a_self, const RE::NiCamera* a_camera, RE::NiAVObject* a_scene, RE::NiVisibleArray* a_visibleSet)
	{
		auto* feature = OcclusionCulling::GetSingleton();
		if (feature->IsActive())
			HiZCull::BeginCull(a_camera);

		HookT::func(a_self, a_camera, a_scene, a_visibleSet);
	}

	struct Process2_Hook
	{
		static void thunk(RE::NiCullingProcess* a_self, const RE::NiCamera* a_camera, RE::NiAVObject* a_scene, RE::NiVisibleArray* a_visibleSet)
		{
			Process2_Impl<Process2_Hook>(a_self, a_camera, a_scene, a_visibleSet);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PProcess2_Hook
	{
		static void thunk(RE::NiCullingProcess* a_self, const RE::NiCamera* a_camera, RE::NiAVObject* a_scene, RE::NiVisibleArray* a_visibleSet)
		{
			Process2_Impl<PProcess2_Hook>(a_self, a_camera, a_scene, a_visibleSet);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// TestBaseVisibility1(BSMultiBound&) -- the engine's CONTAINER visibility path (rooms, cells,
	// building shells). Multibound nodes never reach Process1, so this is where the high-value
	// tests belong: one occluded container prunes everything inside it. Engine verdict first; we
	// only ever downgrade visible -> occluded.
	template <class HookT>
	bool TestBaseVis1_Impl(RE::BSCullingProcess* a_self, RE::BSMultiBound* a_bound)
	{
		const bool visible = HookT::func(a_self, a_bound);
		if (visible && a_bound &&
			HiZCull::IsSceneListCamera(static_cast<RE::NiCullingProcess*>(a_self)->camera) &&
			!HiZCull::TestMultiBound(a_bound))
			return false;
		return visible;
	}

	struct TestBaseVis1_Hook
	{
		static bool thunk(RE::BSCullingProcess* a_self, RE::BSMultiBound* a_bound)
		{
			return TestBaseVis1_Impl<TestBaseVis1_Hook>(a_self, a_bound);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PTestBaseVis1_Hook
	{
		static bool thunk(RE::BSCullingProcess* a_self, RE::BSMultiBound* a_bound)
		{
			return TestBaseVis1_Impl<PTestBaseVis1_Hook>(a_self, a_bound);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Distant-tree LOD seam. BSMultiStreamInstanceTriShape carries a dummy worldBound plus
	// kAlwaysDraw, so object-level tests never see it; the real culling is per instance group
	// inside OnVisible. This post-hook ANDs an occlusion verdict into groups the engine kept --
	// stateless, since the flag is rewritten by every walk, and downgrade-only. Grass shares this
	// class, so distant trees are selected by shader property and grass is left alone.
	struct MSITS_OnVisible_Hook
	{
		static void thunk(RE::BSMultiStreamInstanceTriShape* a_this, RE::NiCullingProcess* a_process, std::int32_t a_alphaGroupIndex)
		{
			func(a_this, a_process, a_alphaGroupIndex);

			if (!a_process || !HiZCull::IsSceneListCamera(a_process->camera))
				return;
			if (!HiZCull::CullTreeLODGroups)
				return;
			auto* prop = a_this->GetGeometryRuntimeData().shaderProperty.get();
			if (!prop || !netimmerse_cast<RE::BSDistantTreeShaderProperty*>(prop))
				return;
			// InstanceGroup derives from BSMultiBoundAABB, so it is its own bound.
			for (auto* group : a_this->GetMultiStreamTrishapeRuntimeData().instanceGroups) {
				if (!group || !group->isVisible)
					continue;
				if (!HiZCull::TestInstanceGroup(group))
					group->isVisible = false;
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void OcclusionCulling::PostPostLoad()
{
	// CS_OCCLUSION=1 flips the master toggle on at boot for automated runs.
	char buf[16] = {};
	if (GetEnvironmentVariableA("CS_OCCLUSION", buf, sizeof(buf)) && buf[0] == '1')
		settings.EnableOcclusionTesting = true;

	// CS_OCCLUSION_NO_HOOKS=1: install nothing, for a baseline without even trampoline overhead.
	if (GetEnvironmentVariableA("CS_OCCLUSION_NO_HOOKS", buf, sizeof(buf)) && buf[0] == '1') {
		logger::warn("[OcclusionCulling] CS_OCCLUSION_NO_HOOKS=1: no hooks installed");
		return;
	}

	// Detour the function BODIES of both culling-process classes. Body detours are essential:
	// the main cull walk calls Process1 devirtualized, so a vtable patch never sees it.
	//
	// The bodies are reached through the class vtables rather than through address-library ids
	// for the bodies themselves: the ids the SE line used have no AE counterpart, while
	// VTABLE_BSCullingProcess and VTABLE_BSParabolicCullingProcess are variant ids that resolve
	// on every runtime. Installed unconditionally; behaviour is gated inside the thunks.
	constexpr std::size_t kProcess1 = 0x16;
	constexpr std::size_t kProcess2 = 0x17;
	constexpr std::size_t kTestBaseVisibility1 = 0x1A;

	const auto p1b = stl::detour_vtable_body<kProcess1, Process1_Hook>(RE::VTABLE_BSCullingProcess[0]);
	const auto p2b = stl::detour_vtable_body<kProcess2, Process2_Hook>(RE::VTABLE_BSCullingProcess[0]);
	const auto tb1 = stl::detour_vtable_body<kTestBaseVisibility1, TestBaseVis1_Hook>(RE::VTABLE_BSCullingProcess[0]);

	// The parabolic process overrides all three, so its slots are distinct bodies. Guard anyway:
	// detouring one address twice would chain the hook onto itself.
	const auto paraVtable = reinterpret_cast<std::uintptr_t*>(
		REL::Relocation<std::uintptr_t>(RE::VTABLE_BSParabolicCullingProcess[0]).address());

	if (paraVtable[kProcess1] != p1b)
		stl::detour_vtable_body<kProcess1, PProcess1_Hook>(RE::VTABLE_BSParabolicCullingProcess[0]);
	if (paraVtable[kProcess2] != p2b)
		stl::detour_vtable_body<kProcess2, PProcess2_Hook>(RE::VTABLE_BSParabolicCullingProcess[0]);
	if (paraVtable[kTestBaseVisibility1] != tb1)
		stl::detour_vtable_body<kTestBaseVisibility1, PTestBaseVis1_Hook>(RE::VTABLE_BSParabolicCullingProcess[0]);

	stl::write_vfunc<0x34, MSITS_OnVisible_Hook>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);

	SyncSettings();

	logger::info("[OcclusionCulling] hooks installed (testing={})", settings.EnableOcclusionTesting);
}

bool OcclusionCulling::IsActive() const
{
	return loaded && settings.EnableOcclusionTesting;
}

void OcclusionCulling::SyncSettings()
{
	HiZCull::EnableOcclusionTesting = settings.EnableOcclusionTesting;
	HiZCull::HiZOcclusionBias = settings.HiZOcclusionBias;
	HiZCull::HiZMaxCameraMotion = settings.HiZMaxCameraMotion;
	HiZCull::ObjectTestMinRadius = settings.ObjectTestMinRadius;
	HiZCull::CullTreeLODGroups = settings.CullTreeLOD;
	HiZCull::HideFrames = std::max(1u, settings.HideFrames);

	HiZCull::CullSmallObjects = settings.CullSmallObjects;
	HiZCull::ObjectCullNearRadius = settings.ObjectCullNearRadius;
	HiZCull::ObjectCullDistSlope = settings.ObjectCullDistSlope;

	HiZCull::CullSmallShadows = settings.CullSmallShadows;
	HiZCull::ShadowCullNearRadius = settings.ShadowCullNearRadius;
	HiZCull::ShadowCullDistSlope = settings.ShadowCullDistSlope;
}

void OcclusionCulling::DrawSettings()
{
	ImGui::TextWrapped("%s", T(TKEY("about"),
		"Skips drawing objects the camera cannot see, by testing them against the depth buffer "
		"the GPU already produced this frame. Removing a draw is worth more under DXVK than on "
		"the native driver, because DXVK pays more CPU per draw."));
	ImGui::Spacing();

	bool changed = false;
	changed |= ImGui::Checkbox(T(TKEY("enabled"), "Enable Occlusion Culling"), &settings.EnableOcclusionTesting);

	if (ImGui::CollapsingHeader(T(TKEY("occlusion"), "Occlusion"), ImGuiTreeNodeFlags_DefaultOpen)) {
		changed |= ImGui::SliderFloat(T(TKEY("bias"), "Depth Bias"), &settings.HiZOcclusionBias, 0.0f, 0.005f, "%.5f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("bias_tooltip"),
				"Tolerance on the depth comparison. Larger values cull less. Raise it if objects "
				"flicker at the edges of walls."));
		}

		changed |= ImGui::SliderFloat(T(TKEY("max_motion"), "Max Camera Motion"), &settings.HiZMaxCameraMotion, 0.0f, 4096.0f, "%.0f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("max_motion_tooltip"),
				"The depth buffer reaches the CPU a few frames late, so it describes a slightly "
				"older camera. Once the camera has travelled further than this since that "
				"capture, the buffer is ignored rather than trusted. Lower is safer."));
		}

		changed |= ImGui::SliderFloat(T(TKEY("test_min_radius"), "Minimum Test Size"), &settings.ObjectTestMinRadius, 0.0f, 512.0f, "%.0f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("test_min_radius_tooltip"),
				"Objects smaller than this are drawn without being tested. A tiny object costs "
				"little to draw, so testing it is rarely worth the time."));
		}

		changed |= ImGui::Checkbox(T(TKEY("cull_tree_lod"), "Cull Distant Tree LOD"), &settings.CullTreeLOD);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("cull_tree_lod_tooltip"),
				"Tests distant-tree LOD groups as well. Usually costs more than it saves in the open, "
				"where the groups are large and rarely fully hidden; worth enabling in dense forest."));
		}

		int hideFrames = int(settings.HideFrames);
		if (ImGui::SliderInt(T(TKEY("hide_frames"), "Frames Before Hiding"), &hideFrames, 1, 8)) {
			settings.HideFrames = std::uint32_t(hideFrames);
			changed = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("hide_frames_tooltip"),
				"How many frames in a row an object must test as hidden before it stops being "
				"drawn. 1 reacts fastest and can make distant detail flicker; higher is steadier."));
		}
	}

	if (ImGui::CollapsingHeader(T(TKEY("small_objects"), "Small Objects"))) {
		ImGui::TextWrapped("%s", T(TKEY("small_objects_about"),
			"Drops objects that cover almost none of the screen. Unlike occlusion culling this is "
			"a quality trade, not a lossless one: the threshold grows with distance, so distant "
			"clutter disappears while nearby objects never do."));
		changed |= ImGui::Checkbox(T(TKEY("cull_small_objects"), "Cull Small Objects"), &settings.CullSmallObjects);
		changed |= ImGui::SliderFloat(T(TKEY("object_near_radius"), "Near Size"), &settings.ObjectCullNearRadius, 0.0f, 64.0f, "%.1f");
		changed |= ImGui::SliderFloat(T(TKEY("object_slope"), "Size Growth"), &settings.ObjectCullDistSlope, 0.0f, 0.05f, "%.4f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("object_slope_tooltip"),
				"An object is dropped when its radius is below Near Size + Size Growth x distance."));
		}
	}

	if (ImGui::CollapsingHeader(T(TKEY("small_shadows"), "Small Shadow Casters"))) {
		ImGui::TextWrapped("%s", T(TKEY("small_shadows_about"),
			"The same rule applied to shadows. Dropping a caster removes it from every cascade, "
			"and screen-space shadows regenerate the near-field contact shadows those small "
			"casters would have contributed."));
		changed |= ImGui::Checkbox(T(TKEY("cull_small_shadows"), "Cull Small Shadow Casters"), &settings.CullSmallShadows);
		changed |= ImGui::SliderFloat(T(TKEY("shadow_near_radius"), "Near Size"), &settings.ShadowCullNearRadius, 0.0f, 128.0f, "%.1f");
		changed |= ImGui::SliderFloat(T(TKEY("shadow_slope"), "Size Growth"), &settings.ShadowCullDistSlope, 0.0f, 0.05f, "%.4f");
	}

	if (ImGui::CollapsingHeader(T(TKEY("statistics"), "Statistics"))) {
		const auto stats = HiZCull::GetStats();
		ImGui::Text("%s: %u", T(TKEY("stat_tested"), "Tested"), stats.tested);
		ImGui::Text("%s: %u", T(TKEY("stat_culled"), "Culled"), stats.culled);
		ImGui::Text("%s: %u", T(TKEY("stat_small_objects"), "Small objects dropped"), stats.smallObjects);
		ImGui::Text("%s: %u", T(TKEY("stat_small_shadows"), "Small shadow casters dropped"), stats.smallShadows);
		ImGui::Text("%s: %u", T(TKEY("stat_age"), "Depth buffer age (frames)"), stats.snapshotAge);
	}

	if (changed)
		SyncSettings();
}

void OcclusionCulling::LoadSettings(json& o_json)
{
	const auto loadBool = [&](const char* key, bool& dst) {
		if (o_json[key].is_boolean())
			dst = o_json[key];
	};
	const auto loadFloat = [&](const char* key, float& dst) {
		if (o_json[key].is_number())
			dst = o_json[key];
	};

	loadBool("EnableOcclusionTesting", settings.EnableOcclusionTesting);
	loadFloat("HiZOcclusionBias", settings.HiZOcclusionBias);
	loadFloat("HiZMaxCameraMotion", settings.HiZMaxCameraMotion);
	loadFloat("ObjectTestMinRadius", settings.ObjectTestMinRadius);
	loadBool("CullTreeLOD", settings.CullTreeLOD);
	if (o_json["HideFrames"].is_number_unsigned())
		settings.HideFrames = o_json["HideFrames"];

	loadBool("CullSmallObjects", settings.CullSmallObjects);
	loadFloat("ObjectCullNearRadius", settings.ObjectCullNearRadius);
	loadFloat("ObjectCullDistSlope", settings.ObjectCullDistSlope);

	loadBool("CullSmallShadows", settings.CullSmallShadows);
	loadFloat("ShadowCullNearRadius", settings.ShadowCullNearRadius);
	loadFloat("ShadowCullDistSlope", settings.ShadowCullDistSlope);

	SyncSettings();
}

void OcclusionCulling::SaveSettings(json& o_json)
{
	o_json["EnableOcclusionTesting"] = settings.EnableOcclusionTesting;
	o_json["HiZOcclusionBias"] = settings.HiZOcclusionBias;
	o_json["HiZMaxCameraMotion"] = settings.HiZMaxCameraMotion;
	o_json["ObjectTestMinRadius"] = settings.ObjectTestMinRadius;
	o_json["CullTreeLOD"] = settings.CullTreeLOD;
	o_json["HideFrames"] = settings.HideFrames;

	o_json["CullSmallObjects"] = settings.CullSmallObjects;
	o_json["ObjectCullNearRadius"] = settings.ObjectCullNearRadius;
	o_json["ObjectCullDistSlope"] = settings.ObjectCullDistSlope;

	o_json["CullSmallShadows"] = settings.CullSmallShadows;
	o_json["ShadowCullNearRadius"] = settings.ShadowCullNearRadius;
	o_json["ShadowCullDistSlope"] = settings.ShadowCullDistSlope;
}

void OcclusionCulling::RestoreDefaultSettings()
{
	settings = {};
	SyncSettings();
}

#undef I18N_KEY_PREFIX
