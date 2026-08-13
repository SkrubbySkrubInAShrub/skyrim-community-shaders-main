# Scene Settings Framework

Catalog-backed system that overrides feature settings by **interior**, **time of day**, **weather**, and
**location**, blending between them as the game state changes.

**Provenance:** ported from `Dlizzio/open-shaders` branch `feat/scene-manager` (upstream range
`06eaa584a..1119234f9`), squashed into `feat(scene-manager): port scene settings framework`. The port is
**framework only**: the entire user-facing authoring UI was deliberately excluded, and Community Shaders
branding was kept throughout. See [What was dropped](#what-was-dropped) before assuming a missing piece is a
bug.

## Files

| File | Role |
| ---- | ---- |
| `src/SceneSettingsManager.{h,cpp}` | The system. Storage, persistence, resolver, apply/restore, blending. |
| `src/SceneSettingsPolicy.h` | Hand-maintained allow/deny lists consumed by the manager. |
| `src/Features/SceneManager.{h,cpp}` | Thin `Feature` wrapper that drives the manager's lifecycle. |
| `cmake/generate_scene_settings_catalog.py` | Build-time generator; parses `src/**/*.{h,hpp,cpp,cxx}`. |
| `features/Scene Manager/` | `CORE` marker + `SceneManager.ini` (version `1-0-0`). |
| `tests/test_scene_settings_catalog_generator.py` | Generator unit tests (hermetic + catalog assertions). |
| `tests/test_scene_settings_policy.py` | Checks the policy lists against the real catalog. |

Generated into `${CMAKE_CURRENT_BINARY_DIR}/generated` (e.g. `build/ALL/generated`), never committed:

-   `SceneSettingsCatalog.generated.h` / `.cpp` — the `SceneSettingsCatalog` namespace and the catalog array.
-   `FeatureSceneSettingsAdapters.generated.cpp` — per-feature control resolvers (see
    [Known gaps](#known-gaps)).

## How the catalog is built

`CMakeLists.txt` runs the generator as a custom command before compiling, with
`--min-entries 250` as a regression gate. It statically parses feature sources and derives, for every
persisted setting:

-   the serialized address (`serializedPath` / `serializedKey` / `serializedComponent`) used to reach the
    value inside a feature's settings JSON
-   the display address (`displayPath`, `selectorPath`) reconstructed from `DrawSettings()` brace scopes and
    tab helpers
-   the editor semantic (`Toggle` / `Numeric` / `Choice` / `Text` / `Generic`), numeric bounds, display scale,
    `Log2` transforms, and combo choice values, read from the actual ImGui call
-   i18n keys from the `T(TKEY(...), "...")` call wrapping the control
-   flags: `Persisted`, `Transitionable`, `Hidden`, `BooleanControl`, `SceneControllable`

`validate_entries()` fails the build on duplicates, contradictory visibility, choices with fewer than two
distinct values, `Log2` bounds that include zero, and incomplete aggregates (a vector exposing only some
components).

This makes the framework **feature-agnostic**: a feature exposes scene-controllable settings simply by
persisting them and drawing them with a recognized ImGui call. There is no registration API and no
per-feature code to write. This is what replaced the deleted `WeatherVariableRegistry`.

Current catalog on this fork: **325 entries**.

## Runtime flow

1.  **Boot** — `SceneManager::SetupResources()` calls `LoadAll()` (overwrites + user settings for
    non-weather scene types). `globals::sceneSettingsManager` is wired in `globals::OnInit()`.
2.  **Data loaded** — `SceneManager::DataLoaded()` calls `OnDataLoaded()` (weather/location data needs
    `TESDataHandler` for SPID resolution) and registers `MenuOpenCloseEventHandler`, which flags a cell
    transition when the loading menu closes.
3.  **Per frame** — `State::Draw()` calls `SceneManager::Update()` → `SceneSettingsManager::Update()` →
    `ResolveAndApply()`. The resolver early-outs unless something it depends on moved: interior flag,
    location/cell FormID, game hour (`kHourUpdateThreshold`), or weather pair + lerp.

### How values are applied

There is no per-setting writer and no pointer patching. `ApplyCatalogSceneSettings()`:

1.  `feature.SaveSettings(json)` to snapshot the feature's own serialization
2.  walk the catalog's serialized address and overwrite the primitives, rejecting type mismatches
3.  `feature.LoadSettings(json)` to push the whole block back
4.  on exception, reload the original snapshot

`baselineSettings` holds the pre-override value so a setting that leaves scope is restored rather than left
stuck. Failed applies back off (`kApplyRetryDelay`, 2s) and log once per signature instead of every frame.

**Consequence:** a feature's `SaveSettings`/`LoadSettings` pair is the contract. A setting that is not
round-trippable through them cannot be scene-controlled, no matter what its UI looks like.

### Precedence and blending

`BuildResolvedSettings()` overlays in order, later winning per setting address:

```
interior  →  time of day  →  weather  →  location/cell
```

Within a layer, `EntrySource::Overwrite` (mod-shipped files) is overlaid first and `EntrySource::User`
overrides it, so a shipped overwrite acts as that layer's default and the user's own entry for the same
address always wins. `SettingsUser.json` remains the baseline beneath all of this.

-   **Time of day** — six periods (`Dawn`, `Sunrise`, `Day`, `Sunset`, `Dusk`, `Night`) with hour ranges in
    `kPeriodHours`; `Night` wraps midnight as `21..28`. Floats cross-fade across a `kTransitionHours` (0.5h)
    zone at each boundary. Non-float settings snap.
-   **Weather** — per-weather configs are always stored per period; floats blend across
    `Sky::currentWeatherPct` between the outgoing and incoming weather.
-   Writes smaller than `kBlendEpsilon` (1e-3) are skipped so blending does not spam `LoadSettings`.

### SceneLayerGuard

`SceneSettingsManager::SceneLayerGuard` is an RAII suspend of the scene layer. Anything that reads or writes
a feature's *base* settings must hold one, otherwise it captures an overridden value as if it were the user's
choice. It is default-constructed (`SceneLayerGuard guard;`) and no-ops when the manager singleton does not
exist yet. Current holders: `State::Load`, `State::SaveToJson` and `State::LoadFromJson`, two internal manager
paths, and six DevBench bridge endpoints. Add one to any new code path that serializes feature settings.

In `State::SaveToJson` / `State::LoadFromJson` the guard is declared **before** `m_mutex` is taken, so the
resolve it triggers on destruction does not run while the lock is held.

## On-disk layout

Rooted at `Util::PathHelpers::GetSceneSettingsPath()` = `<CommunityShaders>/SceneSettings`.

| Path | Contents |
| ---- | ---- |
| `SceneManager.json` | All user-authored entries (interior, TOD, weather, location) in one document. |
| `InteriorOnly/`, `TimeOfDay/<Period>/` | Mod-shipped overwrite files per scene type. |
| `Weather/<SPID>/` | Per-weather overwrites, folder keyed by `Util::FormIdToSpid`. |
| `Locations/<form key>/` | Location **and** cell overwrites share one tree; the target's type comes from its form. |

`SceneManager.json` is written atomically. If the existing document is present but not a JSON object, saves
are **blocked** rather than clobbering it, and unknown fields on an entry are preserved through
`serializedTemplate` for forward compatibility.

## Policy

`src/SceneSettingsPolicy.h` is hand-maintained and pruned to features that exist in this fork:

-   `kSettingBlacklist` — settings that must never be scene-overridden, matched by catalog address prefix.
    **Currently empty**; upstream's entries all pointed at features this fork does not have.
-   `kLocationFeatureWhitelist` (5) and `kTimeOfDayFeatureWhitelist` (7) — which features those scene types
    may target.

When adding a feature to a whitelist, run `tests/test_scene_settings_policy.py`; it fails if a name is not
discovered in the generated catalog.

## Feature-facing contract

`Feature` gained three virtuals in this port (`src/Feature.h`):

| Virtual | Default | Meaning |
| ------- | ------- | ------- |
| `IsAlwaysEnabled()` | `false` | Infrastructure that cannot be disabled at boot. `State` erases it from `disabledFeatures` and refuses toggles. |
| `UsesMainSettings()` | `true` | Persists through the shared settings JSON; gates override discovery. |
| `HasRestoreDefaults()` | `true` | Whether the UI offers "Restore Defaults". Currently unread (see [Known gaps](#known-gaps)). |

`Feature::RegisterWeatherVariables()` was **removed**. Features no longer register anything; they just draw
plain ImGui controls over persisted members.

## What was dropped

Everything below exists upstream and was intentionally left out. Do not treat it as missing work unless
someone asks for the UI layer.

### Excluded upstream files

| File | Lines | What it was |
| ---- | ----- | ----------- |
| `src/CSEditor/SceneSettingsUI.{h,cpp}` | ~3180 | The authoring UI: add-setting dialogs, per-scene panels, weather scene panel. |
| `src/SceneSettingsUIHooks.{h,cpp}` | ~776 | ImGui interception marking scene-controlled widgets and offering right-click capture. |
| `src/Features/SceneManagerUI.{h,cpp}` | ~34 | `SceneManager::DrawSettings()` body. |

Correspondingly, `SceneManager` here has **no** `DrawSettings()` and **no** `PostPostLoad()` (upstream's
called `SceneSettingsUIHooks::Install()`).

### Removed from this fork

-   `src/WeatherManager.{h,cpp}`, `src/WeatherVariableRegistry.h` and `docs/weather-system-docs/` — the old
    registry the catalog replaces.
-   `Util::WeatherUI::{IsWeatherControlled,SliderFloat,Checkbox,ColorEdit3,ColorEdit4}` — its only call sites
    were in `ExponentialHeightFog` (22) and `IBL` (7), and both are now plain `ImGui::` calls.
-   `src/CSEditor/InteriorOnlyPanel.{h,cpp}` and its EditorWindow category. `EditorWindow::LoadSettings()`
    remaps a saved `"Interior Only"` category to `"Weather"`.
-   The WeatherWidget per-feature "Features" tab and its `featureSettings` map (upstream replaced it with a
    "Scene Settings" tab that lives in the excluded UI), plus `OpenWeatherFeatureSetting()`.
-   The "Pause Weather Overrides" checkbox in `FeatureListRenderer`.
-   `SupportsVR()` on `SceneManager` — that virtual is an `alandtse` lineage concept and does not exist on
    this fork's `Feature`.
-   Upstream's `InvertedCheckbox`, `RadioButton`, `ActiveControlStorageGuard` and
    `GetActiveControlStorageAddress` UI helpers, used only by the excluded UI.

### Surviving UI

-   `FeatureListRenderer` shows a scene-controlled indicator and a **Scene Specific Settings** pause toggle
    per feature (`IsFeaturePaused` / `SetFeaturePaused`).
-   `CSEditor` flags a weather that has scene settings via `HasWeatherConfig`.

## Known gaps

-   **No authoring UI.** Entries can only be created by editing `SceneManager.json` or shipping overwrite
    files. Every `AddSetting` / `UpdateEntryValue` / `Export*` API is implemented and unused. Restoring the
    UI means porting `SceneSettingsUI` + `SceneSettingsUIHooks` and re-adding the two `SceneManager`
    overrides.
-   **The control resolvers are dead.** `FeatureSceneSettingsAdapters.generated.cpp` compiles and its static
    initializers call `SceneSettingsCatalog::RegisterControlResolver`, but nothing in `src/` calls
    `FindSettingForControl` or `GetVirtualAggregateControls` — those were the excluded UI hooks' entry
    points. The registrations are inert, not broken; they are the seam the UI layer plugs into.
-   **`Feature::HasRestoreDefaults()` is unread.** Nothing in `src/` calls it; the "Restore Defaults" action it
    gates lives in the excluded UI. Kept as the seam for that layer, same as the control resolvers.
-   Catalog metadata aimed purely at presentation (`displayPath`, `selectorPath`, `AggregatePresentation`,
    `UnifiedEditMode`, choice display names) is generated and validated but unread at runtime for the same
    reason.

## Testing

```bash
python -m unittest tests.test_scene_settings_catalog_generator tests.test_scene_settings_policy
```

69 tests. Neither suite runs in CI; run them after touching the generator or the policy lists.

The generator can be run standalone to inspect its output:

```bash
python cmake/generate_scene_settings_catalog.py --source-dir . --out-dir /tmp/catalog --min-entries 250
```

The generator test file is upstream's with the open-shaders-coupled assertions removed (`CSUtility`,
`PostProcessing`, `LightLimitFix` contact shadows, the `ExtendedTranslucency`/`Upscaling`/`VR` regression
list, and the inverted-display IBL toggles). Do not re-add assertions naming features this fork lacks.
