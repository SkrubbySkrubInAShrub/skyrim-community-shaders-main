#include "SceneSettingsManager.h"

#include "Feature.h"
#include "Globals.h"
#include "SceneSettingsCatalog.generated.h"
#include "SceneSettingsPolicy.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"
#include "Utils/Game.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <tuple>

namespace
{
	using SceneSettingControlType = SceneSettingsManager::SettingControlType;
	using ManagerAggregatePresentation = SceneSettingsManager::AggregatePresentation;
	using ManagerUnifiedEditMode = SceneSettingsManager::UnifiedEditMode;
	using ManagerSettingDescriptor = SceneSettingsManager::SettingDescriptor;
	struct CatalogSceneSettingUpdate
	{
		std::vector<std::string> settingPath;
		std::string key;
		json value;
	};

	SceneSettingsManager* sceneSettingsManagerSingleton = nullptr;
}

SceneSettingsManager::SceneSettingsManager()
{
	assert(!sceneSettingsManagerSingleton);
	sceneSettingsManagerSingleton = this;
}

SceneSettingsManager::~SceneSettingsManager()
{
	if (sceneSettingsManagerSingleton == this)
		sceneSettingsManagerSingleton = nullptr;
}

SceneSettingsManager* SceneSettingsManager::GetSingleton()
{
	return sceneSettingsManagerSingleton;
}

namespace
{
	constexpr auto kOverwriteJsonIndent = 2;
	constexpr auto kMaxSceneOverwriteFileSize = 1024 * 1024;
	constexpr const char* kFeatureKey = "_feature";
	constexpr const char* kMetadataKey = "_metadata";
	constexpr const char* kMetadataDescriptionKey = "description";
	constexpr std::string_view kSceneSettingDisplaySeparator = " / ";
	constexpr std::string_view kImGuiIdSeparator = "##";

	bool IsSceneSettingPrimitive(const json& value)
	{
		return value.is_boolean() || value.is_number_integer() || value.is_number_float() || value.is_string();
	}

	bool IsEntryListSceneType(SceneSettingsManager::SceneType type)
	{
		return type == SceneSettingsManager::SceneType::InteriorOnly ||
		       type == SceneSettingsManager::SceneType::TimeOfDay;
	}

	bool WriteJsonAtomically(const std::filesystem::path& path, const json& data, int indent,
		std::string_view context)
	{
		std::string serialized;
		try {
			serialized = data.dump(indent);
		} catch (const std::exception& e) {
			logger::error("[SceneSettings] Could not serialize {} '{}': {}", context, path.string(), e.what());
			return false;
		}

		std::error_code ec;
		if (!path.parent_path().empty()) {
			std::filesystem::create_directories(path.parent_path(), ec);
			if (ec) {
				logger::error("[SceneSettings] Could not create directory for {} '{}': {}",
					context, path.string(), ec.message());
				return false;
			}
		}

		auto temporaryPath = path;
		temporaryPath += std::format(".{}.tmp", ::GetCurrentProcessId());
		{
			std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
			if (!file.is_open()) {
				logger::error("[SceneSettings] Could not open temporary {} file '{}'", context, temporaryPath.string());
				return false;
			}
			file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
			file.flush();
			if (file.fail()) {
				logger::error("[SceneSettings] Could not write temporary {} file '{}'", context, temporaryPath.string());
				file.close();
				std::filesystem::remove(temporaryPath, ec);
				return false;
			}
			file.close();
			if (file.fail()) {
				logger::error("[SceneSettings] Could not close temporary {} file '{}'", context, temporaryPath.string());
				std::filesystem::remove(temporaryPath, ec);
				return false;
			}
		}

		if (!::MoveFileExW(temporaryPath.c_str(), path.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			const auto error = ::GetLastError();
			logger::error("[SceneSettings] Could not replace {} '{}' (Win32 error {})",
				context, path.string(), error);
			std::filesystem::remove(temporaryPath, ec);
			return false;
		}
		return true;
	}

	std::string StripImGuiId(std::string_view label)
	{
		return std::string(label.substr(0, label.find(kImGuiIdSeparator)));
	}

	std::vector<std::filesystem::path> GetSortedDirectoryPaths(
		const std::filesystem::path& directory, bool directories, std::string_view context)
	{
		std::vector<std::filesystem::path> paths;
		std::error_code ec;
		std::filesystem::directory_iterator iterator(
			directory, std::filesystem::directory_options::skip_permission_denied, ec);
		if (ec) {
			logger::error("[SceneSettings] Failed to enumerate {} '{}': {}", context, directory.string(), ec.message());
			return paths;
		}

		const std::filesystem::directory_iterator end;
		while (iterator != end) {
			const auto& entry = *iterator;
			std::error_code statusError;
			const bool matches = directories ? entry.is_directory(statusError) : entry.is_regular_file(statusError);
			if (statusError) {
				logger::warn("[SceneSettings] Could not inspect '{}': {}", entry.path().string(), statusError.message());
			} else if (matches) {
				paths.push_back(entry.path());
			}

			iterator.increment(ec);
			if (ec) {
				logger::error("[SceneSettings] Failed while enumerating {} '{}': {}", context, directory.string(), ec.message());
				break;
			}
		}

		std::sort(paths.begin(), paths.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.generic_string() < rhs.generic_string();
		});
		return paths;
	}

	std::vector<std::filesystem::path> GetSortedJsonFiles(
		const std::filesystem::path& directory, std::string_view context)
	{
		auto paths = GetSortedDirectoryPaths(directory, false, context);
		std::erase_if(paths, [](const auto& path) { return path.extension() != ".json"; });
		return paths;
	}

