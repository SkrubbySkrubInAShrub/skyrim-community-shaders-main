#include "SceneSettingsManager.h"

#include "SceneSettingsInternal.h"
#include "SceneSettingsOverwrites.h"
#include "Utils/FileSystem.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <numeric>

using namespace SceneSettingsInternal;
using namespace SceneSettingsOverwrites;

void SceneSettingsManager::DiscoverOverwrites(SceneType type)
{
	if (!IsEntryListSceneType(type))
		return;
	const auto previousEntryCount = GetEntries(type).size();
	const auto basePath = GetOverwritesPath(type);
	if (type == SceneType::TimeOfDay) {
		for (auto period : kPeriods)
			DiscoverOverwritesInDir(type, GetOverwriteDir(basePath, period), period);
	} else {
		DiscoverOverwritesInDir(type, basePath);
	}

	if (GetEntries(type).size() != previousEntryCount)
		BumpEntryPresentationRevision();
}

void SceneSettingsManager::DiscoverOverwritesInDir(SceneType type, const std::filesystem::path& dir, TimeOfDayPeriod period)
{
	auto typeName = GetSceneTypeName(type);

	std::error_code ec;
	if (!std::filesystem::exists(dir, ec))
		return;

	logger::info("[SceneSettings] Discovering {} overwrites in: {}", typeName, dir.string());

	bool requireNumeric = (type == SceneType::TimeOfDay);
	auto& vec = GetEntriesMut(type);
	int filesFound = 0, overwritesLoaded = 0;
	FeatureSettingsCache featureSettingsCache;

	for (const auto& filePath : GetSortedJsonFiles(dir, std::format("{} overwrite files", typeName))) {
		filesFound++;
		try {
			std::vector<SettingEntry> parsedEntries;
			if (!ParseOverwriteFileEntries(filePath, type, requireNumeric, parsedEntries, &featureSettingsCache))
				continue;
			for (auto& entry : parsedEntries) {
				entry.period = period;
				if (AddOverwriteEntryIfUnique(vec, std::move(entry), typeName))
					overwritesLoaded++;
			}
		} catch (const std::exception& e) {
			logger::error("[SceneSettings] Failed to load {} overwrite '{}': {}", typeName, filePath.filename().string(), e.what());
		}
	}

	if (filesFound > 0)
		logger::info("[SceneSettings] {} overwrite scan: {} files, {} loaded", typeName, filesFound, overwritesLoaded);
}

std::vector<std::string> SceneSettingsManager::GetOverwriteModNames()
{
	// Best-effort: unlike ExportPreset, nothing is written here, so a partial load still returns
	// whatever overwrites are already known rather than reporting nothing.
	TryEnsureWeatherDataLoaded();
	TryEnsureLocationDataLoaded();

	std::vector<std::string> names;
	const auto collect = [&](const std::vector<SettingEntry>& sourceEntries) {
		for (const auto& entry : sourceEntries) {
			if (entry.source != EntrySource::Overwrite)
				continue;
			auto name = GetOverwriteModName(entry);
			if (!name.empty() && std::ranges::find(names, name) == names.end())
				names.push_back(std::move(name));
		}
	};

	for (const auto& [type, sourceEntries] : entries)
		collect(sourceEntries);
	for (const auto& [weatherId, config] : weatherSceneConfigs)
		collect(config.entries);
	for (const auto& [configKey, config] : locationSceneConfigs)
		collect(config.entries);
	return names;
}

