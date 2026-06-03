#include "TerrainVariation.h"
#include "../FeatureBuffer.h"
#include "../Globals.h"
#include "../I18n/I18n.h"
#include "../State.h"
#include "../Util.h"

#define I18N_KEY_PREFIX "feature.terrain_variation."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainVariation::Settings,
	enableLODTerrainTilingFix)

void TerrainVariation::DrawSettings()
{
	{
		MenuFonts::FontRoleGuard bodyGuard(Menu::FontRole::Body);
		ImGui::TextWrapped(
			"Terrain variation is always enabled when installed. Use disable at boot to turn off.");
	}

	ImGui::Spacing();

	bool oldLODEnabled = settings.enableLODTerrainTilingFix;
	ImGui::Checkbox(T(TKEY("apply_to_lod_terrain"), "Apply to LOD Terrain"), (bool*)&settings.enableLODTerrainTilingFix);
	if (oldLODEnabled != (bool)settings.enableLODTerrainTilingFix) {
		UpdateShaderSettings();
		logger::info("TerrainVariation LOD setting changed to: {}", settings.enableLODTerrainTilingFix);
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("apply_to_lod_terrain_tooltip"),
							  "Applies the tiling fix to LOD terrain objects.\nThis helps reduce the visible tiling effect on distant terrain."));
	}
}

void TerrainVariation::PostPostLoad()
{
	logger::info("TerrainVariation: Feature initialized");
}

void TerrainVariation::LoadSettings(json& o_json)
{
	settings = o_json;
}

void TerrainVariation::SaveSettings(json& o_json)
{
	o_json = settings;
}

void TerrainVariation::RestoreDefaultSettings()
{
	settings = {};
}

bool TerrainVariation::DrawFailLoadMessage() const
{
	return false;
}
