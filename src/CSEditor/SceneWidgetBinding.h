#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "SceneSettingsCatalog.generated.h"
#include "SceneSettingsManager.h"

/// Binds one intercepted ImGui control to a scene entry and owns its gutter toggle.
namespace SceneWidgetBinding
{
	/// The caller's storage, erased to the primitive kinds the catalog can persist.
	struct Value
	{
		/// Widest control the interceptor covers: ColorEdit4 and float4 aggregates.
		static constexpr std::uint8_t kMaxComponents = 4;

		enum class Kind : std::uint8_t
		{
			Bool,
			Int,
			Float,
			FloatVector,
			Scalar
		};

		Kind kind = Kind::Float;
		void* data = nullptr;
		std::uint8_t componentCount = 1;
		ImGuiDataType scalarType = ImGuiDataType_COUNT;

		static Value Bool(bool* a_data) { return { Kind::Bool, a_data, 1, ImGuiDataType_COUNT }; }
		static Value Int(int* a_data) { return { Kind::Int, a_data, 1, ImGuiDataType_COUNT }; }
		static Value Float(float* a_data) { return { Kind::Float, a_data, 1, ImGuiDataType_COUNT }; }
		static Value FloatVector(float* a_data, std::uint8_t a_count)
		{
			return { Kind::FloatVector, a_data, a_count, ImGuiDataType_COUNT };
		}
		static Value Scalar(void* a_data, ImGuiDataType a_type)
		{
			return { Kind::Scalar, a_data, 1, a_type };
		}
	};

	/// Writes a number into a scalar of the given ImGui data type; no-op for a type the intercepted
	/// scalar widgets don't accept. Shared so palette value drops write the same way the widgets do.
	void WriteScalarValue(void* a_destination, ImGuiDataType a_type, double a_value);

	/// Scratch for one widget value of any intercepted kind, sized for the widest control.
	struct ValueStorage
	{
		alignas(std::uint64_t) std::byte bytes[sizeof(float) * Value::kMaxComponents]{};
	};

	/// Whether this call owns the gutter. A radio group is several calls against one address, so
	/// its members defer ownership rather than drawing one toggle per button.
	enum class GutterPolicy : std::uint8_t
	{
		Owner,
		GroupMember
	};

	/// How the bound control resolved this frame. Kept public so the deferred winning/losing
	/// colouring can read it without reshaping the guard.
	enum class State : std::uint8_t
	{
		Unsupported,  // the interceptor cannot bind it: behaves exactly like the normal menu
		Unbound,      // no scene can hold it: greyed for good, never bindable anywhere
		Unavailable,  // scene-controllable, but not by this scene type: greyed, never bindable here
		Absent,       // scene-controllable, no entry yet
		Overwritten,  // no user entry: a mod's overwrite supplies the value
		Active,       // entry exists and applies
		Paused,       // entry exists and is held back
		Deleted       // a tombstone suppresses every lower layer at this address
	};

	/// Wraps one intercepted widget call for the duration of that call.
	class Guard
	{
	public:
		Guard(const char* a_label, const Value& a_value, GutterPolicy a_policy = GutterPolicy::Owner);
		~Guard();

		Guard(const Guard&) = delete;
		Guard& operator=(const Guard&) = delete;

		/// Pointer the real ImGui call must bind: the caller's storage, or the paused holding value.
		void* Raw();
		bool* Bool();
		int* Int();
		float* Float();

		/** @brief Closes the disabled scope, commits any edit, and draws the gutter and menu.
		 *  @return What the intercepted function should return: never true while paused. */
		bool Finish(bool a_changed);

		State GetState() const { return state; }
		std::optional<size_t> GetEntryIndex() const { return entryIndex; }
		const SceneSettingsCatalog::SettingMetadata* GetMetadata() const { return metadata; }
		const SceneSettingsManager::SceneContextId& GetContextId() const { return contextId; }

		/// The resolved entry, or nullptr when none exists. Valid until the manager mutates entries.
		const SceneSettingsManager::SettingEntry* GetEntry() const;

	private:
		/// Greys the control for the rest of the call, so an unbindable one cannot edit the base value.
		void OpenDisabled();

