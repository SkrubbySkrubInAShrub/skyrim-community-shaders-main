
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using json = nlohmann::json;

#include "Feature.h"
#include "Globals.h"
#include "Utils/Form.h"

/// Manages interior, time-of-day, weather, and location-specific setting overrides.
/// Applies catalog-backed settings in priority order and avoids redundant feature updates.
class SceneSettingsManager
{
public:
	static SceneSettingsManager* GetSingleton();

	// --- Scene Types ---

	enum class SceneType
	{
		InteriorOnly,
		TimeOfDay,
		Location
	};

	// --- Time of Day Periods ---

	enum class TimeOfDayPeriod
	{
		Dawn = 0,
		Sunrise,
		Day,
		Sunset,
		Dusk,
		Night,
		Count
	};

	/// Number of time-of-day periods (avoids repeated static_cast).
	static constexpr int kPeriodCount = static_cast<int>(TimeOfDayPeriod::Count);

	/// Display names for each period - must match TimeOfDayPeriod order.
	static constexpr std::array<const char*, kPeriodCount> kPeriodNames = {
		"Dawn", "Sunrise", "Day", "Sunset", "Dusk", "Night"
	};

	/// Hour boundaries for each period [start, end).  Night wraps around midnight (21-28 i.e. 21-4).
	static constexpr float kPeriodHours[kPeriodCount][2] = {
		{ 4.0f, 6.0f },    // Dawn
		{ 6.0f, 8.0f },    // Sunrise
		{ 8.0f, 17.0f },   // Day
		{ 17.0f, 19.0f },  // Sunset
		{ 19.0f, 21.0f },  // Dusk
		{ 21.0f, 28.0f }   // Night (wraps past midnight)
	};

	/// Transition blend zone in hours at each period boundary.
	static constexpr float kTransitionHours = 0.5f;

	// --- Event Handler ---

	/// Listens for LoadingMenu close to detect cell transitions.
	/// Defers reset work until the menu closes.
	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