	std::string NormalizeLocationFormKey(std::string_view formKey)
	{
		const auto components = Util::ParseSpid(std::string(formKey));
		if (components.localFormId == 0)
			return std::string(formKey);
		if (components.pluginName.empty())
			return std::format("0x{:X}", components.localFormId);

		auto pluginName = components.pluginName;
		std::transform(pluginName.begin(), pluginName.end(), pluginName.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return std::format("0x{:X}~{}", components.localFormId, pluginName);
	}

	std::string CanonicalizeResolvedLocationFormKey(std::string_view formKey)
	{
		const auto components = Util::ParseSpid(std::string(formKey));
		if (components.pluginName.empty() || components.localFormId == 0)
			return std::string(formKey);
		if (!RE::TESDataHandler::GetSingleton())
			return std::string(formKey);
		const auto formId = Util::SpidToFormId(std::string(formKey));
		return formId != 0 ? Util::FormIdToSpid(formId) : std::string(formKey);
	}

	bool ReadOptionalStringField(const json& object, std::string_view field, std::string& value,
		std::string_view context)
	{
		auto it = object.find(std::string(field));
		if (it == object.end())
			return true;
		if (!it->is_string()) {
			logger::warn("[SceneSettings] {} field '{}' must be a string", context, field);
			return false;
		}
		value = it->get<std::string>();
		return true;
	}

	bool IsSceneMetadataKey(std::string_view key)
	{
		return !key.empty() && key.front() == '_';
	}

	bool ReadBoundedSceneJson(const std::filesystem::path& path, json& data)
	{
		std::error_code ec;
		const auto fileSize = std::filesystem::file_size(path, ec);
		if (ec || fileSize > kMaxSceneOverwriteFileSize)
			return false;

		std::ifstream file(path);
		if (!file.is_open())
			return false;
		data = json::parse(file, nullptr, false);
		return data.is_object();
	}

	// TOD/weather can only interpolate float settings, not integer toggles or enum values.
	bool IsNumericValue(const json& value)
	{
		return value.is_number_float();
	}

	bool IsSceneSettingPathWrapper(std::string_view token)
	{
		return token == "settings";
	}

	std::string NormalizeSceneSettingAddressToken(std::string_view token)
	{
		auto normalized = token.find(' ') == std::string_view::npos ?
		                      Util::PrettifyIdentifier(token) :
		                      std::string(token);
		std::erase_if(normalized, [](unsigned char c) { return std::isspace(c); });
		std::transform(normalized.begin(), normalized.end(), normalized.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return normalized;
	}

	bool SceneSettingAddressTokensEqual(std::string_view lhs, std::string_view rhs)
	{
		return NormalizeSceneSettingAddressToken(lhs) == NormalizeSceneSettingAddressToken(rhs);
	}

	bool IsSceneSettingBlacklistPrefix(
		const std::vector<std::string>& address, const SceneSettingsPolicy::SettingBlacklistPath& prefix)
	{
		if (prefix.size() > address.size())
			return false;

		for (size_t index = 0; index < prefix.size(); ++index)
			if (!SceneSettingAddressTokensEqual(address[index], prefix[index]))
				return false;
		return true;
	}

	std::vector<std::string> GetSceneSettingAddress(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		std::vector<std::string> address{ featureShortName };
		address.reserve(settingPath.size() + 2);
		for (const auto& segment : settingPath)
			if (!IsSceneSettingPathWrapper(segment))
				address.push_back(segment);
		address.push_back(settingKey);
		return address;
	}

	bool IsBlacklistedSceneSetting(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		auto address = GetSceneSettingAddress(featureShortName, settingPath, settingKey);
		return std::any_of(SceneSettingsPolicy::kSettingBlacklist.begin(), SceneSettingsPolicy::kSettingBlacklist.end(),
			[&](const auto& prefix) { return IsSceneSettingBlacklistPrefix(address, prefix); });
	}

	bool HasSceneOverwriteContent(const json& data)
	{
		if (!data.is_object())
			return false;

		for (const auto& [key, _] : data.items())
			if (!IsSceneMetadataKey(key))
				return true;
		return false;
	}

	bool IsCompatibleSceneSettingValue(const json& featureValue, const json& value)
	{
		if (featureValue.type() == value.type())
			return true;
		if (featureValue.is_number() && value.is_number())
			return true;
		return false;
	}

	std::string JoinDisplayParts(const std::vector<std::string>& parts, std::string_view leaf)
	{
		std::string displayName;
		for (const auto& part : parts) {
			if (!displayName.empty())
				displayName += kSceneSettingDisplaySeparator;
			displayName += part;
		}
		if (!leaf.empty()) {
			if (!displayName.empty())
				displayName += kSceneSettingDisplaySeparator;
			displayName += leaf;
		}
		return displayName;
	}

	std::vector<std::string> SplitCatalogPath(std::string_view path)
	{
		std::vector<std::string> parts;
		size_t start = 0;
		while (start < path.size()) {
			auto end = path.find('/', start);
			auto part = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
			if (!part.empty()) {
				std::string decoded(part);
				for (size_t pos = 0; (pos = decoded.find('~', pos)) != std::string::npos;) {
					if (pos + 1 < decoded.size() && decoded[pos + 1] == '1')
						decoded.replace(pos, 2, "/");
					else if (pos + 1 < decoded.size() && decoded[pos + 1] == '0')
						decoded.replace(pos, 2, "~");
					++pos;
				}
				parts.push_back(std::move(decoded));
			}
			if (end == std::string_view::npos)
				break;
			start = end + 1;
		}
		return parts;
	}

	std::string ToCatalogPath(const std::vector<std::string>& path)
	{
		std::string result;
		for (const auto& part : path) {
			if (!result.empty())
				result += '/';
			for (const char ch : part) {
				if (ch == '~')
					result += "~0";
				else if (ch == '/')
					result += "~1";
				else
					result += ch;
			}
		}
		return result;
	}

	bool IsStructuralDisplayPart(std::string_view part)
	{
		std::string normalized;
		normalized.reserve(part.size());
		for (const char ch : part)
			if (std::isalnum(static_cast<unsigned char>(ch)))
				normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
		return normalized == "settings" || normalized == "values" || normalized == "baseline";
	}

	std::string NormalizeDisplayPart(std::string part)
	{
		part = StripImGuiId(part);
		if (!part.empty() && std::all_of(part.begin(), part.end(), [](const char ch) {
				return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
			}))
			part = Util::PrettifyIdentifier(part);
		return part;
	}

	std::vector<std::string> GetCatalogDisplayPath(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		auto parts = SplitCatalogPath(setting.displayPath.empty() ? setting.settingPath : setting.displayPath);
		const auto keys = SplitCatalogPath(setting.displayPathKeys);
		for (size_t index = 0; index < parts.size(); ++index) {
			if (index < keys.size() && keys[index] != "-")
				parts[index] = T(keys[index], parts[index].c_str());
			parts[index] = NormalizeDisplayPart(std::move(parts[index]));
		}
		std::erase_if(parts, [](const auto& part) { return part.empty() || IsStructuralDisplayPart(part); });
		return parts;
	}

	std::vector<std::string> GetCatalogSelectorPath(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		auto parts = SplitCatalogPath(setting.selectorPath);
		auto keys = SplitCatalogPath(setting.selectorPathKeys);
		for (size_t i = 0; i < parts.size(); ++i) {
			if (i < keys.size() && keys[i] != "-")
				parts[i] = T(keys[i], parts[i].c_str());
			parts[i] = StripImGuiId(parts[i]);
		}
		return parts;
	}

	bool EqualDisplayText(std::string_view lhs, std::string_view rhs)
	{
		return std::ranges::equal(lhs, rhs, [](const char a, const char b) {
			return std::tolower(static_cast<unsigned char>(a)) ==
			       std::tolower(static_cast<unsigned char>(b));
		});
	}

	std::vector<std::string> GetCatalogContextPath(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		auto parts = GetCatalogDisplayPath(setting);
		auto selectorDefaults = GetCatalogSelectorPath(setting);
		auto rawParts = SplitCatalogPath(setting.displayPath.empty() ? setting.settingPath : setting.displayPath);
		const auto rawKeys = SplitCatalogPath(setting.displayPathKeys);
		const auto settingParts = SplitCatalogPath(setting.settingPath);
		const bool hasSelector = !selectorDefaults.empty();
		size_t rawOffset = 0;
		for (auto& part : selectorDefaults)
			part = NormalizeDisplayPart(std::move(part));
		while (!parts.empty() && !selectorDefaults.empty() &&
		       EqualDisplayText(parts.front(), selectorDefaults.front())) {
			parts.erase(parts.begin());
			selectorDefaults.erase(selectorDefaults.begin());
			++rawOffset;
		}
		if (hasSelector) {
			while (rawOffset < rawParts.size() && IsStructuralDisplayPart(rawParts[rawOffset]))
				++rawOffset;
			if (!parts.empty() && rawOffset < rawParts.size() && rawOffset < settingParts.size()) {
				const bool translated = rawOffset < rawKeys.size() && rawKeys[rawOffset] != "-";
				auto rawPart = NormalizeDisplayPart(rawParts[rawOffset]);
				auto settingPart = NormalizeDisplayPart(settingParts[rawOffset]);
				if (!translated && EqualDisplayText(parts.front(), rawPart) &&
					EqualDisplayText(rawPart, settingPart))
					parts.erase(parts.begin());
			}
		}
		return parts;
	}

	std::string GetCatalogLeafDisplayName(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		if (setting.displayName.empty() && setting.displayNameKey.empty() &&
			setting.editorSemantic == SceneSettingsCatalog::EditorSemantic::Choice)
			return T("feature.scene_manager.selection", "Selection");

		auto displayName = StripImGuiId(setting.displayName.empty() ? setting.settingKey : setting.displayName);
		if (!setting.displayNameKey.empty())
			displayName = StripImGuiId(T(setting.displayNameKey, displayName.c_str()));
		return displayName;
	}

	double GetCatalogNumericDisplayScale(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		return std::isfinite(setting.displayScale) && setting.displayScale > 0.0 ?
		           setting.displayScale :
		           1.0;
	}

	bool ConvertCatalogNumericStoredToDisplay(const SceneSettingsCatalog::SettingMetadata& setting,
		double storedValue, double& displayValue)
	{
		if (!std::isfinite(storedValue))
			return false;
		if (setting.editorSemantic == SceneSettingsCatalog::EditorSemantic::Generic) {
			displayValue = storedValue;
			return true;
		}
		if (setting.editorSemantic != SceneSettingsCatalog::EditorSemantic::Numeric)
			return false;

		double transformedValue = storedValue;
		switch (setting.numericTransform) {
		case SceneSettingsCatalog::NumericTransform::Identity:
			break;
		case SceneSettingsCatalog::NumericTransform::Log2:
			if (storedValue <= 0.0)
				return false;
			transformedValue = std::log2(storedValue);
			break;
		default:
			return false;
		}

		displayValue = transformedValue * GetCatalogNumericDisplayScale(setting);
		return std::isfinite(displayValue);
	}

	bool ConvertCatalogNumericDisplayToStored(const SceneSettingsCatalog::SettingMetadata& setting,
		double displayValue, double& storedValue)
	{
		if (!std::isfinite(displayValue))
			return false;
		if (setting.editorSemantic == SceneSettingsCatalog::EditorSemantic::Generic) {
			storedValue = displayValue;
			return true;
		}
		if (setting.editorSemantic != SceneSettingsCatalog::EditorSemantic::Numeric)
			return false;

		const double transformedValue = displayValue / GetCatalogNumericDisplayScale(setting);
		if (!std::isfinite(transformedValue))
			return false;
		switch (setting.numericTransform) {
		case SceneSettingsCatalog::NumericTransform::Identity:
			storedValue = transformedValue;
			break;
		case SceneSettingsCatalog::NumericTransform::Log2:
			storedValue = std::exp2(transformedValue);
			break;
		default:
			return false;
		}
		return std::isfinite(storedValue) &&
		       (setting.numericTransform != SceneSettingsCatalog::NumericTransform::Log2 || storedValue > 0.0);
	}

	const SceneSettingsCatalog::SettingMetadata* FindStoredAllComponent(
		const SceneSettingsCatalog::SettingMetadata& setting)
	{
		const auto settings = SceneSettingsCatalog::GetSettings();
		static const auto storedAllComponents = [] {
			using AggregateKey = std::tuple<std::string_view, std::string_view, std::string_view,
				SceneSettingsCatalog::AggregateSemantic, std::int8_t, std::uint8_t>;
			const auto makeKey = [](const auto& candidate) {
				return AggregateKey{ candidate.featureShortName, candidate.serializedPath,
					candidate.serializedKey, candidate.aggregateSemantic,
					candidate.aggregateStart, candidate.aggregateCount };
			};
			std::map<AggregateKey, const SceneSettingsCatalog::SettingMetadata*> storedAll;
			for (const auto& candidate : SceneSettingsCatalog::GetSettings())
				if (candidate.aggregateAll)
					storedAll.try_emplace(makeKey(candidate), &candidate);
			std::vector<const SceneSettingsCatalog::SettingMetadata*> components(
				SceneSettingsCatalog::GetSettings().size(), nullptr);
			for (size_t index = 0; index < SceneSettingsCatalog::GetSettings().size(); ++index) {
				const auto& source = SceneSettingsCatalog::GetSettings()[index];
				if (auto component = storedAll.find(makeKey(source)); component != storedAll.end())
					components[index] = component->second;
			}
			return components;
		}();
		const auto index = static_cast<size_t>(&setting - settings.data());
		assert(index < storedAllComponents.size());
		return index < storedAllComponents.size() ? storedAllComponents[index] : nullptr;
	}

	SceneSettingControlType GetCatalogControlType(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		using enum SceneSettingsCatalog::AggregateSemantic;
		switch (setting.aggregateSemantic) {
		case Color:
			return FindStoredAllComponent(setting) ?
			           SceneSettingControlType::Numeric :
			           SceneSettingControlType::Color;
		case Numeric:
			return SceneSettingControlType::Numeric;
		default:
			return SceneSettingControlType::Scalar;
		}
	}

	std::string GetSettingComponentName(SceneSettingControlType type, std::int8_t componentIndex)
	{
		if (componentIndex < 0 || componentIndex > 3)
			return {};
		if (type == SceneSettingControlType::Color) {
			switch (componentIndex) {
			case 0:
				return T("feature.scene_manager.channel.red", "R");
			case 1:
				return T("feature.scene_manager.channel.green", "G");
			case 2:
				return T("feature.scene_manager.channel.blue", "B");
			default:
				return T("feature.scene_manager.channel.alpha", "A");
			}
		}
		switch (componentIndex) {
		case 0:
			return T("feature.scene_manager.channel.x", "X");
		case 1:
			return T("feature.scene_manager.channel.y", "Y");
		case 2:
			return T("feature.scene_manager.channel.z", "Z");
		default:
			return T("feature.scene_manager.channel.w", "W");
		}
	}

	std::string GetCatalogComponentDisplayName(
		const SceneSettingsCatalog::SettingMetadata& setting, SceneSettingControlType controlType)
	{
		auto displayName = StripImGuiId(setting.componentDisplayName);
		if (!setting.componentDisplayNameKey.empty())
			displayName = StripImGuiId(T(setting.componentDisplayNameKey, displayName.c_str()));
		if (!displayName.empty())
			return displayName;
		if (setting.aggregateAll)
			return T("feature.scene_manager.channel.all", "All");

		auto componentIndex = static_cast<std::int8_t>(setting.aggregateCount > 1 ?
		                                                     setting.serializedComponent - setting.aggregateStart :
		                                                     setting.serializedComponent);
		const auto* storedAll = FindStoredAllComponent(setting);
		if (storedAll && storedAll->serializedComponent < setting.serializedComponent)
			--componentIndex;
		const auto componentType = setting.aggregateSemantic == SceneSettingsCatalog::AggregateSemantic::Color ?
		                               SceneSettingControlType::Color :
		                               controlType;
		return GetSettingComponentName(componentType, componentIndex);
	}

	SceneSettingsManager::SettingControlInfo MakeSettingControlInfo(
		const SceneSettingsCatalog::SettingMetadata& setting)
	{
		SceneSettingsManager::SettingControlInfo info;
		info.controlType = GetCatalogControlType(setting);
		info.settingPath = info.controlType == SceneSettingControlType::Scalar ?
		                       SplitCatalogPath(setting.settingPath) :
		                       SplitCatalogPath(setting.serializedPath);
		info.settingKey = std::string(info.controlType == SceneSettingControlType::Scalar ?
		                                  setting.settingKey : setting.serializedKey);
		info.displayName = GetCatalogLeafDisplayName(setting);
		info.componentDisplayName = GetCatalogComponentDisplayName(setting, info.controlType);
		info.displayPath = GetCatalogContextPath(setting);
		info.componentIndex = setting.serializedComponent;
		info.aggregateAll = setting.aggregateAll;
		if (info.controlType != SceneSettingControlType::Scalar) {
			info.componentStart = setting.aggregateStart;
			info.componentCount = setting.aggregateCount;
			info.aggregatePresentation =
				info.controlType == SceneSettingControlType::Color &&
						setting.aggregatePresentation == SceneSettingsCatalog::AggregatePresentation::ColorPicker ?
					ManagerAggregatePresentation::ColorPicker :
					ManagerAggregatePresentation::Components;
			switch (setting.unifiedEditMode) {
			case SceneSettingsCatalog::UnifiedEditMode::Always:
				info.unifiedEditMode = ManagerUnifiedEditMode::Always;
				break;
			case SceneSettingsCatalog::UnifiedEditMode::Shift:
				info.unifiedEditMode = ManagerUnifiedEditMode::Shift;
				break;
			default:
				info.unifiedEditMode = ManagerUnifiedEditMode::None;
				break;
			}
		}
		return info;
	}

	bool IsCatalogValueCompatible(const SceneSettingsCatalog::SettingMetadata& setting, const json& value)
	{
		using enum SceneSettingsCatalog::ValueType;
		switch (setting.valueType) {
		case Boolean:
			return value.is_boolean();
		case Integer:
			return value.is_number_integer();
		case Float:
			return value.is_number_float() || value.is_number_integer();
		case String:
			return value.is_string();
		default:
			return false;
		}
	}

	bool IsSameSetting(const SceneSettingsManager::SettingEntry& entry, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		return entry.featureShortName == featureShortName &&
		       entry.settingPath == settingPath &&
		       entry.settingKey == settingKey;
	}

	std::string GetSettingLogName(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		return JoinDisplayParts(settingPath, std::format("{}.{}", featureShortName, settingKey));
	}

	json* GetObjectAtPath(json& data, const std::vector<std::string>& path, bool create)
	{
		json* node = &data;
		for (const auto& segment : path) {
			if (!node->is_object()) {
				return nullptr;
			}

			auto it = node->find(segment);
			if (it == node->end()) {
				if (!create)
					return nullptr;
				it = node->emplace(segment, json::object()).first;
			}
			node = &*it;
		}
		return node->is_object() ? node : nullptr;
	}

	bool RemoveObjectValueAtPath(json& data, const std::vector<std::string>& path,
		size_t pathIndex, const std::string& settingKey)
	{
		if (!data.is_object())
			return false;
		if (pathIndex == path.size())
			return data.erase(settingKey) == 1;

		auto childIt = data.find(path[pathIndex]);
		if (childIt == data.end() || !childIt->is_object() ||
			!RemoveObjectValueAtPath(*childIt, path, pathIndex + 1, settingKey))
			return false;
		if (childIt->empty())
			data.erase(childIt);
		return true;
	}

	const json* GetObjectAtPath(const json& data, const std::vector<std::string>& path)
	{
		const json* node = &data;
		for (const auto& segment : path) {
			if (!node->is_object())
				return nullptr;
			auto it = node->find(segment);
			if (it == node->end())
				return nullptr;
			node = &*it;
		}
		return node->is_object() ? node : nullptr;
	}

	json* GetObjectAtPath(json& data, const std::vector<std::string>& path)
	{
		return const_cast<json*>(GetObjectAtPath(std::as_const(data), path));
	}

	bool ParseCatalogArrayIndex(std::string_view value, size_t& index)
	{
		const auto result = std::from_chars(value.data(), value.data() + value.size(), index);
		return result.ec == std::errc{} && result.ptr == value.data() + value.size();
	}

	template <class Json>
	Json* GetCatalogNodeAtPath(Json& data, const std::vector<std::string>& path)
	{
		auto* node = &data;
		for (const auto& segment : path) {
			if (node->is_object()) {
				auto it = node->find(segment);
				if (it == node->end())
					return nullptr;
				node = &*it;
				continue;
			}

			size_t index = 0;
			if (!node->is_array() || !ParseCatalogArrayIndex(segment, index) || index >= node->size())
				return nullptr;
			node = &(*node)[index];
		}
		return node;
	}

	template <class Json>
	Json* GetCatalogSerializedValue(Json& data, const SceneSettingsCatalog::SettingMetadata& setting)
	{
		auto* parent = GetCatalogNodeAtPath(data, SplitCatalogPath(setting.serializedPath));
		if (!parent)
			return nullptr;

		Json* value = nullptr;
		if (parent->is_object()) {
			auto valueIt = parent->find(setting.serializedKey);
			if (valueIt == parent->end())
				return nullptr;
			value = &*valueIt;
		} else {
			size_t index = 0;
			if (!parent->is_array() || !ParseCatalogArrayIndex(setting.serializedKey, index) ||
				index >= parent->size())
				return nullptr;
			value = &(*parent)[index];
		}

		if (setting.serializedComponent < 0)
			return value;
		const auto component = static_cast<size_t>(setting.serializedComponent);
		if (!value->is_array() || component >= value->size())
			return nullptr;
		return &(*value)[component];
	}

	void CollectOverwriteEntries(const json& data, const std::vector<std::string>& settingPath,
		const std::function<void(const std::vector<std::string>&, const std::string&, const json&)>& callback)
	{
		if (!data.is_object())
			return;

		for (const auto& [key, value] : data.items()) {
			if (IsSceneMetadataKey(key))
				continue;
			if (IsSceneSettingPrimitive(value)) {
				callback(settingPath, key, value);

				continue;
			}
			if (!value.is_object())
				continue;

			auto childPath = settingPath;
			childPath.push_back(key);
			CollectOverwriteEntries(value, childPath, callback);
		}
	}

	template <size_t Size>
	bool ContainsFeatureName(
		const std::array<std::string_view, Size>& featureNames, std::string_view featureShortName)
	{
		return std::find(featureNames.begin(), featureNames.end(), featureShortName) != featureNames.end();
	}

	bool IsInteriorOnlyFeatureAllowed(std::string_view featureShortName)
	{
		return ContainsFeatureName(SceneSettingsPolicy::kLocationFeatureWhitelist, featureShortName);
	}

	bool IsTimeOfDayFeatureAllowed(std::string_view featureShortName)
	{
		return ContainsFeatureName(SceneSettingsPolicy::kTimeOfDayFeatureWhitelist, featureShortName);
	}

	bool ComputeCatalogSettingAllowedByPolicy(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		return SceneSettingsCatalog::IsSceneControllable(setting) &&
		       !IsBlacklistedSceneSetting(
			       std::string(setting.featureShortName),
			       SplitCatalogPath(setting.settingPath),
			       std::string(setting.settingKey));
	}

	bool IsCatalogSettingAllowedByPolicy(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		const auto settings = SceneSettingsCatalog::GetSettings();
		static const auto allowedSettings = [] {
			std::vector<uint8_t> allowed;
			allowed.reserve(SceneSettingsCatalog::GetSettings().size());
			for (const auto& candidate : SceneSettingsCatalog::GetSettings())
				allowed.push_back(ComputeCatalogSettingAllowedByPolicy(candidate) ? 1 : 0);
			return allowed;
		}();
		const auto index = static_cast<size_t>(&setting - settings.data());
		assert(index < allowedSettings.size());
		return index < allowedSettings.size() && allowedSettings[index] != 0;
	}

	const SceneSettingsCatalog::SettingMetadata* FindAllowedCatalogSetting(
		std::string_view featureShortName, const std::vector<std::string>& settingPath,
		std::string_view settingKey, bool requireTransitionable = false)
	{
		auto* setting = SceneSettingsCatalog::FindSetting(
			featureShortName, ToCatalogPath(settingPath), settingKey);
		if (!setting || !IsCatalogSettingAllowedByPolicy(*setting))
			return nullptr;
		if (requireTransitionable &&
			!SceneSettingsCatalog::HasFlag(setting->flags, SceneSettingsCatalog::SettingFlag::Transitionable))
			return nullptr;
		return setting;
	}

	bool GetCatalogSettingValue(
		Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting, json& value)
	{
		json featureSettings;
		feature.SaveSettings(featureSettings);
		if (!featureSettings.is_object())
			return false;
		const auto* serializedValue = GetCatalogSerializedValue(featureSettings, setting);
		if (!serializedValue || !IsSceneSettingPrimitive(*serializedValue))
			return false;
		value = *serializedValue;
		return true;
	}

	bool ApplyCatalogSceneSettings(Feature& feature, const std::vector<CatalogSceneSettingUpdate>& updates)
	{
		if (updates.empty())
			return true;

		json originalSettings;
		feature.SaveSettings(originalSettings);
		if (!originalSettings.is_object())
			return false;

		auto candidateSettings = originalSettings;
		for (const auto& update : updates) {
			auto* setting = FindAllowedCatalogSetting(
				feature.GetShortName(), update.settingPath, update.key);
			auto* currentValue = setting ? GetCatalogSerializedValue(candidateSettings, *setting) : nullptr;
			if (!currentValue || !IsSceneSettingPrimitive(*currentValue) ||
				!IsSceneSettingPrimitive(update.value) ||
				!IsCompatibleSceneSettingValue(*currentValue, update.value))
				return false;
			*currentValue = update.value;
		}

		try {
			feature.LoadSettings(candidateSettings);
			return true;
		} catch (const std::exception& e) {
			logger::warn("[SceneSettings] Failed to apply settings for {}: {}", feature.GetShortName(), e.what());
		} catch (...) {
			logger::warn("[SceneSettings] Failed to apply settings for {}", feature.GetShortName());
		}

		try {
			feature.LoadSettings(originalSettings);
		} catch (...) {
			logger::error("[SceneSettings] Failed to restore {} after an apply error", feature.GetShortName());
		}
		return false;
	}

	bool CatalogHasSceneSettings(std::string_view featureShortName, bool transitionableOnly)
	{
		for (const auto& setting : SceneSettingsCatalog::GetSettings()) {
			if (setting.featureShortName != featureShortName || !IsCatalogSettingAllowedByPolicy(setting))
				continue;
			if (!transitionableOnly ||
				SceneSettingsCatalog::HasFlag(setting.flags, SceneSettingsCatalog::SettingFlag::Transitionable))
				return true;
		}
		return false;
	}

	std::vector<std::string> GetLoadedCatalogFeatureNames(bool transitionableOnly)
	{
		auto names = Feature::GetLoadedFeatureNames();
		std::erase_if(names, [&](const auto& name) { return !CatalogHasSceneSettings(name, transitionableOnly); });
		return names;
	}
}

static std::filesystem::path GetSceneOverwritePath(SceneSettingsManager::SceneType type, const SceneSettingsManager::SettingEntry& entry);
static bool RemoveSettingFromOverwriteFile(const std::filesystem::path& path,
	const std::vector<std::string>& settingPath, const std::string& settingKey);

static bool HasOverwriteEntryForPeriod(const std::vector<SceneSettingsManager::SettingEntry>& entries,
	const SceneSettingsManager::SettingEntry& candidate)
{
	return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
		return entry.source == SceneSettingsManager::EntrySource::Overwrite &&
		       entry.period == candidate.period &&
		       IsSameSetting(entry, candidate.featureShortName, candidate.settingPath, candidate.settingKey);
	});
}

