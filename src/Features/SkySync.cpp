#include "SkySync.h"
#include "../I18n/I18n.h"
#include "RE/B/BSVolumetricLightingRenderData.h"

#include "Utils/Game.h"

#define I18N_KEY_PREFIX "feature.sky_sync."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SkySync::Settings,
	Enabled,
	UseAlternateSunPath,
	MoonLightSource,
	SunPath,
	CustomAngle,
	MinShadowElevation,
	ShadowTransitionDuration,
	DimSunlightUnderHorizon,
	DimVolumetricLighting,
	HorizonFadeHours,
	HorizonFadeElevation,
	NewMoonIntensity,
	CrescentMoonIntensity,
	FullMoonIntensity)

void SkySync::DrawSettings()
{
	const char* sunPathNames[] = {
		T(TKEY("sun_path_southern"), "Southern Sky"),
		T(TKEY("sun_path_northern"), "Northern Sky"),
		T(TKEY("sun_path_vanilla"), "Vanilla"),
		T(TKEY("sun_path_custom"), "Custom")
	};
	const char* moonLightSourceNames[] = {
		T(TKEY("moon_light_source_brightest"), "Brightest"),
		T(TKEY("moon_light_source_masser"), "Masser"),
		T(TKEY("moon_light_source_secunda"), "Secunda")
	};

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("enabled_tooltip"), "Enable or disable Sky Sync features."));
	}

	ImGui::Checkbox(T(TKEY("use_alternate_sun_path"), "Use alternate sun path"), &settings.UseAlternateSunPath);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("use_alternate_sun_path_tooltip"), "Calculate sun position based on time of day and season instead of vanilla movement."));
	}

	if (settings.UseAlternateSunPath) {
		if (ImGui::SliderInt(T(TKEY("sun_path"), "Sun path"), &settings.SunPath, 0, static_cast<uint8_t>(SunPath::Count) - 1, sunPathNames[settings.SunPath], ImGuiSliderFlags_AlwaysClamp))
			SetSunAngle();
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("sun_path_tooltip"), "Choose the trajectory the sun takes across the sky."));
		}

		if (settings.SunPath == static_cast<int32_t>(SunPath::Custom)) {
			if (ImGui::SliderFloat(T(TKEY("custom_angle"), "Custom angle"), &settings.CustomAngle, -90.0f, 90.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp))
				SetSunAngle();
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(T(TKEY("custom_angle_tooltip"), "Set a custom angle for the sun's trajectory."));
			}
		}
	}

	ImGui::SliderInt(T(TKEY("moon_light_source"), "Moon light source"), &settings.MoonLightSource, 0, static_cast<uint8_t>(MoonLightSource::Count) - 1, moonLightSourceNames[settings.MoonLightSource], ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("moon_light_source_tooltip"), "Select which moon casts shadows during the night."));
	}

	ImGui::SliderFloat(T(TKEY("min_shadow_elevation"), "Min Shadow Elevation"), &settings.MinShadowElevation, 0.0f, 45.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("min_shadow_elevation_tooltip"), "The minimum angle sunlight will set to. Caps shadow length. Higher = shorter shadows at sunset/sunrise."));
	}

	ImGui::SliderFloat(T(TKEY("shadow_transition_duration"), "Shadow Transition Duration"), &settings.ShadowTransitionDuration, 0.0f, 500.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("shadow_transition_duration_tooltip"), "How long (in game-time units) the shadow direction takes to fade between sources. 300 = ~15 seconds at timescale 20."));
	}

	ImGui::Checkbox(T(TKEY("dim_sunlight_under_horizon"), "Dim Sunlight Under Horizon"), &settings.DimSunlightUnderHorizon);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("dim_sunlight_under_horizon_tooltip"), "Fade directional light to zero as the sun or moon approaches the horizon."));
	}

	ImGui::Checkbox(T(TKEY("fade_volumetric_lighting"), "Fade Volumetric Lighting"), &settings.DimVolumetricLighting);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("fade_volumetric_lighting_tooltip"), "Also fade volumetric lighting with the directional dim around dawn and dusk."));
	}

	if (settings.DimSunlightUnderHorizon || settings.DimVolumetricLighting) {
		ImGui::SliderFloat(T(TKEY("horizon_fade_duration"), "Horizon Fade Duration"), &settings.HorizonFadeHours, 0.0f, MaxHorizonFadeHours, "%.1f h", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("horizon_fade_duration_tooltip"), "How long (in game hours) moonlight takes to fade in once a moon casts shadows, and to fade out before sunrise."));
		}

		ImGui::SliderFloat(T(TKEY("horizon_fade_elevation"), "Horizon Fade Elevation"), &settings.HorizonFadeElevation, 0.0f, MaxHorizonFadeElevation, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("horizon_fade_elevation_tooltip"), "Height above the horizon at which the sun or moon light starts fading out. Higher = longer fade."));
		}
	}

	ImGui::SliderFloat(T(TKEY("new_moon_intensity"), "New Moon Intensity"), &settings.NewMoonIntensity, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("crescent_intensity"), "Crescent Intensity"), &settings.CrescentMoonIntensity, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("full_moon_intensity"), "Full Moon Intensity"), &settings.FullMoonIntensity, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);

	if (ImGui::TreeNodeEx("Debug", ImGuiTreeNodeFlags_None)) {
		static constexpr const char* CasterNames[] = { "Sun", "Masser", "Secunda", "None" };
		static constexpr const char* PhaseNames[] = { "Full", "Waning Gibbous", "Waning Quarter", "Waning Crescent", "New", "Waxing Crescent", "Waxing Quarter", "Waxing Gibbous" };

		auto getPhase = [](const RE::Moon* moon) -> const char* {
			if (!moon || !moon->moonMesh)
				return "Unknown";
			if (const auto prop = skyrim_cast<RE::BSSkyShaderProperty*>(moon->moonMesh->GetGeometryRuntimeData().shaderProperty.get())) {
				if (auto tex = prop->GetBaseTexture())
					return PhaseNames[static_cast<int>(Util::Moon::GetPhaseFromTexture(tex->name.c_str()))];
			}
			return "Unknown";
		};

		auto drawMoonEntry = [&](const char* label, Caster caster, const char* phase) {
			auto& color = colors[static_cast<int>(caster)];
			ImVec4 swatch = { color.x, color.y, color.z, 1.0f };
			ImGui::ColorButton(label, swatch, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, { ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight() });
			ImGui::SameLine();
			ImGui::Text("%s  [%s]  color (%.3f, %.3f, %.3f, %.3f)", label, phase, color.x, color.y, color.z, color.w);
		};

		const auto sky = globals::game::sky;
		drawMoonEntry("Masser", Caster::Masser, sky ? getPhase(sky->masser) : "Unknown");
		drawMoonEntry("Secunda", Caster::Secunda, sky ? getPhase(sky->secunda) : "Unknown");

		ImGui::Text("Dim: %.3f", currentDim);

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::Text("Shadow target: %s", CasterNames[static_cast<int>(shadowFader.target)]);
		ImGui::Text("Shadow dir:    (%.2f, %.2f, %.2f)", shadowFader.currentDir.x, shadowFader.currentDir.y, shadowFader.currentDir.z);
		ImGui::Text("Transition intensity factor: %.3f", shadowFader.intensityFactor);
		if (shadowFader.transitioning) {
			const float t = settings.ShadowTransitionDuration > 0.0f ? shadowFader.fadeTimer / settings.ShadowTransitionDuration : 1.0f;
			ImGui::ProgressBar(t, { -1.0f, 0.0f }, "");
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::Text("Transitioning %.0f%%", t * 100.0f);
		} else {
			ImGui::TextDisabled("No transition");
		}

		ImGui::TreePop();
	}
}

