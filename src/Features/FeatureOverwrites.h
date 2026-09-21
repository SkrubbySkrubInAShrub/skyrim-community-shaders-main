#pragma once

#include "Feature.h"

struct FeatureOverwrites : Feature
{
	std::string GetName() override { return "Feature Overwrites"; }
	std::string GetDisplayName() override { return T("feature.feature_overwrites.name", "Feature Overwrites"); }
	std::string GetShortName() override { return "FeatureOverwrites"; }
	std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	bool IsCore() const override { return true; }
	bool IsAlwaysEnabled() const override { return true; }
	/// This feature manages other features' overwrite files and persists nothing of its own.
	bool UsesMainSettings() const override { return false; }

	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
	void DrawSettings() override;
};