		static bool Register()
		{
			static bool registered = false;
			if (registered)
				return true;

			static MenuOpenCloseEventHandler singleton;
			auto ui = globals::game::ui;
			if (!ui) {
				logger::error("[SceneSettings] UI event source not found");
				return false;
			}
			auto eventSource = ui->GetEventSource<RE::MenuOpenCloseEvent>();
			if (!eventSource) {
				logger::error("[SceneSettings] MenuOpenCloseEvent source not found");
				return false;
			}
			eventSource->AddEventSink(&singleton);
			registered = true;
			logger::info("[SceneSettings] Registered MenuOpenCloseEventHandler");
			return true;
		}
	};

	// --- Setting Entry ---

	enum class EntrySource
	{
		User,      // User-added via UI
		Overwrite  // Loaded from overwrite file
	};

	struct SettingEntry
	{
		std::string featureShortName;  // Feature's GetShortName()
		std::vector<std::string> settingPath;  // Feature-owned subfeature/object path
		std::string settingKey;        // Feature-owned scene setting key
		std::string displayName;       // Cached UI label
		json value;                    // Override value (bool, float, int, etc.)
		json originalValue;            // Value at time of creation, for revert
		json serializedTemplate = json::object();  // Preserved forward-compatible fields
		bool paused = false;           // Temporarily disabled
		EntrySource source = EntrySource::User;
		std::string sourceFilename;                       // For overwrites: the filename it came from
		std::filesystem::path sourcePath;                 // For overwrites: exact file path
		TimeOfDayPeriod period = TimeOfDayPeriod::Count;  // Which period this entry belongs to (TimeOfDay only)
	};

	/// One indexed value in an atomic scene-setting update.
	struct EntryValueUpdate
	{
		size_t index;
		json value;
	};

	// --- Generic Entry Management (scene-type agnostic) ---

	const std::vector<SettingEntry>& GetEntries(SceneType type) const;
	/// Monotonic revision for entry structure and pause state used by presentation caches.
	std::uint64_t GetEntryPresentationRevision() const { return entryPresentationRevision; }
	bool HasEntryFromSource(SceneType type, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey, EntrySource source) const;
	/// Add a setting.  For TimeOfDay entries, specify the target period.
	bool AddSetting(SceneType type, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey, const json& value,
		TimeOfDayPeriod period = TimeOfDayPeriod::Count, bool deferCommit = false);
	void RemoveSetting(SceneType type, size_t index);
	void TogglePauseEntry(SceneType type, size_t index);
	void UpdateEntryValue(SceneType type, size_t index, const json& newValue, bool deferSave = false);
	/// Validate and update a group of entries before applying any of them.
	void UpdateEntryValues(SceneType type, std::span<const EntryValueUpdate> updates, bool deferSave = false);
	void CommitSceneSettingChanges();

	/// Revert an entry's value to its originalValue (captured at creation).
	void RevertEntryToDefault(SceneType type, size_t index);

	/// Check if an entry already exists for a specific period (TimeOfDay)
	bool HasEntryForPeriod(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey,
		TimeOfDayPeriod period, EntrySource source) const;

	void SetAllOverwritesPaused(SceneType type, bool paused);
	bool AreAllOverwritesPaused(SceneType type) const;
	void DeleteAllOverwrites(SceneType type);

	void SetAllUserPaused(SceneType type, bool paused);
	bool AreAllUserPaused(SceneType type) const;
	void DeleteAllUserSettings(SceneType type);

	/// Export selected user entries to grouped per-feature overwrite JSON files.
	void ExportUserSettingsToOverwrites(SceneType type, const std::vector<size_t>& indices, const std::string& modName);
	void ExportWeatherUserSettingsToOverwrites(RE::FormID weatherId, const std::vector<size_t>& indices, const std::string& modName);
	void DeleteAllWeatherUserSettings(RE::FormID weatherId);

	// --- Scene Application ---

	/// Called every frame from State::Update().
	void Update();

	/// Called by MenuOpenCloseEventHandler when a cell transition is detected.
	void OnCellTransition();

	/// Check if any scene settings are active for a given feature
	bool HasActiveSettingsForFeature(const std::string& featureShortName) const;
	bool HasAnySceneEntriesForFeature(const std::string& featureShortName) const;
	bool IsActiveSceneSetting(std::string_view featureShortName,
		std::string_view settingPath, std::string_view settingKey) const;
	bool IsActiveSceneSetting(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey) const;
	void CaptureExternalFeatureChanges(Feature* feature);

	/// Per-feature pause: temporarily disable all scene-specific settings for a feature
	bool IsFeaturePaused(const std::string& featureShortName) const;
	void SetFeaturePaused(const std::string& featureShortName, bool paused);

	class SceneLayerGuard
	{
	public:
		explicit SceneLayerGuard(SceneSettingsManager& manager);
		~SceneLayerGuard();

		SceneLayerGuard(const SceneLayerGuard&) = delete;
		SceneLayerGuard& operator=(const SceneLayerGuard&) = delete;

	private:
		SceneSettingsManager& manager;
	};

	// --- Persistence ---

	/// Save all user data (interior, TOD, weather) to unified SceneManager.json.
	void SaveAllUserSettings();

	void DiscoverOverwrites(SceneType type);

	/// Discover weather-specific overwrite files from Weather/{SPID}/ folders.
	void DiscoverWeatherOverwrites();

	/// Load non-weather scene types (overwrites + user settings). Called early from Setup().
	void LoadAll();

	// --- Path Resolution ---

	static std::string GetSceneTypeName(SceneType type);
	static std::filesystem::path GetUserSettingsFilePath();
	static std::filesystem::path GetOverwritesPath(SceneType type);

	// --- Time of Day Helpers (public for UI) ---

	static const char* GetPeriodName(TimeOfDayPeriod period);
	static TimeOfDayPeriod GetPeriodFromName(const std::string& name);
	static float GetCurrentGameHour();
	void GetTimeOfDayFactors(float outFactors[static_cast<int>(TimeOfDayPeriod::Count)]);

	/// Returns the period whose hour range contains the current game hour.
	static TimeOfDayPeriod GetCurrentPeriod();

	// --- Feature Metadata ---

	/// Get loaded feature short names with scene-visible settings.
	static std::vector<std::string> GetInteriorRelevantFeatureNames();

	/// Get loaded feature short names with transitionable settings.
	static std::vector<std::string> GetExteriorRelevantFeatureNames();
	static std::vector<std::string> GetLocationRelevantFeatureNames();

	/// Check whether the feature exposes settings supported by the scene type.
	static bool IsFeatureAllowedForType(SceneType type, const std::string& featureShortName);

	/// Check the shared catalog and settings blacklist policy.
	static bool IsSceneSettingAllowed(
		std::string_view featureShortName, std::string_view settingPath, std::string_view settingKey);

	/// Get the localized display name for a feature.
	static std::string GetFeatureDisplayName(const std::string& featureShortName);

	/// Logical Scene Manager editor represented by one or more persisted values.
	enum class SettingControlType : std::uint8_t
	{
		Scalar,
		Numeric,
		Color,
	};

	/// Visual treatment used for a logical aggregate setting.
	enum class AggregatePresentation : std::uint8_t
	{
		Components,
		ColorPicker,
	};

	/// Interaction used to edit all components of a logical aggregate.
	enum class UnifiedEditMode : std::uint8_t
	{
		None,
		Always,
		Shift,
	};

	/// One persisted primitive belonging to a logical Scene Manager control.
	struct SettingDescriptorMember
	{
		std::vector<std::string> settingPath;
		std::string key;
		std::string componentDisplayName;
		json value;
		std::int8_t componentIndex = -1;
		bool aggregateAll = false;
	};

	/// Catalog-backed setting presented in the add-setting dialog.
	struct SettingDescriptor
	{
		std::vector<std::string> settingPath;
		std::string key;
		std::string displayName;
		std::vector<std::string> displayPath;
		json value;
		SettingControlType controlType = SettingControlType::Scalar;
		AggregatePresentation aggregatePresentation = AggregatePresentation::Components;
		UnifiedEditMode unifiedEditMode = UnifiedEditMode::None;
		std::vector<SettingDescriptorMember> members;
	};

	/// Get scene-safe setting descriptors for a feature.
	static std::vector<SettingDescriptor> GetFeatureSceneSettings(const std::string& featureShortName);

	/// Get scene-safe float setting descriptors for time/weather blending.
	static std::vector<SettingDescriptor> GetTransitionableSceneSettings(const std::string& featureShortName);

	/// Logical-control metadata for one stored scene-setting entry.
	struct SettingControlInfo
	{
		std::vector<std::string> settingPath;
		std::string settingKey;
		std::string displayName;
		std::string componentDisplayName;
		std::vector<std::string> displayPath;
		SettingControlType controlType = SettingControlType::Scalar;
		std::int8_t componentIndex = -1;
		std::int8_t componentStart = -1;
		std::uint8_t componentCount = 0;
		bool aggregateAll = false;
		AggregatePresentation aggregatePresentation = AggregatePresentation::Components;
		UnifiedEditMode unifiedEditMode = UnifiedEditMode::None;
	};

	/// Get the logical ImGui control represented by a scalar scene setting.
	static bool GetSettingControlInfo(const SettingEntry& entry, SettingControlInfo& info);

	/// Get a UI-friendly display label for a setting key.
	static std::string GetSettingDisplayName(const std::string& settingKey);

	/// Get current value of a specific setting from a feature
	static json GetFeatureSettingValue(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey);

	/// Detect the JSON type of a setting value for UI rendering
	enum class SettingType
	{
		Boolean,
		Integer,
		Float,
		String,
		Unknown
	};
	static SettingType DetectSettingType(const json& value);
	static bool IsBooleanControlSetting(const SettingEntry& entry);
	static bool IsInvertedDisplaySetting(const SettingEntry& entry);
	static bool GetNumericBounds(const SettingEntry& entry, double& minimum, double& maximum);
	static double GetNumericDisplayScale(const SettingEntry& entry);
	/// Convert a raw stored numeric setting value to its Scene Manager display value.
	static bool GetNumericDisplayValue(const SettingEntry& entry, double storedValue, double& displayValue);
	/// Convert a Scene Manager display value to its raw stored numeric setting value.
	static bool GetNumericStoredValue(const SettingEntry& entry, double displayValue, double& storedValue);
	static size_t GetSettingChoiceCount(const SettingEntry& entry);
	static bool GetSettingChoice(const SettingEntry& entry, size_t index, std::int64_t& value, std::string& displayName);

	// --- Per-Weather Scene Settings ---

	/// Per-weather configuration: all entries are per-period (TOD).
	/// The UI flat/TOD toggle is a view-only preference, not a data mode.
	struct WeatherSceneConfig
	{
		std::vector<SettingEntry> entries;
	};

	const WeatherSceneConfig& GetWeatherConfig(RE::FormID weatherId);
	bool HasWeatherConfig(RE::FormID weatherId);

	/// Add a weather setting.  Requires a valid period (all entries are per-period).
	bool AddWeatherSetting(RE::FormID weatherId, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey, TimeOfDayPeriod period,
		bool deferSave = false);
	void RemoveWeatherSetting(RE::FormID weatherId, size_t index);
	void TogglePauseWeatherEntry(RE::FormID weatherId, size_t index);
	void UpdateWeatherEntryValue(RE::FormID weatherId, size_t index, const json& newValue, bool deferSave = false);
	/// Validate and update weather entries as one mutation.
	void UpdateWeatherEntryValues(
		RE::FormID weatherId, std::span<const EntryValueUpdate> updates, bool deferSave = false);
	void RevertWeatherEntryToDefault(RE::FormID weatherId, size_t index);
	bool HasWeatherEntryForPeriod(RE::FormID weatherId, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey, TimeOfDayPeriod period,
		std::optional<EntrySource> source = std::nullopt);

	/// Weather UI preference: show TOD table vs flat view (view-only, data is always per-period).
	bool IsWeatherShowTimeOfDay(RE::FormID weatherId);
	void SetWeatherShowTimeOfDay(RE::FormID weatherId, bool show);

	static std::filesystem::path GetWeatherOverwritesDir();

	// --- Per-Location Scene Settings ---

	enum class LocationTargetType
	{
		Location,
		Cell
	};

	struct LocationTarget
	{
		LocationTargetType type = LocationTargetType::Location;
		std::string formKey;
		std::string name;
		std::string cocCode;
		RE::FormID formId = 0;
	};

	struct LocationSceneConfig
	{
		LocationTargetType type = LocationTargetType::Location;
		std::string formKey;
		std::string name;
		std::string cocCode;
		std::vector<SettingEntry> entries;
	};

	std::vector<LocationTarget> GetCurrentLocationTargets() const;
	const LocationSceneConfig& GetLocationConfig(LocationTargetType type, std::string_view formKey) const;
	bool HasLocationConfig(LocationTargetType type, std::string_view formKey) const;
	bool AddLocationSetting(LocationTargetType type, const std::string& formKey, const std::string& name,
		const std::string& cocCode,
		const std::string& featureShortName, const std::vector<std::string>& settingPath,
		const std::string& settingKey, bool deferSave = false);
	void RemoveLocationSetting(LocationTargetType type, const std::string& formKey, size_t index);
	void TogglePauseLocationEntry(LocationTargetType type, const std::string& formKey, size_t index);
	void UpdateLocationEntryValue(LocationTargetType type, const std::string& formKey, size_t index,
		const json& newValue, bool deferSave = false);
	/// Validate and update location entries as one mutation.
	void UpdateLocationEntryValues(LocationTargetType type, const std::string& formKey,
		std::span<const EntryValueUpdate> updates, bool deferSave = false);
	void RevertLocationEntryToDefault(LocationTargetType type, const std::string& formKey, size_t index);
	bool HasLocationEntry(LocationTargetType type, std::string_view formKey,
		const std::string& featureShortName, const std::vector<std::string>& settingPath,
		const std::string& settingKey, std::optional<EntrySource> source = std::nullopt) const;
	void ExportLocationUserSettingsToOverwrites(LocationTargetType type, const std::string& formKey,
		const std::vector<size_t>& indices, const std::string& modName);
	void DeleteAllLocationUserSettings(LocationTargetType type, const std::string& formKey);
	static std::filesystem::path GetLocationOverwritesDir(LocationTargetType type);

	/// Enables location discovery once Skyrim form data is guaranteed to be available.
	void OnDataLoaded();