void SkySync::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.MoonLightSource = std::clamp(settings.MoonLightSource, static_cast<int32_t>(MoonLightSource::Brightest), static_cast<int32_t>(MoonLightSource::Secunda));
	settings.SunPath = std::clamp(settings.SunPath, static_cast<int32_t>(SunPath::Southern), static_cast<int32_t>(SunPath::Custom));
	settings.CustomAngle = std::clamp(settings.CustomAngle, -90.0f, 90.0f);
	settings.MinShadowElevation = std::clamp(settings.MinShadowElevation, 0.0f, 45.0f);
	settings.HorizonFadeHours = std::clamp(settings.HorizonFadeHours, 0.0f, MaxHorizonFadeHours);
	settings.HorizonFadeElevation = std::clamp(settings.HorizonFadeElevation, 0.0f, MaxHorizonFadeElevation);
	SetSunAngle();
}

void SkySync::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SkySync::RestoreDefaultSettings()
{
	settings = {};
	SetSunAngle();
}

void SkySync::PostPostLoad()
{
	if (Util::Moon::IsMoonAndStarsLoaded())
		logger::info("[Sky Sync] Moon and Stars detected, compatibility enabled");

	if (GetModuleHandle(L"EVLaS.dll")) {
		DisableOnConflict("EVLaS");
		return;
	}

	stl::detour_thunk<Sky_Update>(REL::RelocationID(25682, 26229));
	Util::SetCelestialTransitionHandlerAvailable(true);

	gSunPosition = reinterpret_cast<RE::NiPoint3*>(REL::RelocationID(527924, 414871).address());

	gVolumetricLighting = reinterpret_cast<RE::BSVolumetricLightingRenderData*>(
		REL::RelocationID(527719, 414629).address() - offsetof(RE::BSVolumetricLightingRenderData, color));

	logger::info("[Sky Sync] Installed hooks");
}

