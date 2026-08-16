#pragma once

#include "SceneSettingsManager.h"

/// Page-wide actions for one scene context: pause or resume it, copy another context into it,
/// export it (reserved), and clear it. The destination is always the page being drawn.
namespace ScenePageToolbar
{
	/// Draws the actions right-aligned on the current row, along with the dialogs they open.
	/// Precede it with ImGui::SameLine to share a row with the content already on it.
	/// The period scope has to match what the page authors: a page with time of day off owns every period.
	void Draw(const SceneSettingsManager::SceneContextId& context,
		SceneSettingsManager::PeriodScope periodScope);
}