static bool AddOverwriteEntryIfUnique(std::vector<SceneSettingsManager::SettingEntry>& entries,
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

// --- Path Resolution ---

std::string SceneSettingsManager::GetSceneTypeName(SceneType type)
{
	switch (type) {
	case SceneType::InteriorOnly:
		return "InteriorOnly";
	case SceneType::TimeOfDay:
		return "TimeOfDay";
	case SceneType::Location:
		return "Location";
	default:
		return "Unknown";
	}
}

std::filesystem::path SceneSettingsManager::GetUserSettingsFilePath()
{
	return Util::PathHelpers::GetSceneSettingsPath() / "SceneManager.json";
}

std::filesystem::path SceneSettingsManager::GetOverwritesPath(SceneType type)
{
	// Location overwrites are keyed by form under GetLocationOverwritesDir(), not by scene type name.
	assert(IsEntryListSceneType(type));
	return Util::PathHelpers::GetSceneSettingsPath() / GetSceneTypeName(type);
}

std::filesystem::path SceneSettingsManager::GetWeatherOverwritesDir()
{
	return Util::PathHelpers::GetSceneSettingsPath() / "Weather";
}

std::filesystem::path SceneSettingsManager::GetLocationOverwritesDir()
{
	return Util::PathHelpers::GetSceneSettingsPath() / "Locations";
}

// --- Time of Day Period Helpers ---

const char* SceneSettingsManager::GetPeriodName(TimeOfDayPeriod period)
{
	int idx = static_cast<int>(period);
	return (idx >= 0 && idx < kPeriodCount) ? kPeriodNames[idx] : "Unknown";
}

SceneSettingsManager::TimeOfDayPeriod SceneSettingsManager::GetPeriodFromName(const std::string& name)
{
	for (auto period : kPeriods) {
		if (name == GetPeriodName(period))
			return period;
	}
	return TimeOfDayPeriod::Count;
}

float SceneSettingsManager::GetCurrentGameHour()
{
	// Prefer calendar (ground truth), which the Weather Editor slider writes to.
	// sky->currentGameHour may lag when timeScale is 0 (time paused).
	auto calendar = globals::game::calendar ? globals::game::calendar : RE::Calendar::GetSingleton();
	float hour = 12.0f;
	if (calendar && calendar->gameHour)
		hour = calendar->gameHour->value;
	else if (auto sky = globals::game::sky)
		hour = sky->currentGameHour;
	if (!std::isfinite(hour))
		hour = 12.0f;

	// Normalize into [0, 24) so midnight is 0 and never 24.
	hour = std::clamp(hour, 0.0f, 24.0f);
	if (hour >= 24.0f)
		hour = 0.0f;
	return hour;
}

SceneSettingsManager::PeriodLookup SceneSettingsManager::FindPeriodForHour(float hour)
{
	for (int index = 0; index < kPeriodCount; ++index) {
		const float start = kPeriodHours[index][0];
		const float end = kPeriodHours[index][1];
		// Night ends past 24, so pre-dawn hours have to be compared against hour + 24.
		const float periodHour = (end > 24.0f && hour < start) ? hour + 24.0f : hour;
		if (periodHour >= start && periodHour < end)
			return { index, periodHour };
	}
	return {};
}

std::array<float, SceneSettingsManager::kPeriodCount> SceneSettingsManager::GetTimeOfDayFactors()
{
	std::array<float, kPeriodCount> factors{};
	const auto lookup = FindPeriodForHour(GetCurrentGameHour());
	if (lookup.index < 0) {
		factors[static_cast<int>(TimeOfDayPeriod::Day)] = 1.0f;
		return factors;
	}

	const float hoursToEnd = kPeriodHours[lookup.index][1] - lookup.hour;
	if (hoursToEnd >= kTransitionHours) {
		factors[lookup.index] = 1.0f;
		return factors;
	}

	// Inside the blend-out zone: cross-fade into the next period.
	const float weight = hoursToEnd / kTransitionHours;
	factors[lookup.index] = weight;
	factors[(lookup.index + 1) % kPeriodCount] = 1.0f - weight;
	return factors;
}

SceneSettingsManager::TimeOfDayPeriod SceneSettingsManager::GetCurrentPeriod()
{
	const auto lookup = FindPeriodForHour(GetCurrentGameHour());
	return lookup.index < 0 ? TimeOfDayPeriod::Day : static_cast<TimeOfDayPeriod>(lookup.index);
}

// --- Feature Metadata ---

bool SceneSettingsManager::IsFeatureAllowedForType(SceneType type, const std::string& featureShortName)
{
	if (!Feature::FindFeatureByShortName(featureShortName))
		return false;

	switch (type) {
	case SceneType::InteriorOnly:
		return IsInteriorOnlyFeatureAllowed(featureShortName) &&
		       CatalogHasSceneSettings(featureShortName, false);
	case SceneType::TimeOfDay:
		return IsTimeOfDayFeatureAllowed(featureShortName) &&
		       CatalogHasSceneSettings(featureShortName, true);
	case SceneType::Location:
		return (IsInteriorOnlyFeatureAllowed(featureShortName) ||
		           IsTimeOfDayFeatureAllowed(featureShortName)) &&
		       CatalogHasSceneSettings(featureShortName, false);
	default:
		return true;
	}
}

bool SceneSettingsManager::IsSceneSettingAllowed(
	std::string_view featureShortName, std::string_view settingPath, std::string_view settingKey)
{
	auto* setting = SceneSettingsCatalog::FindSetting(featureShortName, settingPath, settingKey);
	return setting && IsCatalogSettingAllowedByPolicy(*setting);
}

std::vector<std::string> SceneSettingsManager::GetInteriorRelevantFeatureNames()
{
	auto names = GetLoadedCatalogFeatureNames(false);
	std::erase_if(names, [](const auto& name) { return !IsInteriorOnlyFeatureAllowed(name); });
	return names;
}

std::vector<std::string> SceneSettingsManager::GetExteriorRelevantFeatureNames()
{
	auto names = GetLoadedCatalogFeatureNames(true);
	std::erase_if(names, [](const auto& name) { return !IsTimeOfDayFeatureAllowed(name); });
	return names;
}

std::vector<std::string> SceneSettingsManager::GetLocationRelevantFeatureNames()
{
	auto names = GetLoadedCatalogFeatureNames(false);
	std::erase_if(names, [](const auto& name) {
		return !IsInteriorOnlyFeatureAllowed(name) && !IsTimeOfDayFeatureAllowed(name);
	});
	return names;
}

std::string SceneSettingsManager::GetFeatureDisplayName(const std::string& featureShortName)
{
	auto* feature = Feature::FindFeatureByShortName(featureShortName);
	return feature ? feature->GetDisplayName() : featureShortName;
}

namespace
{
	std::string GetDescriptorLabel(const SceneSettingsManager::SettingControlInfo& info,
		std::string_view component = {})
	{
		std::string leaf = info.displayName;
		if (!component.empty())
			leaf += std::format(" ({})", component);
		if (info.displayPath.empty())
			return leaf;
		return std::format("{}: {}", JoinDisplayParts(info.displayPath, {}), leaf);
	}

	ManagerSettingDescriptor MakeScalarDescriptor(
		const SceneSettingsCatalog::SettingMetadata& setting, const json& value)
	{
		auto info = MakeSettingControlInfo(setting);
		const auto physicalPath = SplitCatalogPath(setting.settingPath);
		const auto physicalKey = std::string(setting.settingKey);
		const auto component = info.controlType == SceneSettingControlType::Scalar ?
		                           std::string() : info.componentDisplayName;
		return {
			.settingPath = physicalPath,
			.key = physicalKey,
			.displayName = GetDescriptorLabel(info, component),
			.displayPath = GetCatalogSelectorPath(setting),
			.value = value,
			.controlType = SceneSettingControlType::Scalar,
			.aggregatePresentation = ManagerAggregatePresentation::Components,
			.unifiedEditMode = ManagerUnifiedEditMode::None,
			.members = { { physicalPath, physicalKey, info.componentDisplayName, value,
				setting.serializedComponent, info.aggregateAll } },
		};
	}

	using DescriptorGroupKey = std::tuple<std::string, std::string, std::int8_t, std::uint8_t, SceneSettingControlType>;

	std::vector<ManagerSettingDescriptor> CollectFeatureSceneSettings(
		const std::string& featureShortName, bool transitionableOnly)
	{
		auto* feature = Feature::FindFeatureByShortName(featureShortName);
		if (!feature)
			return {};

		SceneSettingsManager::SceneLayerGuard guard;
		json featureSettings;
		feature->SaveSettings(featureSettings);
		if (!featureSettings.is_object())
			return {};

		std::vector<ManagerSettingDescriptor> descriptors;
		std::map<DescriptorGroupKey, ManagerSettingDescriptor> groups;
		for (const auto& setting : SceneSettingsCatalog::GetSettings()) {
			if (setting.featureShortName != featureShortName || !IsCatalogSettingAllowedByPolicy(setting))
				continue;
			if (transitionableOnly &&
				!SceneSettingsCatalog::HasFlag(setting.flags, SceneSettingsCatalog::SettingFlag::Transitionable))
				continue;

			auto settingPath = SplitCatalogPath(setting.settingPath);
			if (setting.settingKey.empty())
				continue;

			const auto* value = GetCatalogSerializedValue(featureSettings, setting);
			if (!value || !IsSceneSettingPrimitive(*value) ||
				!IsCatalogValueCompatible(setting, *value) ||
				(transitionableOnly && !IsNumericValue(*value)))
				continue;

			auto info = MakeSettingControlInfo(setting);
			if (info.controlType == SceneSettingControlType::Scalar || info.componentCount < 2) {
				descriptors.push_back(MakeScalarDescriptor(setting, *value));
				continue;
			}

			DescriptorGroupKey key{
				std::string(setting.serializedPath), std::string(setting.serializedKey),
				info.componentStart, info.componentCount, info.controlType
			};
			auto [groupIt, inserted] = groups.try_emplace(key);
			auto& descriptor = groupIt->second;
			if (inserted) {
				descriptor.settingPath = settingPath;
				descriptor.key = std::string(setting.settingKey);
				descriptor.displayName = GetDescriptorLabel(info);
				descriptor.displayPath = GetCatalogSelectorPath(setting);
				descriptor.value = *value;
				descriptor.controlType = info.controlType;
				descriptor.aggregatePresentation = info.aggregatePresentation;
				descriptor.unifiedEditMode = info.unifiedEditMode;
			} else {
				if (descriptor.aggregatePresentation != info.aggregatePresentation)
					descriptor.aggregatePresentation = ManagerAggregatePresentation::Components;
				if (descriptor.unifiedEditMode != info.unifiedEditMode)
					descriptor.unifiedEditMode = ManagerUnifiedEditMode::None;
			}
			descriptor.members.push_back({
				std::move(settingPath), std::string(setting.settingKey), info.componentDisplayName,
				*value, setting.serializedComponent, info.aggregateAll
			});
		}

		for (auto& [key, descriptor] : groups) {
			const auto expectedCount = std::get<3>(key);
			const auto expectedStart = std::get<2>(key);
			std::sort(descriptor.members.begin(), descriptor.members.end(), [](const auto& lhs, const auto& rhs) {
				return lhs.componentIndex < rhs.componentIndex;
			});
			bool complete = descriptor.members.size() == expectedCount;
			for (size_t index = 0; complete && index < descriptor.members.size(); ++index)
				complete = descriptor.members[index].componentIndex == expectedStart + index;
			if (complete) {
				descriptors.push_back(std::move(descriptor));
				continue;
			}
			for (const auto& member : descriptor.members) {
				auto* setting = FindAllowedCatalogSetting(
					featureShortName, member.settingPath, member.key, transitionableOnly);
				if (setting)
					descriptors.push_back(MakeScalarDescriptor(*setting, member.value));
			}
		}

		std::sort(descriptors.begin(), descriptors.end(), [](const auto& lhs, const auto& rhs) {
			return std::tie(lhs.displayPath, lhs.displayName, lhs.settingPath, lhs.key) <
			       std::tie(rhs.displayPath, rhs.displayName, rhs.settingPath, rhs.key);
		});
		return descriptors;
	}
}

std::vector<SceneSettingsManager::SettingDescriptor> SceneSettingsManager::GetFeatureSceneSettings(const std::string& featureShortName)
{
	return CollectFeatureSceneSettings(featureShortName, false);
}

std::vector<SceneSettingsManager::SettingDescriptor> SceneSettingsManager::GetTransitionableSceneSettings(const std::string& featureShortName)
{
	return CollectFeatureSceneSettings(featureShortName, true);
}

bool SceneSettingsManager::GetSettingControlInfo(const SettingEntry& entry, SettingControlInfo& info)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	if (!setting)
		return false;
	info = MakeSettingControlInfo(*setting);
	return true;
}

std::string SceneSettingsManager::GetSettingDisplayName(const std::string& settingKey)
{
	return StripImGuiId(Util::PrettifyIdentifier(settingKey));
}

static std::string GetSceneSettingDisplayName(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey)
{
	auto* setting = FindAllowedCatalogSetting(featureShortName, settingPath, settingKey);
	if (setting) {
		auto info = MakeSettingControlInfo(*setting);
		auto displayName = info.displayName;
		if (info.controlType != SceneSettingControlType::Scalar && !info.componentDisplayName.empty())
			displayName += std::format(" ({})", info.componentDisplayName);
		return JoinDisplayParts(info.displayPath, displayName);
	}
	return SceneSettingsManager::GetSettingDisplayName(settingKey);
}

json SceneSettingsManager::GetFeatureSettingValue(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey)
{
	auto* setting = FindAllowedCatalogSetting(featureShortName, settingPath, settingKey);
	if (!setting)
		return {};
	auto* feature = Feature::FindFeatureByShortName(featureShortName);
	if (!feature)
		return {};

	SceneLayerGuard guard;
	json value;
	if (GetCatalogSettingValue(*feature, *setting, value))
		return value;
	return {};
}

SceneSettingsManager::SettingType SceneSettingsManager::DetectSettingType(const json& value)
{
	if (value.is_boolean())
		return SettingType::Boolean;
	if (value.is_number_integer())
		return SettingType::Integer;
	if (value.is_number_float())
		return SettingType::Float;
	if (value.is_string())
		return SettingType::String;
	return SettingType::Unknown;
}

bool SceneSettingsManager::IsBooleanControlSetting(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && SceneSettingsCatalog::HasFlag(
		setting->flags, SceneSettingsCatalog::SettingFlag::BooleanControl);
}

bool SceneSettingsManager::IsInvertedDisplaySetting(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && setting->invertedDisplay;
}

bool SceneSettingsManager::GetNumericBounds(const SettingEntry& entry, double& minimum, double& maximum)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	if (!setting || setting->editorSemantic != SceneSettingsCatalog::EditorSemantic::Numeric ||
		!setting->hasNumericBounds || !std::isfinite(setting->minimumValue) ||
		!std::isfinite(setting->maximumValue) || setting->minimumValue > setting->maximumValue)
		return false;
	minimum = setting->minimumValue;
	maximum = setting->maximumValue;
	return true;
}

double SceneSettingsManager::GetNumericDisplayScale(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	if (!setting || setting->editorSemantic != SceneSettingsCatalog::EditorSemantic::Numeric)
		return 1.0;
	return GetCatalogNumericDisplayScale(*setting);
}

bool SceneSettingsManager::GetNumericDisplayValue(
	const SettingEntry& entry, double storedValue, double& displayValue)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && ConvertCatalogNumericStoredToDisplay(*setting, storedValue, displayValue);
}

bool SceneSettingsManager::GetNumericStoredValue(
	const SettingEntry& entry, double displayValue, double& storedValue)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && ConvertCatalogNumericDisplayToStored(*setting, displayValue, storedValue);
}

size_t SceneSettingsManager::GetSettingChoiceCount(const SettingEntry& entry)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	return setting && setting->editorSemantic == SceneSettingsCatalog::EditorSemantic::Choice ?
	           setting->choiceCount :
	           0;
}

bool SceneSettingsManager::GetSettingChoice(
	const SettingEntry& entry, size_t index, std::int64_t& value, std::string& displayName)
{
	auto* setting = FindAllowedCatalogSetting(
		entry.featureShortName, entry.settingPath, entry.settingKey);
	if (!setting || setting->editorSemantic != SceneSettingsCatalog::EditorSemantic::Choice ||
		index >= setting->choiceCount)
		return false;
	const auto& choice = setting->choices[index];
	value = choice.value;
	displayName = StripImGuiId(choice.displayName);
	if (!choice.displayNameKey.empty())
		displayName = StripImGuiId(T(choice.displayNameKey, displayName.c_str()));
	return true;
}

using FeatureSettingsCache = std::map<std::string, json>;

static bool GetFeatureSettingValueForValidation(Feature& feature, const std::string& featureShortName,
	const SceneSettingsCatalog::SettingMetadata& setting,
	FeatureSettingsCache* featureSettingsCache, json& featureValue)
{
	if (!featureSettingsCache)
		return GetCatalogSettingValue(feature, setting, featureValue);

	auto [snapshotIt, inserted] = featureSettingsCache->try_emplace(featureShortName);
	if (inserted) {
		try {
			feature.SaveSettings(snapshotIt->second);
		} catch (const std::exception& e) {
			logger::warn("[SceneSettings] Could not snapshot {} while loading scene settings: {}",
				featureShortName, e.what());
			snapshotIt->second = nullptr;
		} catch (...) {
			logger::warn("[SceneSettings] Could not snapshot {} while loading scene settings", featureShortName);
			snapshotIt->second = nullptr;
		}
	}
	if (!snapshotIt->second.is_object())
		return false;

	const auto* value = GetCatalogSerializedValue(snapshotIt->second, setting);
	if (!value || !IsSceneSettingPrimitive(*value))
		return false;
	featureValue = *value;
	return true;
}

static bool IsSceneSettingValueAllowed(const json& featureValue,
	const SceneSettingsCatalog::SettingMetadata& setting, const json& value, bool requireNumeric)
{
	if (!IsCatalogValueCompatible(setting, featureValue) || !IsCatalogValueCompatible(setting, value))
		return false;

	if (value.is_number() && !std::isfinite(value.get<double>()))
		return false;
	if (setting.editorSemantic == SceneSettingsCatalog::EditorSemantic::Numeric) {
		double ignoredDisplayValue = 0.0;
		if (!featureValue.is_number() || !value.is_number() ||
			!ConvertCatalogNumericStoredToDisplay(setting, featureValue.get<double>(), ignoredDisplayValue) ||
			!ConvertCatalogNumericStoredToDisplay(setting, value.get<double>(), ignoredDisplayValue))
			return false;
	}

	if (SceneSettingsCatalog::HasFlag(setting.flags, SceneSettingsCatalog::SettingFlag::BooleanControl)) {
		if (setting.valueType == SceneSettingsCatalog::ValueType::Integer &&
			(!value.is_number_integer() || (value.get<std::int64_t>() != 0 && value.get<std::int64_t>() != 1)))
			return false;
		if (setting.valueType == SceneSettingsCatalog::ValueType::Boolean && !value.is_boolean())
			return false;
	}

	if (setting.choiceCount > 0) {
		if (!value.is_number_integer())
			return false;
		const auto choiceValue = value.get<std::int64_t>();
		if (std::none_of(setting.choices, setting.choices + setting.choiceCount,
				[&](const auto& choice) { return choice.value == choiceValue; }))
			return false;
	}

	if (requireNumeric && (!SceneSettingsCatalog::HasFlag(setting.flags, SceneSettingsCatalog::SettingFlag::Transitionable) ||
		                      !IsNumericValue(featureValue) || !IsNumericValue(value) || !std::isfinite(value.get<float>())))
		return false;
	if (!requireNumeric && !IsSceneSettingPrimitive(value))
		return false;

	return IsCompatibleSceneSettingValue(featureValue, value);
}

static bool ValidateSceneSettingEntry(std::string_view context, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, const json& value,
	bool requireNumeric, FeatureSettingsCache* featureSettingsCache = nullptr)
{
	if (IsBlacklistedSceneSetting(featureShortName, settingPath, settingKey)) {
		logger::warn("[SceneSettings] {} entry {} is blacklisted",
			context, GetSettingLogName(featureShortName, settingPath, settingKey));
		return false;
	}

	auto* setting = FindAllowedCatalogSetting(featureShortName, settingPath, settingKey, requireNumeric);
	if (!setting) {
		logger::warn("[SceneSettings] {} entry {} is not permitted by the compiled scene settings catalog",
			context, GetSettingLogName(featureShortName, settingPath, settingKey));
		return false;
	}

	auto* feature = Feature::FindFeatureByShortName(featureShortName);
	if (!feature) {
		logger::warn("[SceneSettings] {} entry {} - feature '{}' not found/loaded",
			context, GetSettingLogName(featureShortName, settingPath, settingKey), featureShortName);
		return false;
	}

	json featureValue;
	if (!GetFeatureSettingValueForValidation(*feature, featureShortName, *setting,
			featureSettingsCache, featureValue) ||
		!IsSceneSettingValueAllowed(featureValue, *setting, value, requireNumeric)) {
		logger::warn("[SceneSettings] {} entry {} is not a supported scene-manager setting",
			context, GetSettingLogName(featureShortName, settingPath, settingKey));
		return false;
	}
	return true;
}

