# Reverse Z: depth consumers that must be converted

`ReverseZ` (feature short name `ReverseZ`) reallocates the scene depth targets to
`R32G8X24_TYPELESS` / `D32_FLOAT_S8X24_UINT`, makes the camera publish a reversed
projection, and flips depth-stencil comparison, depth-bias and viewport depth range for
every pass drawing into a converted target. From then on the depth buffer holds
`1 - standardDepth` and every _shader that reads depth_ sees the inverted convention.

`EnableReverseZ` is restart-gated and ships **off**. Do not turn it on by default until it has
been validated in game on SE and AE.

## Two rules

1. **Standard-space depth** (comparisons, linearization, thickness tests, sky/far-plane
   sentinels): wrap the raw read in `FrameBuffer::ToStandardDepth(...)`
   (`package/Shaders/Common/ReverseZ.hlsli`). It is identity when `REVERSE_Z` is undefined.
2. **Native-space depth** (anything fed back into `CameraProjInverse` /
   `CameraViewProjInverse`, or written out to another depth target): keep the raw read and
   wrap only a value that was already converted, with `FrameBuffer::ToNativeDepth(...)`.

Both are `1 - x`, so a wrong choice is not a crash: it is a wrong pixel or a wrong fade.

`Common/ReverseZ.hlsli` provides the helper set: `ToStandardDepth` / `ToNativeDepth` (float and
vector forms), `ToStandardClipZ`, `ToStandardClip`, `OffsetClipDepth`, `IsReverseProjection`,
`FarPlaneDepth` / `NearPlaneDepth`, `FarPlaneClipZ`, the ordering helpers `NearerDepth`,
`FartherDepth`, `NearestDepth`, `FarthestDepth` and `IsNearerDepth`, and the
`SCENE_DEPTH_FORMAT` texture element type (`float` under `REVERSE_Z`, `unorm float` otherwise).
Everything is identity unless `REVERSE_Z` is defined, so wrapping a call is free when the
feature is off. C++ reads the same state through `ReverseZ::IsActive()`, `GetFarDepth()` and
`GetNearDepth()`.

## Status of the converted set

`ReverseZ::SetupDepthTargets()` converts `kMAIN`, `kMAIN_COPY`, `kDECAL_OCCLUSION`,
`kPOST_ZPREPASS_COPY`, `kPOST_WATER_COPY` and `kMAIN_DOWNSAMPLE`. `kCUBEMAP_REFLECTIONS` and the
shadow maps keep the standard convention, so a consumer reading one of those needs no
conversion at all. The feature reverses every published camera whose target _is_ converted.

## Handled: raster-time consumers

These write or test depth through a converted target and need no shader change; the feature's
detours cover them (comparison func, depth bias sign, viewport depth range, clear value).

| Consumer                                                                                                  | Note                                                                                                                   |
| --------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| Engine depth writes / tests on `kMAIN` (Lighting, Effect, Water, DistantTree, Grass, Sky, Particle, Tree) | Depth-stencil state and depth bias are flipped per pass by the feature.                                                |
| `ClearDepthStencilView` on a converted target                                                             | Clear value is mirrored.                                                                                               |
| Reflection cubemap face renders into a converted depth view                                               | The feature substitutes the cubemap's own (unconverted) depth view so the capture does not write into the scene depth. |

## Adding a new depth consumer

-   Read raw depth only through the rules above; never hard-code `1.0` as "far" or `0.0` as
    "near": use `FarPlaneDepth()` / `NearPlaneDepth()`.
-   Min/max reductions and nearer/farther tests flip meaning: use the ordering helpers. A Hi-Z or
    SPD chain that keeps the farthest depth seeds with `NearPlaneDepth()` and pads out-of-bounds
    texels with `FarPlaneDepth()`.
-   Declare depth textures bound to converted targets as `Texture2D<SCENE_DEPTH_FORMAT>`.
-   Shaders compiled through `Util::CompileShader` get `REVERSE_Z` automatically while the feature
    is active; engine shader types get it through `ReverseZ::HasShaderDefine`.

## Known open items

-   `EnableReverseZ` ships off, is restart-gated, and the feature itself is Beta (disabled by default).
    Toggling it rebuilds the shader cache.
-   ENB `.fx` files receive a standard-Z copy of the depth buffer (`Effects11::StandardDepth`), but the
    ENB extender's matrix bindings (`Effect::UpdateExternBindings`) come from the reversed camera
    constants. An `.fx` file that unprojects `TextureDepth` with those matrices needs `1 - depth`.
-   `ISDepthOfField` converts its own depth samples, but the average focus depth (`AvgDepthTex`) is
    produced by a vanilla pass that still reads the reversed buffer.
-   `LensFlare.hlsl` assumes the engine computes each flare's threshold depth (`ScreenSpaceLightPos.z`)
    with the camera's standard projection. If flares never fade behind geometry under reverse-Z,
    check that convention first.
-   `BSImagespaceShaderWorldMapNoSkyBlur` stays on the vanilla shader: there is no replacement.
-   Inherited from the original: `IsReverseDepthView` caches its verdict per DSV pointer until
    the next reallocation, and if reallocation fails, shaders still compile with `REVERSE_Z` while
    the targets stay standard.
