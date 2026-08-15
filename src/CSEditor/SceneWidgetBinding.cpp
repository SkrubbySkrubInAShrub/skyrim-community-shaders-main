#include "SceneWidgetBinding.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include <imgui_internal.h>

#include "../I18n/I18n.h"
#include "Menu.h"
#include "SceneWidgetInterceptor.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	using Kind = SceneWidgetBinding::Value::Kind;
	using SettingMetadata = SceneSettingsCatalog::SettingMetadata;

	/// Smaller than the location menu's delete icon (SceneSettingsUI.cpp): the gutter sits inline
	/// with a checkbox rather than a table row, so the icon needs to read as the lighter action.
	constexpr float kRemoveIconScale = 0.75f;

	/// Keeps the gutter off the window's scrollbar/border instead of sitting flush against it.
	constexpr float kGutterRightMargin = 8.0f;

	constexpr int kPeriodCount = SceneSettingsManager::kPeriodCount;

	/// Reads and writes one ImGui scalar through a double. A zero size means the type is not one
	/// the scalar widgets accept, which the binding treats as an unresolvable control.
	struct ScalarTraits
	{
		std::size_t size = 0;
		double (*read)(const void*) = nullptr;
		void (*write)(void*, double) = nullptr;
	};

	template <typename T>
	constexpr ScalarTraits MakeScalarTraits()
	{
		return { sizeof(T),
			[](const void* source) { return static_cast<double>(*static_cast<const T*>(source)); },
			[](void* destination, double number) { *static_cast<T*>(destination) = static_cast<T>(number); } };
	}

	ScalarTraits GetScalarTraits(ImGuiDataType type)
	{
		switch (type) {
		case ImGuiDataType_S8:
			return MakeScalarTraits<std::int8_t>();
		case ImGuiDataType_U8:
			return MakeScalarTraits<std::uint8_t>();
		case ImGuiDataType_S16:
			return MakeScalarTraits<std::int16_t>();
		case ImGuiDataType_U16:
			return MakeScalarTraits<std::uint16_t>();
		case ImGuiDataType_S32:
			return MakeScalarTraits<std::int32_t>();
		case ImGuiDataType_U32:
			return MakeScalarTraits<std::uint32_t>();
		case ImGuiDataType_S64:
			return MakeScalarTraits<std::int64_t>();
		case ImGuiDataType_U64:
			return MakeScalarTraits<std::uint64_t>();
		case ImGuiDataType_Float:
			return MakeScalarTraits<float>();
		case ImGuiDataType_Double:
			return MakeScalarTraits<double>();
		default:
			return {};
		}
	}

	/// Bytes the caller's storage occupies, or zero when the control cannot be bound.
	std::size_t ValueSize(const SceneWidgetBinding::Value& value)
	{
		switch (value.kind) {
		case Kind::Bool:
			return sizeof(bool);
		case Kind::Int:
			return sizeof(int);
		case Kind::Float:
		case Kind::FloatVector:
			return value.componentCount <= SceneWidgetBinding::Value::kMaxComponents ?
			           sizeof(float) * value.componentCount :
			           0;
		case Kind::Scalar:
			return GetScalarTraits(value.scalarType).size;
		default:
			return 0;
		}
	}

	using ComponentGroup = std::vector<const SettingMetadata*>;

	// Same grouping the manager uses to fold stored scalars back into one aggregate control, plus
	// settingPath so two controls over one serialized member stay apart.
	using AggregateKey = std::tuple<std::string_view, std::string_view, std::string_view,
		std::string_view, SceneSettingsCatalog::AggregateSemantic, std::int8_t, std::uint8_t>;

	AggregateKey MakeAggregateKey(const SettingMetadata& setting)
	{
		return { setting.featureShortName, setting.settingPath, setting.serializedPath,
			setting.serializedKey, setting.aggregateSemantic, setting.aggregateStart,
			setting.aggregateCount };
	}

	/// The catalog rows one control drives: its own row alone, or every sibling of its aggregate.
	ComponentGroup GetControlComponents(const SettingMetadata& setting)
	{
		// A single-component row is its own control even when it sits inside a vector member, so
		// the four sliders over one float4 must not be folded into one aggregate.
		if (setting.aggregateCount <= 1 || setting.aggregateAll)
			return { &setting };

		static const std::map<AggregateKey, ComponentGroup> groups = [] {
			std::map<AggregateKey, ComponentGroup> built;
			for (const auto& candidate : SceneSettingsCatalog::GetSettings()) {
				if (candidate.aggregateCount <= 1 || candidate.aggregateAll)
					continue;
				built[MakeAggregateKey(candidate)].push_back(&candidate);
			}
			for (auto& [key, group] : built)
				std::ranges::sort(group, {}, &SettingMetadata::serializedComponent);
			return built;
		}();

		const auto found = groups.find(MakeAggregateKey(setting));
		return found != groups.end() ? found->second : ComponentGroup{ &setting };
	}

	int gutterFrame = -1;
	// Keyed by window as well as address: two panels drawing the same feature must each get their
	// own gutter instead of the second panel finding the first's claim already taken.
	std::set<std::pair<ImGuiID, const void*>> gutterOwners;

	/// First call against a (window, address) pair in a frame owns its gutter, so one radio group
	/// still draws a single toggle while the same group in another window gets its own.
	bool ClaimGutter(const void* address)
	{
		if (const auto frame = ImGui::GetFrameCount(); frame != gutterFrame) {
			gutterFrame = frame;
			gutterOwners.clear();
		}
		return gutterOwners.insert({ ImGui::GetCurrentWindow()->ID, address }).second;
	}

	/// Same scene-type split the manager applies when persisting a new entry (AddContextSetting),
	/// so a control never resolves as allowed here and then fails to gain an entry on the gutter tick.
	SceneSettingsManager::SceneType SceneTypeForContext(SceneSettingsManager::SceneContextType a_type)
	{
		using SceneContextType = SceneSettingsManager::SceneContextType;
		using SceneType = SceneSettingsManager::SceneType;
		switch (a_type) {
		case SceneContextType::Interior:
			return SceneType::InteriorOnly;
		case SceneContextType::Location:
			return SceneType::Location;
		case SceneContextType::TimeOfDay:
		case SceneContextType::Weather:
		default:
			return SceneType::TimeOfDay;
		}
	}
}