std::vector<std::filesystem::path> SceneSettingsManager::FindPresetFiles(const std::string& modName) const
{
	std::vector<std::filesystem::path> found;
	if (modName.empty())
		return found;
	const auto prefix = modName + "_";

	const auto sweepDir = [&](const std::filesystem::path& directory) {
		std::error_code ec;
		if (!std::filesystem::exists(directory, ec))
			return;
		for (const auto& path : GetSortedJsonFiles(directory, "preset files"))
			if (path.filename().string().starts_with(prefix))
				found.push_back(path);
	};
	const auto childDirectories = [](const std::filesystem::path& root, std::string_view context) {
		std::error_code ec;
		return std::filesystem::exists(root, ec) ?
		           GetSortedDirectoryPaths(root, true, context) :
		           std::vector<std::filesystem::path>{};
	};

	sweepDir(GetOverwritesPath(SceneType::InteriorOnly));
	for (auto period : kPeriods)
		sweepDir(GetOverwriteDir(GetOverwritesPath(SceneType::TimeOfDay), period));
	for (const auto& weatherDir : childDirectories(GetWeatherOverwritesDir(), "weather overwrite directories")) {
		// A flat weather file feeds every period, so the preset has to claim it alongside its per-period files.
		sweepDir(weatherDir);
		for (auto period : kPeriods)
			sweepDir(GetOverwriteDir(weatherDir, period));
	}
	for (const auto& locationDir : childDirectories(GetLocationOverwritesDir(), "location overwrite directories"))
		sweepDir(locationDir);
	return found;
}

bool SceneSettingsManager::ExportPreset(const std::string& modName)
{
	// Weather and location configs load lazily; baking before they exist would sweep their files and
	// write nothing back.
	if (!TryEnsureWeatherDataLoaded() || !TryEnsureLocationDataLoaded()) {
		logger::error("[SceneSettings] Preset '{}' not exported: weather or location data is not loaded", modName);
		return false;
	}

	const auto safeModName = Util::FileHelpers::SanitizeFileName(modName);
	if (safeModName.empty())
		return false;

	/// One output file: the type description its metadata carries, and the entries baked into it.
	struct PresetFile
	{
		std::string typeDescription;
		json extraMetadata = json::object();
		std::vector<const SettingEntry*> entries;
	};
	std::map<std::pair<std::filesystem::path, std::string>, PresetFile> files;

	const auto bakeContext = [&](const SceneContextId& context, const std::vector<SettingEntry>& sourceEntries,
								 const std::filesystem::path& baseDir, std::string_view sceneLabel,
								 const json& extraMetadata = json::object()) {
		const auto directory = GetOverwriteDir(baseDir, context.period);
		for (const auto& [identity, entry] : BuildEffectiveContextEntries(sourceEntries, context)) {
			auto& file = files[{ directory, identity.featureShortName }];
			file.typeDescription = GetOverwriteTypeDescription(sceneLabel, context.period);
			file.extraMetadata = extraMetadata;
			file.entries.push_back(entry);
		}
	};

	bakeContext({ .type = SceneContextType::Interior, .period = TimeOfDayPeriod::Count },
		GetEntries(SceneType::InteriorOnly), GetOverwritesPath(SceneType::InteriorOnly), "Interior Only");

	for (auto period : kPeriods)
		bakeContext({ .type = SceneContextType::TimeOfDay, .period = period },
			GetEntries(SceneType::TimeOfDay), GetOverwritesPath(SceneType::TimeOfDay), "Time of Day");

	for (const auto& [weatherId, config] : weatherSceneConfigs) {
		const auto weatherDir = GetWeatherOverwritesDir() / Util::FormIdToSpid(weatherId);
		for (auto period : kPeriods)
			bakeContext({ .type = SceneContextType::Weather, .period = period, .weatherId = weatherId },
				config.entries, weatherDir, "Weather");
	}

	for (const auto& [configKey, config] : locationSceneConfigs) {
		const auto* targetDescription = GetLocationTargetTypeName(config.type);
		bakeContext({ .type = SceneContextType::Location,
						.locationType = config.type,
						.locationFormKey = config.formKey },
			config.entries, GetLocationOverwritesDir() / config.formKey, targetDescription,
			json{ { "targetType", targetDescription },
				{ "targetName", config.name },
				{ "coc", config.cocCode } });
	}

	bool wroteAll = true;
	// The sweep runs only once every output is known, so a failure above costs nothing on disk. A file
	// that survives it would be merged into rather than replaced, silently reviving a deleted setting.
	for (const auto& path : FindPresetFiles(safeModName)) {
		std::error_code ec;
		std::filesystem::remove(path, ec);
		if (ec) {
			logger::error("[SceneSettings] Could not remove stale preset file '{}': {}", path.string(), ec.message());
			wroteAll = false;
		}
	}

	for (const auto& [key, file] : files) {
		const auto& [directory, featureShortName] = key;
		const auto path = directory / std::format("{}_{}.json", safeModName, featureShortName);
		if (!WriteGroupedOverwriteFile(path, featureShortName, file.typeDescription, file.entries,
				file.extraMetadata)) {
			logger::error("[SceneSettings] Preset '{}' failed to write '{}'", safeModName, path.string());
			wroteAll = false;
		}
	}
	logger::info("[SceneSettings] Exported preset '{}' as {} file(s)", safeModName, files.size());
	return wroteAll;
}

