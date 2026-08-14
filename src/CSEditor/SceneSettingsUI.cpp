#include "SceneSettingsUI.h"

#include <algorithm>
#include <format>

#include "../I18n/I18n.h"
#include "EditorWindow.h"
#include "Menu.h"
#include "SceneSettingsManager.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	using TimeOfDayPeriod = SceneSettingsManager::TimeOfDayPeriod;
	constexpr int kPeriodCount = SceneSettingsManager::kPeriodCount;

	/// Game-hour delta that counts as a deliberate scrub. Deltas below it accumulate rather than
	/// reset the baseline, so time running at any timescale still re-couples the bar.
	constexpr float kScrubEpsilon = 1e-3f;

	/// Period labels are centered so the row reads as one segmented control.
	constexpr ImVec2 kSegmentTextAlign{ 0.5f, 0.5f };

	constexpr ImGuiTableFlags kPeriodBarFlags =
		ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV;

	/// Starting width of the weather tab's feature column at the baseline font size; the user can drag it.
	constexpr float kFeatureListWidth = 180.0f;

	/// The divider doubles as the resize grip, so the feature column and the panel share one border.
	constexpr ImGuiTableFlags kFeatureLayoutFlags =
		ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;

	/// The objects window nests its list inside the category list, so it reads as a sub-level.
	constexpr float kNestedFeatureFontScale = 0.85f;

	/// One bar shared by every panel: it tracks live time, which is global.
	struct PeriodBarState
	{
		int selected = -1;       // -1 follows the live period
		float lastHour = -1.0f;  // game hour the coupling was last judged against
	};
	PeriodBarState periodBar;

	/// Off by default: the bar only takes over time, and pauses the game, once the user asks for it.
	bool timeOfDayEnabled = false;

	/// A selectable feature, with its label built once because the list is fixed after boot.
	struct FeatureListEntry
	{
		std::string shortName;
		std::string label;
	};

	/// Each panel keeps its own selection: both can be on screen at once.
	std::string weatherSelectedFeature;
	std::string panelSelectedFeature;

	// Latch driving the automatic time pause.
	bool panelVisible = false;
	bool wasEditingTimeOfDay = false;
	bool pausedByPanel = false;

	const char* GetPeriodLabel(int period)
	{
		switch (static_cast<TimeOfDayPeriod>(period)) {
		case TimeOfDayPeriod::Dawn:
			return T(TKEY("tod_dawn"), "Dawn");
		case TimeOfDayPeriod::Sunrise:
			return T(TKEY("tod_sunrise"), "Sunrise");
		case TimeOfDayPeriod::Day:
			return T(TKEY("tod_day"), "Day");
		case TimeOfDayPeriod::Sunset:
			return T(TKEY("tod_sunset"), "Sunset");
		case TimeOfDayPeriod::Dusk:
			return T(TKEY("tod_dusk"), "Dusk");
		default:
			return T(TKEY("tod_night"), "Night");
		}
	}

	/// Draws the period bar with its enable toggle and returns the period the panel below it should edit.
	int DrawPeriodBar()
	{
		const int live = static_cast<int>(SceneSettingsManager::GetCurrentPeriod());

		if (timeOfDayEnabled) {
			const float hour = SceneSettingsManager::GetCurrentGameHour();
			if (std::abs(hour - periodBar.lastHour) > kScrubEpsilon) {
				periodBar.selected = -1;
				periodBar.lastHour = hour;
			}
		} else if (periodBar.selected < 0) {
			// Pin the follow state so a disabled bar stops reacting to time entirely.
			periodBar.selected = live;
		}

		const int active = periodBar.selected < 0 ? live : periodBar.selected;

		ImGui::BeginDisabled(!timeOfDayEnabled);
		ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, kSegmentTextAlign);
		if (ImGui::BeginTable("PeriodBar", kPeriodCount, kPeriodBarFlags)) {
			for (int period = 0; period < kPeriodCount; ++period) {
				ImGui::TableNextColumn();
				// Colour the live period only when the selection has left it, so the bar always
				// shows where time actually is.
				const bool marksLive = timeOfDayEnabled && period == live && period != active;
				if (marksLive)
					ImGui::PushStyleColor(ImGuiCol_Text, Menu::GetSingleton()->GetTheme().StatusPalette.CurrentHotkey);
				if (ImGui::Selectable(GetPeriodLabel(period), period == active))
					periodBar.selected = period;
				if (marksLive)
					ImGui::PopStyleColor();
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleVar();
		ImGui::EndDisabled();

		// Enabling re-couples the bar to live time, so it always lands on the current period.
		if (ImGui::Checkbox(T(TKEY("time_of_day_toggle"), "Time of Day"), &timeOfDayEnabled) && timeOfDayEnabled) {
			periodBar.selected = -1;
			periodBar.lastHour = SceneSettingsManager::GetCurrentGameHour();
		}
		Util::AddTooltip(T(TKEY("time_of_day_toggle_tooltip"),
			"Edit one period at a time. Game time pauses while a Scene Manager panel is open."));

		if (!timeOfDayEnabled)
			Util::Text::Disabled("%s", T(TKEY("period_bar_off"), "Time of day editing is off. The bar does not follow game time."));
		else if (periodBar.selected < 0)
			Util::Text::Disabled("%s", T(TKEY("period_bar_following"), "Following the time of day. Click a period to edit it on its own."));
		else
			Util::Text::Disabled(T(TKEY("period_bar_manual"), "Editing %s. Scrub the time of day to follow it again."), GetPeriodLabel(active));

		return active;
	}

	/// Scene-capable features, resolved once: loaded features and the catalog are fixed after boot.
	/// Transitionable-only is the weather and time-of-day set; the rest also covers interior and location.
	const std::vector<FeatureListEntry>& GetFeatureEntries(bool transitionableOnly)
	{
		auto build = [](const std::vector<std::string>& names) {
			std::vector<FeatureListEntry> entries;
			entries.reserve(names.size());
			for (const auto& name : names)
				entries.push_back({ name, std::format("{}##{}", SceneSettingsManager::GetFeatureDisplayName(name), name) });
			return entries;
		};
		static const std::vector<FeatureListEntry> transitionable = build(SceneSettingsManager::GetExteriorRelevantFeatureNames());
		static const std::vector<FeatureListEntry> all = build(SceneSettingsManager::GetLocationRelevantFeatureNames());
		return transitionableOnly ? transitionable : all;
	}

	/// Draws the feature selectables and returns the feature the panel below should edit.
	const std::string& DrawFeatureList(std::string& selected, const std::vector<FeatureListEntry>& entries)
	{
		if (entries.empty()) {
			selected.clear();
			Util::Text::WrappedDisabled("%s",
				T(TKEY("scene_feature_list_empty"), "No loaded feature exposes scene settings."));
			return selected;
		}

		if (std::ranges::none_of(entries, [&](const auto& entry) { return entry.shortName == selected; }))
			selected = entries.front().shortName;

		for (const auto& entry : entries) {
			if (ImGui::Selectable(entry.label.c_str(), entry.shortName == selected))
				selected = entry.shortName;
		}
		return selected;
	}

	/// Period bar plus the shared notice, so both panels stay visually identical while empty.
	void DrawPanel(const char* intro)
	{
		DrawPeriodBar();
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		ImGui::TextWrapped("%s", intro);
		ImGui::Spacing();
		Util::Text::WrappedDisabled("%s",
			T(TKEY("scene_manager_unavailable"), "Scene settings authoring is not yet available."));
	}
}

