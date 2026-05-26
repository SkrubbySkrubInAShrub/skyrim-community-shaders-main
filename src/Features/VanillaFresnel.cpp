#include "VanillaFresnel.h"
#include "I18n/I18n.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VanillaFresnel::Settings,
	Enable,
	EnableGGX,
	EnableGGXOnGrass,
	EnableDynamicCubemapsConversion,
	EnableEyeSpecialHandling,
	RoughnessMultiplier,
	SpecularRoughnessBlend,
	BaseF0Multiplier,
	MinF0,
	CubemapToF0Multiplier,
	ComplexMaterialF0Multiplier)

void VanillaFresnel::RestoreDefaultSettings()
{
	settings = {};
}

void VanillaFresnel::LoadSettings(json& o_json)
{
	settings = o_json;
}

void VanillaFresnel::SaveSettings(json& o_json)
{
	o_json = settings;
}

void VanillaFresnel::DrawSettings()
{
	ImGui::Checkbox(T("feature.vanilla_fresnel.enable_vanilla_fresnel", "Enable Vanilla Fresnel"), reinterpret_cast<bool*>(&settings.Enable));
	ImGui::Checkbox(T("feature.vanilla_fresnel.enable_phong_to_ggx", "Enable Phong to GGX"), reinterpret_cast<bool*>(&settings.EnableGGX));
	ImGui::Checkbox(T("feature.vanilla_fresnel.enable_phong_to_ggx_on_grass", "Enable Phong to GGX on Grass"), reinterpret_cast<bool*>(&settings.EnableGGXOnGrass));
	ImGui::Checkbox(T("feature.vanilla_fresnel.enable_auto_cubemaps_conversion", "Enable Auto Cubemaps Conversion"), reinterpret_cast<bool*>(&settings.EnableDynamicCubemapsConversion));
	ImGui::Checkbox(T("feature.vanilla_fresnel.enable_eye_special_handling", "Enable Eye Special Handling"), reinterpret_cast<bool*>(&settings.EnableEyeSpecialHandling));

	ImGui::SliderFloat(T("feature.vanilla_fresnel.roughness_multiplier", "Roughness Multiplier"), &settings.RoughnessMultiplier, 0.0f, 10.0f, "%.2f");
	ImGui::SliderFloat(T("feature.vanilla_fresnel.specular_roughness_blend", "Specular Roughness Blend"), &settings.SpecularRoughnessBlend, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat(T("feature.vanilla_fresnel.base_f0_multiplier", "Base F0 Multiplier"), &settings.BaseF0Multiplier, 0.0f, 10.0f, "%.2f");
	ImGui::SliderFloat(T("feature.vanilla_fresnel.min_f0", "Min F0"), &settings.MinF0, 0.0f, 0.04f, "%.3f");
	ImGui::SliderFloat(T("feature.vanilla_fresnel.cubemap_to_f0_multiplier", "Cubemap to F0 Multiplier"), &settings.CubemapToF0Multiplier, 0.0f, 10.0f, "%.2f");
	ImGui::SliderFloat(T("feature.vanilla_fresnel.complex_material_env_f0_multiplier", "Complex Material Env F0 Multiplier"), &settings.ComplexMaterialF0Multiplier, 0.0f, 10.0f, "%.2f");
}
