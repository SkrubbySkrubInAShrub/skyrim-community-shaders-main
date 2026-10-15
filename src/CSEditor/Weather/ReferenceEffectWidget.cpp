#include "ReferenceEffectWidget.h"
#include "../../I18n/I18n.h"
#include "../EditorWindow.h"
#include "../WeatherUtils.h"

#include <format>

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	namespace ReferenceEffectSetting
	{
		constexpr const char* kArtObject = "Art Object";
		constexpr const char* kEffectShader = "Effect Shader";
		constexpr const char* kFaceTarget = "Face Target";
		constexpr const char* kAttachToCamera = "Attach To Camera";
		constexpr const char* kInheritRotation = "Inherit Rotation";
	}
}

void ReferenceEffectWidget::DrawWidget()
{
	WeatherUtils::SetCurrentWidget(this);
	if (BeginWidgetWindow(true, true, false, nullptr, "##ReferenceEffectSearch")) {
		DrawWidgetHeader("##ReferenceEffectSearch", true, true);
		DrawSearchDropdown();
		BeginScrollableContent("##REScroll");
		{
			bool changed = false;

			auto editorWindow = EditorWindow::GetSingleton();
			auto drawFormPicker = [&](const char* label, auto& currentForm, const auto& widgets) {
				const char* displayLabel = label == ReferenceEffectSetting::kArtObject ?
				                               T(TKEY("art_object"), "Art Object") :
				                               T(TKEY("effect_shader"), "Effect Shader");
				const auto controlLabel = std::format("{}##{}", displayLabel, label);
				return DrawWithHighlight(label, [&]() {
					return WeatherUtils::DrawFormPickerCached(controlLabel.c_str(), currentForm, widgets, false, true);
				});
			};

			if (DrawIfMatchesSearch(ReferenceEffectSetting::kArtObject, [&](const char* label) {
					ImGui::SeparatorText(label);
					if (editorWindow->artObjectWidgets.empty()) {
						ImGui::TextDisabled("%s", T(TKEY("no_art_objects_available"), "No Art Objects available"));
						return false;
					}
					return drawFormPicker(label, settings.artObject, editorWindow->artObjectWidgets);
				}))
				changed = true;
			if (DrawIfMatchesSearch(ReferenceEffectSetting::kEffectShader, [&](const char* label) {
					ImGui::SeparatorText(label);
					if (editorWindow->effectShaderWidgets.empty()) {
						ImGui::TextDisabled("%s", T(TKEY("no_effect_shaders_available"), "No Effect Shaders available"));
						return false;
					}
					return drawFormPicker(label, settings.effectShader, editorWindow->effectShaderWidgets);
				}))
				changed = true;
			if (MatchesAnySearch({ ReferenceEffectSetting::kFaceTarget, ReferenceEffectSetting::kAttachToCamera, ReferenceEffectSetting::kInheritRotation })) {
				ImGui::SeparatorText(T(TKEY("flags"), "Flags"));
				if (WeatherUtils::DrawCheckbox(ReferenceEffectSetting::kFaceTarget, settings.faceTarget))
					changed = true;
				if (WeatherUtils::DrawCheckbox(ReferenceEffectSetting::kAttachToCamera, settings.attachToCamera))
					changed = true;
				if (WeatherUtils::DrawCheckbox(ReferenceEffectSetting::kInheritRotation, settings.inheritRotation))
					changed = true;
			}

			AutoApply(changed);
		}
		EndScrollableContent();
	}
	ImGui::End();
}

#undef I18N_KEY_PREFIX

void ReferenceEffectWidget::LoadSettings()
{
	if (!referenceEffect)
		return;

	LoadSavedSettings([this] {
		const auto* editorWindow = EditorWindow::GetSingleton();
		settings.artObject = WeatherUtils::ReadFormRef(js, "artObject", editorWindow->artObjectWidgets, settings.artObject);
		settings.effectShader = WeatherUtils::ReadFormRef(js, "effectShader", editorWindow->effectShaderWidgets, settings.effectShader);
		ReadSaved("faceTarget", settings.faceTarget);
		ReadSaved("attachToCamera", settings.attachToCamera);
		ReadSaved("inheritRotation", settings.inheritRotation);
	});
}

void ReferenceEffectWidget::LoadFromGameSettings()
{
	if (!referenceEffect)
		return;
	settings.artObject = referenceEffect->data.artObject;
	settings.effectShader = referenceEffect->data.effectShader;
	settings.faceTarget = referenceEffect->data.flags.any(RE::BGSReferenceEffect::Flag::kFaceTarget);
	settings.attachToCamera = referenceEffect->data.flags.any(RE::BGSReferenceEffect::Flag::kAttachToCamera);
	settings.inheritRotation = referenceEffect->data.flags.any(RE::BGSReferenceEffect::Flag::kInheritRotation);
}

void ReferenceEffectWidget::SaveSettings()
{
	const auto* editorWindow = EditorWindow::GetSingleton();
	js["artObject"] = WeatherUtils::FindEditorIDByForm(settings.artObject, editorWindow->artObjectWidgets);
	js["effectShader"] = WeatherUtils::FindEditorIDByForm(settings.effectShader, editorWindow->effectShaderWidgets);
	js["faceTarget"] = settings.faceTarget;
	js["attachToCamera"] = settings.attachToCamera;
	js["inheritRotation"] = settings.inheritRotation;
	originalSettings = settings;
}

void ReferenceEffectWidget::ApplyChanges()
{
	if (!referenceEffect)
		return;

	referenceEffect->data.artObject = settings.artObject;
	referenceEffect->data.effectShader = settings.effectShader;

	referenceEffect->data.flags.reset();
	if (settings.faceTarget)
		referenceEffect->data.flags.set(RE::BGSReferenceEffect::Flag::kFaceTarget);
	if (settings.attachToCamera)
		referenceEffect->data.flags.set(RE::BGSReferenceEffect::Flag::kAttachToCamera);
	if (settings.inheritRotation)
		referenceEffect->data.flags.set(RE::BGSReferenceEffect::Flag::kInheritRotation);

	Widget::ForceCurrentWeatherReinit();
}

std::vector<Widget::SearchResult> ReferenceEffectWidget::CollectSearchableSettings() const
{
	return WeatherUtils::LabelSearchResults({ { "", { ReferenceEffectSetting::kArtObject, ReferenceEffectSetting::kEffectShader,
														ReferenceEffectSetting::kFaceTarget, ReferenceEffectSetting::kAttachToCamera,
														ReferenceEffectSetting::kInheritRotation } } });
}