static bool ApplyEntryValueUpdates(std::string_view context,
	std::vector<SceneSettingsManager::SettingEntry>& entries,
	std::span<const SceneSettingsManager::EntryValueUpdate> updates,
	bool requireNumeric, bool& userEntriesChanged)
{
	if (updates.empty())
		return false;

	std::set<size_t> updatedIndices;
	FeatureSettingsCache featureSettingsCache;
	for (const auto& update : updates) {
		if (update.index >= entries.size() || !updatedIndices.insert(update.index).second)
			return false;
		const auto& entry = entries[update.index];
		if (!ValidateSceneSettingEntry(context, entry.featureShortName, entry.settingPath,
				entry.settingKey, update.value, requireNumeric, &featureSettingsCache))
			return false;
	}

	userEntriesChanged = false;
	for (const auto& update : updates) {
		auto& entry = entries[update.index];
		entry.value = update.value;
		userEntriesChanged |= entry.source == SceneSettingsManager::EntrySource::User;
	}
	return true;
}

// --- Generic Entry Management ---

std::vector<SceneSettingsManager::SettingEntry>& SceneSettingsManager::GetEntriesMut(SceneType type)
{
	assert(IsEntryListSceneType(type));
	return entries[type];
}

void SceneSettingsManager::BumpEntryPresentationRevision()
{
	++entryPresentationRevision;
}

const std::vector<SceneSettingsManager::SettingEntry>& SceneSettingsManager::GetEntries(SceneType type) const
{
	static const std::vector<SettingEntry> empty;
	if (!IsEntryListSceneType(type))
		return empty;
	auto it = entries.find(type);
	return (it != entries.end()) ? it->second : empty;
}

void SceneSettingsManager::MarkEntryListUserSettingsModified(SceneType type)
{
	assert(IsEntryListSceneType(type));
	if (type == SceneType::InteriorOnly)
		interiorUserSettingsModified = true;
	else
		timeOfDayUserSettingsModified = true;
}

bool SceneSettingsManager::IsEntryActive(const SettingEntry& entry) const
{
	return !entry.paused && !IsFeaturePaused(entry.featureShortName);
}

bool SceneSettingsManager::HasEntryFromSource(SceneType type, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, EntrySource source) const
{
	for (const auto& entry : GetEntries(type)) {
		if (entry.source == source && IsSameSetting(entry, featureShortName, settingPath, settingKey))
			return true;
	}
	return false;
}

bool SceneSettingsManager::HasEntryForPeriod(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey,
	TimeOfDayPeriod period, EntrySource source) const
{
	for (const auto& entry : GetEntries(SceneType::TimeOfDay)) {
		if (entry.source == source && entry.period == period &&
			IsSameSetting(entry, featureShortName, settingPath, settingKey))
			return true;
	}
	return false;
}

bool SceneSettingsManager::HasDuplicateEntry(SceneType type, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, EntrySource source, TimeOfDayPeriod period) const
{
	if (!IsEntryListSceneType(type))
		return false;
	if (type == SceneType::TimeOfDay)
		return HasEntryForPeriod(featureShortName, settingPath, settingKey, period, source);
	return HasEntryFromSource(type, featureShortName, settingPath, settingKey, source);
}

bool SceneSettingsManager::AddSetting(SceneType type, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, const json& value,
	TimeOfDayPeriod period, bool deferCommit)
{
	if (!IsEntryListSceneType(type) || !IsFeatureAllowedForType(type, featureShortName))
		return false;

	const bool requireNumeric = type == SceneType::TimeOfDay;
	if (requireNumeric) {
		// Reject invalid period values (Count is the sentinel, not a real period)
		if (period == TimeOfDayPeriod::Count || static_cast<int>(period) < 0 || static_cast<int>(period) >= kPeriodCount) {
			logger::warn("[SceneSettings] Rejecting TOD setting with invalid period: {}", GetSettingLogName(featureShortName, settingPath, settingKey));
			return false;
		}
	}
	if (!ValidateSceneSettingEntry(GetSceneTypeName(type), featureShortName, settingPath, settingKey, value, requireNumeric))
		return false;

	if (HasDuplicateEntry(type, featureShortName, settingPath, settingKey, EntrySource::User, period))
		return false;

	auto& vec = GetEntriesMut(type);

	SettingEntry entry;
	entry.featureShortName = featureShortName;
	entry.settingPath = settingPath;
	entry.settingKey = settingKey;
	entry.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, settingKey);
	entry.value = value;
	entry.originalValue = entry.value;
	entry.source = EntrySource::User;
	entry.period = period;
	vec.push_back(std::move(entry));
	BumpEntryPresentationRevision();
	MarkEntryListUserSettingsModified(type);
	if (deferCommit)
		MarkDeferredSceneChanges();
	else
		CommitSceneSettingChanges();
	return true;
}

void SceneSettingsManager::RemoveSetting(SceneType type, size_t index)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	if (index >= vec.size())
		return;

	const auto entry = vec[index];
	if (entry.source == EntrySource::Overwrite && !entry.sourceFilename.empty() &&
		!RemoveSettingFromOverwriteFile(GetSceneOverwritePath(type, entry), entry.settingPath, entry.settingKey))
		return;

	logger::info("[SceneSettings] Removed {} entry: {} (source={})", GetSceneTypeName(type),
		GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey),
		entry.source == EntrySource::Overwrite ? "overwrite" : "user");

	vec.erase(vec.begin() + static_cast<ptrdiff_t>(index));
	BumpEntryPresentationRevision();
	if (entry.source == EntrySource::User) {
		MarkEntryListUserSettingsModified(type);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::TogglePauseEntry(SceneType type, size_t index)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	if (index < vec.size()) {
		vec[index].paused = !vec[index].paused;
		BumpEntryPresentationRevision();
		if (vec[index].source == EntrySource::User) {
			MarkEntryListUserSettingsModified(type);
			SaveAllUserSettings();
		}
		ReapplyIfActive();
	}
}

void SceneSettingsManager::RevertEntryToDefault(SceneType type, size_t index)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	if (index >= vec.size())
		return;
	auto& entry = vec[index];
	if (entry.originalValue.is_null() ||
		!ValidateSceneSettingEntry(GetSceneTypeName(type), entry.featureShortName,
			entry.settingPath, entry.settingKey, entry.originalValue, type == SceneType::TimeOfDay))
		return;

	entry.value = entry.originalValue;
	if (entry.source == EntrySource::User) {
		MarkEntryListUserSettingsModified(type);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::SetAllOverwritesPaused(SceneType type, bool paused)
{
	if (!IsEntryListSceneType(type))
		return;
	bool changed = false;
	for (auto& entry : GetEntriesMut(type)) {
		if (entry.source == EntrySource::Overwrite && entry.paused != paused) {
			entry.paused = paused;
			changed = true;
		}
	}
	if (changed)
		BumpEntryPresentationRevision();
	ReapplyIfActive();
}

bool SceneSettingsManager::AreAllOverwritesPaused(SceneType type) const
{
	if (!IsEntryListSceneType(type))
		return false;
	bool found = false;
	for (const auto& entry : GetEntries(type)) {
		if (entry.source != EntrySource::Overwrite)
			continue;
		found = true;
		if (!entry.paused)
			return false;
	}
	return found;
}

void SceneSettingsManager::DeleteAllOverwrites(SceneType type)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);

	std::vector<bool> shouldErase(vec.size(), false);
	std::map<std::filesystem::path, bool> deleteResults;
	for (size_t i = 0; i < vec.size(); ++i) {
		const auto& entry = vec[i];
		if (entry.source != EntrySource::Overwrite)
			continue;
		if (entry.sourceFilename.empty()) {
			shouldErase[i] = true;
			continue;
		}
		auto filepath = GetSceneOverwritePath(type, entry);
		auto [resultIt, inserted] = deleteResults.try_emplace(filepath, false);
		if (inserted) {
			std::error_code ec;
			auto removed = std::filesystem::remove(filepath, ec);
			resultIt->second = removed || !ec;
			if (!resultIt->second)
				logger::error("[SceneSettings] Failed to delete overwrite file: {} ({}) - keeping entry", filepath.string(), ec.message());
		}

		if (resultIt->second)
			shouldErase[i] = true;
	}
	// Erase only entries whose backing files were successfully cleaned up
	// (iterate in reverse to preserve index validity)
	bool changed = false;
	for (size_t i = vec.size(); i-- > 0;) {
		if (shouldErase[i]) {
			vec.erase(vec.begin() + static_cast<ptrdiff_t>(i));
			changed = true;
		}
	}
	if (changed)
		BumpEntryPresentationRevision();

	ReapplyIfActive();
}

void SceneSettingsManager::SetAllUserPaused(SceneType type, bool paused)
{
	if (!IsEntryListSceneType(type))
		return;
	bool changed = false;
	for (auto& entry : GetEntriesMut(type)) {
		if (entry.source == EntrySource::User && entry.paused != paused) {
			entry.paused = paused;
			changed = true;
		}
	}
	if (changed)
		BumpEntryPresentationRevision();
	MarkEntryListUserSettingsModified(type);
	SaveAllUserSettings();
	ReapplyIfActive();
}

bool SceneSettingsManager::AreAllUserPaused(SceneType type) const
{
	if (!IsEntryListSceneType(type))
		return false;
	bool found = false;
	for (const auto& entry : GetEntries(type)) {
		if (entry.source != EntrySource::User)
			continue;
		found = true;
		if (!entry.paused)
			return false;
	}
	return found;
}

void SceneSettingsManager::DeleteAllUserSettings(SceneType type)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	const auto removed = std::erase_if(vec, [](const SettingEntry& e) {
		return e.source == EntrySource::User;
	});
	if (removed != 0)
		BumpEntryPresentationRevision();
	unresolvedUserEntries[type].clear();

	MarkEntryListUserSettingsModified(type);
	SaveAllUserSettings();
	ReapplyIfActive();
}

/// Per-period overwrites live in a period subfolder of their scene's directory.
static std::filesystem::path GetOverwriteDir(const std::filesystem::path& baseDir,
	SceneSettingsManager::TimeOfDayPeriod period)
{
	return period != SceneSettingsManager::TimeOfDayPeriod::Count ?
	           baseDir / SceneSettingsManager::GetPeriodName(period) :
	           baseDir;
}

/// Discovered entries keep the exact file they came from; authored ones derive it from their period.
static std::filesystem::path GetOverwriteFilePath(const std::filesystem::path& baseDir,
	const SceneSettingsManager::SettingEntry& entry)
{
	if (!entry.sourcePath.empty())
		return entry.sourcePath;
	return GetOverwriteDir(baseDir, entry.period) / entry.sourceFilename;
}

static std::string GetOverwriteTypeDescription(std::string_view sceneLabel,
	SceneSettingsManager::TimeOfDayPeriod period)
{
	return period != SceneSettingsManager::TimeOfDayPeriod::Count ?
	           std::format("{} - {}", sceneLabel, SceneSettingsManager::GetPeriodName(period)) :
	           std::string(sceneLabel);
}

static std::filesystem::path GetSceneOverwritePath(SceneSettingsManager::SceneType type,
	const SceneSettingsManager::SettingEntry& entry)
{
	return GetOverwriteFilePath(SceneSettingsManager::GetOverwritesPath(type), entry);
}

static std::filesystem::path GetWeatherOverwritePath(RE::FormID weatherId, const SceneSettingsManager::SettingEntry& entry)
{
	return GetOverwriteFilePath(
		SceneSettingsManager::GetWeatherOverwritesDir() / Util::FormIdToSpid(weatherId), entry);
}

static std::filesystem::path GetLocationOverwritePath(std::string_view formKey,
	const SceneSettingsManager::SettingEntry& entry)
{
	return GetOverwriteFilePath(SceneSettingsManager::GetLocationOverwritesDir() / formKey, entry);
}

static bool WriteGroupedOverwriteFile(const std::filesystem::path& path, const std::string& featureShortName,
	const std::string& overwriteType, const std::vector<const SceneSettingsManager::SettingEntry*>& entries,
	const json& extraMetadata = json::object())
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