protected:
	SceneSettingsManager();
	~SceneSettingsManager();

private:
	SceneSettingsManager(const SceneSettingsManager&) = delete;
	SceneSettingsManager& operator=(const SceneSettingsManager&) = delete;

	// --- Per scene-type storage ---
	std::map<SceneType, std::vector<SettingEntry>> entries;
	std::map<SceneType, std::vector<json>> unresolvedUserEntries;
	std::uint64_t entryPresentationRevision = 0;
	json preservedUserSettingsRoot = json::object();
	bool userSettingsDocumentLoaded = false;
	bool userSettingsDocumentWritable = true;
	bool userSettingsWriteBlockedWarning = false;
	bool interiorUserSettingsModified = false;
	bool timeOfDayUserSettingsModified = false;
	bool weatherUserSettingsModified = false;
	bool locationUserSettingsModified = false;
	bool dataLoaded = false;
	bool deferredSceneChangesPending = false;
	std::chrono::steady_clock::time_point deferredSceneChangesDeadline{};
	static constexpr auto kDeferredSaveDelay = std::chrono::milliseconds(250);
	static constexpr auto kDeferredSaveRetryDelay = std::chrono::seconds(2);

	std::atomic<bool> queuedCellTransition = false;

	/// Float epsilon - changes smaller than this skip the LoadSettings call.
	static constexpr float kBlendEpsilon = 1e-3f;

	/// Minimum game-hour delta before re-running the blend. At the default
	/// timescale (20x), this equals about 0.18 real seconds.
	static constexpr float kHourUpdateThreshold = 1e-3f;

	// --- Pause states ---
	std::map<std::string, bool> featurePauseStates;
	int sceneLayerSuspendDepth = 0;

	// --- Per-Weather Scene storage ---
	std::map<RE::FormID, WeatherSceneConfig> weatherSceneConfigs;
	static const WeatherSceneConfig kEmptyWeatherConfig;

	/// UI preference per weather: show TOD table vs flat view (keyed by FormID for fast access).
	std::map<RE::FormID, bool> weatherShowTimeOfDay_;
	json unresolvedWeatherUserSettings = json::object();
	bool weatherDataLoaded = false;

	// --- Per-Location Scene storage ---
	std::map<std::string, LocationSceneConfig> locationSceneConfigs;
	static const LocationSceneConfig kEmptyLocationConfig;
	json unresolvedLocationUserSettings = json::object();
	bool locationDataLoaded = false;
	bool gameDataReady = false;

	struct SettingAddress
	{
		std::string featureShortName;
		std::vector<std::string> settingPath;
		std::string settingKey;

		auto operator<=>(const SettingAddress&) const = default;
	};

	using ResolvedSettingMap = std::map<SettingAddress, json>;
	ResolvedSettingMap baselineSettings;
	ResolvedSettingMap appliedSettings;
	std::set<std::string> restoreFailureWarnings;
	std::map<std::string, std::chrono::steady_clock::time_point> restoreRetryAfter;
	struct ApplyFailureState
	{
		json signature;
		std::chrono::steady_clock::time_point retryAfter{};
		bool warningLogged = false;
	};
	std::map<std::string, ApplyFailureState> applyFailures;
	static constexpr auto kApplyRetryDelay = std::chrono::seconds(2);
	bool resolverDirty = true;
	bool resolverSuspended = false;
	bool activeEntryCacheDirty = true;
	bool hasActiveSceneEntries = false;
	std::uint32_t lastUpdateFrame = std::numeric_limits<std::uint32_t>::max();
	bool lastResolvedInterior = false;
	RE::FormID lastResolvedLocationId = 0;
	RE::FormID lastResolvedCellId = 0;
	float lastResolvedHour = -1.0f;
	RE::FormID lastResolvedCurrentWeatherId = 0;
	RE::FormID lastResolvedPreviousWeatherId = 0;
	float lastResolvedWeatherLerp = -1.0f;
	mutable RE::FormID cachedPreviousWeatherId = 0;
	mutable RE::FormID cachedTargetLocationId = 0;
	mutable RE::FormID cachedTargetCellId = 0;
	mutable bool locationTargetsCached = false;
	mutable std::vector<LocationTarget> cachedLocationTargets;

	// --- Per-Weather helpers ---
	/// Load weather overwrites/user settings once game data is available for SPID resolution.
	bool TryEnsureWeatherDataLoaded();
	bool TryEnsureLocationDataLoaded();
	void LoadWeatherData();
	WeatherSceneConfig& GetWeatherConfigMut(RE::FormID weatherId);
	RE::FormID GetEffectivePreviousWeatherId(const RE::Sky* sky, float weatherLerp) const;
	float GetTimeOfDayPeriodFallbackFloat(float baseVal, const std::string& shortName,
		const std::vector<std::string>& settingPath, const std::string& key, int periodIdx) const;

	// --- Central runtime resolver ---
	void ResolveAndApply(bool force = false);
	bool HasActiveSceneEntriesCached();
	ResolvedSettingMap BuildResolvedSettings();
	void ApplyResolvedSettings(const ResolvedSettingMap& resolved, bool forceRetry);
	void RestoreAppliedSettings();
	void ResolveInteriorSettings(ResolvedSettingMap& resolved) const;
	void ResolveTimeOfDaySettings(ResolvedSettingMap& resolved) const;
	void ResolveWeatherSettings(ResolvedSettingMap& resolved) const;
	void ResolveLocationSettings(ResolvedSettingMap& resolved, const std::vector<LocationTarget>& locationTargets) const;
	void OverlayEntries(ResolvedSettingMap& resolved, const std::vector<SettingEntry>& sourceEntries,
		SceneType type, std::optional<EntrySource> source = std::nullopt) const;
	float ResolveTimeOfDayFloat(const SettingAddress& address, float baseValue) const;
	std::optional<float> ResolveWeatherFloat(const SettingAddress& address, float baseValue) const;
	std::optional<float> ResolveWeatherLowerValue(RE::FormID weatherId, const SettingAddress& address,
		TimeOfDayPeriod period, EntrySource selectedSource);
	json GetBaselineValue(const SettingAddress& address);
	std::optional<json> ResolveLocationLowerValue(LocationTargetType type, std::string_view formKey,
		const SettingAddress& address, EntrySource selectedSource);
	static bool ResolvedValuesEqual(const json& lhs, const json& rhs);

	// --- Per-Location helpers ---
	static std::string GetLocationConfigKey(LocationTargetType type, std::string_view formKey);
	LocationSceneConfig& GetLocationConfigMut(LocationTargetType type, const std::string& formKey,
		const std::string& name = {});
	void DiscoverLocationOverwrites();
	void DiscoverLocationOverwritesForTarget(LocationTargetType type, const std::filesystem::path& targetDir);
	void LoadLocationUserSettings(const json& data);
	void PrepareLocationUserSettingsMutation(LocationTargetType type, std::string_view formKey,
		bool replaceMalformedEntries);

	// --- Helpers ---
	std::vector<SettingEntry>& GetEntriesMut(SceneType type);
	void BumpEntryPresentationRevision();
	bool IsEntryActive(const SettingEntry& entry) const;
	bool HasDuplicateEntry(SceneType type, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey,
		EntrySource source, TimeOfDayPeriod period = TimeOfDayPeriod::Count) const;

	void ReapplyIfActive();
	void MarkEntryListUserSettingsModified(SceneType type);
	void PrepareWeatherUserSettingsMutation(RE::FormID weatherId, bool replaceMalformedEntries);
	void MarkDeferredSceneChanges();
	void FlushDeferredSceneChanges();
	void SuspendSceneLayer();
	void ResumeSceneLayer();

	// --- Overwrite discovery helper ---
	void DiscoverOverwritesInDir(SceneType type, const std::filesystem::path& dir,
		TimeOfDayPeriod period = TimeOfDayPeriod::Count);

	/// Discover overwrite files for a single weather SPID folder.
	void DiscoverWeatherOverwritesForSpid(RE::FormID weatherId, const std::filesystem::path& weatherDir);

	/// Load non-weather user settings from unified SceneManager.json.
	void LoadAllUserSettings();

	/// Load weather user settings from SceneManager.json. Requires TESDataHandler.
	void LoadWeatherUserSettings();
};