SceneWidgetBinding::Guard::Guard(const char* a_label, const Value& a_value, GutterPolicy a_policy) :
	label(a_label), value(a_value), policy(a_policy)
{
	assert(value.data && "intercepted widget bound a null address");
	valueSize = ValueSize(value);
	assert(valueSize <= sizeof(ValueStorage::bytes));
	if (!value.data || valueSize == 0 || valueSize > sizeof(ValueStorage::bytes))
		return;

	// Captured before any early return, so entry creation always has the base value to restore.
	std::memcpy(preCall.bytes, value.data, valueSize);

	const auto* context = SceneWidgetInterceptor::GetArmedContext();
	if (!context)
		return;

	metadata = SceneSettingsCatalog::FindSettingForControl(context->feature, value.data);
	if (!metadata || !SceneSettingsManager::IsSceneSettingAllowed(
						 metadata->featureShortName, metadata->settingPath, metadata->settingKey)) {
		metadata = nullptr;
		return;
	}

	contextId = context->contextId;
	featureShortName = std::string{ metadata->featureShortName };
	settingPath = SceneSettingsManager::SplitSettingPath(metadata->settingPath);

	// Interior and Location store one entry with no period, so only these two can fan out.
	const bool periodic = contextId.type == SceneSettingsManager::SceneContextType::TimeOfDay ||
	                      contextId.type == SceneSettingsManager::SceneContextType::Weather;
	flatAcrossPeriods = periodic && !context->perPeriod;
	if (const auto period = static_cast<int>(contextId.period);
		periodic && period >= 0 && period < kPeriodCount)
		armedSlot = period;

	ResolveComponents();
	if (components.empty()) {
		metadata = nullptr;
		return;
	}

	ResolveState();
	if (state == State::Paused) {
		// The greyed control shows what is stored, not what the scene is currently running.
		StoreHoldingValue();
		ImGui::BeginDisabled();
		disabledOpened = true;
	}
	if (mixedAcrossPeriods && value.kind == Kind::Bool) {
		// Only Checkbox honours the flag; every other kind gets the tinted gutter instead.
		ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
		mixedFlagPushed = true;
	}
}

