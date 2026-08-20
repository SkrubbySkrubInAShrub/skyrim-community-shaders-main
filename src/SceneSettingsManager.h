#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
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

	/// Every real period in order, excluding the Count sentinel.
	static constexpr std::array<TimeOfDayPeriod, kPeriodCount> kPeriods = {
		TimeOfDayPeriod::Dawn, TimeOfDayPeriod::Sunrise, TimeOfDayPeriod::Day,
		TimeOfDayPeriod::Sunset, TimeOfDayPeriod::Dusk, TimeOfDayPeriod::Night
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

	/// Layer a fresh capture is taken from. Passing Overwrite here means "resolve the layers beneath
	/// the overwrites", so ticking the gutter over a mod-supplied value pins the value that would
	/// apply without the mod rather than the mod's own. Lower user layers still stack.
	static constexpr EntrySource kCaptureSourceLayer = EntrySource::Overwrite;

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
		/// Suppresses every lower layer at this address instead of supplying a value. An explicit
		/// state, not an empty `value`: the resolve, copy and export paths all read `value` unguarded.
		bool deleted = false;
		EntrySource source = EntrySource::User;
		std::string sourceFilename;                       // For overwrites: the filename it came from
		std::filesystem::path sourcePath;                 // For overwrites: exact file path
		TimeOfDayPeriod period = TimeOfDayPeriod::Count;  // Which period this entry belongs to (TimeOfDay only)
		std::optional<float> transitionSeconds;           // Location float transition override
		// A transition this build cannot honor is dropped from the entry but kept in the template,
		// so a document authored by another implementation round-trips with its field intact.
		bool retainSerializedTransition = false;
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

	/// Mods supplying overwrite entries anywhere, in discovery order, deduplicated. Loads weather and
	/// location data first if not already loaded; returns whatever it has if either fails to load.
	std::vector<std::string> GetOverwriteModNames();

	/// Every file a preset of this name currently owns, across every scene directory.
	std::vector<std::filesystem::path> FindPresetFiles(const std::string& modName) const;

	/** @brief Bakes the winning values of every context into a preset, replacing its file set.
	 *  Tombstoned addresses are omitted; a paused user entry lets the mod's value through.
	 *  @return Whether every file was written. */
	bool ExportPreset(const std::string& modName);

	void DeleteAllWeatherUserSettings(RE::FormID weatherId);

	// --- Scene Application ---

	/// Called every frame from State::Draw().
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

	/// RAII suspend of the scene layer. Anything reading or writing a feature's *base* settings must
	/// hold one, otherwise it captures an overridden value as if it were the user's choice.
	class SceneLayerGuard
	{
	public:
		SceneLayerGuard();
		~SceneLayerGuard();

		SceneLayerGuard(const SceneLayerGuard&) = delete;
		SceneLayerGuard& operator=(const SceneLayerGuard&) = delete;

	private:
		SceneSettingsManager* manager;
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

	/// Writes the game hour every reader, including GetCurrentGameHour, resolves against.
	static void SetGameHour(float hour);

	/// Middle of a period's hour range, wrapped into [0, 24): Night runs past midnight.
	static float GetPeriodMidHour(TimeOfDayPeriod period);

	/// Per-period blend weights for the current game hour. Weights sum to 1.
	static std::array<float, kPeriodCount> GetTimeOfDayFactors();

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

	/// Check whether a single catalog setting is scene-controllable for the scene type.
	static bool IsSettingAllowedForType(SceneType type, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey);

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

	/// Broadest to narrowest: a category groups locations by their LocType keyword.
	enum class LocationTargetType
	{
		Category,
		Location,
		Cell
	};

	/// Persisted "type" discriminator, also used as the overwrite metadata targetType.
	static const char* GetLocationTargetTypeName(LocationTargetType type);

	struct LocationTarget
	{
		LocationTargetType type = LocationTargetType::Location;
		std::string formKey;
		std::string name;
		/// Several links of a chain can share a full name (Winterhold the hold and the town), so the
		/// editor ID is what tells them apart.
		std::string editorId;
		std::string cocCode;
		RE::FormID formId = 0;
	};

	struct LocationSceneConfig
	{
		LocationTargetType type = LocationTargetType::Location;
		std::string formKey;
		std::string name;
		/// Captured when the target is taken on, so same-named targets stay distinguishable in the
		/// editor even when the plugin that defines them is not installed.
		std::string editorId;
		std::string cocCode;
		std::vector<SettingEntry> entries;
		/// The user put this target on their list. Keeps a target that has no settings yet persistable,
		/// and separates it from a config that only exists because a mod shipped an overwrite for it.
		bool userAuthored = false;
	};

	/// The player's location chain, outermost first. Cached until the location or cell FormID moves.
	const std::vector<LocationTarget>& GetCurrentLocationTargets() const;

	/// Targets the user has taken on, for the editor's location list.
	std::vector<LocationTarget> GetAuthoredLocationTargets() const;

	/// Put a target on the user's list so it can be authored before it has any settings.
	bool AddLocationTarget(const LocationTarget& target);
	bool IsLocationTargetAuthored(LocationTargetType type, std::string_view formKey) const;

	/// Drop a target from the user's list, discarding the settings they authored for it.
	void RemoveLocationTarget(LocationTargetType type, const std::string& formKey);

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
	void DeleteAllLocationUserSettings(LocationTargetType type, const std::string& formKey);

	/// Locations and cells share one directory; each target's type comes from its form, not its path.
	static std::filesystem::path GetLocationOverwritesDir();

	/// Default duration used by location float transitions.
	static constexpr float kDefaultLocationTransitionSeconds = 5.0f;
	/// Largest accepted typed location transition duration.
	static constexpr float kMaxLocationTransitionSeconds = 300.0f;

	/// Return the global location float transition duration in seconds.
	float GetLocationTransitionSeconds() const { return locationTransitionSeconds; }
	/// Set and persist the global location float transition duration.
	void SetLocationTransitionSeconds(float seconds, bool deferSave = false);
	/// Return an entry-specific location transition duration, or null for the global duration.
	std::optional<float> GetLocationEntryTransitionSeconds(
		LocationTargetType type, std::string_view formKey, size_t index) const;
	/// Set one or more location entries to the same transition duration as one atomic edit.
	void SetLocationEntryTransitionSeconds(LocationTargetType type, const std::string& formKey,
		std::span<const size_t> indices, std::optional<float> seconds, bool deferSave = false);

	// --- Generic Scene Copy ---

	/// Identifies one physical persisted setting.
	struct SettingIdentity
	{
		std::string featureShortName;
		std::vector<std::string> settingPath;
		std::string settingKey;

		auto operator<=>(const SettingIdentity&) const = default;
	};

	/// Kind of scene context participating in a copy or authoring operation.
	enum class SceneContextType : std::uint8_t
	{
		Interior,
		TimeOfDay,
		Weather,
		Location,
	};

	/// Stable identity for a time period, weather period, or location target.
	struct SceneContextId
	{
		SceneContextType type = SceneContextType::TimeOfDay;
		TimeOfDayPeriod period = TimeOfDayPeriod::Count;
		RE::FormID weatherId = 0;
		LocationTargetType locationType = LocationTargetType::Location;
		std::string locationFormKey;

		auto operator<=>(const SceneContextId&) const = default;
	};

	/// Whether a context stores one entry per time-of-day period. Interior and location do not.
	static bool IsPeriodicContext(SceneContextType type);

	/// How much of a periodic context an action covers. A page with time of day off authors every
	/// period at once, so its actions have to cover every period too.
	enum class PeriodScope : std::uint8_t
	{
		ActivePeriod,
		AllPeriods,
	};

	/// Amount copied from a source context.
	enum class CopyScope : std::uint8_t
	{
		EntireContext,
		Setting,
	};

	/// How an existing destination user setting is handled.
	enum class CopyConflictPolicy : std::uint8_t
	{
		SkipExisting,
		OverwriteExisting,
		Cancel,
	};

	/// Why a candidate cannot be copied, so a preview can explain itself. None when it can.
	enum class CopyRejection : std::uint8_t
	{
		None,
		NotInCatalog,            ///< No catalog entry the destination layer permits.
		NotAllowedInLayer,       ///< Feature not loaded, or the destination layer forbids the setting.
		ValueRejected,           ///< The source value is not a legal value at the destination.
		BlockedByOverwrite,      ///< An unpaused mod-authored overwrite holds the destination address.
		GroupCompanionRejected,  ///< This row is fine; a sibling in the same control is not.
	};

	/// One source context with settings compatible with a destination.
	struct CopySource
	{
		SceneContextId context;
		std::string displayName;
		size_t settingCount = 0;
	};

	/// One physical setting available to copy.
	struct CopyCandidate
	{
		SettingIdentity setting;
		std::string displayName;
		json value;
		/// The user value this would replace, so a preview can render the transition.
		std::optional<json> destinationValue;
		CopyRejection rejection = CopyRejection::None;
		bool compatible = false;
		bool conflicts = false;
	};

	/// Aggregate result of one transactional copy.
	struct CopyResult
	{
		size_t copied = 0;
		size_t skipped = 0;
		size_t overwritten = 0;
		size_t incompatible = 0;
		bool hadConflicts = false;
		bool cancelled = false;

		/// Return whether the operation changed the destination.
		bool Changed() const { return copied != 0 || overwritten != 0; }
	};

	/// Return non-empty contexts that contain compatible data for a destination.
	std::vector<CopySource> GetCopySources(const SceneContextId& destination,
		CopyScope scope = CopyScope::EntireContext,
		const std::optional<SettingIdentity>& setting = std::nullopt) const;
	/// Return every context a source can copy compatible data into, including pages with no
	/// settings yet: every weather and known location, not just the ones already authored.
	std::vector<CopySource> GetCopyDestinations(const SceneContextId& source,
		CopyScope scope = CopyScope::EntireContext,
		const std::optional<SettingIdentity>& setting = std::nullopt) const;
	/// Inspect the settings and conflicts in a proposed copy without mutating state.
	/// AllPeriods answers for every period at once: a row conflicts if any of them already holds it.
	std::vector<CopyCandidate> GetCopyCandidates(const SceneContextId& source,
		const SceneContextId& destination, CopyScope scope = CopyScope::EntireContext,
		const std::optional<SettingIdentity>& setting = std::nullopt,
		PeriodScope periodScope = PeriodScope::ActivePeriod) const;
	/// Copy settings as one validated mutation and one save/reapply operation.
	CopyResult CopySettings(const SceneContextId& source, const SceneContextId& destination,
		CopyConflictPolicy conflictPolicy, CopyScope scope = CopyScope::EntireContext,
		const std::optional<SettingIdentity>& setting = std::nullopt);
	/// Copy into every period of a flat page, or into the one period of a normal page, as one save.
	CopyResult CopySettingsAcrossPeriods(const SceneContextId& source, const SceneContextId& destination,
		CopyConflictPolicy conflictPolicy, PeriodScope periodScope);
	/// Name one context the way the copy source list spells it.
	std::string GetSceneContextDisplayName(const SceneContextId& context) const;

	// --- Context-Keyed Entry Access (Scene Manager authoring UI) ---

	/// Split a catalog setting path into the segment vector every entry API takes.
	static std::vector<std::string> SplitSettingPath(std::string_view catalogPath);

	/// Index of the user entry for one setting in one context, or nullopt when none exists.
	std::optional<size_t> FindContextUserEntry(const SceneContextId& context,
		const std::string& featureShortName, const std::vector<std::string>& settingPath,
		const std::string& settingKey) const;

	/// Per-period user entry indices for one setting. Contexts without periods populate index 0 only.
	std::array<std::optional<size_t>, kPeriodCount> FindContextUserEntryPerPeriod(
		const SceneContextId& context, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey) const;

	/// Add a user entry capturing the feature's current base value. Returns its index.
	std::optional<size_t> AddContextSetting(const SceneContextId& context,
		const std::string& featureShortName, const std::vector<std::string>& settingPath,
		const std::string& settingKey, bool deferSave = false);

	/// Validate and update a group of context entries before applying any of them.
	void UpdateContextEntryValues(const SceneContextId& context,
		std::span<const EntryValueUpdate> updates, bool deferSave = false);
	/// Remove one entry from a context.
	void RemoveContextSetting(const SceneContextId& context, size_t index);

	/** @brief Suppresses every lower layer at an address by persisting a user tombstone.
	 *  Never touches a mod's file; an existing user entry becomes the tombstone in place.
	 *  @return Whether a tombstone now covers the address. */
	bool TombstoneContextSetting(const SceneContextId& context, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey);

	/// Clears a tombstone at an address, restoring whatever the lower layers supply.
	void ClearContextTombstone(const SceneContextId& context, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey);
	/// Toggle the paused state of one context entry.
	void TogglePauseContextEntry(const SceneContextId& context, size_t index);
	/// Revert one context entry's value to its originalValue.
	void RevertContextEntryToDefault(const SceneContextId& context, size_t index);

	/// Entries stored for one context, unfiltered by period. Empty when the context holds none.
	std::span<const SettingEntry> GetContextEntries(const SceneContextId& context) const;

	/// Which layer supplies the winning value at an address in a context.
	enum class SettingLayer : std::uint8_t
	{
		None,       // nothing supplies it: the feature's base applies
		Overwrite,  // a discovered overwrite file wins
		User,       // a user entry wins
		Deleted,    // a user tombstone suppresses every lower layer
	};

	struct SettingProvenance
	{
		SettingLayer layer = SettingLayer::None;
	};

	/** @brief Names the layer driving one address, for the gutter's colour and the export modal.
	 *  @return The winning layer. */
	SettingProvenance GetSettingProvenance(const SceneContextId& context,
		const std::string& featureShortName, const std::vector<std::string>& settingPath,
		const std::string& settingKey) const;

	/// Mod name an overwrite entry came from: the filename stem up to the last underscore.
	static std::string GetOverwriteModName(const SettingEntry& entry);

	/// What a context's user entries amount to, for the page-wide actions that act on all of them.
	struct ContextEntrySummary
	{
		size_t total = 0;
		size_t paused = 0;

		bool AllPaused() const { return total != 0 && paused == total; }
	};

	/// Count the user entries belonging to one context, and how many of them are paused.
	ContextEntrySummary GetContextUserEntrySummary(const SceneContextId& context,
		PeriodScope periodScope = PeriodScope::ActivePeriod) const;
	/// Pause or resume every user entry in a context as one save.
	void SetContextEntriesPaused(const SceneContextId& context, bool paused,
		PeriodScope periodScope = PeriodScope::ActivePeriod);
	/// Remove every user entry in a context as one save. Mod-authored overwrites are left alone,
	/// as are raw entries this session could not resolve to a loaded feature.
	void ClearContextEntries(const SceneContextId& context,
		PeriodScope periodScope = PeriodScope::ActivePeriod);

	/// Whether any user entry exists in any context, tombstones included.
	bool HasAnyUserEntries() const;

	/** @brief Drops every user entry in every context, tombstones included.
	 *  Suppressed mod values come back. Unresolved raw entries are left in the document. */
	void ClearAllUserEntries();

	/// Enables location discovery once Skyrim form data is guaranteed to be available.
	void OnDataLoaded();

	// --- Runtime State ---

	/// Current and outgoing weather with the sky's blend factor. Ids are 0 when no weather is active.
	struct WeatherBlend
	{
		RE::FormID currentWeatherId = 0;
		RE::FormID previousWeatherId = 0;
		float lerp = 0.0f;
	};

	// --- Debug Inspection ---

	/// One stored entry, flattened for the debug UI.
	struct DebugEntry
	{
		std::string feature;
		std::string path;
		std::string key;
		std::string value;
		std::string period;                       // Empty when the entry is not per-period
		std::optional<float> transitionSeconds;   // Only set when the entry overrides the global duration
		bool overwrite = false;
		bool paused = false;
		bool active = false;
		bool resolvable = false;
	};

	/// A group of entries the resolver treats as one layer (scene type, weather, or location).
	struct DebugLayer
	{
		std::string name;
		std::string detail;
		bool matchesCurrentScene = false;
		std::vector<DebugEntry> entries;
	};

	/// One address the resolver currently drives, with the inputs that produced its value.
	struct DebugResolvedSetting
	{
		std::string feature;
		std::string path;
		std::string key;
		std::string baseline;
		std::string applied;
		std::array<std::optional<float>, kPeriodCount> timeOfDayValues{};
		std::array<std::optional<float>, kPeriodCount> currentWeatherValues{};
		std::array<std::optional<float>, kPeriodCount> previousWeatherValues{};
	};

	/// One location float transition mid-flight, sampled for the debug UI.
	struct DebugLocationTransition
	{
		std::string feature;
		std::string path;
		std::string key;
		float startValue = 0.0f;
		float targetValue = 0.0f;
		float currentValue = 0.0f;
		float progress = 0.0f;
		float duration = 0.0f;
		bool restoreAtEnd = false;
	};

	/// Everything the resolver has in flight, sampled for the debug UI.
	struct DebugSnapshot
	{
		// Live scene context
		bool playerReady = false;
		bool menuOpen = false;
		bool interior = false;
		RE::FormID cellId = 0;
		std::string cellName;
		std::string cellEditorId;
		RE::FormID locationId = 0;
		std::string locationName;
		std::vector<LocationTarget> locationTargets;
		float gameHour = 0.0f;
		TimeOfDayPeriod period = TimeOfDayPeriod::Count;
		std::array<float, kPeriodCount> timeOfDayFactors{};
		WeatherBlend weather;
		std::string currentWeatherName;
		std::string previousWeatherName;

		// Load and resolver state
		bool dataLoaded = false;
		bool weatherDataLoaded = false;
		bool locationDataLoaded = false;
		bool gameDataReady = false;
		bool resolverSuspended = false;
		bool resolverDirty = false;
		bool activeEntryCacheDirty = false;
		bool hasActiveSceneEntries = false;
		bool deferredSceneChangesPending = false;
		int sceneLayerSuspendDepth = 0;

		// Inputs of the most recent resolve, and the factors it blended with
		bool lastInterior = false;
		RE::FormID lastCellId = 0;
		RE::FormID lastLocationId = 0;
		float lastHour = -1.0f;
		WeatherBlend lastWeather;
		std::array<float, kPeriodCount> blendFactors{};

		// Location float transitions
		float transitionTime = 0.0f;  // Pause-aware clock the transitions ease against
		float lastTransitionTick = -1.0f;
		float globalTransitionSeconds = 0.0f;
		bool transitionBatchesDirty = false;
		size_t transitionBatchCount = 0;
		std::vector<DebugLocationTransition> locationTransitions;
		std::vector<std::string> transitionApplyFailures;

		std::vector<DebugLayer> sceneLayers;
		std::vector<DebugLayer> weatherLayers;
		std::vector<DebugLayer> locationLayers;
		std::vector<DebugResolvedSetting> resolvedSettings;
		std::vector<std::string> applyFailures;
		std::vector<std::string> restoreFailures;
		std::vector<std::string> pausedFeatures;
	};

	/// Sample the full resolver state. Built on demand, only for the debug UI.
	DebugSnapshot GetDebugSnapshot() const;

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
	bool locationTransitionModified = false;
	bool dataLoaded = false;
	bool deferredSceneChangesPending = false;
	std::chrono::steady_clock::time_point deferredSceneChangesDeadline{};
	int deferredSaveFailures = 0;
	static constexpr auto kDeferredSaveDelay = std::chrono::milliseconds(250);
	static constexpr auto kDeferredSaveRetryDelay = std::chrono::seconds(2);
	/// A permanently locked file (MO2 VFS, antivirus, read-only install) must not log every retry forever.
	static constexpr int kMaxDeferredSaveRetries = 5;

	std::atomic<bool> queuedCellTransition = false;

	/// Float epsilon - changes smaller than this skip the LoadSettings call.
	static constexpr float kBlendEpsilon = 1e-3f;

	/// Minimum game-hour delta before re-running the blend. At the default
	/// timescale (20x), this equals about 0.18 real seconds.
	static constexpr float kHourUpdateThreshold = 1e-3f;

	/// Location transitions are the one per-frame path, and each tick costs a full
	/// SaveSettings/LoadSettings round trip per feature. A smoothstep blend is indistinguishable
	/// at 30 Hz, so the tick is decoupled from the frame rate.
	static constexpr float kLocationTransitionTickInterval = 1.0f / 30.0f;

	// --- Pause states ---
	std::map<std::string, bool> featurePauseStates;
	int sceneLayerSuspendDepth = 0;

	// --- Per-Weather Scene storage ---
	std::map<RE::FormID, WeatherSceneConfig> weatherSceneConfigs;
	static const WeatherSceneConfig kEmptyWeatherConfig;

	/// UI preference per weather: show TOD table vs flat view (keyed by FormID for fast access).
	std::map<RE::FormID, bool> weatherShowTimeOfDay;
	json unresolvedWeatherUserSettings = json::object();
	bool weatherDataLoaded = false;

	// --- Per-Location Scene storage ---
	std::map<std::string, LocationSceneConfig> locationSceneConfigs;
	static const LocationSceneConfig kEmptyLocationConfig;
	json unresolvedLocationUserSettings = json::object();
	bool locationDataLoaded = false;
	bool gameDataReady = false;
	float locationTransitionSeconds = kDefaultLocationTransitionSeconds;

	struct SettingAddress
	{
		std::string featureShortName;
		std::vector<std::string> settingPath;
		std::string settingKey;

		auto operator<=>(const SettingAddress&) const = default;
	};
	struct CatalogSceneSettingUpdate
	{
		std::vector<std::string> settingPath;
		std::string key;
		json value;
	};

	using ResolvedSettingMap = std::map<SettingAddress, json>;
	ResolvedSettingMap baselineSettings;
	ResolvedSettingMap appliedSettings;
	/// Reused across resolves so the per-frame path does not reallocate the map.
	ResolvedSettingMap resolvedSettingsScratch;
	std::set<std::string> restoreFailureWarnings;
	std::map<std::string, std::chrono::steady_clock::time_point> restoreRetryAfter;
	struct ApplyFailureState
	{
		size_t signature = 0;
		std::chrono::steady_clock::time_point retryAfter{};
		bool warningLogged = false;
	};
	std::map<std::string, ApplyFailureState> applyFailures;
	std::map<std::string, ApplyFailureState> transitionApplyFailures;
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

	/// One float easing from its pre-location value to the location override, or back.
	struct LocationTransition
	{
		float startValue = 0.0f;
		float targetValue = 0.0f;
		float startTime = 0.0f;
		float duration = 0.0f;
		bool restoreAtEnd = false;
	};
	/// A feature's in-flight transitions, pushed as one LoadSettings call per tick.
	struct LocationTransitionBatch
	{
		std::vector<SettingAddress> addresses;
		/// Points into activeLocationTransitions, which must stay node-based for these to survive
		/// the erases the batch loop performs while iterating.
		std::vector<LocationTransition*> transitions;
		std::vector<CatalogSceneSettingUpdate> updates;
		size_t signature = 0;
	};
	std::map<SettingAddress, LocationTransition> activeLocationTransitions;
	std::map<std::string, LocationTransitionBatch> locationTransitionBatches;
	bool locationTransitionBatchesDirty = true;
	float lastLocationTransitionTick = -1.0f;
	ResolvedSettingMap lastLocationOverrideValues;
	std::map<SettingAddress, float> lastLocationTransitionDurations;
	std::map<SettingAddress, float> pendingLocationTransitionDurations;
	ResolvedSettingMap cachedLocationOverrides;
	bool cachedLocationOverridesValid = false;
	bool locationOverridesDirty = true;

	/// Feature settings as they were before the scene layer, so baselines cost one SaveSettings each.
	std::map<std::string, json> featureBaseSnapshots;
	std::set<std::string> appliedFeatureNames;
	mutable std::set<std::string> configuredFeatureNamesCache;
	mutable std::uint64_t configuredFeatureNamesRevision = std::numeric_limits<std::uint64_t>::max();

	/// Period containing a game hour, with that hour normalized into the period's range.
	struct PeriodLookup
	{
		int index = -1;
		float hour = 0.0f;
	};
	static PeriodLookup FindPeriodForHour(float hour);

	// --- Per-Weather helpers ---
	/// Load weather overwrites/user settings once game data is available for SPID resolution.
	bool TryEnsureWeatherDataLoaded();
	bool TryEnsureLocationDataLoaded();
	void LoadWeatherData();
	WeatherSceneConfig& GetWeatherConfigMut(RE::FormID weatherId);
	RE::FormID GetEffectivePreviousWeatherId(const RE::Sky* sky, float weatherLerp) const;
	WeatherBlend GetWeatherBlend() const;
	float GetTimeOfDayPeriodFallbackFloat(float baseValue, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey, int periodIndex) const;

	/// Live sky/time state sampled once per resolve so every setting blends against identical factors.
	struct BlendSnapshot
	{
		WeatherBlend weather;
		std::array<float, kPeriodCount> timeOfDayFactors{};
	};
	/// Resamples the blend inputs. Call once at the top of any pass that runs the resolvers.
	void RefreshBlendSnapshot(bool interior);
	BlendSnapshot blendSnapshot;

	/// Active per-period values for one address, indexed by period.
	using PeriodValues = std::array<std::optional<float>, kPeriodCount>;
	using PeriodSettingMap = std::map<SettingAddress, PeriodValues>;
	struct CachedPeriodSettingMap
	{
		std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
		PeriodSettingMap values;
	};
	/// Bumped whenever an entry's value, pause state or membership changes.
	std::uint64_t sceneValueRevision = 0;
	mutable CachedPeriodSettingMap timeOfDayValueGroups;
	mutable std::map<RE::FormID, CachedPeriodSettingMap> weatherValueGroups;
	/// Groups an entry list's per-period floats by address, dropping anything unresolvable.
	void CollectPeriodValueGroups(const std::vector<SettingEntry>& sourceEntries, PeriodSettingMap& values) const;
	const PeriodSettingMap& BuildTimeOfDayValueGroups() const;
	const PeriodSettingMap& BuildWeatherValueGroups(RE::FormID weatherId) const;

	// --- Central runtime resolver ---
	void ResolveAndApply(bool force = false);
	bool HasActiveSceneEntriesCached();
	/// @param interior Passed down from the caller's resolve, which already sampled it.
	ResolvedSettingMap& BuildResolvedSettings(bool collectLocationTransitionDurations, bool interior);
	void ApplyResolvedSettings(const ResolvedSettingMap& resolved, bool forceRetry);
	void RestoreAppliedSettings();
	void ResolveInteriorSettings(ResolvedSettingMap& resolved) const;
	void ResolveTimeOfDaySettings(ResolvedSettingMap& resolved, const PeriodSettingMap& values) const;
	void ResolveWeatherSettings(ResolvedSettingMap& resolved, const PeriodSettingMap& timeOfDayValues) const;
	void ResolveLocationSettings(ResolvedSettingMap& resolved,
		const std::vector<LocationTarget>& locationTargets, bool collectTransitionDurations);
	void OverlayEntries(ResolvedSettingMap& resolved, const std::vector<SettingEntry>& sourceEntries,
		SceneType type, EntrySource source,
		std::map<SettingAddress, float>* transitionDurations = nullptr) const;
	/// Overlay both sources, shipped overwrites first so user entries win.
	void OverlayAllEntries(ResolvedSettingMap& resolved, const std::vector<SettingEntry>& sourceEntries,
		SceneType type, std::map<SettingAddress, float>* transitionDurations = nullptr) const;
	std::optional<float> ResolveWeatherLowerValue(RE::FormID weatherId, const SettingAddress& address,
		TimeOfDayPeriod period, EntrySource selectedSource);
	json GetBaselineValue(const SettingAddress& address);
	/// Feature settings with the live scene layer folded back out, cached until invalidated.
	const json* GetFeatureBaseSnapshot(const std::string& featureShortName);
	void EnsureBaselines(std::span<const SettingAddress> addresses);
	void InvalidateFeatureSnapshot(std::string_view featureShortName = {});
	/// Forget a feature once its last applied setting is gone, so it stops counting as scene-driven.
	void PruneAppliedFeatureName(const std::string& featureShortName);
	std::optional<json> ResolveLocationLowerValue(LocationTargetType type, std::string_view formKey,
		const SettingAddress& address, EntrySource selectedSource);
	/// Resolve everything a location target sits on top of, or null when the target is unreachable.
	std::optional<ResolvedSettingMap> BuildLocationLowerLayers(LocationTargetType type,
		std::string_view formKey, std::optional<EntrySource> selectedSource = std::nullopt);

	// --- Generic Scene Copy ---
	/// The entry that wins for each setting in a context, user over overwrite.
	using EffectiveContextEntries = std::map<SettingIdentity, const SettingEntry*>;
	static bool IsValidSceneContext(const SceneContextId& context);
	static bool IsSameSceneContext(const SceneContextId& lhs, const SceneContextId& rhs);
	static EffectiveContextEntries BuildEffectiveContextEntries(
		const std::vector<SettingEntry>& contextEntries, const SceneContextId& context);
	const std::vector<SettingEntry>* GetCopyContextEntries(const SceneContextId& context) const;
	std::vector<SettingEntry>* GetContextEntriesMut(const SceneContextId& context);
	/// One presentation bump, one mark and one save for a mutation spanning a whole context.
	void CommitContextUserEntryMutation(const SceneContextId& context);
	std::vector<CopyCandidate> BuildCopyCandidates(const SceneContextId& source,
		const SceneContextId& destination, CopyScope scope,
		const std::optional<SettingIdentity>& selectedSetting, PeriodScope periodScope) const;
	/// Deferring the commit lets a fan-out over the periods land as one save.
	CopyResult CopySettingsToContext(const SceneContextId& source, const SceneContextId& destination,
		CopyConflictPolicy conflictPolicy, CopyScope scope,
		const std::optional<SettingIdentity>& setting, bool deferCommit);
	static bool ResolvedValuesEqual(const json& lhs, const json& rhs);
	static size_t GetCatalogUpdateSignature(std::string_view featureShortName,
		std::span<const CatalogSceneSettingUpdate> updates);
	static bool ApplyCatalogSceneSettings(
		Feature& feature, const std::vector<CatalogSceneSettingUpdate>& updates);

	// --- Location float transitions ---
	/// Seconds on the game clock, so transitions freeze with the game rather than the wall clock.
	float GetPauseAwareTime() const;
	/// Smoothstep position of a transition at the given time.
	static float EaseLocationTransition(const LocationTransition& transition, float now);
	static bool IsLocationTransitionFinished(const LocationTransition& transition, float now);
	void StartLocationTransitions(const ResolvedSettingMap& resolved, float now, bool animateChanges);
	bool AdvanceLocationTransitions(float now);
	/// Drop transitions the main apply already landed on, restoring the ones that eased back out.
	void RetireFinishedLocationTransitions(float now);
	void RebuildLocationTransitionBatches();
	void ClearLocationTransitions();

	// --- Per-Location helpers ---
	static std::string GetLocationConfigKey(LocationTargetType type, std::string_view formKey);
	/// User-document section holding the targets of this type.
	static const char* GetLocationSectionName(LocationTargetType type);
	LocationSceneConfig& GetLocationConfigMut(LocationTargetType type, const std::string& formKey,
		const std::string& name = {});
	/// Upserts a target's identity and claims it for the user, shared by adding a target and its first setting.
	LocationSceneConfig& EnsureAuthoredLocationConfig(LocationTargetType type, const std::string& formKey,
		const std::string& name, const std::string& cocCode, const std::string& editorId = {});
	/// Raw user-document keys that resolve to one target: a form can be spelled several ways in the file.
	static std::vector<std::string> MatchingRawLocationKeys(const json& section, LocationTargetType type,
		std::string_view formKey);
	void DiscoverLocationOverwrites();
	void DiscoverLocationOverwritesForTarget(const std::filesystem::path& targetDir);
	void LoadLocationUserSettings(const json& data);
	void PrepareLocationUserSettingsMutation(LocationTargetType type, std::string_view formKey,
		bool replaceMalformedEntries);

	// --- Helpers ---
	std::vector<SettingEntry>& GetEntriesMut(SceneType type);
	void BumpEntryPresentationRevision();
	/// Entry values changed: drop the per-period caches and re-resolve the location layer.
	void MarkSceneValuesDirty();
	bool IsEntryActive(const SettingEntry& entry) const;
	/// Active, catalog-permitted and, for TimeOfDay, transitionable float entry.
	bool IsResolvableEntry(const SettingEntry& entry, SceneType type) const;
	static SettingAddress GetEntryAddress(const SettingEntry& entry);
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
