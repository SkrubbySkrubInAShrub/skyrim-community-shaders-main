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
	 * @brief Pauses game time while a panel is on screen and restores it once none are.
	 *
	 * Call once per frame, including while the editor is closed, so the pause is always released.
	 */
	void SyncTimePause();
}