SceneWidgetBinding::Guard::~Guard()
{
	// Finish always runs on the happy path; this only closes scopes an exception skipped.
	if (mixedFlagPushed)
		ImGui::PopItemFlag();
	if (disabledOpened)
		ImGui::EndDisabled();
}

void* SceneWidgetBinding::Guard::Raw()
{
	return state == State::Paused ? static_cast<void*>(holding.bytes) : value.data;
}

bool* SceneWidgetBinding::Guard::Bool()
{
	return static_cast<bool*>(Raw());
}

int* SceneWidgetBinding::Guard::Int()
{
	return static_cast<int*>(Raw());
}

float* SceneWidgetBinding::Guard::Float()
{
	return static_cast<float*>(Raw());
}

const SceneSettingsManager::SettingEntry* SceneWidgetBinding::Guard::GetEntry() const
{
	if (!entryIndex)
		return nullptr;
	const auto entries = SceneSettingsManager::GetSingleton()->GetContextEntries(contextId);
	return *entryIndex < entries.size() ? &entries[*entryIndex] : nullptr;
}

void SceneWidgetBinding::Guard::ResolveComponents()
{
	// The resolver answers a control's base address with its first component, so an aggregate has
	// to walk out to its siblings: each one is a separate entry keyed by its own settingKey.
	const auto start = std::max<int>(metadata->aggregateStart, 0);
	const auto sceneType = SceneTypeForContext(contextId.type);

	for (const auto* setting : GetControlComponents(*metadata)) {
		// Siblings share featureShortName/settingPath (see MakeAggregateKey), so the guard's own
		// resolved values apply to every component here; only settingKey varies.
		if (!SceneSettingsManager::IsSettingAllowedForType(
				sceneType, featureShortName, settingPath, std::string{ setting->settingKey }))
			continue;

		const auto slot = setting->aggregateCount <= 1 ?
		                      0 :
		                      static_cast<int>(setting->serializedComponent) - start;
		if (slot < 0 || slot >= value.componentCount)
			continue;

		components.push_back(
			Component{ setting, std::string{ setting->settingKey }, static_cast<std::uint8_t>(slot), {} });
	}
}

void SceneWidgetBinding::Guard::ResolveState()
{
	auto* manager = SceneSettingsManager::GetSingleton();
	const auto entries = manager->GetContextEntries(contextId);

	entryIndex.reset();
	mixedAcrossPeriods = false;

	bool anyActive = false;
	bool anyPaused = false;
	bool anyMissing = false;

	for (auto& component : components) {
		component.periodEntries = manager->FindContextUserEntryPerPeriod(
			contextId, featureShortName, settingPath, component.settingKey);

		const json* reference = nullptr;
		for (int slot = 0; slot < kPeriodCount; ++slot) {
			auto& resolved = component.periodEntries[static_cast<size_t>(slot)];
			if (!IsCoveredSlot(slot)) {
				resolved.reset();
				continue;
			}
			if (!resolved || *resolved >= entries.size()) {
				resolved.reset();
				anyMissing = true;
				continue;
			}

			const auto& entry = entries[*resolved];
			if (entry.paused)
				anyPaused = true;
			else
				anyActive = true;
			if (!reference)
				reference = &entry.value;
			else if (*reference != entry.value)
				mixedAcrossPeriods = true;
		}
	}

	if (!anyActive && !anyPaused) {
		state = State::Absent;
		return;
	}

	// A partly covered or partly paused control is as mixed as one whose periods hold two values.
	state = anyActive ? State::Active : State::Paused;
	mixedAcrossPeriods = mixedAcrossPeriods || anyMissing || (anyActive && anyPaused);

	for (const auto& component : components) {
		entryIndex = PrimaryEntry(component);
		if (entryIndex)
			break;
	}
}

bool SceneWidgetBinding::Guard::IsCoveredSlot(int a_slot) const
{
	return flatAcrossPeriods || a_slot == armedSlot;
}

