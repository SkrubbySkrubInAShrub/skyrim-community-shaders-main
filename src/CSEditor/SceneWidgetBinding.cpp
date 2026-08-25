#include "SceneWidgetBinding.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <map>
#include <tuple>
#include <utility>

#include <imgui_internal.h>

#include "../I18n/I18n.h"
#include "Menu.h"
#include "SceneWidgetInterceptor.h"
#include "Utils/Game.h"
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
	using GutterKey = std::pair<ImGuiID, const void*>;
	std::map<GutterKey, int> gutterCalls;
	std::map<GutterKey, int> previousGutterCalls;

	/// Last call against a (window, address) pair in a frame owns its gutter, so a radio group draws
	/// a single toggle and draws it past its final button rather than crowding the first. The group
	/// size only becomes known once a frame has drawn it, so its first frame falls back to the first
	/// call and settles on the next.
	bool ClaimGutter(const void* address)
	{
		if (const auto frame = ImGui::GetFrameCount(); frame != gutterFrame) {
			gutterFrame = frame;
			previousGutterCalls.swap(gutterCalls);
			gutterCalls.clear();
		}
		const GutterKey key{ ImGui::GetCurrentWindow()->ID, address };
		const auto call = ++gutterCalls[key];
		const auto previous = previousGutterCalls.find(key);
		return previous == previousGutterCalls.end() ? call == 1 : call == previous->second;
	}

	using SceneContextId = SceneSettingsManager::SceneContextId;
	using SceneContextType = SceneSettingsManager::SceneContextType;
	using SettingLayer = SceneSettingsManager::SettingLayer;

	/// Which layer supplies one address, per period. An aperiodic context fills every slot, so a
	/// periodic page above it reads its own period out of the same row.
	using PeriodLayers = std::array<SettingLayer, kPeriodCount>;
	using LayerIndex = std::map<SceneSettingsManager::SettingIdentity, PeriodLayers>;

	/// The layers a page sits on top of, highest first: weather resolves over time of day, and a
	/// location over whichever stack is running. Interior and the exterior stack never resolve at the
	/// same time, so the live cell picks the one a location sits on, exactly as the resolver does.
	/// Periods are filled in because a context is only fetchable with a valid one.
	std::vector<SceneContextId> CollectLowerContexts(const SceneContextId& a_page)
	{
		std::vector<SceneContextId> lower;
		if (a_page.type == SceneContextType::Location && Util::IsInterior()) {
			lower.push_back({ .type = SceneContextType::Interior });
			return lower;
		}

		switch (a_page.type) {
		case SceneContextType::Location:
			if (const auto* sky = globals::game::sky; sky && sky->currentWeather)
				lower.push_back({ .type = SceneContextType::Weather,
					.period = SceneSettingsManager::kPeriods[0],
					.weatherId = sky->currentWeather->GetFormID() });
			[[fallthrough]];
		case SceneContextType::Weather:
			lower.push_back({ .type = SceneContextType::TimeOfDay, .period = SceneSettingsManager::kPeriods[0] });
			break;
		default:
			break;
		}
		return lower;
	}

	/// Fills one feature's rows from a context, highest-ranking source last so a user entry shadows
	/// the overwrite it was authored over.
	void CollectContextLayers(const SceneContextId& a_context, const std::string& a_feature, LayerIndex& a_index)
	{
		using EntrySource = SceneSettingsManager::EntrySource;
		auto* manager = SceneSettingsManager::GetSingleton();

		for (const auto source : { EntrySource::Overwrite, EntrySource::User }) {
			for (const auto& entry : manager->GetContextEntries(a_context)) {
				if (entry.source != source || entry.paused || entry.featureShortName != a_feature)
					continue;
				auto layer = source == EntrySource::Overwrite ? SettingLayer::Overwrite : SettingLayer::User;
				if (entry.deleted)
					layer = SettingLayer::Deleted;

				auto& row = a_index[{ entry.featureShortName, entry.settingPath, entry.settingKey }];
				if (const auto period = static_cast<size_t>(entry.period); period < static_cast<size_t>(kPeriodCount))
					row[period] = layer;
				else
					row.fill(layer);
			}
		}
	}

	/// Layers supplying one feature's addresses in one context. Every control on a page asks this of
	/// the same entries, and the walk is the whole context, so it is cached until the entries change.
	const LayerIndex& GetContextLayerIndex(const SceneContextId& a_context, const std::string& a_feature)
	{
		struct CachedIndex
		{
			bool built = false;
			std::uint64_t revision = 0;
			LayerIndex layers;
		};
		static std::map<std::pair<SceneContextId, std::string>, CachedIndex> cache;

		auto* manager = SceneSettingsManager::GetSingleton();
		const auto revision = manager->GetEntryPresentationRevision();
		auto& cached = cache[{ a_context, a_feature }];
		if (!cached.built || cached.revision != revision) {
			cached.built = true;
			cached.revision = revision;
			cached.layers.clear();
			// Nothing a paused feature holds applies, so its every context reads as empty.
			if (!manager->IsFeaturePaused(a_feature))
				CollectContextLayers(a_context, a_feature, cached.layers);
		}
		return cached.layers;
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

void SceneWidgetBinding::WriteScalarValue(void* a_destination, ImGuiDataType a_type, double a_value)
{
	if (const auto traits = GetScalarTraits(a_type); traits.write)
		traits.write(a_destination, a_value);
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

	// A Util:: wrapper may hand ImGui a rescaled temporary, so the member it stands for is what
	// resolves and what gets persisted.
	const auto* proxy = SceneWidgetInterceptor::GetArmedProxy();
	if (proxy)
		widgetScale = proxy->displayScale;

	metadata = SceneSettingsCatalog::FindSettingForControl(
		context->feature, proxy ? proxy->member : value.data);
	if (!metadata) {
		// Not a catalogued setting at all (e.g. a plain UI toggle like "Show Advanced"), so the
		// interceptor has nothing to bind. Left live rather than greyed: it never promised an
		// override in the first place.
		state = State::Unsupported;
		return;
	}
	if (!SceneSettingsManager::IsSceneSettingAllowed(
			metadata->featureShortName, metadata->settingPath, metadata->settingKey)) {
		// Barred by policy, so no scene will ever hold it. Greyed rather than left live, because an
		// edit here would rewrite the feature's base value from a panel that only promises overrides.
		state = State::Unbound;
		metadata = nullptr;
		OpenDisabled();
		return;
	}

	contextId = context->contextId;
	featureShortName = std::string{ metadata->featureShortName };
	settingPath = SceneSettingsManager::SplitSettingPath(metadata->settingPath);

	// Interior and Location store one entry with no period, so only a periodic context can fan out.
	const bool periodic = SceneSettingsManager::IsPeriodicContext(contextId.type);
	flatAcrossPeriods = periodic && !context->perPeriod;
	if (const auto period = static_cast<int>(contextId.period);
		periodic && period >= 0 && period < kPeriodCount)
		armedSlot = period;

	ResolveComponents();
	if (components.empty()) {
		// A scene setting this context cannot hold, e.g. a non-transitionable one under time of day.
		state = State::Unavailable;
		OpenDisabled();
		return;
	}

	ResolveState();
	if (state == State::Paused) {
		// The greyed control shows what is stored, not what the scene is currently running.
		StoreHoldingValue();
		OpenDisabled();
	} else if (IsDrivenFromBelow()) {
		// A layer beneath this page owns the address, so editing here would author against a value
		// this page never resolves. The gutter stays live so the override can still be captured.
		OpenDisabled();
	}
	if (mixedAcrossPeriods && value.kind == Kind::Bool) {
		// Only Checkbox honours the flag; every other kind gets the tinted gutter instead.
		ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
		mixedFlagPushed = true;
	}

	// The control itself carries the provenance too, so a slider reads without tracing back to its gutter.
	if (const auto tint = ResolveProvenanceColor()) {
		Util::PushTintedFrameStyle(*tint);
		tintPushed = true;
	}
}

SceneWidgetBinding::Guard::~Guard()
{
	// Finish always runs on the happy path; this only closes scopes an exception skipped.
	if (tintPushed)
		Util::PopTintedFrameStyle();
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
	lowerLayer = SceneSettingsManager::SettingLayer::None;

	bool anyActive = false;
	bool anyPaused = false;
	bool anyMissing = false;
	bool anyDeleted = false;
	bool anyLiveValue = false;  // a covered slot holding a real value: unpaused and not a tombstone

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
			// A paused tombstone suppresses nothing, so the state must not claim the value is gone.
			if (entry.deleted && !entry.paused)
				anyDeleted = true;
			if (entry.paused)
				anyPaused = true;
			else
				anyActive = true;
			if (!entry.paused && !entry.deleted)
				anyLiveValue = true;
			// A tombstone's value is stale and meaningless; never compare against it.
			if (!entry.deleted) {
				if (!reference)
					reference = &entry.value;
				else if (*reference != entry.value)
					mixedAcrossPeriods = true;
			}
		}
	}

	// Nothing left to resolve, and the provenance query below needs a component to key off of.
	if (components.empty())
		return;

	// Provenance is a separate axis from state: it says who wins, state says what the user's own
	// entry is doing. Only the first component is queried; an aggregate shares one address family.
	winningLayer = ResolveWinningLayer(components.front().settingKey);

	if (!anyActive && !anyPaused) {
		state = winningLayer == SceneSettingsManager::SettingLayer::Overwrite ? State::Overwritten : State::Absent;
		// Nothing in this context holds the address, so a layer under this page may be driving it and
		// the page has to say so. A tombstone here already suppressed everything below.
		if (winningLayer == SceneSettingsManager::SettingLayer::None)
			lowerLayer = ResolveLowerLayer(components.front().settingKey);
		return;
	}

	// A partly covered or partly paused control is as mixed as one whose periods hold two values.
	state = anyDeleted ? State::Deleted : (anyActive ? State::Active : State::Paused);
	mixedAcrossPeriods = mixedAcrossPeriods || anyMissing || (anyActive && anyPaused) || (anyDeleted && anyLiveValue);

	for (const auto& component : components) {
		entryIndex = PrimaryEntry(component);
		if (entryIndex)
			break;
	}
}