void SceneSettingsManager::DiscoverLocationOverwrites()
{
	const auto root = GetLocationOverwritesDir();
	std::error_code ec;
	if (!std::filesystem::exists(root, ec))
		return;
	for (const auto& directory : GetSortedDirectoryPaths(root, true, "location overwrite directories"))
		DiscoverLocationOverwritesForTarget(directory);
}

void SceneSettingsManager::DiscoverLocationOverwritesForTarget(const std::filesystem::path& targetDir)
{
	const auto formKey = targetDir.filename().string();
	if (formKey.empty())
		return;

	std::optional<LocationTargetType> resolvedType;
	std::string resolvedName;
	std::string resolvedCocCode;
	std::string canonicalFormKey = formKey;
	if (const auto formId = Util::SpidToFormId(formKey); formId != 0) {
		canonicalFormKey = Util::FormIdToSpid(formId);
		if (auto* form = RE::TESForm::LookupByID(formId)) {
			if (form->GetFormType() == RE::FormType::Region)
				resolvedType = LocationTargetType::Region;
			else if (form->GetFormType() == RE::FormType::Location)
				resolvedType = LocationTargetType::Location;
			else if (form->GetFormType() == RE::FormType::Cell)
				resolvedType = LocationTargetType::Cell;
			else {
				logger::warn("[SceneSettings] Location overwrite target '{}' is not a region, location, or cell", formKey);
				return;
			}
			resolvedName = Util::GetFormDisplayName(formId);
			if (*resolvedType == LocationTargetType::Cell)
				resolvedCocCode = Util::GetFormEditorID(form);
		}
	}

	FeatureSettingsCache featureSettingsCache;
	for (const auto& filePath : GetSortedJsonFiles(targetDir, "location overwrite files")) {
		try {
			json data;
			if (!ReadBoundedSceneJson(filePath, data)) {
				logger::warn("[SceneSettings] Location overwrite '{}' is invalid or exceeds {} bytes",
					filePath.string(), kMaxSceneOverwriteFileSize);
				continue;
			}

			std::optional<LocationTargetType> metadataType;
			std::string metadataName;
			std::string metadataCocCode;
			if (auto metadataIt = data.find(kMetadataKey); metadataIt != data.end()) {
				if (!metadataIt->is_object()) {
					logger::warn("[SceneSettings] Location overwrite '{}' metadata must be an object",
						filePath.string());
					continue;
				}
				const auto metadataContext = std::format("Location overwrite '{}' metadata", filePath.string());
				std::string targetType;
				if (!ReadOptionalStringField(*metadataIt, "targetType", targetType, metadataContext) ||
					!ReadOptionalStringField(*metadataIt, "targetName", metadataName, metadataContext) ||
					!ReadOptionalStringField(*metadataIt, "coc", metadataCocCode, metadataContext))
					continue;
				if (targetType == "Region")
					metadataType = LocationTargetType::Region;
				else if (targetType == "Location")
					metadataType = LocationTargetType::Location;
				else if (targetType == "Cell")
					metadataType = LocationTargetType::Cell;
				else if (!targetType.empty()) {
					logger::warn("[SceneSettings] {} has invalid targetType '{}'", metadataContext, targetType);
					continue;
				}
			}
			if (resolvedType && metadataType && *resolvedType != *metadataType) {
				logger::warn("[SceneSettings] Location overwrite '{}' targetType does not match resolved form '{}'",
					filePath.string(), formKey);
				continue;
			}
			const auto targetType = resolvedType ? resolvedType : metadataType;
			if (!targetType) {
				logger::warn("[SceneSettings] Location overwrite '{}' has no resolvable target type",
					filePath.string());
				continue;
			}

			auto& config = GetLocationConfigMut(*targetType, canonicalFormKey,
				!metadataName.empty() ? metadataName : resolvedName);
			if (!metadataCocCode.empty())
				config.cocCode = metadataCocCode;
			else if (!resolvedCocCode.empty())
				config.cocCode = resolvedCocCode;

			std::vector<SettingEntry> parsedEntries;
			if (!ParseOverwriteFileEntries(filePath, SceneType::Location, false, parsedEntries,
					&featureSettingsCache))
				continue;
			for (auto& entry : parsedEntries)
				AddOverwriteEntryIfUnique(config.entries, std::move(entry), "location");
		} catch (const std::exception& e) {
			logger::error("[SceneSettings] Failed to load location overwrite '{}': {}",
				filePath.filename().string(), e.what());
		}
	}
}