void SkySync::DataLoaded()
{
	const auto data = RE::TESDataHandler::GetSingleton();
	if (data && (data->LookupLoadedModByName("DVLaSS.esp"sv) || data->LookupLoadedLightModByName("DVLaSS.esp"sv)))
		DisableOnConflict("DVLaSS");

	if (const auto collection = globals::game::gameSettingCollection)
		gSunAlphaTransTime = collection->GetSetting("fSunAlphaTransTime");
}

void SkySync::GameLoaded()
{
	Util::RequestGameLoadTransition();
}

void SkySync::DisableOnConflict(std::string_view conflictName)
{
	failedLoadedMessage = fmt::format("Disabled as {} has been detected, both cannot be used together", conflictName);
	loaded = false;
	settings.Enabled = false;
	Util::SetCelestialTransitionHandlerAvailable(false);
	logger::warn("[Sky Sync] {}", failedLoadedMessage);
}

void SkySync::OnSkyUpdateColors(RE::Sky* sky)
{
	if (!settings.Enabled || !sky)
		return;

	const float transitionFactor = shadowFader.intensityFactor;
	const float dirLightFactor = settings.DimSunlightUnderHorizon ? transitionFactor * currentDim : transitionFactor;
	if (dirLightFactor < 1.0f) {
		auto& dirLight = sky->skyColor[static_cast<uint>(RE::TESWeather::ColorTypes::kSunlight)];
		dirLight.red *= dirLightFactor;
		dirLight.green *= dirLightFactor;
		dirLight.blue *= dirLightFactor;
	}

	if (gVolumetricLighting)
		gVolumetricLighting->intensity *= settings.DimVolumetricLighting ? transitionFactor * currentDim : transitionFactor;
}

void SkySync::Sky_Update::thunk(RE::Sky* sky)
{
	func(sky);
	auto& skySync = globals::features::skySync;
	skySync.PreparePendingTransitions();
	if (skySync.Update(sky))
		Util::CompleteCelestialTransition();
}

void SkySync::PreparePendingTransitions()
{
	const auto request = Util::ConsumeCelestialTransitionRequest();
	if (!request.timeJump && !request.gameLoad)
		return;

	if (request.gameLoad) {
		shadowFader.Reset();
		currentCell = nullptr;
		currentCellInterior = false;
		currentCellWorldspace = nullptr;
		currentSkyRotation = D3D11_FLOAT32_MAX;
	}

	immediateTransitionReady = true;
}