bool SceneWidgetBinding::Guard::HasAllCoveredEntries() const
{
	for (const auto& component : components)
		for (int slot = 0; slot < kPeriodCount; ++slot)
			if (IsCoveredSlot(slot) && !component.periodEntries[static_cast<size_t>(slot)])
				return false;
	return true;
}

std::optional<size_t> SceneWidgetBinding::Guard::PrimaryEntry(const Component& a_component) const
{
	if (const auto armed = a_component.periodEntries[static_cast<size_t>(armedSlot)])
		return armed;
	for (int slot = 0; slot < kPeriodCount; ++slot)
		if (const auto index = a_component.periodEntries[static_cast<size_t>(slot)])
			return index;
	return std::nullopt;
}

std::vector<size_t> SceneWidgetBinding::Guard::CollectOwnedEntries() const
{
	std::vector<size_t> owned;
	for (const auto& component : components)
		for (int slot = 0; slot < kPeriodCount; ++slot)
			if (const auto index = component.periodEntries[static_cast<size_t>(slot)])
				owned.push_back(*index);
	return owned;
}

void SceneWidgetBinding::Guard::ForgetEntries()
{
	for (auto& component : components)
		component.periodEntries = {};
	entryIndex.reset();
	state = State::Absent;
	mixedAcrossPeriods = false;
}

bool SceneWidgetBinding::Guard::EnsureEntries(bool a_deferSave)
{
	auto* manager = SceneSettingsManager::GetSingleton();

	bool added = false;
	for (const auto& component : components) {
		for (int slot = 0; slot < kPeriodCount; ++slot) {
			if (!IsCoveredSlot(slot) || component.periodEntries[static_cast<size_t>(slot)])
				continue;
			auto slotContext = contextId;
			if (flatAcrossPeriods)
				slotContext.period = SceneSettingsManager::kPeriods[static_cast<size_t>(slot)];
			const auto created = manager->AddContextSetting(slotContext, featureShortName,
				settingPath, component.settingKey, a_deferSave);
			added = added || created.has_value();
		}
	}

	// Insertion renumbers the entries behind it, so nothing may reuse the indices resolved earlier.
	if (added)
		ResolveState();
	return entryIndex.has_value();
}

void SceneWidgetBinding::Guard::StoreHoldingValue()
{
	// Components without an entry still read live, so start from the caller's value.
	std::memcpy(holding.bytes, value.data, valueSize);

	const auto entries = SceneSettingsManager::GetSingleton()->GetContextEntries(contextId);
	for (const auto& component : components) {
		const auto index = PrimaryEntry(component);
		if (index && *index < entries.size())
			WriteHoldingComponent(component, entries[*index].value);
	}
}

void SceneWidgetBinding::Guard::WriteHoldingComponent(const Component& a_component, const json& a_stored)
{
	if (value.kind == Kind::Bool) {
		if (a_stored.is_boolean())
			*reinterpret_cast<bool*>(holding.bytes) = a_stored.get<bool>();
		return;
	}
	if (!a_stored.is_number())
		return;

	const auto number = a_stored.get<double>();
	switch (value.kind) {
	case Kind::Int:
		*reinterpret_cast<int*>(holding.bytes) = static_cast<int>(number);
		break;
	case Kind::Float:
	case Kind::FloatVector:
		reinterpret_cast<float*>(holding.bytes)[a_component.widgetComponent] = static_cast<float>(number);
		break;
	case Kind::Scalar:
		GetScalarTraits(value.scalarType).write(holding.bytes, number);
		break;
	default:
		break;
	}
}

json SceneWidgetBinding::Guard::ReadEditedValue(const Component& a_component) const
{
	if (value.kind == Kind::Bool)
		return *static_cast<const bool*>(value.data);

	double number = 0.0;
	switch (value.kind) {
	case Kind::Int:
		number = *static_cast<const int*>(value.data);
		break;
	case Kind::Float:
	case Kind::FloatVector:
		number = static_cast<const float*>(value.data)[a_component.widgetComponent];
		break;
	case Kind::Scalar:
		number = GetScalarTraits(value.scalarType).read(value.data);
		break;
	default:
		return {};
	}

	// The catalog owns the persisted type, so an integer setting never lands as a float literal.
	return a_component.setting->valueType == SceneSettingsCatalog::ValueType::Integer ?
	           json(static_cast<std::int64_t>(number)) :
	           json(number);
}