SceneSettingsManager::SettingLayer SceneWidgetBinding::Guard::ResolveWinningLayer(
	const std::string& a_settingKey) const
{
	using SettingLayer = SceneSettingsManager::SettingLayer;
	auto* manager = SceneSettingsManager::GetSingleton();

	// Interior and location store one entry with no period; synthesising one would fail
	// IsValidSceneContext and silently answer None.
	if (!SceneSettingsManager::IsPeriodicContext(contextId.type))
		return manager->GetSettingProvenance(contextId, featureShortName, settingPath, a_settingKey).layer;

	bool sawUser = false, sawDeleted = false, sawOverwrite = false;
	for (int slot = 0; slot < kPeriodCount; ++slot) {
		if (!IsCoveredSlot(slot))
			continue;
		auto slotContext = contextId;
		slotContext.period = SceneSettingsManager::kPeriods[static_cast<size_t>(slot)];
		switch (manager->GetSettingProvenance(slotContext, featureShortName, settingPath, a_settingKey).layer) {
		case SettingLayer::User:
			sawUser = true;
			break;
		case SettingLayer::Deleted:
			sawDeleted = true;
			break;
		case SettingLayer::Overwrite:
			sawOverwrite = true;
			break;
		default:
			break;
		}
	}

	// First match wins: mirrors the "any" semantics ResolveState already uses for anyActive/anyPaused.
	if (sawUser)
		return SettingLayer::User;
	if (sawDeleted)
		return SettingLayer::Deleted;
	return sawOverwrite ? SettingLayer::Overwrite : SettingLayer::None;
}

