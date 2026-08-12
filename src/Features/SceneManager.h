#pragma once

#include "Feature.h"
#include "SceneSettingsManager.h"

struct SceneManager : Feature, SceneSettingsManager
{
	std::string GetName() override { return "Scene Manager"; }
	std::string GetDisplayName() override { return T("feature.scene_manager.name", "Scene Manager"); }
	std::string GetShortName() override { return "SceneManager"; }
	std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	bool SupportsVR() override { return true; }
	bool IsCore() const override { return true; }
	bool IsAlwaysEnabled() const override { return true; }
	bool IsDisabledByDefault() const override { return false; }
	bool UsesMainSettings() const override { return false; }
	bool HasRestoreDefaults() const override { return false; }

	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
	void SetupResources() override;
	void DataLoaded() override;
	void Update();
};
