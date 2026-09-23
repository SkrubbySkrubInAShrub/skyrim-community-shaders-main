#include "SceneSettingsOverwrites.h"

#include "Feature.h"

#include <algorithm>
#include <filesystem>

using namespace SceneSettingsInternal;

namespace SceneSettingsOverwrites
{
	bool HasOverwriteEntryForPeriod(const std::vector<SceneSettingsManager::SettingEntry>& entries,
		const SceneSettingsManager::SettingEntry& candidate)
	{
		return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
			return entry.source == SceneSettingsManager::EntrySource::Overwrite &&
			       entry.period == candidate.period &&
			       IsSameSetting(entry, candidate.featureShortName, candidate.settingPath, candidate.settingKey);
		});
	}

	bool AddOverwriteEntryIfUnique(std::vector<SceneSettingsManager::SettingEntry>& entries,
		SceneSettingsManager::SettingEntry&& entry, std::string_view context)
	{
		// Files are scanned lexicographically. The first overwrite for an address and period wins.
		if (HasOverwriteEntryForPeriod(entries, entry)) {
			logger::warn("[SceneSettings] Duplicate {} overwrite for {}.{} ({}) skipped",
				context, entry.featureShortName, entry.settingKey, entry.sourceFilename);
			return false;
		}

		entries.push_back(std::move(entry));
		return true;
	}

	std::filesystem::path GetOverwriteDir(const std::filesystem::path& baseDir,
		SceneSettingsManager::TimeOfDayPeriod period)
	{
		return period != SceneSettingsManager::TimeOfDayPeriod::Count ?
		           baseDir / SceneSettingsManager::GetPeriodName(period) :
		           baseDir;
	}

	std::filesystem::path GetOverwriteFilePath(const std::filesystem::path& baseDir,
		const SceneSettingsManager::SettingEntry& entry)
	{
		if (!entry.sourcePath.empty())
			return entry.sourcePath;
		return GetOverwriteDir(baseDir, entry.period) / entry.sourceFilename;
	}

	std::string GetOverwriteTypeDescription(std::string_view sceneLabel,
		SceneSettingsManager::TimeOfDayPeriod period)
	{
		return period != SceneSettingsManager::TimeOfDayPeriod::Count ?
		           std::format("{} - {}", sceneLabel, SceneSettingsManager::GetPeriodName(period)) :
		           std::string(sceneLabel);
	}

	std::filesystem::path GetSceneOverwritePath(SceneSettingsManager::SceneType type,
		const SceneSettingsManager::SettingEntry& entry)
	{
		return GetOverwriteFilePath(SceneSettingsManager::GetOverwritesPath(type), entry);
	}

	std::filesystem::path GetWeatherOverwritePath(RE::FormID weatherId, const SceneSettingsManager::SettingEntry& entry)
	{
		return GetOverwriteFilePath(
			SceneSettingsManager::GetWeatherOverwritesDir() / Util::FormIdToSpid(weatherId), entry);
	}

	std::filesystem::path GetLocationOverwritePath(std::string_view formKey,
		const SceneSettingsManager::SettingEntry& entry)
	{
		return GetOverwriteFilePath(SceneSettingsManager::GetLocationOverwritesDir() / formKey, entry);
	}

	bool WriteGroupedOverwriteFile(const std::filesystem::path& path, const std::string& featureShortName,
		const std::string& overwriteType, const std::vector<const SceneSettingsManager::SettingEntry*>& entries,
		const json& extraMetadata)
	{
		std::error_code ec;
		const auto pathExists = std::filesystem::exists(path, ec);
		if (ec) {
			logger::error("[SceneSettings] WriteGroupedOverwriteFile: could not inspect '{}': {}", path.string(), ec.message());
			return false;
		}

		json data = json::object();
		if (pathExists && !ReadBoundedSceneJson(path, data)) {
			logger::error("[SceneSettings] Refusing to replace invalid overwrite file '{}'", path.string());
			return false;
		}

		if (auto featureIt = data.find(kFeatureKey); featureIt != data.end() &&
			(!featureIt->is_string() || featureIt->get<std::string>() != featureShortName)) {
			logger::error("[SceneSettings] Refusing to relabel overwrite file '{}' from another feature", path.string());
			return false;
		}
		data[kFeatureKey] = featureShortName;
		auto& metadata = data[kMetadataKey];
		if (!metadata.is_null() && !metadata.is_object()) {
			logger::error("[SceneSettings] Refusing to replace invalid metadata in overwrite file '{}'", path.string());
			return false;
		}
		if (metadata.is_null())
			metadata = json::object();
		metadata[kMetadataDescriptionKey] = std::format("{} scene settings overwrite ({})",
			SceneSettingsManager::GetFeatureDisplayName(featureShortName), overwriteType);
		if (extraMetadata.is_object())
			for (const auto& [key, value] : extraMetadata.items())
				metadata[key] = value;
		for (const auto* entry : entries) {
			auto* node = GetObjectAtPath(data, entry->settingPath, true);
			if (!node) {
				logger::error("[SceneSettings] Refusing to replace a non-object path in overwrite file '{}'",
					path.string());
				return false;
			}
			(*node)[entry->settingKey] = entry->value;
		}

		return WriteJsonAtomically(path, data, kOverwriteJsonIndent, "overwrite file");
	}

	bool RemoveSettingFromOverwriteFile(const std::filesystem::path& path,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		if (path.empty())
			return true;

		std::error_code ec;
		if (!std::filesystem::exists(path, ec))
			return !ec;

		// The read handle has to be closed before the rewrite below: Windows refuses to replace or
		// delete a file that still has one open.
		json data;
		if (!ReadBoundedSceneJson(path, data)) {
			logger::error("[SceneSettings] Could not read overwrite file '{}' for editing", path.string());
			return false;
		}

		if (!RemoveObjectValueAtPath(data, settingPath, 0, settingKey)) {
			logger::error("[SceneSettings] Overwrite setting '{}' was not found in '{}'",
				settingKey, path.string());
			return false;
		}
		if (!HasSceneOverwriteContent(data)) {
			auto removed = std::filesystem::remove(path, ec);
			if (removed || !ec)
				return true;
			logger::error("[SceneSettings] Failed to delete overwrite file '{}': {}", path.string(), ec.message());
			return false;
		}

		return WriteJsonAtomically(path, data, kOverwriteJsonIndent, "overwrite file");
	}

	bool ParseOverwriteFileEntries(const std::filesystem::path& filePath,
		SceneSettingsManager::SceneType allowedType, bool requireNumeric,
		std::vector<SceneSettingsManager::SettingEntry>& outEntries, FeatureSettingsCache* featureSettingsCache,
		std::optional<bool>* timeOfDayEnabled)
	{
		using SSM = SceneSettingsManager;

		json data;
		if (!ReadBoundedSceneJson(filePath, data))
			return false;
		if (auto metadataIt = data.find(kMetadataKey); timeOfDayEnabled && metadataIt != data.end() &&
													   metadataIt->is_object()) {
			if (auto modeIt = metadataIt->find(kTimeOfDayEnabledKey); modeIt != metadataIt->end()) {
				if (modeIt->is_boolean())
					*timeOfDayEnabled = modeIt->get<bool>();
				else
					logger::warn("[SceneSettings] Overwrite '{}' {} metadata must be boolean", filePath.string(),
						kTimeOfDayEnabledKey);
			}
		}

		std::string featureShortName = data.value(kFeatureKey, "");
		if (featureShortName.empty()) {
			auto stem = filePath.stem().string();
			auto lastUnderscore = stem.rfind('_');
			if (lastUnderscore != std::string::npos)
				featureShortName = stem.substr(lastUnderscore + 1);
		}

		auto* featurePtr = Feature::FindFeatureByShortName(featureShortName);
		if (!featurePtr || !SSM::IsFeatureAllowedForType(allowedType, featureShortName))
			return false;

		bool foundAny = false;
		CollectOverwriteEntries(data, {}, [&](const auto& settingPath, const auto& key, const auto& value) {
			json parsedValue = value;
			if (requireNumeric)
				WidenParsedIntegerToFloat(parsedValue);
			if (!ValidateSceneSettingEntry("Overwrite", featureShortName, settingPath, key, parsedValue,
					requireNumeric, featureSettingsCache))
				return;

			SSM::SettingEntry entry;
			entry.featureShortName = featureShortName;
			entry.settingPath = settingPath;
			entry.settingKey = key;
			entry.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, key);
			entry.value = std::move(parsedValue);
			entry.originalValue = entry.value;
			entry.source = SSM::EntrySource::Overwrite;
			entry.sourceFilename = filePath.filename().string();
			entry.sourcePath = filePath;
			outEntries.push_back(std::move(entry));
			foundAny = true;
		});
		return foundAny;
	}
}