SceneSettingsManager::SettingLayer SceneWidgetBinding::Guard::ResolveLowerLayer(
	const std::string& a_settingKey) const
{
	const SceneSettingsManager::SettingIdentity identity{ featureShortName, settingPath, a_settingKey };
	// A page with no period of its own sits above every period of a periodic layer beneath it.
	const bool everyPeriod = !SceneSettingsManager::IsPeriodicContext(contextId.type);

	for (const auto& lower : CollectLowerContexts(contextId)) {
		const auto& index = GetContextLayerIndex(lower, featureShortName);
		const auto row = index.find(identity);
		if (row == index.end())
			continue;

		bool sawDeleted = false, sawOverwrite = false;
		for (int slot = 0; slot < kPeriodCount; ++slot) {
			if (!everyPeriod && !IsCoveredSlot(slot))
				continue;
			switch (row->second[static_cast<size_t>(slot)]) {
			case SettingLayer::User:
				return SettingLayer::User;
			case SettingLayer::Deleted:
				sawDeleted = true;
				break;
			case SettingLayer::Overwrite:
				sawOverwrite = true;
				break;
			default:
				break;
			}
		}
		// A tombstone suppresses the layers under it and then supplies nothing itself, so this page
		// is left free to author here.
		if (sawDeleted)
			return SettingLayer::None;
		if (sawOverwrite)
			return SettingLayer::Overwrite;
	}
	return SettingLayer::None;
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
		else if (a_stored.is_number_integer())
			*reinterpret_cast<bool*>(holding.bytes) = a_stored.get<std::int64_t>() != 0;
		return;
	}
	if (!a_stored.is_number())
		return;

	const auto number = a_stored.get<double>() * widgetScale;
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
	if (value.kind == Kind::Bool) {
		// A checkbox over an int-backed member (uint flags cast to bool*) persists as an integer, or
		// validation rejects the edit and the entry keeps the base value the scene then re-applies.
		const bool checked = *static_cast<const bool*>(value.data);
		return a_component.setting->valueType == SceneSettingsCatalog::ValueType::Integer ?
		           json(static_cast<std::int64_t>(checked)) :
		           json(checked);
	}

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
	number /= widgetScale;

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
		// A tombstone's retained value supplies nothing, so the menu must not offer it as stored.
		if (!index || *index >= entries.size() || entries[*index].deleted)
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

