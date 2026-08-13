#include "SceneSettingsUI.h"

#include "../I18n/I18n.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	/// Intro line plus the shared notice, so both panels stay visually identical while empty.
	void DrawPanel(const char* intro)
	{
		ImGui::Spacing();
		ImGui::TextWrapped("%s", intro);
		ImGui::Spacing();
		ImGui::Separator();
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