static bool RemoveSettingFromOverwriteFile(const std::filesystem::path& path,
	const std::vector<std::string>& settingPath, const std::string& settingKey)
{
	if (path.empty())
		return true;

	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
		return !ec;

	std::ifstream in(path);
	if (!in.is_open()) {
		logger::error("[SceneSettings] Could not open overwrite file '{}' for editing", path.string());
		return false;
	}

	auto data = json::parse(in, nullptr, false);
	if (!data.is_object()) {
		logger::error("[SceneSettings] Could not parse overwrite file '{}' for editing", path.string());
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

/// Groups the selected user entries by target directory and feature, one overwrite file per group.
static void ExportUserEntriesToOverwrites(const std::vector<SceneSettingsManager::SettingEntry>& entries,
	const std::vector<size_t>& indices, const std::filesystem::path& baseDir, const std::string& modName,
	std::string_view sceneLabel, const json& extraMetadata = json::object())
{
	const auto safeModName = Util::FileHelpers::SanitizeFileName(modName);
	if (safeModName.empty())
		return;

	std::map<std::pair<std::filesystem::path, std::string>, std::vector<const SceneSettingsManager::SettingEntry*>> groupedEntries;
	for (auto index : indices) {
		if (index >= entries.size() || entries[index].source != SceneSettingsManager::EntrySource::User)
			continue;
		const auto& entry = entries[index];
		groupedEntries[{ GetOverwriteDir(baseDir, entry.period), entry.featureShortName }].push_back(&entry);
	}

	for (const auto& [group, grouped] : groupedEntries) {
		const auto& [directory, featureShortName] = group;
		WriteGroupedOverwriteFile(directory / std::format("{}_{}.json", safeModName, featureShortName),
			featureShortName, GetOverwriteTypeDescription(sceneLabel, grouped.front()->period), grouped, extraMetadata);
	}
}

void SceneSettingsManager::ExportUserSettingsToOverwrites(SceneType type, const std::vector<size_t>& indices, const std::string& modName)
{
	if (!IsEntryListSceneType(type))
		return;
	ExportUserEntriesToOverwrites(GetEntriesMut(type), indices, GetOverwritesPath(type), modName,
		type == SceneType::InteriorOnly ? "Interior Only" : "Time of Day");
}

void SceneSettingsManager::ExportWeatherUserSettingsToOverwrites(RE::FormID weatherId, const std::vector<size_t>& indices, const std::string& modName)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	ExportUserEntriesToOverwrites(GetWeatherConfigMut(weatherId).entries, indices,
		GetWeatherOverwritesDir() / Util::FormIdToSpid(weatherId), modName, "Weather");
}

void SceneSettingsManager::UpdateEntryValue(SceneType type, size_t index, const json& newValue, bool deferSave)
{
	const EntryValueUpdate update{ index, newValue };
	UpdateEntryValues(type, std::span{ &update, 1 }, deferSave);
}

void SceneSettingsManager::UpdateEntryValues(
	SceneType type, std::span<const EntryValueUpdate> updates, bool deferSave)
{
	if (!IsEntryListSceneType(type))
		return;
	auto& vec = GetEntriesMut(type);
	const bool requireNumeric = type == SceneType::TimeOfDay;
	bool userEntriesChanged = false;
	if (!ApplyEntryValueUpdates(
			GetSceneTypeName(type), vec, updates, requireNumeric, userEntriesChanged))
		return;

	if (userEntriesChanged) {
		MarkEntryListUserSettingsModified(type);
		if (deferSave)
			MarkDeferredSceneChanges();
		else
			SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::CommitSceneSettingChanges()
{
	SaveAllUserSettings();
	ReapplyIfActive();
}

void SceneSettingsManager::MarkDeferredSceneChanges()
{
	deferredSceneChangesPending = true;
	deferredSceneChangesDeadline = std::chrono::steady_clock::now() + kDeferredSaveDelay;
}

void SceneSettingsManager::FlushDeferredSceneChanges()
{
	if (!deferredSceneChangesPending || std::chrono::steady_clock::now() < deferredSceneChangesDeadline)
		return;

	SaveAllUserSettings();
	ReapplyIfActive();
}

// --- Event Handler ---

RE::BSEventNotifyControl SceneSettingsManager::MenuOpenCloseEventHandler::ProcessEvent(
	const RE::MenuOpenCloseEvent* a_event,
	RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME && !a_event->opening) {
		// Defer cell transition to next frame - cell data isn't available yet
		// Queue reset work until the menu closes.
		GetSingleton()->queuedCellTransition.store(true, std::memory_order_relaxed);
	}

	return RE::BSEventNotifyControl::kContinue;
}

// --- Scene Application ---

void SceneSettingsManager::Update()
{
	if (globals::state) {
		const auto frame = globals::state->frameCount;
		if (lastUpdateFrame == frame)
			return;
		lastUpdateFrame = frame;
	}
	FlushDeferredSceneChanges();

	if (queuedCellTransition.exchange(false, std::memory_order_relaxed)) {
		OnCellTransition();
	}

	ResolveAndApply();
}

void SceneSettingsManager::OnCellTransition()
{
	resolverDirty = true;
	ResolveAndApply(true);
}

void SceneSettingsManager::ReapplyIfActive()
{
	activeEntryCacheDirty = true;
	resolverDirty = true;
	if (!resolverSuspended)
		ResolveAndApply(true);
}

bool SceneSettingsManager::HasActiveSettingsForFeature(const std::string& featureShortName) const
{
	return std::any_of(appliedSettings.begin(), appliedSettings.end(), [&](const auto& item) {
		return item.first.featureShortName == featureShortName;
	});
}

bool SceneSettingsManager::HasAnySceneEntriesForFeature(const std::string& featureShortName) const
{
	const auto hasFeature = [&](const auto& sourceEntries) {
		return std::any_of(sourceEntries.begin(), sourceEntries.end(), [&](const auto& entry) {
			return entry.featureShortName == featureShortName;
		});
	};

	for (const auto& [_, sourceEntries] : entries)
		if (hasFeature(sourceEntries))
			return true;
	for (const auto& [_, config] : weatherSceneConfigs)
		if (hasFeature(config.entries))
			return true;
	for (const auto& [_, config] : locationSceneConfigs)
		if (hasFeature(config.entries))
			return true;
	return false;
}

bool SceneSettingsManager::IsActiveSceneSetting(std::string_view featureShortName,
	std::string_view settingPath, std::string_view settingKey) const
{
	return IsActiveSceneSetting(std::string(featureShortName), SplitCatalogPath(settingPath), std::string(settingKey));
}

bool SceneSettingsManager::IsActiveSceneSetting(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey) const
{
	return appliedSettings.contains({ featureShortName, settingPath, settingKey });
}

void SceneSettingsManager::CaptureExternalFeatureChanges(Feature* feature)
{
	if (!feature || appliedSettings.empty())
		return;

	json featureSettings;
	try {
		feature->SaveSettings(featureSettings);
	} catch (const std::exception& e) {
		logger::warn("[SceneSettings] Could not inspect external changes for {}: {}",
			feature->GetShortName(), e.what());
		return;
	} catch (...) {
		logger::warn("[SceneSettings] Could not inspect external changes for {}",
			feature->GetShortName());
		return;
	}
	if (!featureSettings.is_object())
		return;

	std::vector<std::pair<SettingAddress, json>> changedSettings;
	const auto featureShortName = feature->GetShortName();
	for (const auto& [address, appliedValue] : appliedSettings) {
		if (address.featureShortName != featureShortName)
			continue;
		auto* setting = FindAllowedCatalogSetting(
			address.featureShortName, address.settingPath, address.settingKey);
		if (!setting)
			continue;
		const auto* value = GetCatalogSerializedValue(featureSettings, *setting);
		if (!value || !IsSceneSettingPrimitive(*value) ||
			!IsCompatibleSceneSettingValue(appliedValue, *value) || appliedValue == *value)
			continue;
		changedSettings.emplace_back(address, *value);
	}

	if (changedSettings.empty())
		return;
	for (const auto& [address, value] : changedSettings) {
		baselineSettings[address] = value;
		appliedSettings[address] = value;
	}
	resolverDirty = true;
	if (!resolverSuspended)
		ResolveAndApply(true);
}

SceneSettingsManager::SceneLayerGuard::SceneLayerGuard() :
	manager(GetSingleton())
{
	if (manager)
		manager->SuspendSceneLayer();
}

SceneSettingsManager::SceneLayerGuard::~SceneLayerGuard()
{
	if (manager)
		manager->ResumeSceneLayer();
}

bool SceneSettingsManager::IsFeaturePaused(const std::string& featureShortName) const
{
	auto it = featurePauseStates.find(featureShortName);
	return it != featurePauseStates.end() && it->second;
}

void SceneSettingsManager::SetFeaturePaused(const std::string& featureShortName, bool paused)
{
	featurePauseStates[featureShortName] = paused;
	ReapplyIfActive();
}

void SceneSettingsManager::SuspendSceneLayer()
{
	if (++sceneLayerSuspendDepth > 1)
		return;

	resolverSuspended = true;
	RestoreAppliedSettings();
}

void SceneSettingsManager::ResumeSceneLayer()
{
	if (sceneLayerSuspendDepth <= 0) {
		logger::warn("[SceneSettings] ResumeSceneLayer called without a matching suspend");
		sceneLayerSuspendDepth = 0;
		return;
	}
	if (--sceneLayerSuspendDepth > 0)
		return;

	resolverSuspended = false;
	resolverDirty = true;
	ResolveAndApply(true);
}

void SceneSettingsManager::ResolveAndApply(bool force)
{
	if (resolverSuspended || sceneLayerSuspendDepth > 0)
		return;
	if (!locationDataLoaded)
		TryEnsureLocationDataLoaded();
	if (!weatherDataLoaded)
		TryEnsureWeatherDataLoaded();
	if (!HasActiveSceneEntriesCached()) {
		applyFailures.clear();
		if (!appliedSettings.empty())
			RestoreAppliedSettings();
		resolverDirty = !appliedSettings.empty();
		return;
	}

	if (globals::state && globals::state->IsMainOrLoadingMenuOpen()) {
		RestoreAppliedSettings();
		resolverDirty = true;
		return;
	}

	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* cell = player ? player->GetParentCell() : nullptr;
	if (!player || !cell) {
		RestoreAppliedSettings();
		resolverDirty = true;
		return;
	}

	const bool interior = Util::IsInterior();
	const auto hour = GetCurrentGameHour();
	auto* location = player->GetCurrentLocation();
	if (!location)
		location = cell->GetLocation();
	const auto locationId = location ? location->GetFormID() : 0;
	const auto cellId = cell->GetFormID();

	WeatherBlend weather;
	if (!interior) {
		TryEnsureWeatherDataLoaded();
		weather = GetWeatherBlend();
	}

	const bool contextChanged = interior != lastResolvedInterior ||
	                            locationId != lastResolvedLocationId ||
	                            cellId != lastResolvedCellId ||
	                            weather.currentWeatherId != lastResolvedCurrentWeatherId ||
	                            weather.previousWeatherId != lastResolvedPreviousWeatherId ||
	                            std::abs(weather.lerp - lastResolvedWeatherLerp) >= kBlendEpsilon ||
	                            lastResolvedHour < 0.0f || std::abs(hour - lastResolvedHour) >= kHourUpdateThreshold;
	const auto now = std::chrono::steady_clock::now();
	const bool applyRetryDue = std::any_of(applyFailures.begin(), applyFailures.end(),
		[&](const auto& item) { return now >= item.second.retryAfter; });
	if (!force && !resolverDirty && !contextChanged && !applyRetryDue)
		return;

	resolverDirty = false;
	auto resolved = BuildResolvedSettings();
	ApplyResolvedSettings(resolved, force);

	lastResolvedInterior = interior;
	lastResolvedLocationId = locationId;
	lastResolvedCellId = cellId;
	lastResolvedHour = hour;
	lastResolvedCurrentWeatherId = weather.currentWeatherId;
	lastResolvedPreviousWeatherId = weather.previousWeatherId;
	lastResolvedWeatherLerp = weather.lerp;
}

SceneSettingsManager::WeatherBlend SceneSettingsManager::GetWeatherBlend() const
{
	WeatherBlend blend;
	auto* sky = globals::game::sky;
	if (!sky)
		return blend;
	blend.currentWeatherId = sky->currentWeather ? sky->currentWeather->GetFormID() : 0;
	blend.lerp = std::isfinite(sky->currentWeatherPct) ? std::clamp(sky->currentWeatherPct, 0.0f, 1.0f) : 0.0f;
	blend.previousWeatherId = GetEffectivePreviousWeatherId(sky, blend.lerp);
	return blend;
}

SceneSettingsManager::SettingAddress SceneSettingsManager::GetEntryAddress(const SettingEntry& entry)
{
	return { entry.featureShortName, entry.settingPath, entry.settingKey };
}

bool SceneSettingsManager::IsResolvableEntry(const SettingEntry& entry, SceneType type) const
{
	const bool floatsOnly = type == SceneType::TimeOfDay;
	return IsEntryActive(entry) &&
	       IsFeatureAllowedForType(type, entry.featureShortName) &&
	       (!floatsOnly || IsNumericValue(entry.value)) &&
	       FindAllowedCatalogSetting(entry.featureShortName, entry.settingPath, entry.settingKey, floatsOnly);
}

bool SceneSettingsManager::HasActiveSceneEntriesCached()
{
	if (!activeEntryCacheDirty)
		return hasActiveSceneEntries;

	const auto hasResolvable = [&](const std::vector<SettingEntry>& sourceEntries, SceneType type) {
		return std::any_of(sourceEntries.begin(), sourceEntries.end(),
			[&](const SettingEntry& entry) { return IsResolvableEntry(entry, type); });
	};

	hasActiveSceneEntries =
		std::any_of(entries.begin(), entries.end(),
			[&](const auto& item) { return hasResolvable(item.second, item.first); }) ||
		std::any_of(weatherSceneConfigs.begin(), weatherSceneConfigs.end(),
			[&](const auto& item) { return hasResolvable(item.second.entries, SceneType::TimeOfDay); }) ||
		std::any_of(locationSceneConfigs.begin(), locationSceneConfigs.end(),
			[&](const auto& item) { return hasResolvable(item.second.entries, SceneType::Location); });

	activeEntryCacheDirty = false;
	return hasActiveSceneEntries;
}

SceneSettingsManager::ResolvedSettingMap SceneSettingsManager::BuildResolvedSettings()
{
	ResolvedSettingMap resolved;
	const bool interior = Util::IsInterior();

	const auto ensureBaselines = [&](const std::vector<SettingEntry>& sourceEntries, SceneType type) {
		for (const auto& entry : sourceEntries) {
			if (!IsResolvableEntry(entry, type))
				continue;
			if (const auto address = GetEntryAddress(entry); !baselineSettings.contains(address))
				GetBaselineValue(address);
		}
	};

	if (interior) {
		ensureBaselines(GetEntries(SceneType::InteriorOnly), SceneType::InteriorOnly);
	} else {
		ensureBaselines(GetEntries(SceneType::TimeOfDay), SceneType::TimeOfDay);
		const auto weather = GetWeatherBlend();
		for (auto weatherId : { weather.currentWeatherId, weather.previousWeatherId }) {
			auto it = weatherSceneConfigs.find(weatherId);
			if (it != weatherSceneConfigs.end())
				ensureBaselines(it->second.entries, SceneType::TimeOfDay);
		}
	}

	const auto locationTargets = GetCurrentLocationTargets();
	for (const auto& target : locationTargets) {
		auto it = locationSceneConfigs.find(GetLocationConfigKey(target.type, target.formKey));
		if (it != locationSceneConfigs.end())
			ensureBaselines(it->second.entries, SceneType::Location);
	}

	if (interior) {
		ResolveInteriorSettings(resolved);
	} else {
		ResolveTimeOfDaySettings(resolved);
		ResolveWeatherSettings(resolved);
	}
	ResolveLocationSettings(resolved, locationTargets);
	return resolved;
}

void SceneSettingsManager::ApplyResolvedSettings(const ResolvedSettingMap& resolved, bool forceRetry)
{
	struct PendingUpdate
	{
		SettingAddress address;
		json value;
		bool restore = false;
	};

	std::map<std::string, std::vector<PendingUpdate>> pendingByFeature;
	for (const auto& [address, _] : appliedSettings) {
		if (resolved.contains(address))
			continue;
		auto baselineIt = baselineSettings.find(address);
		if (baselineIt != baselineSettings.end())
			pendingByFeature[address.featureShortName].push_back({ address, baselineIt->second, true });
	}

	for (const auto& [address, value] : resolved) {
		auto appliedIt = appliedSettings.find(address);
		if (appliedIt != appliedSettings.end() && ResolvedValuesEqual(appliedIt->second, value))
			continue;
		pendingByFeature[address.featureShortName].push_back({ address, value, false });
	}
	std::erase_if(applyFailures, [&](const auto& item) { return !pendingByFeature.contains(item.first); });

	// Warn once per distinct failure signature, then back off, so a stuck feature cannot spam the log.
	const auto recordApplyFailure = [&](const std::string& featureShortName, const json& signature,
									std::chrono::steady_clock::time_point now, std::string_view message) {
		auto& failure = applyFailures[featureShortName];
		if (failure.signature != signature) {
			failure.signature = signature;
			failure.warningLogged = false;
		}
		if (!failure.warningLogged) {
			logger::warn("[SceneSettings] {}", message);
			failure.warningLogged = true;
		}
		failure.retryAfter = now + kApplyRetryDelay;
	};

	for (const auto& [featureShortName, pending] : pendingByFeature) {
		json signature = json::array();
		for (const auto& update : pending) {
			signature.push_back({
				{ "path", update.address.settingPath },
				{ "setting", update.address.settingKey },
				{ "restore", update.restore },
			});
		}

		auto failureIt = applyFailures.find(featureShortName);
		if (failureIt != applyFailures.end() && failureIt->second.signature != signature) {
			applyFailures.erase(failureIt);
			failureIt = applyFailures.end();
		}
		const auto now = std::chrono::steady_clock::now();
		if (!forceRetry && failureIt != applyFailures.end() && now < failureIt->second.retryAfter)
			continue;

		auto* feature = Feature::FindFeatureByShortName(featureShortName);
		if (!feature) {
			recordApplyFailure(featureShortName, signature, now,
				std::format("Cannot apply resolved settings, feature {} is not loaded", featureShortName));
			continue;
		}

		std::vector<CatalogSceneSettingUpdate> updates;
		updates.reserve(pending.size());
		for (const auto& update : pending)
			updates.push_back({ update.address.settingPath, update.address.settingKey, update.value });
		if (!ApplyCatalogSceneSettings(*feature, updates)) {
			recordApplyFailure(featureShortName, signature, now,
				std::format("Failed to apply resolved settings for {}", featureShortName));
			continue;
		}
		applyFailures.erase(featureShortName);
		restoreFailureWarnings.erase(featureShortName);

		for (const auto& update : pending) {
			if (update.restore) {
				appliedSettings.erase(update.address);
				baselineSettings.erase(update.address);
			} else {
				appliedSettings[update.address] = update.value;
			}
		}
	}
}

void SceneSettingsManager::RestoreAppliedSettings()
{
	struct PendingRestore
	{
		SettingAddress address;
		CatalogSceneSettingUpdate update;
	};

	std::map<std::string, std::vector<PendingRestore>> updatesByFeature;
	for (const auto& [address, _] : appliedSettings) {
		auto baselineIt = baselineSettings.find(address);
		if (baselineIt != baselineSettings.end())
			updatesByFeature[address.featureShortName].push_back({
				address, { address.settingPath, address.settingKey, baselineIt->second } });
	}

	for (const auto& [featureShortName, pending] : updatesByFeature) {
		const auto now = std::chrono::steady_clock::now();
		if (auto retryIt = restoreRetryAfter.find(featureShortName);
			retryIt != restoreRetryAfter.end() && now < retryIt->second)
			continue;
		auto* feature = Feature::FindFeatureByShortName(featureShortName);
		if (!feature) {
			if (restoreFailureWarnings.insert(featureShortName).second)
				logger::warn("[SceneSettings] Cannot restore {}, feature is not loaded", featureShortName);
			restoreRetryAfter[featureShortName] = now + kApplyRetryDelay;
			continue;
		}

		std::vector<CatalogSceneSettingUpdate> updates;
		updates.reserve(pending.size());
		for (const auto& item : pending)
			updates.push_back(item.update);
		if (!ApplyCatalogSceneSettings(*feature, updates)) {
			if (restoreFailureWarnings.insert(featureShortName).second)
				logger::warn("[SceneSettings] Failed to restore base settings for {}", featureShortName);
			restoreRetryAfter[featureShortName] = now + kApplyRetryDelay;
			continue;
		}
		restoreFailureWarnings.erase(featureShortName);
		restoreRetryAfter.erase(featureShortName);

		for (const auto& item : pending) {
			appliedSettings.erase(item.address);
			baselineSettings.erase(item.address);
		}
	}

	if (appliedSettings.empty()) {
		baselineSettings.clear();
		restoreFailureWarnings.clear();
		restoreRetryAfter.clear();
	} else {
		resolverDirty = true;
	}
}

void SceneSettingsManager::ResolveInteriorSettings(ResolvedSettingMap& resolved) const
{
	OverlayAllEntries(resolved, GetEntries(SceneType::InteriorOnly), SceneType::InteriorOnly);
}

void SceneSettingsManager::ResolveTimeOfDaySettings(ResolvedSettingMap& resolved) const
{
	std::set<SettingAddress> addresses;
	for (const auto& entry : GetEntries(SceneType::TimeOfDay))
		if (IsResolvableEntry(entry, SceneType::TimeOfDay))
			addresses.insert(GetEntryAddress(entry));

	for (const auto& address : addresses) {
		auto baselineIt = baselineSettings.find(address);
		if (baselineIt == baselineSettings.end() || !IsNumericValue(baselineIt->second))
			continue;
		resolved[address] = ResolveTimeOfDayFloat(address, baselineIt->second.get<float>());
	}
}

void SceneSettingsManager::ResolveWeatherSettings(ResolvedSettingMap& resolved) const
{
	const auto weather = GetWeatherBlend();
	if (weather.currentWeatherId == 0)
		return;

	std::set<SettingAddress> addresses;
	for (auto weatherId : { weather.currentWeatherId, weather.previousWeatherId }) {
		auto it = weatherSceneConfigs.find(weatherId);
		if (it == weatherSceneConfigs.end())
			continue;
		for (const auto& entry : it->second.entries)
			if (IsResolvableEntry(entry, SceneType::TimeOfDay))
				addresses.insert(GetEntryAddress(entry));
	}

	for (const auto& address : addresses) {
		auto baselineIt = baselineSettings.find(address);
		if (baselineIt == baselineSettings.end() || !IsNumericValue(baselineIt->second))
			continue;
		float lowerValue = baselineIt->second.get<float>();
		if (auto resolvedIt = resolved.find(address); resolvedIt != resolved.end() && IsNumericValue(resolvedIt->second))
			lowerValue = resolvedIt->second.get<float>();
		if (auto value = ResolveWeatherFloat(address, lowerValue))
			resolved[address] = *value;
	}
}

void SceneSettingsManager::ResolveLocationSettings(
	ResolvedSettingMap& resolved, const std::vector<LocationTarget>& locationTargets) const
{
	for (const auto& target : locationTargets) {
		auto it = locationSceneConfigs.find(GetLocationConfigKey(target.type, target.formKey));
		if (it == locationSceneConfigs.end())
			continue;
		OverlayAllEntries(resolved, it->second.entries, SceneType::Location);
	}
}

void SceneSettingsManager::OverlayEntries(ResolvedSettingMap& resolved, const std::vector<SettingEntry>& sourceEntries,
	SceneType type, EntrySource source) const
{
	for (const auto& entry : sourceEntries) {
		if (entry.source != source || !IsEntryActive(entry) ||
			!IsFeatureAllowedForType(type, entry.featureShortName) ||
			!FindAllowedCatalogSetting(entry.featureShortName, entry.settingPath, entry.settingKey))
			continue;
		auto address = GetEntryAddress(entry);
		if (baselineSettings.contains(address))
			resolved[std::move(address)] = entry.value;
	}
}

void SceneSettingsManager::OverlayAllEntries(ResolvedSettingMap& resolved,
	const std::vector<SettingEntry>& sourceEntries, SceneType type) const
{
	// Shipped overwrites are the layer's defaults; the user's own entry for the same address wins.
	OverlayEntries(resolved, sourceEntries, type, EntrySource::Overwrite);
	OverlayEntries(resolved, sourceEntries, type, EntrySource::User);
}

std::array<std::optional<float>, SceneSettingsManager::kPeriodCount> SceneSettingsManager::CollectPeriodValues(
	const std::vector<SettingEntry>& sourceEntries, const SettingAddress& address) const
{
	std::array<std::optional<float>, kPeriodCount> values{};
	for (auto source : { EntrySource::Overwrite, EntrySource::User }) {
		for (const auto& entry : sourceEntries) {
			if (entry.source != source || !IsEntryActive(entry) || !IsNumericValue(entry.value) ||
				entry.period == TimeOfDayPeriod::Count ||
				!IsSameSetting(entry, address.featureShortName, address.settingPath, address.settingKey))
				continue;
			const auto periodIndex = static_cast<int>(entry.period);
			const auto value = entry.value.get<float>();
			if (periodIndex >= 0 && periodIndex < kPeriodCount && std::isfinite(value))
				values[periodIndex] = value;
		}
	}
	return values;
}

float SceneSettingsManager::ResolveTimeOfDayFloat(const SettingAddress& address, float baseValue) const
{
	const auto values = CollectPeriodValues(GetEntries(SceneType::TimeOfDay), address);
	const auto factors = GetTimeOfDayFactors();
	float result = 0.0f;
	for (int periodIndex = 0; periodIndex < kPeriodCount; ++periodIndex)
		result += factors[periodIndex] * values[periodIndex].value_or(baseValue);
	return result;
}

std::optional<float> SceneSettingsManager::ResolveWeatherFloat(const SettingAddress& address, float baseValue) const
{
	const auto weather = GetWeatherBlend();
	if (weather.currentWeatherId == 0)
		return std::nullopt;

	const auto factors = GetTimeOfDayFactors();
	auto baselineIt = baselineSettings.find(address);
	const float baselineValue = baselineIt != baselineSettings.end() && IsNumericValue(baselineIt->second) ?
	                                baselineIt->second.get<float>() : baseValue;

	const auto resolveWeather = [&](RE::FormID weatherId) -> std::optional<float> {
		auto configIt = weatherSceneConfigs.find(weatherId);
		if (configIt == weatherSceneConfigs.end())
			return std::nullopt;

		const auto values = CollectPeriodValues(configIt->second.entries, address);
		if (std::none_of(values.begin(), values.end(), [](const auto& value) { return value.has_value(); }))
			return std::nullopt;

		float result = 0.0f;
		for (int periodIndex = 0; periodIndex < kPeriodCount; ++periodIndex) {
			const auto lowerPeriodValue = GetTimeOfDayPeriodFallbackFloat(baselineValue,
				address.featureShortName, address.settingPath, address.settingKey, periodIndex);
			result += factors[periodIndex] * values[periodIndex].value_or(lowerPeriodValue);
		}
		return result;
	};

	const auto currentValue = resolveWeather(weather.currentWeatherId);
	const auto previousValue = weather.previousWeatherId != 0 ? resolveWeather(weather.previousWeatherId) : std::nullopt;
	if (!currentValue && !previousValue)
		return std::nullopt;

	const auto from = previousValue.value_or(baseValue);
	const auto to = currentValue.value_or(baseValue);
	return from + (to - from) * weather.lerp;
}

json SceneSettingsManager::GetBaselineValue(const SettingAddress& address)
{
	auto* setting = FindAllowedCatalogSetting(
		address.featureShortName, address.settingPath, address.settingKey);
	if (!setting)
		return {};
	if (auto it = baselineSettings.find(address); it != baselineSettings.end())
		return it->second;

	auto* feature = Feature::FindFeatureByShortName(address.featureShortName);
	json value;
	if (!feature || !GetCatalogSettingValue(*feature, *setting, value))
		return {};
	baselineSettings[address] = value;
	return value;
}

bool SceneSettingsManager::ResolvedValuesEqual(const json& lhs, const json& rhs)
{
	if (lhs.is_number() && rhs.is_number())
		return std::abs(lhs.get<double>() - rhs.get<double>()) < kBlendEpsilon;
	return lhs == rhs;
}

// --- Unified Persistence ---

static json EntryToJson(const SceneSettingsManager::SettingEntry& entry)
{
	json item = entry.serializedTemplate.is_object() ? entry.serializedTemplate : json::object();
	item["feature"] = entry.featureShortName;
	if (!entry.settingPath.empty())
		item["path"] = entry.settingPath;
	else
		item.erase("path");
	item["setting"] = entry.settingKey;
	item["value"] = entry.value;
	item["originalValue"] = entry.originalValue;
	item["paused"] = entry.paused;
	if (entry.period != SceneSettingsManager::TimeOfDayPeriod::Count)
		item["period"] = SceneSettingsManager::GetPeriodName(entry.period);
	else
		item.erase("period");
	return item;
}

static json UserEntriesToArray(const std::vector<SceneSettingsManager::SettingEntry>& entries, bool transitionOnly = false)
{
	json arr = json::array();
	for (const auto& entry : entries)
		if (entry.source == SceneSettingsManager::EntrySource::User &&
			(!transitionOnly || IsNumericValue(entry.value)))
			arr.push_back(EntryToJson(entry));
	return arr;
}

static void AppendRawEntries(json& arr, const std::vector<json>& rawEntries)
{
	if (!arr.is_array())
		arr = json::array();
	for (const auto& raw : rawEntries)
		arr.push_back(raw);
}

static bool ShouldSerializeUserSection(const json& data, std::string_view key, bool expectObject, bool modified)
{
	auto it = data.find(std::string(key));
	return modified || it == data.end() || (expectObject ? it->is_object() : it->is_array());
}

void SceneSettingsManager::SaveAllUserSettings()
{
	if (!userSettingsDocumentLoaded)
		LoadAllUserSettings();
	const bool weatherLoaded = TryEnsureWeatherDataLoaded();
	const bool locationLoaded = TryEnsureLocationDataLoaded();
	if (!userSettingsDocumentWritable || !preservedUserSettingsRoot.is_object()) {
		if (!userSettingsWriteBlockedWarning) {
			logger::error("[SceneSettings] Refusing to overwrite SceneManager.json because its existing document is invalid");
			userSettingsWriteBlockedWarning = true;
		}
		deferredSceneChangesPending = true;
		deferredSceneChangesDeadline = std::chrono::steady_clock::now() + kDeferredSaveRetryDelay;
		return;
	}

	auto path = GetUserSettingsFilePath();
	json data = preservedUserSettingsRoot;
	if (ShouldSerializeUserSection(data, "interiorOnly", false, interiorUserSettingsModified)) {
		data["interiorOnly"] = UserEntriesToArray(GetEntries(SceneType::InteriorOnly));
		AppendRawEntries(data["interiorOnly"], unresolvedUserEntries[SceneType::InteriorOnly]);
	}
	if (ShouldSerializeUserSection(data, "timeOfDay", false, timeOfDayUserSettingsModified)) {
		data["timeOfDay"] = UserEntriesToArray(GetEntries(SceneType::TimeOfDay), true);
		AppendRawEntries(data["timeOfDay"], unresolvedUserEntries[SceneType::TimeOfDay]);
	}

	// Weather entries (keyed by SPID)
	if (weatherLoaded && ShouldSerializeUserSection(data, "weather", true, weatherUserSettingsModified)) {
		json weatherObj = unresolvedWeatherUserSettings.is_object() ?
		                      unresolvedWeatherUserSettings : json::object();
		std::set<RE::FormID> weatherIds;
		for (const auto& [weatherId, _] : weatherSceneConfigs)
			weatherIds.insert(weatherId);
		for (const auto& [weatherId, _] : weatherShowTimeOfDay)
			weatherIds.insert(weatherId);

		for (auto weatherId : weatherIds) {
			if (weatherId == 0)
				continue;
			const auto spid = Util::FormIdToSpid(weatherId);
			auto configIt = weatherSceneConfigs.find(weatherId);
			auto userEntries = configIt != weatherSceneConfigs.end() ?
			                       UserEntriesToArray(configIt->second.entries, true) : json::array();
			auto showIt = weatherShowTimeOfDay.find(weatherId);
			const bool hasShowPreference = showIt != weatherShowTimeOfDay.end();

			auto rawIt = weatherObj.find(spid);
			const bool hasRaw = rawIt != weatherObj.end();
			if (userEntries.empty() && !hasShowPreference && !hasRaw)
				continue;
			if (hasRaw && !rawIt->is_object()) {
				if (userEntries.empty() && !hasShowPreference)
					continue;
				*rawIt = json::object();
			}

			json weatherEntry = hasRaw ? *rawIt : json::object();
			if (!userEntries.empty()) {
				if (auto entriesIt = weatherEntry.find("entries");
					entriesIt != weatherEntry.end() && entriesIt->is_array())
					for (const auto& rawEntry : *entriesIt)
						userEntries.push_back(rawEntry);
				weatherEntry["entries"] = std::move(userEntries);
			}
			if (hasShowPreference)
				weatherEntry["showTimeOfDay"] = showIt->second;
			weatherObj[spid] = std::move(weatherEntry);
		}
		data["weather"] = std::move(weatherObj);
	}

	if (locationLoaded && ShouldSerializeUserSection(data, "location", true, locationUserSettingsModified)) {
		json locationObj = unresolvedLocationUserSettings.is_object() ?
		                       unresolvedLocationUserSettings : json::object();
		for (const auto& [_, config] : locationSceneConfigs) {
			auto userEntries = UserEntriesToArray(config.entries);
			if (userEntries.empty())
				continue;
			const auto* sectionName = config.type == LocationTargetType::Cell ? "cells" : "locations";
			auto& section = locationObj[sectionName];
			if (!section.is_object())
				section = json::object();
			auto& rawConfig = section[config.formKey];
			json locationEntry = rawConfig.is_object() ? rawConfig : json::object();
			if (auto entriesIt = locationEntry.find("entries");
				entriesIt != locationEntry.end() && entriesIt->is_array())
				for (const auto& rawEntry : *entriesIt)
					userEntries.push_back(rawEntry);
			locationEntry["type"] = config.type == LocationTargetType::Cell ? "Cell" : "Location";
			locationEntry["name"] = config.name;
			locationEntry["coc"] = config.cocCode;
			locationEntry["entries"] = std::move(userEntries);
			rawConfig = std::move(locationEntry);
		}
		data["location"] = std::move(locationObj);
	}

	const bool saved = WriteJsonAtomically(path, data, kOverwriteJsonIndent, "SceneManager.json");
	if (saved) {
		preservedUserSettingsRoot = data;
		interiorUserSettingsModified = false;
		timeOfDayUserSettingsModified = false;
		weatherUserSettingsModified = false;
		locationUserSettingsModified = false;
		userSettingsWriteBlockedWarning = false;
		logger::info("[SceneSettings] Saved SceneManager.json");
	}

	deferredSceneChangesPending = !saved;
	if (!saved)
		deferredSceneChangesDeadline = std::chrono::steady_clock::now() + kDeferredSaveRetryDelay;
}

static bool LoadEntryFromJson(const nlohmann::json& item, SceneSettingsManager::SettingEntry& entry,
	bool requirePeriod, const char* typeName,
	std::optional<SceneSettingsManager::SceneType> allowedSceneType = std::nullopt,
	bool requireNumericValue = false, FeatureSettingsCache* featureSettingsCache = nullptr)
{
	using SSM = SceneSettingsManager;

	if (!item.contains("feature") || !item.contains("setting") || !item.contains("value")) {
		logger::warn("[SceneSettings] {} entry missing feature/setting/value fields", typeName);
		return false;
	}
	if (!item["feature"].is_string() || !item["setting"].is_string()) {
		logger::warn("[SceneSettings] {} entry feature/setting not strings", typeName);
		return false;
	}

	entry.featureShortName = item["feature"].get<std::string>();
	entry.settingPath.clear();
	if (auto it = item.find("path"); it != item.end()) {
		if (!it->is_array()) {
			logger::warn("[SceneSettings] {} entry path is not an array", typeName);
			return false;
		}
		for (const auto& part : *it) {
			if (!part.is_string()) {
				logger::warn("[SceneSettings] {} entry path contains a non-string component", typeName);
				return false;
			}
			entry.settingPath.push_back(part.get<std::string>());
		}
	}
	entry.settingKey = item["setting"].get<std::string>();
	entry.value = item["value"];
	entry.originalValue = item.value("originalValue", entry.value);
	entry.serializedTemplate = item.is_object() ? item : json::object();
	if (auto pausedIt = item.find("paused"); pausedIt != item.end() && !pausedIt->is_boolean()) {
		logger::warn("[SceneSettings] {} entry paused field is not boolean", typeName);
		return false;
	}
	entry.paused = item.value("paused", false);
	entry.source = SSM::EntrySource::User;

	auto sceneType = allowedSceneType.value_or(requirePeriod ? SSM::SceneType::TimeOfDay : SSM::SceneType::InteriorOnly);
	if (!SSM::IsFeatureAllowedForType(sceneType, entry.featureShortName)) {
		logger::warn("[SceneSettings] {} entry feature '{}' is not allowed for this scene type", typeName, entry.featureShortName);
		return false;
	}

	if (requirePeriod) {
		if (!item.contains("period") || !item["period"].is_string()) {
			logger::warn("[SceneSettings] {} entry {}.{} missing period - skipping", typeName, entry.featureShortName, entry.settingKey);
			return false;
		}
		entry.period = SSM::GetPeriodFromName(item["period"].get<std::string>());
		if (entry.period == SSM::TimeOfDayPeriod::Count) {
			logger::warn("[SceneSettings] {} entry {}.{} has invalid period '{}' - skipping", typeName, entry.featureShortName, entry.settingKey, item["period"].get<std::string>());
			return false;
		}
	}

	// Per-period entries always blend as floats, so they carry the same requirement as float-only scenes.
	const bool requireNumeric = requirePeriod || requireNumericValue;
	if (requireNumeric && (!IsNumericValue(entry.value) || !IsNumericValue(entry.originalValue) ||
		!std::isfinite(entry.value.get<float>()))) {
		logger::warn("[SceneSettings] {} entry {} is not a finite float setting - skipping",
			typeName, GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey));
		return false;
	}

	if (!ValidateSceneSettingEntry(typeName, entry.featureShortName, entry.settingPath, entry.settingKey,
			entry.value, requireNumeric, featureSettingsCache) ||
		!ValidateSceneSettingEntry(typeName, entry.featureShortName, entry.settingPath, entry.settingKey,
			entry.originalValue, requireNumeric, featureSettingsCache))
		return false;

	entry.displayName = GetSceneSettingDisplayName(entry.featureShortName, entry.settingPath, entry.settingKey);
	return true;
}

void SceneSettingsManager::LoadAllUserSettings()
{
	auto path = GetUserSettingsFilePath();
	logger::info("[SceneSettings] Loading user settings from: {}", path.string());
	for (auto type : { SceneType::InteriorOnly, SceneType::TimeOfDay })
		std::erase_if(entries[type], [](const SettingEntry& entry) { return entry.source == EntrySource::User; });
	unresolvedUserEntries[SceneType::InteriorOnly].clear();
	unresolvedUserEntries[SceneType::TimeOfDay].clear();
	BumpEntryPresentationRevision();
	interiorUserSettingsModified = false;
	timeOfDayUserSettingsModified = false;
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) {
		userSettingsDocumentLoaded = true;
		userSettingsDocumentWritable = !ec;
		preservedUserSettingsRoot = json::object();
		if (ec)
			logger::error("[SceneSettings] Could not inspect SceneManager.json: {}", ec.message());
		else
			logger::info("[SceneSettings] SceneManager.json not found at {}", path.string());
		return;
	}

	try {
		std::ifstream file(path);
		if (!file.is_open()) {
			userSettingsDocumentLoaded = true;
			userSettingsDocumentWritable = false;
			logger::error("[SceneSettings] Could not open SceneManager.json for reading");
			return;
		}

		json data = json::parse(file, nullptr, false);
		userSettingsDocumentLoaded = true;
		preservedUserSettingsRoot = data;
		if (!data.is_object()) {
			userSettingsDocumentWritable = false;
			logger::error("[SceneSettings] SceneManager.json must contain a valid JSON object; automatic saves are blocked");
			return;
		}
		userSettingsDocumentWritable = true;
		FeatureSettingsCache featureSettingsCache;

		// Interior Only
		if (data.contains("interiorOnly") && data["interiorOnly"].is_array()) {
			auto& vec = GetEntriesMut(SceneType::InteriorOnly);
			int loaded = 0;
			for (const auto& item : data["interiorOnly"]) {
				SettingEntry entry;
				if (!LoadEntryFromJson(item, entry, false, "InteriorOnly", std::nullopt, false,
						&featureSettingsCache)) {
					unresolvedUserEntries[SceneType::InteriorOnly].push_back(item);
					continue;
				}
				if (HasDuplicateEntry(SceneType::InteriorOnly, entry.featureShortName, entry.settingPath,
						entry.settingKey, EntrySource::User, entry.period)) {
					unresolvedUserEntries[SceneType::InteriorOnly].push_back(item);
					continue;
				}
				vec.push_back(std::move(entry));
				loaded++;
			}
			if (loaded > 0)
				logger::info("[SceneSettings] Loaded {} InteriorOnly user settings", loaded);
		} else if (data.contains("interiorOnly"))
			logger::warn("[SceneSettings] Preserving non-array interiorOnly section");

		// Time of Day
		if (data.contains("timeOfDay") && data["timeOfDay"].is_array()) {
			auto& vec = GetEntriesMut(SceneType::TimeOfDay);
			int loaded = 0;
			for (const auto& item : data["timeOfDay"]) {
				SettingEntry entry;
				if (!LoadEntryFromJson(item, entry, true, "TimeOfDay", std::nullopt, false,
						&featureSettingsCache)) {
					unresolvedUserEntries[SceneType::TimeOfDay].push_back(item);
					continue;
				}
				if (HasDuplicateEntry(SceneType::TimeOfDay, entry.featureShortName, entry.settingPath,
						entry.settingKey, EntrySource::User, entry.period)) {
					unresolvedUserEntries[SceneType::TimeOfDay].push_back(item);
					continue;
				}
				vec.push_back(std::move(entry));
				loaded++;
			}
			if (loaded > 0)
				logger::info("[SceneSettings] Loaded {} TimeOfDay user settings", loaded);
		} else if (data.contains("timeOfDay"))
			logger::warn("[SceneSettings] Preserving non-array timeOfDay section");

		// Weather and location are loaded lazily once game data is available.

		logger::info("[SceneSettings] Loaded SceneManager.json (non-weather)");
	} catch (const std::exception& e) {
		userSettingsDocumentLoaded = true;
		userSettingsDocumentWritable = false;
		logger::error("[SceneSettings] Failed to load SceneManager.json: {}", e.what());
	}
}