bool SkySync::Update(const RE::Sky* sky)
{
	std::fill(std::begin(rawDirections), std::end(rawDirections), RE::NiPoint3{});

	if (!settings.Enabled) {
		currentDim = 1.0f;
		const bool transitionCompleted = immediateTransitionReady;
		immediateTransitionReady = false;
		return transitionCompleted;
	}

	if (!sky)
		return false;

	const auto sun = sky->sun;
	const auto climate = sky->currentClimate;
	const auto player = RE::PlayerCharacter::GetSingleton();
	if (!sun || !climate || !player) {
		currentDim = 1.0f;
		return false;
	}

	const auto cell = player->GetParentCell();
	bool resetTransition = false;

	if (cell != currentCell) {
		resetTransition = true;
		const auto prevCell = currentCell;
		const bool resetFaderForCellChange = cell && prevCell &&
		                                     (cell->IsInteriorCell() != currentCellInterior ||
												 cell->GetRuntimeData().worldSpace != currentCellWorldspace);
		if (cell)
			SetSkyRotation(sky, cell);
		else
			currentCell = nullptr;  // keep the cache in sync so a cell-less frame doesn't reset every frame
		if (resetFaderForCellChange)
			shadowFader.Reset();
	}

	// Exterior worldspaces always run; interior cells require the sunlight-shadows flag.
	if (cell && cell->IsInteriorCell() && !cell->cellFlags.all(static_cast<RE::TESObjectCELL::Flag>(CellFlagExt::kSunlightShadows))) {
		currentDim = 1.0f;
		return false;
	}

	const float hour = sky->currentGameHour;
	const auto [sunriseHorizon, sunsetHorizon] = GetSunHorizonHours(climate->timing, settings.UseAlternateSunPath);
	sunBelowHorizon = hour >= sunsetHorizon || hour < sunriseHorizon;

	RE::NiPoint3 directions[3] = {};
	float intensities[3] = {};

	ProcessSun(sky, directions, intensities);
	ProcessMoon(sky, Caster::Masser, directions, intensities);
	ProcessMoon(sky, Caster::Secunda, directions, intensities);
	std::copy(std::begin(directions), std::end(directions), std::begin(rawDirections));

	const float sunDim = sunBelowHorizon ? 0.0f : GetHorizonDim(directions[static_cast<int>(Caster::Sun)]);
	const bool sunNearHorizon = !sunBelowHorizon && sunDim < 1.0f;
	sunSetting = sunNearHorizon && hour >= (sunriseHorizon + sunsetHorizon) * 0.5f;
	sunRising = sunNearHorizon && !sunSetting;
	currentDim = sunDim;  // ShadowFader reads it to lock the sunset heading

	const auto calendar = globals::game::calendar;
	const auto deltaTime = globals::game::deltaTime;
	float fadeAdvance = calendar && deltaTime ? std::max(*deltaTime * calendar->GetTimescale(), 0.0f) : 0.0f;

	// The clock can outrun real time (waiting, fast travel) or move while frames are paused
	// (console, scrubbing), so advance by whichever of the two elapsed more.
	if (lastGameHour >= 0.0f) {
		float hourDelta = hour - lastGameHour;
		if (hourDelta > 12.0f)
			hourDelta -= 24.0f;
		else if (hourDelta < -12.0f)
			hourDelta += 24.0f;
		fadeAdvance = std::max(fadeAdvance, std::abs(hourDelta) * SecondsPerGameHour);
	}
	lastGameHour = hour;

	const bool transitionCompleted = immediateTransitionReady;
	shadowFader.Update(sky, directions, intensities, settings.ShadowTransitionDuration, fadeAdvance, transitionCompleted || resetTransition);

	const float hoursToSunrise = hour < sunriseHorizon ? sunriseHorizon - hour : sunriseHorizon + 24.0f - hour;
	UpdateMoonlightFade(hoursToSunrise, fadeAdvance / SecondsPerGameHour, transitionCompleted);
	// Follow the light's own direction so the dim stays continuous while it swings between moons
	if (sunBelowHorizon)
		currentDim = moonlightFade * GetHorizonDim(shadowFader.currentDir);

	immediateTransitionReady = false;
	return transitionCompleted;
}