void SceneSettingsUI::DrawWeatherSceneTab()
{
	panelVisible = true;

	if (ImGui::BeginTable("SceneFeatureLayout", 2, kFeatureLayoutFlags)) {
		ImGui::TableSetupColumn("##Features", ImGuiTableColumnFlags_WidthFixed, kFeatureListWidth * Util::GetUIScale());
		ImGui::TableSetupColumn("##Body", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextRow();

		// Each column scrolls on its own, so a long feature list never drags the panel with it.
		ImGui::TableSetColumnIndex(0);
		if (ImGui::BeginChild("##SceneFeatureList")) {
			ImGui::Text("%s", T(TKEY("scene_feature_list_title"), "Features"));
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
			DrawFeatureList(weatherSelectedFeature, GetFeatureEntries(true));
		}
		ImGui::EndChild();

		ImGui::TableSetColumnIndex(1);
		if (ImGui::BeginChild("##SceneFeatureBody"))
			DrawPanel(T(TKEY("scene_manager_weather_intro"), "Settings overridden while this weather is active."));
		ImGui::EndChild();

		ImGui::EndTable();
	}
}

void SceneSettingsUI::DrawSceneManagerPanel()
{
	panelVisible = true;

	DrawPanel(T(TKEY("scene_manager_panel_intro"), "Settings overridden by interior, time of day, and location."));
}

void SceneSettingsUI::DrawSceneManagerCategoryFeatures()
{
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::Indent();
	ImGui::SetWindowFontScale(kNestedFeatureFontScale);
	DrawFeatureList(panelSelectedFeature, GetFeatureEntries(false));
	ImGui::SetWindowFontScale(1.0f);
	ImGui::Unindent();
}

void SceneSettingsUI::SyncTimePause()
{
	// Time only stops for a panel that is actually editing a period.
	const bool editing = panelVisible && timeOfDayEnabled;
	panelVisible = false;
	if (editing == wasEditingTimeOfDay)
		return;
	wasEditingTimeOfDay = editing;

	auto* editorWindow = EditorWindow::GetSingleton();
	if (editing) {
		// A pause the user set themselves is theirs to release, so only track our own.
		pausedByPanel = !editorWindow->IsTimePaused();
		if (pausedByPanel)
			editorWindow->PauseTime();
	} else if (pausedByPanel) {
		editorWindow->ResumeTime();
		pausedByPanel = false;
	}
}