std::string SceneWidgetBinding::Guard::DescribeStoredValue() const
{
	const auto entries = SceneSettingsManager::GetSingleton()->GetContextEntries(contextId);

	std::string description;
	for (const auto& component : components) {
		const auto index = PrimaryEntry(component);
		if (!index || *index >= entries.size())
			continue;
		if (!description.empty())
			description += ", ";
		description += entries[*index].value.dump();
	}
	return description;
}

std::vector<SceneSettingsManager::EntryValueUpdate> SceneWidgetBinding::Guard::BuildEntryValueUpdates() const
{
	std::vector<SceneSettingsManager::EntryValueUpdate> updates;
	for (const auto& component : components) {
		const auto edited = ReadEditedValue(component);
		if (edited.is_null())
			continue;
		for (int slot = 0; slot < kPeriodCount; ++slot)
			if (const auto index = component.periodEntries[static_cast<size_t>(slot)])
				updates.push_back({ *index, edited });
	}
	return updates;
}

void SceneWidgetBinding::Guard::Commit()
{
	if (!HasAllCoveredEntries()) {
		ValueStorage edited;
		std::memcpy(edited.bytes, value.data, valueSize);
		// The base must be back in the member before the manager snapshots its baseline, or the
		// edit itself is recorded as the feature's default.
		std::memcpy(value.data, preCall.bytes, valueSize);
		if (!EnsureEntries(true)) {
			// Nothing will hold the edit, so keeping it would silently rewrite the feature's base.
			state = State::Unbound;
			return;
		}
		std::memcpy(value.data, edited.bytes, valueSize);
	}

	SceneSettingsManager::GetSingleton()->UpdateContextEntryValues(
		contextId, BuildEntryValueUpdates(), commitDeferred);
}

void SceneWidgetBinding::Guard::DrawGutter()
{
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);

	bool enabled = state == State::Active;
	const bool hasOverride = state != State::Absent;

	ImGui::PushID(label);

	const auto& style = ImGui::GetStyle();
	auto* menu = Menu::GetSingleton();
	const bool hasRemoveIcon = menu && menu->uiIcons.deleteSettings.texture;
	const float removeIconSize = ImGui::GetFrameHeight() * kRemoveIconScale;
	const float removeWidth = hasRemoveIcon ?
	                               removeIconSize :
	                               ImGui::CalcTextSize(T(TKEY("scene_override_remove"), "Remove")).x +
	                                   style.FramePadding.x * 2.0f;
	const float gutterWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + removeWidth;
	const auto margin = kGutterRightMargin * Util::GetUIScale();

	// Anchors the checkbox/remove pair to the right edge of whatever space the control left behind.
	if (const auto avail = ImGui::GetContentRegionAvail().x; avail > gutterWidth + margin)
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - gutterWidth - margin);

	if (mixedAcrossPeriods) {
		const auto& mixedColor = Menu::GetSingleton()->GetTheme().StatusPalette.Warning;
		ImGui::PushStyleColor(ImGuiCol_FrameBg, mixedColor);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, mixedColor);
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, mixedColor);
	}
	const bool toggled = ImGui::Checkbox("##SceneOverride", &enabled);
	if (mixedAcrossPeriods)
		ImGui::PopStyleColor(3);

	if (toggled) {
		auto* manager = SceneSettingsManager::GetSingleton();
		if (state == State::Absent) {
			// Ticking an absent control captures the feature's current value as the override.
			// An aggregate fanned over six periods is 24 entries, so it saves once, not per entry.
			if (EnsureEntries(true))
				manager->SaveAllUserSettings();
		} else {
			// One click normalises a partly paused aggregate instead of inverting each entry.
			for (const auto index : CollectOwnedEntries()) {
				const auto entries = manager->GetContextEntries(contextId);
				if (index < entries.size() && entries[index].paused == enabled)
					manager->TogglePauseContextEntry(contextId, index);
			}
			ResolveState();
		}
	}

	const char* tooltip = nullptr;
	if (mixedAcrossPeriods)
		tooltip = T(TKEY("scene_override_mixed"),
			"Values differ across this control. Editing writes the same value to all of them.");
	else if (state == State::Absent)
		tooltip = T(TKEY("scene_override_absent"),
			"No override here. Edit the control, or tick to capture the current value.");
	else if (state == State::Paused)
		tooltip = T(TKEY("scene_override_paused"), "Override stored but held back. Tick to apply it.");
	else
		tooltip = T(TKEY("scene_override_active"),
			"Override applies here. Untick to hold it back without losing the value.");
	Util::AddTooltip(tooltip);

	ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
	ImGui::BeginDisabled(!hasOverride);
	const bool removeClicked = hasRemoveIcon ?
	                                Util::ErrorImageButton("##SceneOverrideRemove", menu->uiIcons.deleteSettings.texture,
	                                    ImVec2(removeIconSize, removeIconSize)) :
	                                Util::ErrorTextButton(T(TKEY("scene_override_remove"), "Remove"));
	ImGui::EndDisabled();
	Util::AddTooltip(T(TKEY("scene_override_remove_tooltip"), "Remove this override from the saved settings."));
	if (removeClicked)
		DeleteOverride();

	ImGui::PopID();
}