float SkySync::GetHorizonDim(const RE::NiPoint3& dir) const
{
	// Elevation-based so every path fades over the same arc, however long it lingers near the horizon
	const float fadeRadians = std::max(DirectX::XMConvertToRadians(settings.HorizonFadeElevation), FLT_EPSILON);
	return std::clamp(DirectX::XMScalarASinEst(dir.z) / fadeRadians, 0.0f, 1.0f);
}

void SkySync::UpdateMoonlightFade(float hoursToSunrise, float advanceHours, bool immediate)
{
	const bool moonCasting = shadowFader.target == Caster::Masser || shadowFader.target == Caster::Secunda;
	const float target = sunBelowHorizon && moonCasting ? 1.0f : 0.0f;
	const float fadeHours = settings.HorizonFadeHours;
	const float step = fadeHours > 0.0f ? advanceHours / fadeHours : 1.0f;
	moonlightFade = immediate ? target : std::clamp(target, moonlightFade - step, moonlightFade + step);

	// The sun takes over at the horizon from zero, so the moonlight must be gone by then
	if (fadeHours > 0.0f)
		moonlightFade = std::min(moonlightFade, hoursToSunrise / fadeHours);
}
void SkySync::SetSunAngle()
{
	switch (static_cast<SunPath>(settings.SunPath)) {
	case SunPath::Southern:
		sunAngle = SouthernSunAngle;
		break;
	case SunPath::Northern:
		sunAngle = NorthernSunAngle;
		break;
	case SunPath::Vanilla:
		sunAngle = VanillaSunAngle;
		break;
	case SunPath::Custom:
		sunAngle = 90.0f + settings.CustomAngle;
		break;
	default:;
	}
}

void SkySync::SetSkyRotation(const RE::Sky* sky, RE::TESObjectCELL* cell)
{
	// If the interior cell isn't initialised it won't have the north rotation extra data ready, skip for a frame
	if (cell->IsInteriorCell() && cell->cellState == static_cast<RE::TESObjectCELL::CellState>(0))
		return;

	currentCell = cell;
	currentCellInterior = cell->IsInteriorCell();
	currentCellWorldspace = cell->GetRuntimeData().worldSpace;
	const float rotation = cell->GetNorthRotation();
	if (rotation == currentSkyRotation)
		return;

	currentSkyRotation = rotation;
	sky->root->local.rotate = RE::NiMatrix3{ RE::NiPoint3{ 0.0f, 0.0f, -rotation } };
	RE::NiUpdateData updateData;
	sky->root->Update(updateData);
}

float SkySync::MiddleHour(const RE::TESClimate::Timing::Interval& interval)
{
	return (interval.end * HoursPerTimingUnit + interval.begin * HoursPerTimingUnit) * 0.5f;
}

std::pair<float, float> SkySync::GetSunHorizonHours(const RE::TESClimate::Timing& timing, bool alternatePath)
{
	// Vanilla's sun touches the horizon where its alpha fade ends: the interval midpoint +- half fSunAlphaTransTime
	const float halfTransition = alternatePath ? AlternateSunHorizonOffsetHours :
	                                             (gSunAlphaTransTime ? gSunAlphaTransTime->GetFloat() : DefaultSunAlphaTransTime) * 0.5f;
	return { MiddleHour(timing.sunrise) - halfTransition, MiddleHour(timing.sunset) + halfTransition };
}

void SkySync::ProcessSun(const RE::Sky* sky, RE::NiPoint3 dirs[], float intensities[])
{
	const auto sun = sky->sun;
	RE::NiPoint3 dir;
	float dist;

	if (settings.UseAlternateSunPath) {
		const auto [sunrise, sunset] = GetSunHorizonHours(sky->currentClimate->timing, true);
		CalculateAlternateSunDirectionAndDistance(dir, dist, sky->currentGameHour, sunrise, sunset, sunAngle);
	} else
		CalculateSunDirectionAndDistance(sun, dir, dist);

	SetSunPosition(sun, dir, dist);
	HideSunOutsideFadeWindow(sky);

	dirs[static_cast<int>(Caster::Sun)] = dir;

	if (const auto prop = skyrim_cast<RE::BSSkyShaderProperty*>(sun->sunBase->GetGeometryRuntimeData().shaderProperty.get()))
		intensities[static_cast<int>(Caster::Sun)] = prop->kBlendColor.alpha;
}

