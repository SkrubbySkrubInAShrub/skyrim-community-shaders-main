# Scene Settings Framework

Catalog-backed system that overrides feature settings by **interior**, **time of day**, **weather**, and
**location**, blending between them as the game state changes.

**Provenance:** ported from `Dlizzio/open-shaders` branch `feat/scene-manager` (upstream range
`06eaa584a..1119234f9`), squashed into `feat(scene-manager): port scene settings framework`, then
re-synced against upstream **rev3** (location categories, location transitions, resolver caching, the
generic scene copy API). The port is **backend only**: upstream's authoring UI was never taken, and
Community Shaders branding was kept throughout. This fork grows its own editor in `src/CSEditor/`, so
several backend APIs are complete and waiting for a caller. See
[Backend ready for UI](#backend-ready-for-ui) and [What was dropped](#what-was-dropped) before assuming a
missing piece is a bug.

**On-disk compatibility with upstream is a hard requirement:** a `SceneManager.json` authored in
open-shaders must load in Community Shaders with every setting honored, and vice versa. Any divergence
below is behavioral, never a change to the file format.

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
interior  →  time of day  →  weather  →  location (category → location → cell)
```

Within a layer, `EntrySource::Overwrite` (mod-shipped files) is overlaid first and `EntrySource::User`
overrides it, so a shipped overwrite acts as that layer's default and the user's own entry for the same
address always wins. `SettingsUser.json` remains the baseline beneath all of this.

-   **Time of day** — six periods (`Dawn`, `Sunrise`, `Day`, `Sunset`, `Dusk`, `Night`) with hour ranges in
    `kPeriodHours`; `Night` wraps midnight as `21..28`. Floats cross-fade across a `kTransitionHours` (0.5h)
    zone at each boundary. Non-float settings snap.
-   **Weather** — per-weather configs are always stored per period; floats blend across
    `Sky::currentWeatherPct` between the outgoing and incoming weather.
-   **Location**: see [Location targets](#location-targets); the chain resolves broadest to narrowest, so a
    cell entry wins over the location that contains it, which wins over the category that classifies it.
-   Writes smaller than `kBlendEpsilon` (1e-3) are skipped so blending does not spam `LoadSettings`.

**Divergence from upstream, deliberate:** upstream overlays `User` first and `Overwrite` second, so a
mod-shipped overwrite wins over the user's own entry for the same address. This fork inverts the order
everywhere (`OverlayAllEntries`, `CollectPeriodValueGroups`, `BuildEffectiveContextEntries`) so the user
wins. Both read the same files; only the winner differs. Preserve the inversion in any new overlay code.

### Location targets

A location resolves to a **chain** of targets, broadest first, built by `BuildLocationTargetChain()`:

| `LocationTargetType` | Source | Notes |
| -------------------- | ------ | ----- |
| `Category` | `LocType*` keywords on every `BGSLocation` in the parent chain | Name is the keyword editor ID with the `LocType` prefix stripped and prettified. Deduplicated by keyword FormID. |
| `Location` | The `BGSLocation` chain, walked through `parentLoc` and reversed | Cycle-guarded by a visited FormID set. |
| `Cell` | The player's parent cell | Its `editorId` is the coc code. |

`GetCurrentLocationTargets()` caches the player's chain by location + cell FormID.
`ResolveLocationTargetChain(type, formKey)` answers the same question for an **arbitrary** target: it
reuses the player's chain when the target is in it, otherwise it looks the form up through
`Util::ParseSpid` / `Util::SpidToFormId` and rebuilds. A `Category` has no standalone chain (it only exists
through the locations that carry it), so an off-chain category resolves to empty.

Persisted under `location.categories` / `location.locations` / `location.cells` in `SceneManager.json` and
under `Locations/<form key>/` for overwrites.

### Location transitions

Location float overrides ease in and out instead of snapping, because crossing a cell boundary otherwise
pops every affected setting.

-   Duration comes from the entry's own `transitionSeconds`, falling back to the global
    `location.transitionSeconds` (`kDefaultLocationTransitionSeconds` = 5s, clamped to
    `kMaxLocationTransitionSeconds` = 300s).
-   Time comes from `globals::state->timer`, so transitions freeze with the game rather than the wall clock.
-   Easing is smoothstep (`t * t * (3 - 2t)`). `StartLocationTransitions()` retargets from the **live** eased
    value, so reversing direction mid-transition never snaps.
-   Only a location **context change** animates. Editing a value in place snaps to it: `ResolveAndApply()`
    passes `locationContextChanged` as the `animateChanges` flag, while `locationOverridesDirty` alone just
    reconciles the targets.
-   In-flight transitions are grouped into per-feature `LocationTransitionBatch`es so one `LoadSettings` call
    covers every animating setting of a feature. A failing batch logs once and backs off by signature.
-   `RestoreAppliedSettings()` clears all transitions first, so tearing the scene layer down cannot leave a
    half-eased value applied.

A duration set on one component of an aggregate control (a colour, a vector) applies to the whole control:
`SetLocationEntryTransitionSeconds()` expands the selection through `GetCopyGroupKey()` before validating.

### Resolver caching

The resolver runs every frame, so everything it can precompute is cached and invalidated by revision
counter rather than rebuilt:

| Cache | Invalidated by | Holds |
| ----- | -------------- | ----- |
| `timeOfDayValueGroups`, `weatherValueGroups` | `sceneValueRevision` | Per-address `std::array<std::optional<float>, kPeriodCount>` period values, so blending never re-walks the entry lists. |
| `featureBaseSnapshots` | `InvalidateFeatureSnapshot()` | A feature's settings JSON with the scene layer folded back out, used as the baseline source. |
| `configuredFeatureNamesCache` | `configuredFeatureNamesRevision` | Which features have any scene entry at all. |
| `cachedLocationOverrides` | `locationOverridesDirty` | The resolved location layer, rebuilt only when the target chain or an entry moved. |
| `resolvedSettingsScratch` | reused every resolve | The resolved map itself, so the per-frame path does not reallocate. |
| `cachedLocationTargets` | location/cell FormID change | The player's target chain. |

**Divergence from upstream, deliberate:** upstream bumps `sceneValueRevision` at ~22 call sites and sets
`locationOverridesDirty` at ~9. This fork funnels both through `MarkSceneValuesDirty()`, called from
`BumpEntryPresentationRevision()` and `ReapplyIfActive()`. That is a strict superset of upstream's
invalidation; the cost is at most one extra period-map rebuild per user action, never per frame. Route new
mutations through those two functions instead of touching the counters directly.

Apply failures are keyed by a `size_t` signature (`CombineHash` / `HashSceneSettingValue`) so a retry is
skipped until the pending values actually change.

### SceneLayerGuard

`SceneSettingsManager::SceneLayerGuard` is an RAII suspend of the scene layer. Anything that reads or writes
a feature's *base* settings must hold one, otherwise it captures an overridden value as if it were the user's
choice. It is default-constructed (`SceneLayerGuard guard;`) and no-ops when the manager singleton does not
exist yet. Current holders: `State::Load`, `State::SaveToJson` and `State::LoadFromJson`, two internal manager
paths, and six DevBench bridge endpoints. Add one to any new code path that serializes feature settings.

In `State::SaveToJson` / `State::LoadFromJson` the guard is declared **before** `m_mutex` is taken, so the
resolve it triggers on destruction does not run while the lock is held.

## Generic Scene Copy API

Copies settings between any two scene contexts (a time-of-day period, a weather period, or a location
target). **Fully implemented, no caller yet**: this is the largest ready-to-hook surface in the manager.

A context is a `SceneContextId`: a `SceneContextType` plus whichever of `period` / `weatherId` /
`locationType` + `locationFormKey` that type uses. `IsValidSceneContext()` rejects any mixed combination,
so a malformed context can never reach the mutation path.

| Method | Const | Purpose |
| ------ | ----- | ------- |
| `GetCopySources(destination, scope, setting)` | yes | Every context that holds something usable, with a localized label and a compatible-setting count. Excludes the destination itself. Sorted by type, then label. |
| `GetCopyCandidates(source, destination, scope, setting)` | yes | Per-setting preview: display name, value, `compatible`, `conflicts`. Drives a confirmation dialog. |
| `CopySettings(source, destination, conflictPolicy, scope, setting)` | no | Performs the copy and returns a `CopyResult` (`copied` / `skipped` / `overwritten` / `incompatible` / `hadConflicts` / `cancelled`). |

`CopyScope::EntireContext` takes everything in the source; `CopyScope::Setting` takes one
`SettingIdentity` (and, if it names an aggregate component, its whole control).

**Compatibility.** A setting is copyable when it is in the catalog, allowed for the destination's scene
type, its value passes `IsSceneSettingValueAllowed`, and the destination has no active `Overwrite` shadowing
it. Only the location layer accepts non-float settings; every other destination requires numerics.
Compatibility is **group-aware**: members of one logical control (a colour, a vector) share a
`CopyGroupKey`, and one unusable member disqualifies the whole group, so a copy can never leave half a
colour behind.

**Conflicts** are compatible settings the destination already holds as a `User` entry.
`CopyConflictPolicy::SkipExisting` leaves the whole conflicting group alone, `OverwriteExisting` replaces
the value in place (keeping the existing `originalValue`), and `Cancel` aborts the entire operation and
returns `cancelled` without touching anything.

**Transactionality.** Everything is validated and staged into a pending list first; the destination config
is only materialized once the copy is known to produce entries, and one `CommitSceneSettingChanges()` at the
end does a single save plus a single reapply.

**New entries get a correct `originalValue`** so revert and restore still work: from the destination's lower
layers for a location, from the time-of-day layer for a weather period, and from the feature baseline
otherwise. Copying into a location also carries a transition duration: the destination entry's own duration
wins, otherwise the source's.

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
| upstream `SceneSettingsUI.{h,cpp}` | ~3180 | The authoring UI: add-setting dialogs, per-scene panels, weather scene panel. This fork's `src/CSEditor/SceneSettingsUI.{h,cpp}` is unrelated in-house work that happens to share the name. |
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

### Existing UI

-   `src/CSEditor/SceneSettingsUI.cpp` is this fork's own editor: the time-of-day period bar with its
    automatic time pause, the interior toggle, the per-feature list, and per-location windows. It uses only
    `GetCurrentGameHour` / `SetGameHour`, `GetCurrentPeriod`, `Get*RelevantFeatureNames`,
    `GetFeatureDisplayName`, and the `LocationTarget` accessors.
-   `FeatureListRenderer` shows a scene-controlled indicator and a **Scene Specific Settings** pause toggle
    per feature (`IsFeaturePaused` / `SetFeaturePaused`).
-   `CSEditor` flags a weather that has scene settings via `HasWeatherConfig`.

## Backend ready for UI

Complete, tested-by-construction backend with no caller in `src/`. Anything here can be wired up without
touching the manager:

| Surface | Entry points | What a UI still needs to build |
| ------- | ------------ | ------------------------------ |
| Entry authoring | `AddSetting`, `AddWeatherSetting`, `AddLocationSetting`, `UpdateEntryValue`, `RemoveSetting`, pause toggles | Add-setting dialogs and per-scene entry tables. Entries can otherwise only be made by hand-editing `SceneManager.json`. |
| [Generic scene copy](#generic-scene-copy-api) | `GetCopySources`, `GetCopyCandidates`, `CopySettings` | A source picker, a candidate preview listing conflicts, and a conflict-policy prompt. |
| Location transitions | `GetLocationTransitionSeconds` / `SetLocationTransitionSeconds`, `GetLocationEntryTransitionSeconds` / `SetLocationEntryTransitionSeconds` | A global duration slider and a per-entry override. The setter already expands aggregates and validates the whole edit before applying it. |
| Location targets | `GetCurrentLocationTargets`, `GetAuthoredLocationTargets`, `AddLocationTarget`, `RemoveLocationTarget`, `IsLocationTargetAuthored` | Partly used by the location windows; the category layer has no UI at all. |
| Overwrite export | `Export*` | Author-facing "ship this as an overwrite" action. |
| Debug inspection | `GetDebugSnapshot` | A resolver inspector. The snapshot already flattens entries, resolved values, per-period values and active targets. |

Nothing on this list is a stub: each path validates its input, persists, and reapplies. Treat a missing UI
as the only missing piece.

## Known gaps

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