std::optional<ImVec4> SceneWidgetBinding::Guard::ResolveProvenanceColor() const
{
	// A disagreement about the data outranks a statement about its source.
	if (mixedAcrossPeriods)
		return Util::Colors::GetWarning();
	// A layer below only answers once this page holds nothing, so the two never both apply.
	const auto layer = winningLayer != SceneSettingsManager::SettingLayer::None ? winningLayer : lowerLayer;
	if (layer == SceneSettingsManager::SettingLayer::Overwrite)
		return Util::Colors::GetInfo();
	if (layer == SceneSettingsManager::SettingLayer::User)
		return Util::Colors::GetSuccess();
	return std::nullopt;
}

const char* SceneWidgetBinding::Guard::ResolveStatusTooltip() const
{
	if (mixedAcrossPeriods)
		return T(TKEY("scene_override_mixed"),
			"Values differ across this control. Editing writes the same value to all of them.");
	if (lowerLayer == SceneSettingsManager::SettingLayer::Overwrite)
		return T(TKEY("scene_override_from_mod_below"),
			"A mod supplies this value from a scene layer below this one. Tick to pin your own here.");
	if (lowerLayer == SceneSettingsManager::SettingLayer::User)
		return T(TKEY("scene_override_from_user_below"),
			"Your override on a scene layer below this one supplies this value. Tick to pin one here too.");
	switch (state) {
	case State::Overwritten:
		return T(TKEY("scene_override_from_mod"),
			"A mod supplies this value. Tick to pin your own, or remove it to suppress the mod's.");
	case State::Deleted:
		return T(TKEY("scene_override_deleted"),
			"You removed the mod's value here. Tick or edit to take it back.");
	case State::Absent:
		return T(TKEY("scene_override_absent"),
			"No override here. Edit the control, or tick to capture the current value.");
	case State::Paused:
		return T(TKEY("scene_override_paused"), "Override stored but held back. Tick to apply it.");
	default:
		return T(TKEY("scene_override_active"),
			"Override applies here. Untick to hold it back without losing the value.");
	}
}

void SceneWidgetBinding::Guard::Commit()
{
	// Editing a killed value revives it: the tombstone goes first, or it would block the new entry.
	if (state == State::Deleted) {
		SetTombstoned(false);
		ForgetEntries();
	}

	if (!HasAllCoveredEntries()) {
		ValueStorage edited;
		std::memcpy(edited.bytes, value.data, valueSize);
		// The base must be back in the member before the manager snapshots its baseline, or the
		// edit itself is recorded as the feature's default.
		std::memcpy(value.data, preCall.bytes, valueSize);
		if (!EnsureEntries(true)) {
			// Nothing will hold the edit, so keeping it would silently rewrite the feature's base.
			state = State::Unsupported;
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

	// Anchors the pair to the right edge of the space the control left behind, which inside a table
	// is its own cell: a table clips each cell to its column, so reaching for the window's right
	// edge from a cell culls the gutter entirely.
	if (const auto avail = ImGui::GetContentRegionAvail().x; avail > gutterWidth + margin)
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - gutterWidth - margin);

	// The gutter fills solid where the control only tints: it reads as a state marker, not a hint.
	const auto frameColor = ResolveProvenanceColor();
	if (frameColor) {
		ImGui::PushStyleColor(ImGuiCol_FrameBg, *frameColor);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, *frameColor);
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, *frameColor);
	}
	const bool toggled = ImGui::Checkbox("##SceneOverride", &enabled);
	if (frameColor)
		ImGui::PopStyleColor(3);

	if (toggled) {
		auto* manager = SceneSettingsManager::GetSingleton();
		// Any gesture on a killed value means the user wants it back, so it captures like an absent one.
		if (state == State::Deleted) {
			SetTombstoned(false);
			ForgetEntries();
		}
		if (state == State::Absent || state == State::Overwritten) {
			// Ticking captures the feature's current value as the override.
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

	Util::AddTooltip(ResolveStatusTooltip());

	ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
	ImGui::BeginDisabled(!hasOverride);
	const bool removeClicked = hasRemoveIcon ?
	                                Util::ErrorImageButton("##SceneOverrideRemove", menu->uiIcons.deleteSettings.texture,
	                                    ImVec2(removeIconSize, removeIconSize)) :
	                                Util::ErrorTextButton(T(TKEY("scene_override_remove"), "Remove"));
	ImGui::EndDisabled();
	const char* removeTooltip = nullptr;
	if (state == State::Overwritten)
		removeTooltip = T(TKEY("scene_override_remove_mod_tooltip"),
			"Suppress the mod's value here. The mod's own file is left untouched.");
	else if (state == State::Deleted)
		removeTooltip = T(TKEY("scene_override_restore_mod_tooltip"), "Bring the mod's value back here.");
	else
		removeTooltip = T(TKEY("scene_override_remove_tooltip"), "Remove this override from the saved settings.");
	Util::AddTooltip(removeTooltip);
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

		// A tombstone has no value to revert to and nothing left to delete.
		if (state == State::Deleted) {
			if (ImGui::MenuItem(T(TKEY("scene_override_restore_mod"), "Restore the mod's value")))
				DeleteOverride();
		} else {
			if (ImGui::MenuItem(T(TKEY("scene_override_revert"), "Revert to original"))) {
				for (const auto index : CollectOwnedEntries())
					manager->RevertContextEntryToDefault(contextId, index);
				ResolveState();
			}
			if (ImGui::MenuItem(T(TKEY("scene_override_delete"), "Delete override")))
				DeleteOverride();
		}

		ImGui::EndPopup();
	}
	ImGui::PopID();
}