void SceneSettingsManager::LoadLocationUserSettings(const json& data)
{
	for (auto& [_, config] : locationSceneConfigs)
		std::erase_if(config.entries, [](const SettingEntry& entry) { return entry.source == EntrySource::User; });
	unresolvedLocationUserSettings = json::object();
	locationUserSettingsModified = false;
	auto locationIt = data.find("location");
	if (locationIt == data.end())
		return;
	if (!locationIt->is_object()) {
		logger::warn("[SceneSettings] Preserving non-object location section");
		return;
	}
	unresolvedLocationUserSettings = *locationIt;
	FeatureSettingsCache featureSettingsCache;

	const auto loadSection = [&](const char* sectionName, LocationTargetType type) {
		auto sectionIt = locationIt->find(sectionName);
		if (sectionIt == locationIt->end() || !sectionIt->is_object())
			return;
		json preservedSection = json::object();

		for (const auto& [formKey, rawConfig] : sectionIt->items()) {
			if (formKey.empty()) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			const auto canonicalFormKey = CanonicalizeResolvedLocationFormKey(formKey);
			if (!rawConfig.is_object()) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			const auto configContext = std::format("Location config '{}'", formKey);
			std::string name;
			std::string persistedType;
			std::string cocCode;
			const auto expectedType = type == LocationTargetType::Cell ? "Cell" : "Location";
			persistedType = expectedType;
			if (!ReadOptionalStringField(rawConfig, "name", name, configContext) ||
				!ReadOptionalStringField(rawConfig, "type", persistedType, configContext) ||
				!ReadOptionalStringField(rawConfig, "coc", cocCode, configContext)) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			if (persistedType != expectedType) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			auto& config = GetLocationConfigMut(type, canonicalFormKey, name);
			if (!cocCode.empty())
				config.cocCode = cocCode;
			auto entriesIt = rawConfig.find("entries");
			if (entriesIt == rawConfig.end()) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			if (!entriesIt->is_array()) {
				preservedSection[formKey] = rawConfig;
				continue;
			}
			auto preservedConfig = rawConfig;
			preservedConfig["entries"] = json::array();
			bool hasValidEntry = false;

			for (const auto& item : *entriesIt) {
				SettingEntry entry;
				if (!LoadEntryFromJson(item, entry, false, "Location", SceneType::Location, false,
						&featureSettingsCache)) {
					preservedConfig["entries"].push_back(item);
					continue;
				}
				hasValidEntry = true;
				if (HasLocationEntry(type, canonicalFormKey, entry.featureShortName, entry.settingPath,
						entry.settingKey, EntrySource::User)) {
					preservedConfig["entries"].push_back(item);
					continue;
				}
				config.entries.push_back(std::move(entry));
			}
			if (hasValidEntry && formKey != canonicalFormKey) {
				preservedConfig.erase("type");
				preservedConfig.erase("name");
				preservedConfig.erase("coc");
			}
			preservedSection[formKey] = std::move(preservedConfig);
		}
		unresolvedLocationUserSettings[sectionName] = std::move(preservedSection);
	};

	loadSection("locations", LocationTargetType::Location);
	loadSection("cells", LocationTargetType::Cell);
}