		/// One catalog component behind this control, and the entries that persist it.
		struct Component
		{
			const SceneSettingsCatalog::SettingMetadata* setting = nullptr;
			std::string settingKey;
			/// Component of the caller's storage this entry drives.
			std::uint8_t widgetComponent = 0;
			/// Owning entry per period; only the periods this control writes are filled.
			std::array<std::optional<size_t>, SceneSettingsManager::kPeriodCount> periodEntries{};
		};

		/// Collects the catalog components the control covers, each with the entries behind it.
		void ResolveComponents();
		/// Derives the state and the mixed flag from the resolved entries.
		void ResolveState();
		/// Provenance across every period this control covers, combined "any user wins" like
		/// ResolveState's anyActive/anyPaused.
		SceneSettingsManager::SettingLayer ResolveWinningLayer(const std::string& a_settingKey) const;

		/// Colour standing for where the value comes from, or nothing when the feature's base wins.
		/// Shared by the control's tint and the gutter's toggle so the two never disagree.
		std::optional<ImVec4> ResolveProvenanceColor() const;

		/// Sentence describing what holds this value, shared by the gutter's toggle and the control.
		const char* ResolveStatusTooltip() const;

		void Commit();
		void DrawGutter();
		void DrawContextMenu();

		/// Drops every entry this control owns, shared by the gutter's remove button and the
		/// context menu's "Delete override" item.
		void DeleteOverride();

		/** @brief Tombstones or clears every covered period of every component this control spans.
		 *  @param a_tombstoned Whether the address ends up suppressed. */
		void SetTombstoned(bool a_tombstoned);

		/// Whether a period slot is one this control reads and writes.
		bool IsCoveredSlot(int a_slot) const;
		bool HasAllCoveredEntries() const;

		/** @brief Creates the entries the covered periods are still missing.
		 *  @return Whether the control owns at least one entry afterwards. */
		bool EnsureEntries(bool a_deferSave);

		/// The entry a component displays: the armed period's, or the first period holding one.
		std::optional<size_t> PrimaryEntry(const Component& a_component) const;

		/// Every entry this control owns, across components and periods.
		std::vector<size_t> CollectOwnedEntries() const;

		/// Drops the resolved entries once they are deleted, so the rest of the frame reads Absent.
		void ForgetEntries();

		/// Loads the stored override into the holding storage the paused control is bound to.
		void StoreHoldingValue();
		void WriteHoldingComponent(const Component& a_component, const json& a_stored);

		/// The caller's post-call storage, as the primitive one component persists.
		json ReadEditedValue(const Component& a_component) const;

		/// The stored override on one line; an aggregate lists every component.
		std::string DescribeStoredValue() const;

		std::vector<SceneSettingsManager::EntryValueUpdate> BuildEntryValueUpdates() const;

		const char* label;
		Value value;
		GutterPolicy policy;
		State state = State::Unsupported;
		/// Layer winning at this address, which drives the colour independently of `state`. A paused
		/// user entry stays Paused so the checkbox has something to resume, but reads as the mod's.
		SceneSettingsManager::SettingLayer winningLayer = SceneSettingsManager::SettingLayer::None;
		std::size_t valueSize = 0;
		/// Widget value = persisted value * widgetScale; only a proxied control scales.
		float widgetScale = 1.0f;

		const SceneSettingsCatalog::SettingMetadata* metadata = nullptr;
		SceneSettingsManager::SceneContextId contextId;
		std::optional<size_t> entryIndex;

		// Resolved once so the commit path does not re-split the catalog address every frame.
		std::string featureShortName;
		std::vector<std::string> settingPath;
		std::vector<Component> components;

		/// Period slot the armed context edits; 0 for a context that has no periods.
		int armedSlot = 0;
		/// One edit writes every period, which is what "time of day off" means.
		bool flatAcrossPeriods = false;
		/// The periods and components this control spans do not agree on a value or on coverage.
		bool mixedAcrossPeriods = false;

		/// Captured unconditionally so entry creation can restore the base before it is snapshotted.
		ValueStorage preCall;
		/// Storage the paused control is bound to, so no write reaches the feature member.
		ValueStorage holding;

		bool commitDeferred = false;
		bool disabledOpened = false;
		bool mixedFlagPushed = false;
		bool tintPushed = false;
	};
}
