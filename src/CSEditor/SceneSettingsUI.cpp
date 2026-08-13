#include "SceneSettingsUI.h"

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

	/// One bar shared by every panel: it tracks live time, which is global.
	struct PeriodBarState
	{
		int selected = -1;       // -1 follows the live period
		float lastHour = -1.0f;  // game hour the coupling was last judged against
	};
	PeriodBarState periodBar;

	// Visibility latch driving the automatic time pause.
	bool panelVisible = false;
	bool panelWasVisible = false;
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

	/// Draws the period bar and returns the period the panel below it should edit.
	int DrawPeriodBar()
	{
		const float hour = SceneSettingsManager::GetCurrentGameHour();
		if (std::abs(hour - periodBar.lastHour) > kScrubEpsilon) {
			periodBar.selected = -1;
			periodBar.lastHour = hour;
		}

		const int live = static_cast<int>(SceneSettingsManager::GetCurrentPeriod());
		const int active = periodBar.selected < 0 ? live : periodBar.selected;

		ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, kSegmentTextAlign);
		if (ImGui::BeginTable("PeriodBar", kPeriodCount, kPeriodBarFlags)) {
			for (int period = 0; period < kPeriodCount; ++period) {
				ImGui::TableNextColumn();
				// Colour the live period only when the selection has left it, so the bar always
				// shows where time actually is.
				const bool marksLive = period == live && period != active;
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

		if (periodBar.selected < 0)
			Util::Text::Disabled("%s", T(TKEY("period_bar_following"), "Following the time of day. Click a period to edit it on its own."));
		else
			Util::Text::Disabled(T(TKEY("period_bar_manual"), "Editing %s. Scrub the time of day to follow it again."), GetPeriodLabel(active));

		return active;
	}

	/// Period bar plus the shared notice, so both panels stay visually identical while empty.
	void DrawPanel(const char* intro)
	{
		panelVisible = true;

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
	DrawPanel(T(TKEY("scene_manager_weather_intro"), "Settings overridden while this weather is active."));
}

void SceneSettingsUI::DrawSceneManagerPanel()
{
	DrawPanel(T(TKEY("scene_manager_panel_intro"), "Settings overridden by interior, time of day, and location."));
}

void SceneSettingsUI::SyncTimePause()
{
	const bool visible = panelVisible;
	panelVisible = false;
	if (visible == panelWasVisible)
		return;
	panelWasVisible = visible;

	auto* editorWindow = EditorWindow::GetSingleton();
	if (visible) {
		// A pause the user set themselves is theirs to release, so only track our own.
		pausedByPanel = !editorWindow->IsTimePaused();
		if (pausedByPanel)
			editorWindow->PauseTime();
	} else if (pausedByPanel) {
		editorWindow->ResumeTime();
		pausedByPanel = false;
	}
}