void SceneSettingsManager::LoadWeatherUserSettings()
{
	for (auto& [_, config] : weatherSceneConfigs)
		std::erase_if(config.entries, [](const SettingEntry& entry) { return entry.source == EntrySource::User; });
	weatherShowTimeOfDay.clear();
	unresolvedWeatherUserSettings = json::object();
	weatherUserSettingsModified = false;
	if (!userSettingsDocumentLoaded || !userSettingsDocumentWritable || !preservedUserSettingsRoot.is_object())
		return;

	try {
		auto weatherIt = preservedUserSettingsRoot.find("weather");
		if (weatherIt == preservedUserSettingsRoot.end())
			return;
		if (!weatherIt->is_object()) {
			logger::warn("[SceneSettings] Preserving non-object weather section");
			return;
		}

		FeatureSettingsCache featureSettingsCache;
		logger::info("[SceneSettings] Weather section found with {} entries", weatherIt->size());
		for (const auto& [spidKey, weatherData] : weatherIt->items()) {
			RE::FormID weatherId = Util::SpidToFormId(spidKey);
			if (weatherId == 0) {
				unresolvedWeatherUserSettings[spidKey] = weatherData;
				logger::warn("[SceneSettings] Weather SPID '{}' could not be resolved - skipping", spidKey);
				continue;
			}
			if (!weatherData.is_object()) {
				unresolvedWeatherUserSettings[spidKey] = weatherData;
				logger::warn("[SceneSettings] Weather config '{}' is not an object - preserving", spidKey);
				continue;
			}
			auto preservedWeather = weatherData;

			if (auto showIt = weatherData.find("showTimeOfDay"); showIt != weatherData.end()) {
				if (!showIt->is_boolean()) {
					logger::warn("[SceneSettings] Weather config '{}' showTimeOfDay is not boolean - preserving", spidKey);
				} else {
					weatherShowTimeOfDay[weatherId] = showIt->get<bool>();
					preservedWeather.erase("showTimeOfDay");
				}
			}

			auto entriesIt = weatherData.find("entries");
			if (entriesIt == weatherData.end()) {
				unresolvedWeatherUserSettings[spidKey] = std::move(preservedWeather);
				continue;
			}
			if (!entriesIt->is_array()) {
				unresolvedWeatherUserSettings[spidKey] = preservedWeather;
				logger::warn("[SceneSettings] Weather config '{}' entries is not an array - preserving", spidKey);
				continue;
			}
			preservedWeather["entries"] = json::array();

			auto& config = GetWeatherConfigMut(weatherId);
			int loaded = 0;
			for (const auto& item : *entriesIt) {
				SettingEntry entry;
				if (!LoadEntryFromJson(item, entry, true, "Weather", std::nullopt, false,
						&featureSettingsCache)) {
					preservedWeather["entries"].push_back(item);
					continue;
				}
				if (HasWeatherEntryForPeriod(weatherId, entry.featureShortName, entry.settingPath,
						entry.settingKey, entry.period, EntrySource::User)) {
					preservedWeather["entries"].push_back(item);
					continue;
				}
				config.entries.push_back(std::move(entry));
				loaded++;
			}
			if (loaded > 0)
				logger::info("[SceneSettings] Loaded {} weather entries for {}", loaded, spidKey);
			unresolvedWeatherUserSettings[spidKey] = std::move(preservedWeather);
		}

		logger::info("[SceneSettings] Loaded weather user settings");
	} catch (const std::exception& e) {
		logger::error("[SceneSettings] Failed to load weather user settings: {}", e.what());
	}
}

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

static bool ParseOverwriteFileEntries(const std::filesystem::path& filePath,
	SceneSettingsManager::SceneType allowedType, bool requireNumeric,
	std::vector<SceneSettingsManager::SettingEntry>& outEntries, FeatureSettingsCache* featureSettingsCache)
{
	using SSM = SceneSettingsManager;

	json data;
	if (!ReadBoundedSceneJson(filePath, data))
		return false;

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
		if (!ValidateSceneSettingEntry("Overwrite", featureShortName, settingPath, key, value,
				requireNumeric, featureSettingsCache))
			return;

		SSM::SettingEntry entry;
		entry.featureShortName = featureShortName;
		entry.settingPath = settingPath;
		entry.settingKey = key;
		entry.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, key);
		entry.value = value;
		entry.originalValue = entry.value;
		entry.source = SSM::EntrySource::Overwrite;
		entry.sourceFilename = filePath.filename().string();
		entry.sourcePath = filePath;
		outEntries.push_back(std::move(entry));
		foundAny = true;
	});
	return foundAny;
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

void SceneSettingsManager::LoadAll()
{
	if (!dataLoaded) {
		dataLoaded = true;
		DiscoverOverwrites(SceneType::InteriorOnly);
		DiscoverOverwrites(SceneType::TimeOfDay);
		LoadAllUserSettings();
		BumpEntryPresentationRevision();
		activeEntryCacheDirty = true;
		resolverDirty = true;
	}
	TryEnsureLocationDataLoaded();
}

void SceneSettingsManager::OnDataLoaded()
{
	gameDataReady = true;
	if (dataLoaded)
		TryEnsureLocationDataLoaded();
}

bool SceneSettingsManager::TryEnsureLocationDataLoaded()
{
	if (locationDataLoaded)
		return true;
	if (!gameDataReady || !RE::TESDataHandler::GetSingleton())
		return false;
	if (!userSettingsDocumentLoaded)
		LoadAllUserSettings();

	try {
		DiscoverLocationOverwrites();
		if (userSettingsDocumentLoaded && userSettingsDocumentWritable && preservedUserSettingsRoot.is_object())
			LoadLocationUserSettings(preservedUserSettingsRoot);
		locationDataLoaded = true;
		locationTargetsCached = false;
		BumpEntryPresentationRevision();
		activeEntryCacheDirty = true;
		resolverDirty = true;
		return true;
	} catch (const std::exception& e) {
		logger::error("[SceneSettings] Failed to load location settings: {}", e.what());
		return false;
	}
}

bool SceneSettingsManager::TryEnsureWeatherDataLoaded()
{
	if (weatherDataLoaded)
		return true;
	if (!globals::game::sky || !RE::TESDataHandler::GetSingleton())
		return false;
	if (!userSettingsDocumentLoaded)
		LoadAllUserSettings();

	weatherDataLoaded = true;
	LoadWeatherData();
	BumpEntryPresentationRevision();
	activeEntryCacheDirty = true;
	resolverDirty = true;
	return true;
}

void SceneSettingsManager::LoadWeatherData()
{
	DiscoverWeatherOverwrites();
	LoadWeatherUserSettings();
}

RE::FormID SceneSettingsManager::GetEffectivePreviousWeatherId(const RE::Sky* sky, float weatherLerp) const
{
	if (!sky)
		return 0;
	if (weatherLerp >= 1.0f) {
		if (sky->currentWeather)
			cachedPreviousWeatherId = sky->currentWeather->GetFormID();
		return 0;
	}
	if (sky->lastWeather)
		cachedPreviousWeatherId = sky->lastWeather->GetFormID();
	return cachedPreviousWeatherId;
}

// --- Per-Weather Scene Settings ---

const SceneSettingsManager::WeatherSceneConfig SceneSettingsManager::kEmptyWeatherConfig{};

const SceneSettingsManager::WeatherSceneConfig& SceneSettingsManager::GetWeatherConfig(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return kEmptyWeatherConfig;

	auto it = weatherSceneConfigs.find(weatherId);
	return (it != weatherSceneConfigs.end()) ? it->second : kEmptyWeatherConfig;
}

SceneSettingsManager::WeatherSceneConfig& SceneSettingsManager::GetWeatherConfigMut(RE::FormID weatherId)
{
	return weatherSceneConfigs[weatherId];
}

bool SceneSettingsManager::HasWeatherConfig(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;

	auto it = weatherSceneConfigs.find(weatherId);
	return it != weatherSceneConfigs.end() && std::any_of(it->second.entries.begin(), it->second.entries.end(),
		[](const auto& entry) { return IsNumericValue(entry.value); });
}

void SceneSettingsManager::PrepareWeatherUserSettingsMutation(RE::FormID weatherId, bool replaceMalformedEntries)
{
	weatherUserSettingsModified = true;
	if (!unresolvedWeatherUserSettings.is_object())
		unresolvedWeatherUserSettings = json::object();
	const auto canonicalSpid = Util::FormIdToSpid(weatherId);
	const auto normalizedSpid = NormalizeLocationFormKey(canonicalSpid);
	if (replaceMalformedEntries) {
		for (auto& [rawSpid, rawWeather] : unresolvedWeatherUserSettings.items()) {
			if (!rawWeather.is_object() || NormalizeLocationFormKey(rawSpid) != normalizedSpid)
				continue;
			auto entriesIt = rawWeather.find("entries");
			if (entriesIt != rawWeather.end() && !entriesIt->is_array())
				*entriesIt = json::array();
		}
	}

	auto& rawWeather = unresolvedWeatherUserSettings[canonicalSpid];
	if (!rawWeather.is_object())
		rawWeather = json::object();
	if (replaceMalformedEntries) {
		auto entriesIt = rawWeather.find("entries");
		if (entriesIt != rawWeather.end() && !entriesIt->is_array())
			*entriesIt = json::array();
	}
}

std::optional<float> SceneSettingsManager::ResolveWeatherLowerValue(RE::FormID weatherId,
	const SettingAddress& address, TimeOfDayPeriod period, EntrySource selectedSource)
{
	const auto periodIndex = static_cast<int>(period);
	if (periodIndex < 0 || periodIndex >= kPeriodCount)
		return std::nullopt;
	auto baseline = GetBaselineValue(address);
	if (!IsNumericValue(baseline))
		return std::nullopt;
	const auto baselineValue = baseline.get<float>();
	if (!std::isfinite(baselineValue))
		return std::nullopt;

	float lowerValue = GetTimeOfDayPeriodFallbackFloat(baselineValue,
		address.featureShortName, address.settingPath, address.settingKey, periodIndex);
	// Only a user entry has anything of the weather layer beneath it.
	if (selectedSource != EntrySource::User)
		return lowerValue;

	auto configIt = weatherSceneConfigs.find(weatherId);
	if (configIt == weatherSceneConfigs.end())
		return lowerValue;
	for (const auto& entry : configIt->second.entries) {
		if (entry.source != EntrySource::Overwrite || entry.period != period || !IsEntryActive(entry) ||
			!IsNumericValue(entry.value) ||
			!IsSameSetting(entry, address.featureShortName, address.settingPath, address.settingKey))
			continue;
		const auto value = entry.value.get<float>();
		if (std::isfinite(value))
			lowerValue = value;
	}
	return lowerValue;
}

bool SceneSettingsManager::AddWeatherSetting(RE::FormID weatherId, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, TimeOfDayPeriod period,
	bool deferSave)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;
	if (!IsFeatureAllowedForType(SceneType::TimeOfDay, featureShortName))
		return false;

	// All weather entries are per-period
	if (period == TimeOfDayPeriod::Count || static_cast<int>(period) < 0 || static_cast<int>(period) >= kPeriodCount)
		return false;
	if (HasWeatherEntryForPeriod(weatherId, featureShortName, settingPath, settingKey, period, EntrySource::User))
		return false;
	SettingAddress address{ featureShortName, settingPath, settingKey };
	auto lowerValue = ResolveWeatherLowerValue(weatherId, address, period, EntrySource::User);
	if (!lowerValue || !ValidateSceneSettingEntry(
			"Weather", featureShortName, settingPath, settingKey, *lowerValue, true))
		return false;

	auto& config = GetWeatherConfigMut(weatherId);

	SettingEntry entry;
	entry.featureShortName = featureShortName;
	entry.settingPath = settingPath;
	entry.settingKey = settingKey;
	entry.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, settingKey);
	entry.value = *lowerValue;
	entry.originalValue = *lowerValue;
	entry.source = EntrySource::User;
	entry.period = period;
	config.entries.push_back(std::move(entry));
	BumpEntryPresentationRevision();
	PrepareWeatherUserSettingsMutation(weatherId, true);
	if (deferSave)
		MarkDeferredSceneChanges();
	else
		SaveAllUserSettings();
	ReapplyIfActive();
	return true;
}

void SceneSettingsManager::RemoveWeatherSetting(RE::FormID weatherId, size_t index)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end() || index >= it->second.entries.size())
		return;
	const auto previousSize = it->second.entries.size();
	const auto entry = it->second.entries[index];
	if (entry.source == EntrySource::Overwrite && !entry.sourceFilename.empty()) {
		const auto backingPath = GetWeatherOverwritePath(weatherId, entry);
		if (!RemoveSettingFromOverwriteFile(backingPath, entry.settingPath, entry.settingKey))
			return;
		std::erase_if(it->second.entries, [&](const auto& candidate) {
			return candidate.source == EntrySource::Overwrite &&
			       GetWeatherOverwritePath(weatherId, candidate) == backingPath &&
			       IsSameSetting(candidate, entry.featureShortName, entry.settingPath, entry.settingKey);
		});
	} else {
		it->second.entries.erase(it->second.entries.begin() + static_cast<ptrdiff_t>(index));
		PrepareWeatherUserSettingsMutation(weatherId, false);
		SaveAllUserSettings();
	}
	if (it->second.entries.size() != previousSize)
		BumpEntryPresentationRevision();
	ReapplyIfActive();
}

