# Scene Settings: comparison against open-shaders `05f084a4a4`

Point-in-time review of this fork's scene settings framework against
[alandtse/open-shaders `05f084a4a4`](https://github.com/alandtse/open-shaders/commit/05f084a4a4abfca143f7f1f03e032be20e33390c)
("feat: Scene Manager (#589)", 2026-09-11). Companion to the design document,
[Scene Settings Framework](./scene-settings-framework.md), which describes what this fork *does*; this one
describes what upstream does that this fork does not, and why.

Read the [Chronology](#chronology) section first. It changes how several of the differences below should
be read.

## Chronology

| Date | Commit | What |
| ---- | ------ | ---- |
| 2026-08-14 | open-shaders `9e726afcd8` ("rev 3") | The revision this fork ported from. |
| 2026-09-11 | open-shaders `05f084a4a4` | The revision compared against here. |
| 2026-09-21 | `ee2f229fd` | `fix(scene): re-resolve location transition endpoints` |
| 2026-09-21 | `4c8038dd4` | `feat(overwrites): add Feature Overwrites page` |
| 2026-09-21 | `0092a8fd2` | `feat(cs-editor): rework viewport layout` |

Three of this branch's commits post-date the upstream revision being compared against. Work in them is
**newer than upstream**, not behind it. Two consequences:

-   The override-persistence layer (`SettingsOverrideManager` + `Utils/SettingsCatalog` +
    `Features/FeatureOverwrites`, given its current shape by `4c8038dd4`) is this fork's own answer to the
    problem upstream solves with `SettingsOverridePersistence.cpp` + `Utils/SettingsPatch.*`. It is
    divergence, not a gap. See [Divergence by design](#divergence-by-design).
-   `RefreshLocationTransitionEndpoints` call ordering differs from upstream because `ee2f229fd` changed it
    deliberately. Upstream's ordering is not a fix waiting to be picked up.

## Method

Both trees are reachable from this repository (`alandtse` remote), so every claim below was checked against
the real files rather than the commit page:

```bash
git show 05f084a4a4:src/SceneSettingsManager.h           # upstream API surface
git show 05f084a4a4:src/SceneSettingsManager.cpp         # upstream resolver
git ls-tree -r --name-only 05f084a4a4 | grep -i scene    # upstream file inventory
```

## Shape of the two trees

Upstream keeps the framework in a few large files; this fork split it by responsibility (`d2e9725d0`).

| | open-shaders `05f084a4a4` | this fork |
| --- | --- | --- |
| Manager core | `src/SceneSettingsManager.cpp` (8,168 lines) + `.h` (1,045) | `src/CSEditor/SceneManager/`, 9 translation units, 12,264 lines including UI |
| Authoring UI | `src/CSEditor/SceneSettingsUI.cpp` (6,056) + `src/SceneSettingsUIHooks.cpp` (926) | `SceneSettingsUI.cpp` (673) + `SceneWidgetBinding.cpp` (1,336) + `SceneWidgetInterceptor.cpp` (395) + `ScenePageToolbar.cpp` (782) |
| Override persistence | `SettingsOverrideManager.{h,cpp}` (1,435) + `SettingsOverridePersistence.cpp` (356) + `Utils/SettingsPatch.{h,cpp}` (71) | `SettingsOverrideManager.{h,cpp}` (1,585) + `Utils/SettingsCatalog.{h,cpp}` (196) + `Features/FeatureOverwrites.{h,cpp}` (304) |
| Python tests | 5 suites, 3,744 lines | 2 suites, 1,782 lines |

The UI layers share a file name and nothing else. This fork's UI is independent work, so a UI difference is
never a port that was missed.

## Genuinely missing

Ordered by what a user would notice.

### 1. Per-period location overrides (ported)

Ported. `LocationSceneConfig` now derives from `PeriodicSceneConfig`, like weather, and upstream per-period
location documents load with their periods intact.

Adapted rather than copied: upstream's per-location `showTimeOfDay` toggle picks between views of one entry
list, whereas here a weather or location keeps a flat set and a per-period set side by side and
`timeOfDayEnabled` picks which one resolves (see *Saved sets* in the
[framework doc](./scene-settings-framework.md#precedence-and-blending)). Switching modes never discards the
other set. A legacy `showTimeOfDay: true` migrates to the per-period mode; `false` is ignored, since it only
ever hid the period bar.

Note that **interior scenes are not periodic upstream either**. Upstream stores interior settings in a flat
`std::vector<SettingDescriptor> interiorSettings`, the same as here.

### 2. `LocationType` and `Worldspace` location targets (ported)

Ported. `LocationTargetType` now has upstream's five kinds, and the chain resolves worldspace, location type,
region, location, cell, broadest first. A location type is a `LocType*` keyword on the innermost location.
Upstream's older `categories` section is read and migrated to `locationTypes`.

The editor keeps the live-chain table and adds a searchable picker over every target the game defines, so a
place the player is not standing in can be authored too.

### 3. The `FeatureSceneEdit` live-preview session

The largest single absent subsystem, roughly 800 lines across upstream's manager plus its UI callers. It is
a modal editing session over one feature in one scene context:

```cpp
bool BeginFeatureSceneEdit(Feature* feature, const SceneContextId& context);
bool CaptureFeatureSceneEditChanges(Feature* feature);   // pull live controls into the preview
bool StoreFeatureSceneEdit();                            // commit the preview as User entries
void EndFeatureSceneEdit(bool storeChanges = true);      // or discard it
bool HasPendingFeatureSceneEdits() const;
void SetFeatureSceneEditOverwritesPaused(bool paused);   // preview without mod overwrites
bool IsFeatureSceneEditSettingOverwritten(...) const;
bool IsFeatureSceneEditSettingAltered(...) const;
```

backed by a `FeatureSceneEditState` holding `originalSettings`, `workingSettings`, a `workingOverrides`
resolved map, a `dirty` flag, and `overwriteAddresses` / `alteredAddresses` sets. The preview is injected
into the resolver by `ApplyFeatureSceneEditPreview(resolved)` at the end of the resolve.

This fork reaches the same **goal** by a different route: `SceneWidgetInterceptor` replays the feature's own
`DrawSettings()` bound to a scene context, and `SceneWidgetBinding::Guard` creates, edits, pauses and
deletes entries in place. The goal shared by both is "author a scene entry using the feature's real
controls," and this fork does achieve that.

What the session model gives that interception does not:

-   **Try before committing.** Upstream's edits live in `workingSettings` until `StoreFeatureSceneEdit`;
    `EndFeatureSceneEdit(false)` throws them away. Here, a widget edit is an entry mutation immediately.
    There is no "preview this weather's look, then decide."
-   **A temporary overwrite bypass.** `SetFeatureSceneEditOverwritesPaused` lets a user see past a
    mod-shipped overwrite for the duration of the session without touching any saved pause flag. Here,
    pausing an overwrite is a persisted state change (and, as of this review, is refused outright on the
    Overwrite layer: see [What was fixed](#what-was-fixed-during-this-review)).
-   **An "altered" set**, distinct from "overwritten", for marking which controls the session has moved.

This subsystem was **not audited**, only confirmed absent. Nothing here corresponds to it. If it is ever
wanted, it is a design port, not a cherry-pick, because the UI it drives does not exist here either.

### 4. Override files with unrecognized keys apply silently (ported)

Ported. `Util::Settings::CollectUnknownSettingKeys` checks each feature override against the feature's
`SaveSettings` blob before `ApplyOverrides` merges it. An override naming any unknown key is skipped whole,
logged as a warning with the dotted paths, and reported under Feature Issues.

Adapted rather than copied: `ApplyOverrides` is also called on an empty object
(`GetMergedOverrideSettings`), which has no shape to check against, so the verdict is stored on
`OverrideInfo::unknownKeys` by the last shaped apply and reused there. `_`-prefixed keys are ignored, as
`MergeJson` already skips them. Global overrides are not checked, since the main settings blob they merge into
is not a canonical shape.

The original finding, for reference:

Upstream's `Util::Settings` carries two functions this fork has no counterpart for:

```cpp
void CollectUnknownSettingKeys(const json& a_incoming, const json& a_known,
    const std::string& a_prefix, std::vector<std::string>& a_out);
bool ApplyPatch(Feature& a_feature, const json& a_patch, std::vector<std::string>& o_unknownKeys);
```

with the rationale stated in upstream's own comment: *"The settings serializers drop keys they don't
recognize, so a mis-nested or misspelled key would otherwise apply nothing while still reporting success."*
`ApplyPatch` returns false and applies nothing when the patch names any key the feature's `SaveSettings`
blob does not.

This fork's `SettingsOverrideManager::ApplyOverrides` (`src/SettingsOverrideManager.cpp:173`) calls
`MergeJson(featureJson, override.overrideData)` and logs `"Applied override from {} to {}"` on success.
`ValidateOverrideFormat` and `ValidateJsonDataTypes` check structure and types but never key existence, so a
mod author who misspells a key, or nests it one level wrong, gets a success log and no effect, with nothing
in the log to find. This is the cheapest of the missing items to adopt and the one with the clearest payoff
for mod authors.

Note this is a gap in the *override* layer, which post-dates the compared revision; the scene layer proper
validates entries against the generated catalog (`ValidateSceneSettingEntry`) and does reject unknown
addresses.

### 5. Runtime, persistence and audit test suites

Upstream ships five Python suites; this fork has two.

| Suite | Upstream | Here |
| ----- | -------- | ---- |
| `test_scene_settings_catalog_generator.py` | 521 | 1,576 (grown well past upstream) |
| `test_scene_settings_policy.py` | 716 | 206 |
| `test_scene_settings_runtime.py` | 1,882 | absent |
| `test_scene_settings_persistence.py` | 340 | absent |
| `test_scene_settings_audit.py` | 285 | absent |

The three absent suites do something unusual: they slice named function bodies out of
`src/SceneSettingsManager.cpp` as text, paste them into a synthetic C++ harness with stubbed globals,
compile it and run it. They cover behavior nothing here tests, including toolbar loading and overwrite
locks, exporter/loader metadata symmetry, and (Windows-only) that overwrite removal releases its read handle
before mutating the file.

They cannot be copied across, because they are all built on
`MANAGER_PATH.read_text()` plus `extract_function(manager, name)` against a single monolithic manager, and
this fork split the manager across nine translation units. Retargeting them means teaching the harness to
search a file set, which is tractable but is real work.

The consequence is worth stating plainly: **this fork has no behavioral test coverage of the manager at
all.** Both local suites test the catalog generator and the policy lists. Every C++ change to the resolver,
the serializer or the apply pipeline is reviewed by reading, including the fixes listed at the end of this
document.

### Not missing, despite appearances

-   **`src/Features/SceneSelector.{h,cpp}`** (1,142 lines), upstream's standalone "Weather Picker" overlay
    feature. This fork has the same functionality integrated into `CSEditor`
    (`CSEditor::DrawWeatherPickerSection`). Packaging difference, not a gap.
-   **`src/Features/Effects11/SettingsPatches.{h,cpp}`** is specific to a feature this fork does not carry.

## Divergence by design

Differences that are deliberate. Do not "fix" these toward upstream without a decision to.

-   **Serialization safety.** Upstream patches the serialized document after the fact
    (`RestoreBaselinesInSerializedSettings`). This fork's `SceneSettingsManager::SceneLayerGuard` suspends
    the scene layer so the serializer never observes scene values in the first place. The guard is the
    stronger mechanism: it cannot miss a caller that forgot to patch. Adopting upstream's would be a
    regression.
-   **Authoring UI.** Widget interception versus the `FeatureSceneEdit` session, discussed above.
-   **Manager layout.** Nine focused translation units versus one 8,168-line file. This is what blocks the
    test-suite port, and it is still the right call.
-   **Override persistence.** `SettingsOverrideManager` + `Utils/SettingsCatalog` +
    `Features/FeatureOverwrites` here, `SettingsOverrideManager` + `SettingsOverridePersistence` +
    `Utils/SettingsPatch` upstream. Comparable size, different models: upstream layers a mask-based patch
    (`BuildUserOverride`, `SelectSettings`, `RestoreSettings`, `ApplyLayers`), this fork keeps per-feature
    user override files with a combined hash. `Util::Settings::SelectSettingPaths` exists in both. Since
    this fork's shape post-dates `05f084a4a4` (`4c8038dd4`), upstream's is not the newer design.
    The one piece worth taking from it regardless, the unknown-key check (item 4 above), has been ported.

## Needs a product decision: Overwrite versus User precedence

The two forks resolve the layers in opposite orders, and this is a confirmed inversion rather than an
artifact of reading.

Here, `CollectPeriodValueGroups` (`SceneSettingsResolve.cpp:696`) iterates
`{ EntrySource::Overwrite, EntrySource::User }` so the user's entry lands last and wins, with the comment
*"Shipped overwrites are the layer's defaults; the user's own entry for the same address wins."*

Upstream resolves the user layer first and then writes the overwrite layer over it: the period blend does
`periodValue = overwrite->second[index].value_or(periodValue)` after the user value is in hand
(`SceneSettingsManager.cpp:3485`), and non-numeric overwrites are assigned unconditionally afterwards
(`:3498`). Upstream's mod-shipped overwrite beats the user's hand-set value.

Which is correct is a product question about whether a mod ships a *default* a user may override, or a
*requirement* the mod needs honored. This fork's answer is the former. Changing it is a one-line reorder
plus a compatibility note; nobody should change it without deciding that first.

## What was fixed during this review

Twelve issues found during the review were fixed in the working tree at the time of writing; see
`git log`/`git diff` on the `scenemanager` branch for the exact changes. Summary, for orientation:

**Data loss.** `State::Save` truncating `SettingsUser.json` when a feature serializer throws; a hash
mismatch calling `std::filesystem::remove` on a user override file; non-atomic writes throughout, now
funneled through shared `Util::FileHelpers::WriteFileAtomically` / `WriteJsonAtomically`; both
`FeatureOverwrites` save paths running without a `SceneLayerGuard`, which baked scene-applied values into
authored overwrite files.

**Resolver.** `VerifyPendingApplies` conflating "the feature did not keep the value" with "we never touched
it", which left a clamping feature stuck on its scene value after the scene ended; `RestoreAppliedSettings`
erasing the baseline before confirming the restore landed; a stale committed context firing a phantom
location transition on the first post-load resolve; the missing `!interior` guard on the hour term; one
value comparator split into an exact `ResolvedValuesEqual` and a float-tolerant `AppliedValuesEqual`.

**UI and persistence.** `weatherShowTimeOfDay` had a serializer and a reader but no setter anywhere in
`src/`, so the persisted per-weather preference was write-never and the UI drove it off a file-static
`bool` shared by every weather window; the JSON parse boundary rejecting `1` where the catalog accepts an
integer for a `Float` setting; tooltips added to blocked and unavailable scene widgets; pause and revert
refusing to mutate Overwrite-layer entries, which previously diverged an entry from its backing file until
the next discovery; a per-frame copy-destination rebuild removed from the page toolbar; four robustness
fixes in the catalog generator (`eastl` smart-pointer factories, tab scopes bounded at `EndTabItem`,
`Draw*` helper gating via the call graph rather than the entry point's name, and alias member suffixes
surviving path extraction).

The generator fixes are behavior-preserving on the current tree: 345 catalog entries before and after, with
byte-identical generated output.

## Reproducing this comparison

```bash
git fetch alandtse
git show 05f084a4a4:src/SceneSettingsManager.h | less
git ls-tree -r --name-only 05f084a4a4 | grep -iE "scene|SettingsOverride|SettingsPatch"

python -m pytest tests/ -q
python cmake/generate_scene_settings_catalog.py --source-dir . --out-dir /tmp/catalog
```