void SkySync::HideSunOutsideFadeWindow(const RE::Sky* sky)
{
	if (!gSunAlphaTransTime)
		return;

	// Same bounds as Sun::Update's fade, but made inclusive
	const auto [fadeInStart, fadeOutEnd] = GetSunHorizonHours(sky->currentClimate->timing, false);
	const float hour = sky->currentGameHour;
	if (hour > fadeInStart && hour < fadeOutEnd)
		return;

	for (const auto& geometry : { sky->sun->sunBase, sky->sun->sunGlare }) {
		if (const auto prop = geometry ? skyrim_cast<RE::BSSkyShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get()) : nullptr)
			prop->kBlendColor.alpha = 0.0f;
	}
}

void SkySync::ProcessMoon(const RE::Sky* sky, const Caster type, RE::NiPoint3 dirs[], float intensities[])
{
	const int idx = static_cast<int>(type);
	colors[idx] = {};

	const auto moon = type == Caster::Masser ? sky->masser : sky->secunda;
	if (!moon || moon->root->GetFlags().any(RE::NiAVObject::Flag::kHidden))
		return;

	dirs[idx] = Util::Moon::GetFacingAxis(moon->root->local.rotate);

	const float4& baseColor = type == Caster::Masser ? Util::Moon::MasserBaseColor : Util::Moon::SecundaBaseColor;
	float4 color = Util::Moon::GetBlendColor(moon, baseColor, settings.NewMoonIntensity, settings.CrescentMoonIntensity, settings.FullMoonIntensity);
	colors[idx] = color;

	const auto src = static_cast<MoonLightSource>(settings.MoonLightSource);
	const bool isValidSource = src == MoonLightSource::Brightest || (src == MoonLightSource::Masser && type == Caster::Masser) || (src == MoonLightSource::Secunda && type == Caster::Secunda);
	if (!isValidSource)
		return;

	// A moon sinking toward the horizon loses out to one that is still up
	intensities[idx] = color.w * GetHorizonDim(dirs[idx]);
}

RE::NiPoint3 SkySync::GetCelestialDirection(const RE::Sky* sky, const Caster caster) const
{
	const auto idx = static_cast<size_t>(caster);
	assert(idx < std::size(rawDirections));
	if (!sky || !sky->root)
		return { 0.0f, 0.0f, 1.0f };

	RE::NiPoint3 dir = rawDirections[idx];
	if (dir.SqrLength() > 0.0f)
		dir = sky->root->world.rotate * dir;
	else if (caster == Caster::Sun)  // SetSunPosition writes only the local translate, so the world one may be stale
		dir = sky->sun && sky->sun->root ? sky->sun->root->world.translate - sky->root->world.translate : RE::NiPoint3{};
	else
		dir = Util::Moon::GetDirection(caster == Caster::Masser ? sky->masser : sky->secunda);

	if (dir.Unitize() <= FLT_EPSILON)
		return { 0.0f, 0.0f, 1.0f };
	return dir;
}

inline void SkySync::CalculateSunDirectionAndDistance(const RE::Sun* sun, RE::NiPoint3& outDir, float& outDistance)
{
	outDir = sun->root->local.translate;
	if (outDistance = outDir.Unitize(); outDistance < FLT_EPSILON) {
		outDir = { 0.0f, 0.0f, 1.0f };
		outDistance = SunPeakDistance;
	}
}

