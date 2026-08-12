#pragma once

#include <array>
#include <string_view>
#include <vector>

namespace SceneSettingsPolicy
{
	using SettingBlacklistPath = std::vector<std::string_view>;

	/// Settings that must never be scene-overridden, matched by catalog address prefix.
	inline const std::vector<SettingBlacklistPath> kSettingBlacklist = {};

	inline constexpr std::array<std::string_view, 5> kLocationFeatureWhitelist = {
		"ExponentialHeightFog",
		"ImageBasedLighting",
		"ScreenSpaceGI",
		"ScreenSpaceShadows",
		"SubsurfaceScattering",
	};

	inline constexpr std::array<std::string_view, 7> kTimeOfDayFeatureWhitelist = {
		"CloudShadows",
		"ExponentialHeightFog",
		"GrassLighting",
		"ImageBasedLighting",
		"Skylighting",
		"SubsurfaceScattering",
		"WetnessEffects",
	};
}
