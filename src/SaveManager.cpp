#include "SaveManager.h"

#include "CSEditor/EditorWindow.h"
#include "CSEditor/SceneManager/SceneSettingsManager.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/UI.h"

namespace SaveManager
{
	namespace
	{
		void WriteEditor(Trigger trigger)
		{
			auto* editor = EditorWindow::GetSingleton();
			editor->SaveUnsavedWidgets();
			if (auto* sceneManager = SceneSettingsManager::GetSingleton())
				sceneManager->WriteUserSettings();
			if (trigger == Trigger::User)
				editor->Save();
		}

		void ConfirmSaved(Trigger trigger)
		{
			// The main menu flashes its own button; the toast is for saves made inside the editor.
			auto* editor = EditorWindow::GetSingleton();
			if (trigger == Trigger::User && editor->open)
				editor->ShowNotification(T("cs_editor.saved", "Saved"), Util::Colors::GetSuccess(), 1.5f);
		}
	}

	void SaveEditor(Trigger trigger)
	{
		WriteEditor(trigger);
		ConfirmSaved(trigger);
	}

	void SaveAll(Trigger trigger)
	{
		if (auto* state = globals::state)
			state->Save();
		WriteEditor(trigger);
		ConfirmSaved(trigger);
	}

	const char* GetEditorSaveTooltip(bool unsaved)
	{
		return unsaved ?
		           T("cs_editor.save_unsaved_tooltip", "Unsaved changes. Click to save every CS editor change (Ctrl+S).") :
		           T("cs_editor.save_tooltip_short", "Save every CS editor change (Ctrl+S)");
	}
}