inline void SkySync::CalculateAlternateSunDirectionAndDistance(RE::NiPoint3& outDir, float& outDist, const float time, const float sunrise, const float sunset, const float sunAngle)
{
	const float phi = DirectX::XM_PI * ((time - sunrise) / (sunset - sunrise));
	float sinPhi, cosPhi;
	DirectX::XMScalarSinCosEst(&sinPhi, &cosPhi, phi);

	float tiltRadians = DirectX::XMConvertToRadians(sunAngle);
	float cosTilt, sinTilt;
	DirectX::XMScalarSinCosEst(&sinTilt, &cosTilt, tiltRadians);

	outDir = { cosPhi, -sinPhi * cosTilt, sinPhi * sinTilt };

	if (const float length = outDir.Unitize(); length < FLT_EPSILON)
		outDir = { 0.0f, 0.0f, 1.0f };

	const float elevationRatio = std::max(sinPhi, 0.0f);
	outDist = std::lerp(SunHorizonDistance, SunPeakDistance, elevationRatio);
}

inline void SkySync::SetSunPosition(const RE::Sun* sun, const RE::NiPoint3& dir, const float distance)
{
	const auto position = dir * distance;
	sun->root->local.translate = position;
	sun->sunGlareNode->local.translate = position;
	*gSunPosition = position;
}

void SkySync::ShadowFader::Reset()
{
	target = Caster::Sun;
	previousTarget = Caster::Sun;
	fadeTimer = 0.0f;
	immediateTransitionRemaining = 0.0f;
	transitioning = false;
	sunriseReleased = false;
	frozenHeading = 0.0f;
	sunsetHeadingLocked = false;
}

void SkySync::ShadowFader::Update(const RE::Sky* sky, RE::NiPoint3 dirs[], float intensities[], float fadeDuration, float fadeAdvance, bool a_immediateTransition)
{
	auto isValidDir = [](const RE::NiPoint3& d) { return d.x != 0.0f || d.y != 0.0f || d.z != 0.0f; };

	if (fadeDuration > 0.0f && a_immediateTransition)
		immediateTransitionRemaining = fadeDuration;
	else
		immediateTransitionRemaining = std::max(immediateTransitionRemaining - fadeAdvance, 0.0f);

	Caster best;

	if (globals::features::skySync.sunBelowHorizon) {
		bool masserValid = isValidDir(dirs[static_cast<int>(Caster::Masser)]);
		bool secundaValid = isValidDir(dirs[static_cast<int>(Caster::Secunda)]);

		if (!masserValid && !secundaValid)
			best = Caster::None;
		else if (!masserValid)
			best = Caster::Secunda;
		else if (!secundaValid || intensities[static_cast<int>(Caster::Secunda)] <= intensities[static_cast<int>(Caster::Masser)])
			best = Caster::Masser;
		else
			best = Caster::Secunda;
	} else {
		best = Caster::Sun;
	}

	LockSunElevation(dirs);

	// No valid caster points straight up so shadows fall directly down.
	auto casterDir = [&](Caster c) {
		return c == Caster::None ? RE::NiPoint3{ 0.0f, 0.0f, 1.0f } : dirs[static_cast<int>(c)];
	};

	// If best source changed, begin a new transition
	if (best != target) {
		previousTarget = target;
		target = best;
		startDir = currentDir;
		startIntensityFactor = intensityFactor;
		fadeTimer = 0.0f;
		transitioning = true;
	}

	const RE::NiPoint3 targetDir = casterDir(target);

	if (!transitioning) {
		currentDir = targetDir;
		intensityFactor = target == Caster::None ? 0.0f : 1.0f;
		if (target != Caster::None)
			immediateTransitionRemaining = 0.0f;
		SetLighting(sky, currentDir);
		return;
	}

	const float effectiveFadeAdvance = immediateTransitionRemaining > 0.0f ? fadeDuration : fadeAdvance;
	fadeTimer = std::min(fadeTimer + effectiveFadeAdvance, fadeDuration);
	const float t = fadeDuration > 0.0f ? fadeTimer / fadeDuration : 1.0f;

	currentDir = {
		std::lerp(startDir.x, targetDir.x, t),
		std::lerp(startDir.y, targetDir.y, t),
		std::lerp(startDir.z, targetDir.z, t)
	};
	// Opposite start and target directions cancel at the midpoint; pass through straight up instead
	if (currentDir.Unitize() <= FLT_EPSILON)
		currentDir = { 0.0f, 0.0f, 1.0f };

	if (t >= 1.0f) {
		currentDir = targetDir;
		transitioning = false;
	}

	// Fade out as the direction leaves the old caster and back in as it reaches the new one; the no-caster fallback only fades out.
	const float fadeOut = startIntensityFactor * (target == Caster::None ? 1.0f - t : ComputeAlignmentFactor(currentDir, startDir));
	intensityFactor = target == Caster::None ? fadeOut : std::max(fadeOut, ComputeAlignmentFactor(currentDir, targetDir));
	if (target != Caster::None && !transitioning)
		immediateTransitionRemaining = 0.0f;
	SetLighting(sky, currentDir);
}