void SceneSettingsManager::DeleteAllWeatherUserSettings(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return;
	auto configIt = weatherSceneConfigs.find(weatherId);
	if (configIt != weatherSceneConfigs.end()) {
		const auto removed = std::erase_if(configIt->second.entries,
			[](const SettingEntry& entry) { return entry.source == EntrySource::User; });
		if (removed != 0)
			BumpEntryPresentationRevision();
	}
	PrepareWeatherUserSettingsMutation(weatherId, false);
	const auto normalizedSpid = NormalizeLocationFormKey(Util::FormIdToSpid(weatherId));
	for (auto& [rawSpid, rawWeather] : unresolvedWeatherUserSettings.items())
		if (rawWeather.is_object() && NormalizeLocationFormKey(rawSpid) == normalizedSpid)
			rawWeather.erase("entries");
	SaveAllUserSettings();
	ReapplyIfActive();
}

void SceneSettingsManager::TogglePauseWeatherEntry(RE::FormID weatherId, size_t index)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end() || index >= it->second.entries.size())
		return;
	it->second.entries[index].paused = !it->second.entries[index].paused;
	BumpEntryPresentationRevision();
	if (it->second.entries[index].source == EntrySource::User) {
		PrepareWeatherUserSettingsMutation(weatherId, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::UpdateWeatherEntryValue(RE::FormID weatherId, size_t index, const json& newValue, bool deferSave)
{
	const EntryValueUpdate update{ index, newValue };
	UpdateWeatherEntryValues(weatherId, std::span{ &update, 1 }, deferSave);
}

void SceneSettingsManager::UpdateWeatherEntryValues(
	RE::FormID weatherId, std::span<const EntryValueUpdate> updates, bool deferSave)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end())
		return;
	bool userEntriesChanged = false;
	if (!ApplyEntryValueUpdates(
			"Weather", it->second.entries, updates, true, userEntriesChanged))
		return;
	if (userEntriesChanged) {
		PrepareWeatherUserSettingsMutation(weatherId, false);
		if (deferSave)
			MarkDeferredSceneChanges();
		else
			SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::RevertWeatherEntryToDefault(RE::FormID weatherId, size_t index)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end() || index >= it->second.entries.size())
		return;
	auto& entry = it->second.entries[index];
	SettingAddress address{ entry.featureShortName, entry.settingPath, entry.settingKey };
	auto lowerValue = ResolveWeatherLowerValue(weatherId, address, entry.period, entry.source);
	if (!lowerValue || !ValidateSceneSettingEntry(
			"Weather", entry.featureShortName, entry.settingPath, entry.settingKey, *lowerValue, true))
		return;
	entry.value = *lowerValue;
	entry.originalValue = *lowerValue;
	if (entry.source == EntrySource::User) {
		PrepareWeatherUserSettingsMutation(weatherId, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

bool SceneSettingsManager::HasWeatherEntryForPeriod(RE::FormID weatherId, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, TimeOfDayPeriod period, std::optional<EntrySource> source)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end())
		return false;
	for (const auto& e : it->second.entries)
		if (IsSameSetting(e, featureShortName, settingPath, settingKey) && e.period == period &&
			(!source || e.source == *source))
			return true;
	return false;
}

// --- Per-Weather Persistence ---

bool SceneSettingsManager::IsWeatherShowTimeOfDay(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;

	auto it = weatherShowTimeOfDay.find(weatherId);
	return it != weatherShowTimeOfDay.end() && it->second;
}

void SceneSettingsManager::SetWeatherShowTimeOfDay(RE::FormID weatherId, bool show)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	weatherShowTimeOfDay[weatherId] = show;
	PrepareWeatherUserSettingsMutation(weatherId, false);
	SaveAllUserSettings();
}

// --- Per-Location Scene Settings ---

const SceneSettingsManager::LocationSceneConfig SceneSettingsManager::kEmptyLocationConfig{};

std::string SceneSettingsManager::GetLocationConfigKey(LocationTargetType type, std::string_view formKey)
{
	return std::format("{}:{}", type == LocationTargetType::Cell ? "Cell" : "Location",
		NormalizeLocationFormKey(formKey));
}

std::vector<SceneSettingsManager::LocationTarget> SceneSettingsManager::GetCurrentLocationTargets() const
{
	std::vector<LocationTarget> targets;
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* cell = player ? player->GetParentCell() : nullptr;
	if (!player || !cell) {
		cachedTargetLocationId = 0;
		cachedTargetCellId = 0;
		locationTargetsCached = false;
		cachedLocationTargets.clear();
		return targets;
	}

	auto* location = player->GetCurrentLocation();
	if (!location)
		location = cell->GetLocation();
	const auto locationId = location ? location->GetFormID() : 0;
	const auto cellId = cell->GetFormID();
	if (locationTargetsCached && cachedTargetLocationId == locationId && cachedTargetCellId == cellId)
		return cachedLocationTargets;

	const auto cocCode = Util::GetFormEditorID(cell);

	std::vector<RE::BGSLocation*> locationChain;
	std::set<RE::FormID> visited;
	for (auto* current = location; current && visited.insert(current->GetFormID()).second; current = current->parentLoc)
		locationChain.push_back(current);
	std::reverse(locationChain.begin(), locationChain.end());

	const auto getDisplayName = [](const auto* form) {
		if (const char* fullName = form->GetFullName(); fullName && fullName[0] != '\0')
			return std::string(fullName);
		return Util::GetFormDisplayName(form->GetFormID());
	};

	for (auto* current : locationChain) {
		targets.push_back({
			.type = LocationTargetType::Location,
			.formKey = Util::GetFormFileKey(current),
			.name = getDisplayName(current),
			.cocCode = cocCode,
			.formId = current->GetFormID(),
		});
	}

	targets.push_back({
		.type = LocationTargetType::Cell,
		.formKey = Util::GetFormFileKey(cell),
		.name = getDisplayName(cell),
		.cocCode = cocCode,
		.formId = cell->GetFormID(),
	});
	cachedTargetLocationId = locationId;
	cachedTargetCellId = cellId;
	locationTargetsCached = true;
	cachedLocationTargets = targets;
	return targets;
}

SceneSettingsManager::LocationSceneConfig& SceneSettingsManager::GetLocationConfigMut(
	LocationTargetType type, const std::string& formKey, const std::string& name)
{
	const auto canonicalFormKey = CanonicalizeResolvedLocationFormKey(formKey);
	auto& config = locationSceneConfigs[GetLocationConfigKey(type, canonicalFormKey)];
	config.type = type;
	config.formKey = canonicalFormKey;
	if (!name.empty())
		config.name = name;
	return config;
}

const SceneSettingsManager::LocationSceneConfig& SceneSettingsManager::GetLocationConfig(
	LocationTargetType type, std::string_view formKey) const
{
	auto it = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	return it != locationSceneConfigs.end() ? it->second : kEmptyLocationConfig;
}

bool SceneSettingsManager::HasLocationConfig(LocationTargetType type, std::string_view formKey) const
{
	const auto& config = GetLocationConfig(type, formKey);
	return std::any_of(config.entries.begin(), config.entries.end(), [&](const auto& entry) {
		return IsEntryActive(entry);
	});
}

std::optional<json> SceneSettingsManager::ResolveLocationLowerValue(LocationTargetType type,
	std::string_view formKey, const SettingAddress& address, EntrySource selectedSource)
{
	auto baseline = GetBaselineValue(address);
	if (!IsSceneSettingPrimitive(baseline))
		return std::nullopt;

	ResolvedSettingMap lowerLayers;
	if (Util::IsInterior()) {
		ResolveInteriorSettings(lowerLayers);
	} else {
		ResolveTimeOfDaySettings(lowerLayers);
		ResolveWeatherSettings(lowerLayers);
	}

	bool targetFound = false;
	const auto selectedTargetKey = GetLocationConfigKey(type, formKey);
	for (const auto& target : GetCurrentLocationTargets()) {
		if (GetLocationConfigKey(target.type, target.formKey) == selectedTargetKey) {
			targetFound = true;
			if (selectedSource == EntrySource::User) {
				auto configIt = locationSceneConfigs.find(GetLocationConfigKey(target.type, target.formKey));
				if (configIt != locationSceneConfigs.end())
					OverlayEntries(
						lowerLayers, configIt->second.entries, SceneType::Location, EntrySource::Overwrite);
			}
			break;
		}
		auto configIt = locationSceneConfigs.find(GetLocationConfigKey(target.type, target.formKey));
		if (configIt == locationSceneConfigs.end())
			continue;
		OverlayAllEntries(lowerLayers, configIt->second.entries, SceneType::Location);
	}
	if (!targetFound)
		return std::nullopt;

	if (auto valueIt = lowerLayers.find(address); valueIt != lowerLayers.end() &&
		IsSceneSettingPrimitive(valueIt->second))
		return valueIt->second;
	return baseline;
}

void SceneSettingsManager::PrepareLocationUserSettingsMutation(LocationTargetType type,
	std::string_view formKey, bool replaceMalformedEntries)
{
	locationUserSettingsModified = true;
	if (!unresolvedLocationUserSettings.is_object())
		unresolvedLocationUserSettings = json::object();
	const auto* sectionName = type == LocationTargetType::Cell ? "cells" : "locations";
	auto& section = unresolvedLocationUserSettings[sectionName];
	if (!section.is_object())
		section = json::object();
	const auto canonicalFormKey = CanonicalizeResolvedLocationFormKey(formKey);
	const auto targetKey = GetLocationConfigKey(type, canonicalFormKey);
	if (replaceMalformedEntries) {
		for (auto& [rawFormKey, rawConfig] : section.items()) {
			if (!rawConfig.is_object() ||
				GetLocationConfigKey(type, CanonicalizeResolvedLocationFormKey(rawFormKey)) != targetKey)
				continue;
			auto entriesIt = rawConfig.find("entries");
			if (entriesIt != rawConfig.end() && !entriesIt->is_array())
				*entriesIt = json::array();
			if (rawFormKey != canonicalFormKey) {
				rawConfig.erase("type");
				rawConfig.erase("name");
				rawConfig.erase("coc");
			}
		}
	}

	auto& rawConfig = section[canonicalFormKey];
	if (!rawConfig.is_object())
		rawConfig = json::object();
	if (replaceMalformedEntries) {
		auto entriesIt = rawConfig.find("entries");
		if (entriesIt != rawConfig.end() && !entriesIt->is_array())
			*entriesIt = json::array();
	}
}

bool SceneSettingsManager::AddLocationSetting(LocationTargetType type, const std::string& formKey,
	const std::string& name, const std::string& cocCode, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, bool deferSave)
{
	if (!TryEnsureLocationDataLoaded() || formKey.empty() ||
		!IsFeatureAllowedForType(SceneType::Location, featureShortName) ||
		HasLocationEntry(type, formKey, featureShortName, settingPath, settingKey, EntrySource::User))
		return false;
	SettingAddress address{ featureShortName, settingPath, settingKey };
	auto lowerValue = ResolveLocationLowerValue(type, formKey, address, EntrySource::User);
	if (!lowerValue || !ValidateSceneSettingEntry("Location", featureShortName, settingPath, settingKey, *lowerValue, false))
		return false;

	auto& config = GetLocationConfigMut(type, formKey, name);
	if (!cocCode.empty())
		config.cocCode = cocCode;
	config.entries.push_back({
		.featureShortName = featureShortName,
		.settingPath = settingPath,
		.settingKey = settingKey,
		.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, settingKey),
		.value = *lowerValue,
		.originalValue = *lowerValue,
		.source = EntrySource::User,
	});
	BumpEntryPresentationRevision();
	PrepareLocationUserSettingsMutation(type, formKey, true);
	if (deferSave)
		MarkDeferredSceneChanges();
	else
		CommitSceneSettingChanges();
	return true;
}

void SceneSettingsManager::RemoveLocationSetting(LocationTargetType type, const std::string& formKey, size_t index)
{
	auto it = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (it == locationSceneConfigs.end() || index >= it->second.entries.size())
		return;

	const auto entry = it->second.entries[index];
	const bool userEntry = entry.source == EntrySource::User;
	if (entry.source == EntrySource::Overwrite && !entry.sourceFilename.empty() &&
		!RemoveSettingFromOverwriteFile(GetLocationOverwritePath(formKey, entry), entry.settingPath, entry.settingKey))
		return;
	it->second.entries.erase(it->second.entries.begin() + static_cast<ptrdiff_t>(index));
	BumpEntryPresentationRevision();
	if (userEntry) {
		PrepareLocationUserSettingsMutation(type, formKey, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::DeleteAllLocationUserSettings(LocationTargetType type, const std::string& formKey)
{
	if (!TryEnsureLocationDataLoaded())
		return;
	auto configIt = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (configIt != locationSceneConfigs.end()) {
		const auto removed = std::erase_if(configIt->second.entries,
			[](const SettingEntry& entry) { return entry.source == EntrySource::User; });
		if (removed != 0)
			BumpEntryPresentationRevision();
	}
	PrepareLocationUserSettingsMutation(type, formKey, false);
	const auto* sectionName = type == LocationTargetType::Cell ? "cells" : "locations";
	auto sectionIt = unresolvedLocationUserSettings.find(sectionName);
	if (sectionIt != unresolvedLocationUserSettings.end() && sectionIt->is_object()) {
		const auto targetKey = GetLocationConfigKey(type, formKey);
		for (auto& [rawFormKey, rawConfig] : sectionIt->items())
			if (rawConfig.is_object() &&
				GetLocationConfigKey(type, CanonicalizeResolvedLocationFormKey(rawFormKey)) == targetKey)
				rawConfig.erase("entries");
	}
	SaveAllUserSettings();
	ReapplyIfActive();
}

void SceneSettingsManager::TogglePauseLocationEntry(LocationTargetType type, const std::string& formKey, size_t index)
{
	auto it = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (it == locationSceneConfigs.end() || index >= it->second.entries.size())
		return;
	it->second.entries[index].paused = !it->second.entries[index].paused;
	BumpEntryPresentationRevision();
	if (it->second.entries[index].source == EntrySource::User) {
		PrepareLocationUserSettingsMutation(type, formKey, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::UpdateLocationEntryValue(LocationTargetType type, const std::string& formKey,
	size_t index, const json& newValue, bool deferSave)
{
	const EntryValueUpdate update{ index, newValue };
	UpdateLocationEntryValues(type, formKey, std::span{ &update, 1 }, deferSave);
}

void SceneSettingsManager::UpdateLocationEntryValues(LocationTargetType type, const std::string& formKey,
	std::span<const EntryValueUpdate> updates, bool deferSave)
{
	auto it = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (it == locationSceneConfigs.end())
		return;
	bool userEntriesChanged = false;
	if (!ApplyEntryValueUpdates(
			"Location", it->second.entries, updates, false, userEntriesChanged))
		return;
	if (userEntriesChanged) {
		PrepareLocationUserSettingsMutation(type, formKey, false);
		if (deferSave)
			MarkDeferredSceneChanges();
		else
			SaveAllUserSettings();
	}
	ReapplyIfActive();
}

void SceneSettingsManager::RevertLocationEntryToDefault(LocationTargetType type, const std::string& formKey, size_t index)
{
	auto it = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (it == locationSceneConfigs.end() || index >= it->second.entries.size())
		return;
	auto& entry = it->second.entries[index];
	SettingAddress address{ entry.featureShortName, entry.settingPath, entry.settingKey };
	auto lowerValue = ResolveLocationLowerValue(type, formKey, address, entry.source);
	if (!lowerValue || !ValidateSceneSettingEntry(
			"Location", entry.featureShortName, entry.settingPath, entry.settingKey, *lowerValue, false))
		return;
	entry.value = *lowerValue;
	entry.originalValue = *lowerValue;
	if (entry.source == EntrySource::User) {
		PrepareLocationUserSettingsMutation(type, formKey, false);
		SaveAllUserSettings();
	}
	ReapplyIfActive();
}

bool SceneSettingsManager::HasLocationEntry(LocationTargetType type, std::string_view formKey,
	const std::string& featureShortName, const std::vector<std::string>& settingPath,
	const std::string& settingKey, std::optional<EntrySource> source) const
{
	const auto& config = GetLocationConfig(type, formKey);
	return std::any_of(config.entries.begin(), config.entries.end(), [&](const auto& entry) {
		return (!source || entry.source == *source) &&
		       IsSameSetting(entry, featureShortName, settingPath, settingKey);
	});
}

void SceneSettingsManager::ExportLocationUserSettingsToOverwrites(LocationTargetType type,
	const std::string& formKey, const std::vector<size_t>& indices, const std::string& modName)
{
	auto configIt = locationSceneConfigs.find(GetLocationConfigKey(type, formKey));
	if (configIt == locationSceneConfigs.end())
		return;

	const auto targetDescription = type == LocationTargetType::Cell ? "Cell" : "Location";
	const json metadata = {
		{ "targetType", targetDescription },
		{ "targetName", configIt->second.name },
		{ "coc", configIt->second.cocCode },
	};
	ExportUserEntriesToOverwrites(configIt->second.entries, indices,
		GetLocationOverwritesDir() / configIt->second.formKey, modName, targetDescription, metadata);
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
			if (form->GetFormType() == RE::FormType::Location)
				resolvedType = LocationTargetType::Location;
			else if (form->GetFormType() == RE::FormType::Cell)
				resolvedType = LocationTargetType::Cell;
			else {
				logger::warn("[SceneSettings] Location overwrite target '{}' is not a location or cell", formKey);
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
				if (targetType == "Location")
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

float SceneSettingsManager::GetTimeOfDayPeriodFallbackFloat(float baseValue, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, int periodIndex) const
{
	const json* value = nullptr;
	EntrySource source = EntrySource::Overwrite;
	const auto period = static_cast<TimeOfDayPeriod>(periodIndex);

	for (const auto& entry : GetEntries(SceneType::TimeOfDay)) {
		if (!IsEntryActive(entry) || entry.period != period ||
			!IsSameSetting(entry, featureShortName, settingPath, settingKey))
			continue;
		if (!value || (entry.source == EntrySource::User && source != EntrySource::User)) {
			value = &entry.value;
			source = entry.source;
		}
	}

	if (!value)
		return baseValue;
	if (!IsNumericValue(*value)) {
		logger::warn("[SceneSettings] Time of day fallback value for '{}' is not a float",
			GetSettingLogName(featureShortName, settingPath, settingKey));
		return baseValue;
	}

	const float result = value->get<float>();
	return std::isfinite(result) ? result : baseValue;
}