void SceneWidgetBinding::Guard::DeleteOverride()
{
	// Remove toggles a mod's value between suppressed and restored; its file is never touched.
	if (state == State::Overwritten || state == State::Deleted) {
		SetTombstoned(state == State::Overwritten);
		ResolveState();
		return;
	}

	// Removal renumbers the entries behind it, so drop the highest index first.
	auto owned = CollectOwnedEntries();
	std::ranges::sort(owned, std::greater{});
	for (const auto index : owned)
		SceneSettingsManager::GetSingleton()->RemoveContextSetting(contextId, index);
	ForgetEntries();
}

void SceneWidgetBinding::Guard::SetTombstoned(bool a_tombstoned)
{
	auto* manager = SceneSettingsManager::GetSingleton();

	// The manager is period-scoped, so a flat control has to name each period it covers itself.
	for (const auto& component : components) {
		for (int slot = 0; slot < kPeriodCount; ++slot) {
			if (!IsCoveredSlot(slot))
				continue;
			auto slotContext = contextId;
			if (flatAcrossPeriods)
				slotContext.period = SceneSettingsManager::kPeriods[static_cast<size_t>(slot)];
			if (a_tombstoned)
				manager->TombstoneContextSetting(slotContext, featureShortName, settingPath, component.settingKey);
			else
				manager->ClearContextTombstone(slotContext, featureShortName, settingPath, component.settingKey);
		}
	}
}

bool SceneWidgetBinding::Guard::Finish(bool a_changed)
{
	if (tintPushed) {
		Util::PopTintedFrameStyle();
		tintPushed = false;
	}
	if (mixedFlagPushed) {
		ImGui::PopItemFlag();
		mixedFlagPushed = false;
	}
	if (disabledOpened) {
		ImGui::EndDisabled();
		disabledOpened = false;
	}

	if (state == State::Unsupported)
		return a_changed;
	// Both were greyed, so neither took input: no gutter to own and nothing to commit.
	if (state == State::Unbound || state == State::Unavailable)
		return false;

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
	if (state != State::Unsupported && state != State::Absent && state != State::Overwritten && !dragging)
		DrawContextMenu();
	if (state != State::Unsupported && (policy == GutterPolicy::Owner || ClaimGutter(value.data)))
		DrawGutter();
	ImGui::GetCurrentContext()->LastItemData = controlItem;

	// The tint says only that something else holds this value; the gutter's words say what. Drawn
	// before the feature's own tooltip, which appends to the same window once the call returns.
	// A control greyed by a lower layer is exactly the one that has to explain itself, so hovering
	// it has to answer even while disabled.
	if (ResolveProvenanceColor())
		Util::AddTooltip(ResolveStatusTooltip(), ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled);

	// A paused control must never report a change: nothing behind it moved.
	return state == State::Paused ? false : a_changed;
}

void SceneWidgetBinding::Guard::OpenDisabled()
{
	ImGui::BeginDisabled();
	disabledOpened = true;
}

#undef I18N_KEY_PREFIX
