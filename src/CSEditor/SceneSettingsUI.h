#pragma once

/// Scene Manager authoring UI. The panels are wired into the CS Editor; the entry
/// authoring controls are built on top of them.
namespace SceneSettingsUI
{
	/** @brief Draws the Scene Manager tab body inside a weather widget. */
	void DrawWeatherSceneTab();

	/** @brief Draws the Scene Manager panel body in the CS Editor objects window. */
	void DrawSceneManagerPanel();

	/**
	 * @brief Draws the objects window's feature list, nested under the Scene Manager category entry.
	 *
	 * The panel body has no room for a feature column, so the list lives with the category that owns it.
	 * Call only while that category is selected.
	 */
	void DrawSceneManagerCategoryFeatures();

	/**
	 * @brief Pauses game time while a panel is on screen and restores it once none are.
	 *
	 * Call once per frame, including while the editor is closed, so the pause is always released.
	 */
	void SyncTimePause();
}
