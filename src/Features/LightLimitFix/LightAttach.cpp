#include "Features/LightLimitFix/LightAttach.h"

#include "Features/LightLimitFix.h"
#include "Features/LightLimitFix/LightAttachGate.h"
#include "Globals.h"
#include "State.h"
#include "Utils/VersionedRelocation.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace LightAttach
{
	namespace
	{
		// The per-frame dynamic light call: inlined in ShadowSceneNode::ProcessQueuedLights on AE (same offset on 1.6 and 1.7).
		constexpr std::uintptr_t kDynamicEnableCallOffset = 0x6E2;
		// On SE 1.5.97 and VR 1.4.15 the same call sits in ShadowSceneNode::PurgeStaleLights, which the address library does not cover on VR.
		constexpr std::uintptr_t kSePurgeEnableCallRva = 0x12BE843;
		constexpr std::uintptr_t kVrPurgeEnableCallRva = 0x12FC2C3;
		// BSLight::frustrumCull value that marks a parabolic shadow caster.
		constexpr std::uint32_t kParabolicShadowCaster = 0xFF;

		std::mutex trackerLock;
		LightAttachGate::Tracker tracker;
		std::atomic<bool> tracking{ false };
		std::atomic<bool> installed{ false };

		/** @brief Captures what the attach walk reads from the light and its scene node. */
		LightAttachGate::Signature MakeSignature(RE::ShadowSceneNode& a_node, RE::BSLight& a_light)
		{
			LightAttachGate::Signature signature{};
			std::size_t next = 0;
			const auto append = [&](const auto& a_value) {
				static_assert(sizeof(a_value) % sizeof(std::uint32_t) == 0);
				std::memcpy(&signature[next], &a_value, sizeof(a_value));
				next += sizeof(a_value) / sizeof(std::uint32_t);
			};

			RE::NiLight* niLight = a_light.light.get();
			append(niLight);
			if (niLight) {
				const auto& runtime = niLight->GetLightRuntimeData();
				append(niLight->world.translate);
				append(runtime.radius);
				append(runtime.fade);
				append(niLight->GetFlags());
			}
			append(a_light.worldTranslate);
			append(a_light.portalGraph);
			append(a_node.GetRuntimeData().portalGraph);
			return signature;
		}

		struct DynamicLightEnable
		{
			static void thunk(RE::ShadowSceneNode* a_node, RE::BSLight* a_light)
			{
				auto& feature = globals::features::lightLimitFix;
				if (!feature.loaded || !feature.settings.SkipRedundantLightAttach) {
					if (tracking.exchange(false)) {
						std::lock_guard lock(trackerLock);
						tracker.Clear();
					}
					func(a_node, a_light);
					return;
				}

				if (!a_node || !a_light || a_light->frustrumCull == kParabolicShadowCaster || a_light->IsShadowLight()) {
					if (a_light) {
						std::lock_guard lock(trackerLock);
						tracker.Forget(a_light);
					}
					func(a_node, a_light);
					return;
				}

				tracking = true;
				bool walk;
				{
					const auto frame = globals::state->frameCountAtomic.load(std::memory_order_relaxed);
					const auto refreshFrames = static_cast<std::uint32_t>(feature.settings.LightAttachRefreshFrames);
					const auto before = MakeSignature(*a_node, *a_light);
					std::lock_guard lock(trackerLock);
					walk = tracker.ShouldWalk(a_light, before, frame, refreshFrames);
				}
				if (!walk) {
					return;
				}

				func(a_node, a_light);
				const auto after = MakeSignature(*a_node, *a_light);
				std::lock_guard lock(trackerLock);
				tracker.RecordWalk(a_light, after);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool IsSupported()
	{
		return installed.load(std::memory_order_relaxed);
	}

	void Install()
	{
		const auto callSite = REL::Module::IsAE() ?
		                          REL::RelocationID(99701, 106335).address() + kDynamicEnableCallOffset :
		                          REL::Offset(REL::Module::IsVR() ? kVrPurgeEnableCallRva : kSePurgeEnableCallRva).address();
		if (!Util::VersionedRelocation::IsCallTo(callSite, REL::RelocationID(99708, 106342).address())) {
			logger::warn("[LLF] Light attach gate not applied: unexpected code at the per-frame light enable (another mod or an unverified runtime)");
			return;
		}
		stl::write_thunk_call<DynamicLightEnable>(callSite);
		installed = true;
		logger::info("[LLF] Installed light attach gate");
	}
}