void SceneWidgetBinding::Guard::DrawContextMenu()
{
	ImGui::PushID(label);
	if (ImGui::BeginPopupContextItem("##SceneOverrideMenu", ImGuiPopupFlags_MouseButtonRight)) {
		auto* manager = SceneSettingsManager::GetSingleton();

		if (const auto stored = DescribeStoredValue(); !stored.empty()) {
			Util::Text::Disabled("%s", stored.c_str());
			ImGui::Separator();
		}

		if (ImGui::MenuItem(T(TKEY("scene_override_revert"), "Revert to original"))) {
			for (const auto index : CollectOwnedEntries())
				manager->RevertContextEntryToDefault(contextId, index);
			ResolveState();
		}
		if (ImGui::MenuItem(T(TKEY("scene_override_delete"), "Delete override")))
			DeleteOverride();

		ImGui::EndPopup();
	}
	ImGui::PopID();
}

void SceneWidgetBinding::Guard::DeleteOverride()
{
	// Removal renumbers the entries behind it, so drop the highest index first.
	auto owned = CollectOwnedEntries();
	std::ranges::sort(owned, std::greater{});
	for (const auto index : owned)
		SceneSettingsManager::GetSingleton()->RemoveContextSetting(contextId, index);
	ForgetEntries();
}

bool SceneWidgetBinding::Guard::Finish(bool a_changed)
{
	if (mixedFlagPushed) {
		ImGui::PopItemFlag();
		mixedFlagPushed = false;
	}
	if (disabledOpened) {
		ImGui::EndDisabled();
		disabledOpened = false;
	}

	if (state == State::Unbound)
		return a_changed;

	// Read the drag state before the menu or the gutter becomes the current item.
	const bool dragging = ImGui::IsItemActive();
	const bool settled = ImGui::IsItemDeactivatedAfterEdit();

	if (a_changed && state != State::Paused) {
		commitDeferred = dragging;
		Commit();
	} else if (settled && state == State::Active) {
		// One non-deferred pass so the debounced save always lands on release.
		commitDeferred = false;
		Commit();
	}

	// Feature code queries the last item after the call, so the control must stay the current one.
	const auto controlItem = ImGui::GetCurrentContext()->LastItemData;
	if (state != State::Unbound && state != State::Absent && !dragging)
		DrawContextMenu();
	if (state != State::Unbound && (policy == GutterPolicy::Owner || ClaimGutter(value.data)))
		DrawGutter();
	ImGui::GetCurrentContext()->LastItemData = controlItem;

	// A paused control must never report a change: nothing behind it moved.
	return state == State::Paused ? false : a_changed;
}

#undef I18N_KEY_PREFIX