void SceneSettingsManager::DiscoverWeatherOverwrites()
{
	const auto countWeatherEntries = [this] {
		return std::accumulate(weatherSceneConfigs.begin(), weatherSceneConfigs.end(), size_t{ 0 },
			[](size_t total, const auto& config) { return total + config.second.entries.size(); });
	};

	const auto previousEntryCount = countWeatherEntries();
	auto baseDir = GetWeatherOverwritesDir();
	std::error_code ec;
	if (!std::filesystem::exists(baseDir, ec))
		return;

	logger::info("[SceneSettings] Discovering weather overwrites in: {}", baseDir.string());

	for (const auto& weatherDirectory : GetSortedDirectoryPaths(baseDir, true, "weather overwrite directories")) {
		auto folderName = weatherDirectory.filename().string();
		RE::FormID weatherId = Util::SpidToFormId(folderName);
		if (weatherId == 0) {
			logger::warn("[SceneSettings] Weather overwrite folder '{}' could not be resolved - skipping", folderName);
			continue;
		}

		DiscoverWeatherOverwritesForSpid(weatherId, weatherDirectory);
	}

	if (countWeatherEntries() != previousEntryCount)
		BumpEntryPresentationRevision();
}

void SceneSettingsManager::DiscoverWeatherOverwritesForSpid(RE::FormID weatherId, const std::filesystem::path& weatherDir)
{
	auto& config = GetWeatherConfigMut(weatherId);
	FeatureSettingsCache featureSettingsCache;

	const auto loadWeatherFile = [&](const std::filesystem::path& filePath, auto&& assignPeriods) {
		try {
			std::vector<SettingEntry> parsedEntries;
			if (ParseOverwriteFileEntries(filePath, SceneType::TimeOfDay, true, parsedEntries, &featureSettingsCache))
				for (auto& parsed : parsedEntries)
					assignPeriods(parsed);
		} catch (const std::exception& e) {
			logger::error("[SceneSettings] Failed to load weather overwrite '{}': {}", filePath.filename().string(), e.what());
		}
	};

	for (auto period : kPeriods) {
		const auto periodDir = weatherDir / GetPeriodName(period);
		std::error_code ec;
		if (!std::filesystem::exists(periodDir, ec))
			continue;

		for (const auto& filePath : GetSortedJsonFiles(periodDir, "weather period overwrite files"))
			loadWeatherFile(filePath, [&](SettingEntry& parsed) {
				parsed.period = period;
				AddOverwriteEntryIfUnique(config.entries, std::move(parsed), "weather");
			});
	}

	// Flat weather files are copied to every period after period-specific files are loaded.
	for (const auto& filePath : GetSortedJsonFiles(weatherDir, "flat weather overwrite files"))
		loadWeatherFile(filePath, [&](const SettingEntry& parsed) {
			for (auto period : kPeriods) {
				SettingEntry entry = parsed;
				entry.period = period;
				AddOverwriteEntryIfUnique(config.entries, std::move(entry), "weather");
			}
		});
}