void SkySync::ShadowFader::LockSunElevation(RE::NiPoint3 dirs[])
{
	// Dusk: lock elevation to the minimum so the shadow can't tilt back up as the sun goes under,
	// and once dimming passes the threshold lock heading too so it stops sweeping while the VL fades.
	// Dawn: lock at the minimum until the sun naturally rises above it, then follow it.
	const auto& skySync = globals::features::skySync;
	const int sunIdx = static_cast<int>(Caster::Sun);
	const float minElev = DirectX::XMConvertToRadians(skySync.settings.MinShadowElevation);
	if (skySync.sunSetting) {
		if (skySync.currentDim <= SunsetHeadingLockThreshold) {
			if (!sunsetHeadingLocked) {
				frozenHeading = std::atan2(dirs[sunIdx].y, dirs[sunIdx].x);
				sunsetHeadingLocked = true;
			}
			SetDirection(dirs[sunIdx], frozenHeading, minElev);
		} else {
			SetElevation(dirs[sunIdx], minElev);
		}
	} else if (skySync.sunRising) {
		if (!sunriseReleased) {
			if (DirectX::XMScalarASinEst(dirs[sunIdx].z) >= minElev)
				sunriseReleased = true;
			else
				SetElevation(dirs[sunIdx], minElev);
		}
	} else {
		sunriseReleased = false;
		sunsetHeadingLocked = false;
	}
}

void SkySync::ShadowFader::SetLighting(const RE::Sky* sky, RE::NiPoint3 dir)
{
	ClampDirection(dir);

	RE::NiMatrix3& m = sky->sun->light->local.rotate;
	m.entry[0][0] = -dir.x;
	m.entry[1][0] = -dir.y;
	m.entry[2][0] = -dir.z;

	RE::NiUpdateData updateData;
	sky->sun->light->Update(updateData);
}

inline void SkySync::ShadowFader::SetDirection(RE::NiPoint3& dir, float headingRadians, float elevRadians)
{
	float sinElev, cosElev, sinHeading, cosHeading;
	DirectX::XMScalarSinCosEst(&sinElev, &cosElev, elevRadians);
	DirectX::XMScalarSinCosEst(&sinHeading, &cosHeading, headingRadians);

	dir.x = cosElev * cosHeading;
	dir.y = cosElev * sinHeading;
	dir.z = sinElev;
}

inline void SkySync::ShadowFader::SetElevation(RE::NiPoint3& dir, float elevRadians)
{
	SetDirection(dir, std::atan2(dir.y, dir.x), elevRadians);
}

float SkySync::ShadowFader::ComputeAlignmentFactor(const RE::NiPoint3& current, const RE::NiPoint3& target)
{
	const float dot = std::clamp(current.Dot(target), -1.0f, 1.0f);
	const float angle = DirectX::XMConvertToDegrees(DirectX::XMScalarACosEst(dot));

	return std::clamp((AlignmentFadeEndAngle - angle) / (AlignmentFadeEndAngle - AlignmentFadeStartAngle), 0.0f, 1.0f);
}

inline void SkySync::ShadowFader::ClampDirection(RE::NiPoint3& dir)
{
	const float minDegrees = globals::features::skySync.settings.MinShadowElevation;
	const float minElev = DirectX::XMConvertToRadians(minDegrees);
	const float elev = DirectX::XMScalarASinEst(dir.z);
	if (elev >= minElev)
		return;

	SetElevation(dir, minElev);
}

#undef I18N_KEY_PREFIX
