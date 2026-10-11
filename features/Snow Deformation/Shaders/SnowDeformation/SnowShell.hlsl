// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// The ground snow's surface and material: the landscape's own meshes,
// replayed by LandscapeSnowShell.hlsl, raised by per-class snow depth and
// carved by the deformation map; a depth prepass writes the deferred
// attributes and one full-screen pass shades them. Per-pixel normals come from
// the same fields, so trench walls shade smoothly on coarse geometry.
// Camera matrices ride the private ShellCB (b0), mirrored from b12.

#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
// Deliot & Heitz tiling-and-blending, already implemented for the landscape.
// Reused rather than rewritten: a frost sheet laid by Blizzard or a breath
// covers hundreds of units, and a plain tiled normal map would grid it.
#include "TerrainVariation/TerrainVariation.hlsli"

#ifdef PSHADER
// Shadows: terrain/cloud via GetWorldShadow, actors via the raw cascade atlas
// copies (SnowShadow.hlsli), falling back to the shared VSM. The screen-space
// mask applies near only when it was marched on the snow (ScreenSpaceShadowsActive
// 2); otherwise it holds the terrain behind the shell along the view ray.
#	define TERRAIN_SHADOWS
#	define CLOUD_SHADOWS
#	define VOLUMETRIC_SHADOWS
SamplerState ShellLinearSampler : register(s1);
#	define LinearSampler ShellLinearSampler
#	include "Common/ShadowSampling.hlsli"
#	include "ScreenSpaceShadows/ScreenSpaceShadows.hlsli"
#	include "Skylighting/Skylighting.hlsli"
#	include "SnowDeformation/SnowShadow.hlsli"
// POM runs EM's GetParallaxCoords through EM_PARALLAX_CUSTOM_HEIGHT, fetching
// via the anti-tiling taps (SnowParallax.hlsli). Soft-shadow taps stay local:
// the statics shell blends two planar projections' raw occlusions, which EM's
// multiplier API cannot express. LANDSCAPE/TRUE_PBR stay undefined here.
#	define EM_PARALLAX_CUSTOM_HEIGHT
float EMParallaxCustomHeight(float2 uv, float mip);
// TerrainVariation.hlsli is already included, so EM takes StochasticOffsets from it.
#	ifndef TERRAIN_VARIATION
#		define TERRAIN_VARIATION
#		define SNOW_EM_TERRAIN_VARIATION
#	endif
#	include "ExtendedMaterials/ExtendedMaterials.hlsli"
#	ifdef SNOW_EM_TERRAIN_VARIATION
#		undef TERRAIN_VARIATION
#		undef SNOW_EM_TERRAIN_VARIATION
#	endif
// Routed PBR tail (defines TRUE_PBR + GLINT for everything it pulls in,
// including the glint NDF) - must come after the includes above (they must
// compile without TRUE_PBR); SnowLights calls into it, so it comes last.
#	include "SnowDeformation/SnowShading.hlsli"
#	include "SnowDeformation/SnowLights.hlsli"
#endif

#if defined(PSHADER) && defined(SNOW_SHELL_DEFERRED)
// Deferred shading: the prepass's own coarse derivatives of GridLocal and of
// WorldPos.z, read back per pixel. A full-screen pass has no triangle to take
// them from; every derivative the shading needs is a chain-rule product of
// these two.
static float2 gDeferredGridDx;
static float2 gDeferredGridDy;
static float gDeferredZDx;
static float gDeferredZDy;
// The prepass's coverage ramp (AttrDepth.w).
static float gDeferredRampDepth;
#endif

// SNOW_SURFACE_ONLY: included for its surface recipe alone (LandscapeSnow.hlsl),
// with ShellCB moved off b0 and no entry points or stage structs. The vertex,
// hull and domain entry points are LandscapeSnowShell.hlsl's.
#ifndef SNOW_SHELL_CB_REGISTER
#	define SNOW_SHELL_CB_REGISTER b0
#endif
cbuffer ShellCB : register(SNOW_SHELL_CB_REGISTER)
{
	// row_major matches the game's FrameBuffer.hlsli declarations; the CPU
	// side copies the b12 bytes verbatim, which are row-major packed.
	row_major float4x4 CameraViewProj;
	row_major float4x4 CameraViewProjUnjittered;
	row_major float4x4 CameraPreviousViewProjUnjittered;
	row_major float4x4 CameraView;

	float4 ShellCameraPosAdjust;
	float4 ShellCameraPreviousPosAdjust;

	float2 GridOrigin;
	float GridSpacing;
	float TerrainTexelSize;

	// Grid-local sampling offsets, precomputed on CPU: absolute world XY at
	// ~1e5 magnitude destroys float32 finite differences (shimmer).
	float2 GridToTerrainOffset;
	float2 GridToDeformOffset;

	float WarpedHalfSpan;
	uint GridDim;
	uint TerrainDim;
	uint ShellDebugData;  // layout keeper

	float DeformInvWorldSize;
	uint HasSnowTexture;
	float SnowTextureIsLinear;
	float HasSnowNormal;

	float HasSnowRmaos;
	float SnowRoughnessScale;
	float2 SnowUVOffset;

	float4 SnowGlintParams;  // x logDensity, y microfacetRoughness, z densityRandomization, w screenSpaceScale

	float SnowSpecularLevel;
	float EnableGlints;
	float BorderNoise;   // layout keeper
	float BorderSmooth;  // layout keeper

	float ShellTriHeight;        // was BorderTrampledFade: mesh-matched terrain height
	float BorderUntrampledFade;  // layout keeper
	float ShellCullBare;         // was SeamFadeUnused: cull fully-bare triangles
	float SkinFadeStart;         // statics skin: distance dissolve start (units)

	float SkinFadeEnd;
	// Object height field mode: 0 = unbound, 0.5 = bound with the ground
	// lift off, >1 = lifting. A gate, never a magnitude.
	float ObjectLiftCap;
	float2 ObjectHeightCenter;

	float ObjectHeightHalfExtent;
	// The raw cascade-atlas copy is bound at t22 this frame (else the
	// shader falls back to the blurred VSM path).
	float CrispShadows;
	// Screen-Space Shadows output is bound at t45: the long-range
	// depth-marched shadows that carry distant LOD tree shadows beyond the
	// two cascades.
	float ScreenSpaceShadowsActive;
	// Dune-field amplitude in world units (0 flattens the undulation).
	float UndulationAmp;

	// Multiplier on the dune field's wavelengths (>1 = broader, calmer waves).
	float UndulationScale;
	float SkinTessCapSlope;  // statics skins only
	// LLF cluster buffers bound at t35-t37, point-shadow table at t38.
	float PointLightsActive;
	// Skylighting probe volume bound at t50.
	float SkylightingActive;

	// PBR displacement companion bound at t8.
	float HasSnowHeight;
	// Tessellated relief amplitude in world units (0 disables the path).
	float SnowReliefDepth;
	// Statics debug view: object snow renders decision variables as colors.
	float StaticsDebugView;
	float BermHeightAmp;

	float ChurnHeightAmp;
	float ChurnSizeScale;
	// Layout keepers.
	float DebugNoFarPad;
	float DebugNoDataMorph;

	float ObjBermHeightAmp;
	float ObjChurnHeightAmp;
	float ObjChurnSizeScale;
	// Units across which the sheet and an object's coat meet, half on each
	// side of the junction; 0 = off.
	float ObjectMeetBand;

	// 1 / Berm Steepness: the berm tap reach (BermFieldTapped, BermGradStep).
	float BermReach;
	uint ShellLODDebug;  // layout keeper
	// 1/width of the depth ramp inside the loaded-cell seam; 0 = no seam
	// data (fall back to the warped-span fade alone).
	float SeamRampInv;
	// >0.5: read the berm field from the bake at t14 instead of recomputing
	// its 17 taps per call.
	float BermBakeActive;

	// Loaded-cell boundary square (minX, minY, maxX, maxY): full terrain
	// inside, LOD terrain outside. The shell ends here and hands off to the
	// horizon recolor, which wears the same snow material.
	float4 SeamBounds;

	// Wide exclusion field window: xy = world centre, z = 1/half extent,
	// w > 0.5 when the field was baked this frame.
	float4 ExclusionFieldWindow;

	// Parallax: x = HeightScale (PBR JSON displacementScale), y = self-shadow
	// strength (0 disables), z = occlusion depth multiplier (0 disables the
	// march), w = spare (the march takes Extended Materials' step budget).
	float4 SnowParallax;

	// x = scorch darkening strength, y = crust shading strength,
	// z = roughness of fully crusted snow, w = how far crust flattens the
	// snow normal map.
	float4 SpellShading;

	// x = reflectance of fully crusted snow, yz = its colour cast (red, green),
	// w = grazing-angle sheen strength. Blue of the cast rides SpellShading is
	// not needed - see CrustTintBlue below.
	float4 CrustLook;
	// x = blue of the crust colour cast. Its own row rather than crowding
	// CrustLook, which the sheen took. y = frost pattern strength, z = its
	// world tile size, w = whether the pattern loaded at all. Those three ride
	// spare room here rather than growing a buffer mirrored across two shaders.
	float4 CrustLook2;
	// xy layout keepers; zw = sun cascades' REAL atlas slices (the shared
	// atlas moves them with the active-light set).
	float4 BorderStyle;
	// x = water cut (1 = the sheet ends at the drawn waterline); yz = the land
	// contest's depth bonus and carve penalty; w spare.
	float4 CompactLook;
	// Stage 3 P5: x = rim lip height (fraction of local depth), y = rim
	// teeth strength; zw spare.
	float4 RimStyle;

	// Baked undulation window: xy = world centre, z = 1/half-extent,
	// w > 0.5 when the bake is live (UndulationFieldCS, t29).
	float4 UndulationFieldWindow;

	// Toroidal deformation-map addressing: physical position of logical
	// texel (0,0). Every DeformationMap Load routes through DeformTexel.
	int2 DeformMapOrigin;
	// x bit 0 horizon march, bit 2 land-exact height OFF, bit 4 churn by
	// parallax; y bit 0 road tops in the skin-depth raster. Mirror in
	// SnowDeformation.h.
	int2 ShellFlags;

	// Land-exact height layer: xy = GridOrigin - fine window origin (world),
	// z = fine dim in texels (0 = none), w = fine texel size (32).
	float4 FineWindow;
	float4 SlopeDrape;
	float4 DebugSkinDepth;  // object skins only (SnowStaticsShell.hlsl)
	// Blood: x = intensity, y = burial clock now, z = game hours now,
	// w = snowfall that buries a mark (fraction of a refill).
	float4 BloodLook;
	// x = hours a mark takes to dry, y = wet sheen, z > 0.5 = map live,
	// w > 0.5 = detail tiles live.
	float4 BloodLook2;
	// xy spare; z = the detail tiles' cell in units, w = their texel in units.
	float4 BloodLook3;
	// Trampled ground: x spare, y = floor churn scale (1 = off), zw = the
	// extent bracket trail -> field. See TrampleChurnScale.
	float4 TrampleStyle;
	// x = tessellation target, pixels per segment (0 = distance only);
	// yz = viewport half size in pixels.
	float4 TessScreen;
	// GetDimensions of the bound field maps (0 = unbound): x deformation (t1),
	// y berm field (t14), z object maps (t4), w exclusion field (t15).
	float4 FieldDims;
}


Texture2D<float4> TerrainWindow : register(t0);
// Drawn water's height per terrain texel, MAX of the bodies touching it,
// -100000 where none. TerrainWindow's frame, but rasterised +Y-up like the
// object maps, so its row is (TerrainDim - 1) - the terrain row.
Texture2D<float> WaterWindow : register(t27);  // data route: lone wet texels dropped
Texture2D<float> WaterRawWindow : register(t28);  // the shore cap: every rasterised body
Texture2D<float> WaterNear : register(t19);  // texels to the nearest vertex under the water (WaterCoverCS)
// Water this shallow over the ground is not water: a placed pond plane's
// rectangle skims the land around its shore. Also where the shore cap's
// zero lands, so keep it small or the toe ends under the surface.
static const float kWaterSkimDepth = 2.0;
// A texel's vertex must be this far under to read as a bare class: shallower
// margins are the cap's job per pixel, and a lone shallow vertex would
// otherwise turn its whole texel into a square.
static const float kWaterTexelDepth = 8.0;

// A texel whose ground lies under drawn water reads as a non-snow class
// (-8), so the shoreline is a class border: same noise, smoothing, dither
// and rim as any texture seam. Callers gate on CompactLook.x.
float3 WaterBareTexel(float3 texel, int2 t)
{
	float water = WaterWindow.Load(int3(t.x, (int)TerrainDim - 1 - t.y, 0));
	texel.y = texel.x < water - kWaterTexelDepth ? min(texel.y, -8.0) : texel.y;
	return texel;
}
// TerrainWindow with WaterBareTexel already applied (TerrainCutCS), or the
// raw window while the cut is off (CompactLook.x): SampleTerrain's taps.
Texture2D<float4> TerrainWindowWet : register(t58);
// Land-blended edges: 1 - the land's snow-texture weight per window texel;
// unbound reads 0, all snow.
Texture2D<float> TerrainBare : register(t26);
// The ground as the engine renders it: bicubic Catmull-Rom of the LAND
// heightmap at 32-unit texels, cell edges extrapolated (TerrainFineCS).
// The land mesh is then flat between these points with a checkerboard
// diagonal, which SampleTerrain reproduces.
Texture2D<float> TerrainFine : register(t13);
// 2x2 and 4x4 maxima of TerrainFine (64- and 128-unit texels): the far bands'
// conservative ground, see SampleTerrain.
Texture2D<float4> DeformationMap : register(t1);

// Toroidal map fetch: logical texel (already clamped by the caller) to
// physical. The map's dim is a power of two, so the wrap is a mask. Hardware
// samplers cannot do this - bilinear across the physical seam would mix two
// unrelated world locations - so every read stays Load-based, per tap.
int3 DeformTexel(int2 t, int2 dims)
{
	return int3((t + DeformMapOrigin) & (dims - 1), 0);
}

Texture2D<float4> SnowDiffuse : register(t2);
// Full-scene depth copy (Terrain Blending's blended depth when available),
// never the bound DSV, so sampling during the shell draw is legal.
Texture2D<float> SceneDepth : register(t3);
// Processed top-down object maps: the slope-limited snow-height FIELD (world
// Z, empty -100000) and the SUPPRESSION mask (1 under floating structures;
// no snow beneath walkways, roofs and bridges).
Texture2D<float> ObjectHeights : register(t4);
Texture2D<float2> ObjectBottoms : register(t5);
// Raw object tops (persistence-scrolled) and this frame's skin-depth raster,
// shared with the trench patch: where the shell rides a captured object, its
// layer wears the object's own skin depth instead of the landscape class
// depth.
Texture2D<float> ObjectTopsRaw : register(t11);
// x = layer depth (all this shell reads), y = the patch's road-heightfield bit.
Texture2D<float2> ObjectSkinDepthMap : register(t12);
// TruePBR snow companion maps (auto-resolved from the Textures\PBR\ variant
// of the snow path): tangent-space normals (_n) and roughness/metal/AO/spec
// (_rmaos). Gated by HasSnowNormal / HasSnowRmaos.
Texture2D<float4> SnowNormalMap : register(t6);
Texture2D<float4> SnowRmaosMap : register(t7);
// Displacement companion (_p): tessellated relief and the parallax
// self-shadow. float4 to match Extended Materials' own TexParallaxSampler
// convention; the SRV is single-channel, so only .x carries data.
Texture2D<float4> SnowHeightMap : register(t8);
// Pre-snow copy of the MASKS target: y carries the land's snow odds
// (Lighting.hlsl WriteLandSnowOdds; 0 = no data - grass, or an object
// behind), the land's side of the contest.
Texture2D<float3> LandMasksCopy : register(t10);
// Baked berm field (BermFieldCS): the 17-tap disc average of the deformation
// map, at the map's own resolution and addressing.
Texture2D<float> BermFieldMap : register(t14);
// Trample extent (ExtentCS): one texel per 8x8 map tile, physical layout.
Texture2D<float> ExtentFieldMap : register(t56);
// 1 per 8x8 physical map tile where the map and the berm field read zero
// within one tile (BermFieldCS UntouchedCS). Unbound reads 0 = touched.
Texture2D<uint> UntouchedTiles : register(t57);
// Blood map + deposit clocks, the deformation map's torus (SnowFields.hlsli).
Texture2D<float4> BloodMap : register(t30);
Texture2D<float2> BloodClock : register(t31);
// Wide exclusion field (ExclusionFieldCS): x = door suppression, y = melt, over
// a window that reaches the shell's own extent. The near mask at t5 still owns
// the SHELTER term, which needs geometry and so cannot travel this far.
Texture2D<float2> ExclusionFieldMap : register(t15);

// The game's own frost impact art, painted onto crusted snow.
Texture2D<float4> FrostPatternNormal : register(t16);
Texture2D<float4> FrostPatternDiffuse : register(t17);

// Baked undulation field (UndulationFieldCS): x = height in world units,
// yz = its +-kUndulationGradStep shading gradient, over UndulationFieldWindow.
Texture2D<float4> UndulationFieldMap : register(t29);

SamplerState SnowSampler : register(s0);

// Shared trench-detail shaping, spell-mark readers, field surfaces and the
// frost pattern - the verbatim-identical pieces of both shells live in one
// file.
// The landscape shell paints rune glyphs and reads the blood detail tiles
// (SnowFields.hlsli).
#define SNOW_RUNE_GLYPHS
#define SNOW_BLOOD_TILES
#define SNOW_FIELD_DIMS
#include "SnowDeformation/SnowFields.hlsli"


// The game's own landscape tiling: 24 texture repeats per 4096-unit cell,
// measured in-game with the tiling ruler. Same texture at the same world
// rate as the ground beside us; see CODE-NOTES.md. Mirrored in
// SnowStaticsShell.hlsl, SnowDeformation.hlsli and Shell.cpp.
static const float kSnowUVTile = 4096.0 / 24.0;

// Distance bands (WarpBand, InverseWarpAxis): ShellBandStep's spacing.
#include "SnowDeformation/SnowGrid.hlsli"

// Shell edge fade: the shell dissolves at the loaded-cell seam, where the
// game swaps full terrain for LOD meshes and the horizon recolor takes over
// in the same snow material. The warped-span fade stays as the fallback for
// the grid's own physical edge (and for frames with no seam data).
float ShellEdgeFade(float2 gridLocal)
{
	float2 delta = abs(gridLocal - WarpedHalfSpan);
	float fade = saturate((WarpedHalfSpan - max(delta.x, delta.y)) / 2048.0);
	[flatten] if (SeamRampInv > 0.0)
	{
		float2 worldXY = GridOrigin + gridLocal;
		float inside = min(min(worldXY.x - SeamBounds.x, worldXY.y - SeamBounds.y),
			min(SeamBounds.z - worldXY.x, SeamBounds.w - worldXY.y));
		fade = min(fade, saturate(inside * SeamRampInv));
	}
	return fade;
}

#ifndef SNOW_SURFACE_ONLY
struct VS_OUTPUT
{
	// noperspective centroid: required interpolation for SV_Position when
	// the PS writes conservative depth (SV_DepthLessEqual).
	linear noperspective centroid float4 Position : SV_POSITION;
	float4 CurrentClip : TEXCOORD0;
	float4 PreviousClip : TEXCOORD1;
	float3 WorldPos : TEXCOORD2;
	float2 GridLocal : TEXCOORD3;
	float Snowness : TEXCOORD4;
	float DebugHeight : TEXCOORD5;
	// xyz: smooth per-vertex terrain normal, w: coverage alpha (taper*fade).
	float4 TerrainNormalAlpha : TEXCOORD6;
};
#endif

// Manual bilinear over the terrain window (Load-based, deterministic).
// Returns (height, rampDepth, coverage): rampDepth is the per-texture-class
// depth blend in world units, precomputed at window-rebuild time on the CPU.
// gridLocal = world XY relative to GridOrigin (small, precision-safe).
// Height at a fractional position inside a terrain quad, matching the LANDSCAPE
// MESH rather than averaging it.
//
// Bilinear is exactly the mean of a quad's two possible triangulations, so it
// sits BELOW whichever one the mesh actually uses by the saddle term
// (h00 + h11 - h10 - h01) / 4 at the centre. Flat ground: zero. A steep saddle
// at 128-unit spacing with a few hundred units of corner variation: tens of
// units - deeper than the snow layer - and the shell sinks through the mesh and
// shows bare rock.
//
// The clearance pad used to hide this; its own comment names the case
// ("filling pinholes where the bilinear surface dips under triangle diagonals").
// Round 4 of the distant-shell work made the coarsest band 128 units, which
// drives padWeight to zero in EVERY band, so the pad is off and this half of its
// job went with it. Round 4 retired it for UNDERSAMPLING, which a 1:1 lattice
// genuinely does fix; the triangulation mismatch is a separate defect that a 1:1
// lattice does not touch.
//
// Taking the max of both triangulations is >= whichever the mesh uses, so the
// shell can no longer sink beneath it. Costs only ALU on corners already
// fetched, and collapses to bilinear wherever the quad is planar - flat ground
// does not move at all.
float TriangulatedHeight(float h00, float h10, float h01, float h11, float2 f)
{
	// Diagonal h00-h11, split along f.y == f.x.
	float triA = f.y <= f.x ?
	                 h00 + (h10 - h00) * f.x + (h11 - h10) * f.y :
	                 h00 + (h01 - h00) * f.y + (h11 - h01) * f.x;
	// Diagonal h10-h01, split along f.x + f.y == 1.
	float triB = (f.x + f.y) <= 1.0 ?
	                 h00 + (h10 - h00) * f.x + (h01 - h00) * f.y :
	                 h11 + (h10 - h11) * (1.0 - f.y) + (h01 - h11) * (1.0 - f.x);
	return max(triA, triB);
}

// The ground as the engine renders it, at any grid-local point: flat
// triangles between the fine layer's 32-unit vertices, split '/' where the
// world 32-quad index sum is even (the fine origin is a cell corner, so the
// texel index sum carries the same parity). The missing sentinel outside the
// fine window or over missing data.
// Single exit on purpose: a return inside a [branch] is the X4000 shape, and
// under the vertex stages' inlining depth it made fxc overflow its stack.
float LandHeightFine(float2 gridLocal)
{
	float h = -100000.0;
	float2 tf = (FineWindow.xy + gridLocal) / FineWindow.w;
	bool inside = FineWindow.z > 0.5 && all(tf >= 0.0) && all(tf < FineWindow.z - 1.0);
	[branch] if (inside)
	{
		int2 f0 = (int2)tf;
		float2 ff = tf - f0;
		float g00 = TerrainFine.Load(int3(f0, 0));
		float g10 = TerrainFine.Load(int3(f0 + int2(1, 0), 0));
		float g01 = TerrainFine.Load(int3(f0 + int2(0, 1), 0));
		float g11 = TerrainFine.Load(int3(f0 + int2(1, 1), 0));
		float lo = min(min(g00, g10), min(g01, g11));
		bool slash = ((f0.x + f0.y) & 1) == 0;
		float hA = ff.y <= ff.x ? g00 + (g10 - g00) * ff.x + (g11 - g10) * ff.y : g00 + (g01 - g00) * ff.y + (g11 - g01) * ff.x;
		float hB = (ff.x + ff.y) <= 1.0 ? g00 + (g10 - g00) * ff.x + (g01 - g00) * ff.y : g11 + (g10 - g11) * (1.0 - ff.y) + (g01 - g11) * (1.0 - ff.x);
		h = lo > -50000.0 ? (slash ? hA : hB) : -100000.0;
	}
	return h;
}

// The vertex spacing the replay tessellates to at a grid-local point (8..128
// world units): the distance bands of SnowGrid.hlsli.
float ShellBandStep(float2 gridLocal)
{
	float2 u = float2(InverseWarpAxis(gridLocal.x - WarpedHalfSpan), InverseWarpAxis(gridLocal.y - WarpedHalfSpan));
	return GridSpacing * max(WarpBand(abs(u.x)).x, WarpBand(abs(u.y)).x);
}

float3 SampleTerrain(float2 gridLocal)
{
	float2 t = (GridToTerrainOffset + gridLocal) / TerrainTexelSize;
	t = clamp(t, 0.0, (float)(TerrainDim - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDim - 1, TerrainDim - 1));

	float3 s00 = TerrainWindowWet.Load(int3(t0.x, t0.y, 0)).xyz;
	float3 s10 = TerrainWindowWet.Load(int3(t1.x, t0.y, 0)).xyz;
	float3 s01 = TerrainWindowWet.Load(int3(t0.x, t1.y, 0)).xyz;
	float3 s11 = TerrainWindowWet.Load(int3(t1.x, t1.y, 0)).xyz;

	float3 result = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
	// Land-exact height: the ground as the engine renders it (LandHeightFine).
	// Outside the fine window, or over missing data, the 128-texel height
	// below stands, so nothing changes at the seam.
	float fine = -100000.0;
	[branch] if (FineWindow.z > 0.5 && (ShellFlags.x & 4) == 0)
		fine = LandHeightFine(gridLocal);
	// Height follows the mesh's triangulation; depth and coverage stay bilinear,
	// being material blends rather than geometry. Skipped where any corner is the
	// missing-data sentinel: bilinear poisons the result so callers can detect it
	// with `< -50000`, and a max would pick the surviving triangle and hide the
	// gap instead.
	[branch] if (fine > -50000.0)
		result.x = fine;
	else if (ShellTriHeight > 0.5 && min(min(s00.x, s10.x), min(s01.x, s11.x)) > -50000.0)
		result.x = TriangulatedHeight(s00.x, s10.x, s01.x, s11.x, f);
	// the accumulated layer scales depth HERE, at the one point
	// every reader funnels through, so geometry, shading, the berm gate and the
	// self-shadow march cannot disagree about how deep the snow is. Positive
	// only - a negative rampDepth is the submerge toward bare ground at a class
	// border, not snow, and scaling it would move class edges as it snowed.
	result.y = max(result.y, 0.0) * RimStyle.w + min(result.y, 0.0);
	return result;
}

// Height as the terrain window's plain bilinear reads it: the object field's
// own base (HeightMapProcessCS SampleTerrainHeight), so a lift measured
// against it is the field's excess and nothing else.
float SampleTerrainBilinearHeight(float2 gridLocal)
{
	float2 t = (GridToTerrainOffset + gridLocal) / TerrainTexelSize;
	t = clamp(t, 0.0, (float)(TerrainDim - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDim - 1, TerrainDim - 1));
	float s00 = TerrainWindow.Load(int3(t0.x, t0.y, 0)).x;
	float s10 = TerrainWindow.Load(int3(t1.x, t0.y, 0)).x;
	float s01 = TerrainWindow.Load(int3(t0.x, t1.y, 0)).x;
	float s11 = TerrainWindow.Load(int3(t1.x, t1.y, 0)).x;
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// Bilinear helper at fractional texel coordinates (Load-based).
float SampleDeformationBilinear(float2 t, float2 dims)
{
	t = clamp(t, 0.0, dims - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float s00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims))).x;
	float s10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims))).x;
	float s01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims))).x;
	float s11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims))).x;

	// Clamped here rather than at each call site: melt writes past 1.0 into
	// the refill headroom, and this is the single tap every consumer goes
	// through (bicubic, fast, and the berm field's 17 taps). The B-spline
	// weights are a convex combination, so clamping here bounds them all.
	return saturate(lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y));
}


// B-spline bicubic sample of the deformation map, built from four bilinear
// taps at fractional offsets. Smooth value and gradient: plain bilinear
// leaves texel-rate creases in trench walls, and value-only smoothing
// terraces the gradient the normals consume.
// Returns 0 outside the deformation window.
float SampleDeformation(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	SNOW_DEFORM_DIMS(dims);
	float2 t = uv * dims - 0.5;
	float2 i = floor(t);
	float2 f = t - i;

	float2 f2 = f * f;
	float2 f3 = f2 * f;
	float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
	float2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
	float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
	float2 w3 = f3 / 6.0;

	float2 g0 = w0 + w1;
	float2 g1 = w2 + w3;
	float2 h0 = i - 1.0 + w1 / g0;
	float2 h1 = i + 1.0 + w3 / g1;

	float v00 = SampleDeformationBilinear(float2(h0.x, h0.y), dims);
	float v10 = SampleDeformationBilinear(float2(h1.x, h0.y), dims);
	float v01 = SampleDeformationBilinear(float2(h0.x, h1.y), dims);
	float v11 = SampleDeformationBilinear(float2(h1.x, h1.y), dims);

	return g0.y * (g0.x * v00 + g1.x * v10) + g1.y * (g0.x * v01 + g1.x * v11);
}

// DISPLACED depth at a point: total minus the melted portion. A berm is snow
// that had to go somewhere, and melted snow leaves no spoil, so the berm
// field is built from this rather than from total depth. Doing it here rather
// than scaling the finished berm down means a boot print through a melt basin
// still throws its own ridge.
float SampleDisplacedFast(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	SNOW_DEFORM_DIMS(dims);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float4 s00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims)));
	float4 s10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims)));
	float4 s01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims)));
	float4 s11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims)));
	float4 v = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
	// Only MELTED depth is spoil-free. Channel y is signed and negative means
	// scorch, which was displaced and keeps its berm.
	return saturate(v.x - max(v.y, 0.0));
}

// SampleMelted / SampleScorch / SampleCrust live in SnowFields.hlsli
//: both shells read the surface-state channels identically.

// Single-bilinear deformation tap: for many-tap averages (BermField) where
// the sum provides the smoothness and bicubic per tap would be waste.
float SampleDeformationFast(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	SNOW_DEFORM_DIMS(dims);
	return SampleDeformationBilinear(uv * dims - 0.5, dims);
}

// March-grade deformation tap: one bilinear (4 loads) where the visible
// surface pays bicubic (16). The horizon march samples 28-1000 units out,
// where the samplers' difference is invisible - the same too-fine-to-matter
// rule that keeps churn and clods out of the marches.
float SampleDeformationMarch(float2 gridLocal)
{
	return SampleDeformationFast(gridLocal);
}

// Bilinear samples of the object field maps at absolute world XY. The raster
// pass maps +worldY to +ndcY = texture v0 (top), so v mirrors.
float2 ObjectMapTexel(float2 worldXY, out float2 dims, out bool valid)
{
	float2 local = (worldXY - ObjectHeightCenter) / ObjectHeightHalfExtent;
	valid = all(abs(local) < 0.98);
	dims = FieldDims.zz;
	float2 uv = float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5);
	return clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
}

float SampleObjectHeight(float2 worldXY)
{
	float2 dims;
	bool valid;
	float2 t = ObjectMapTexel(worldXY, dims, valid);
	if (!valid)
		return -100000.0;
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float s00 = ObjectHeights.Load(int3(t0.x, t0.y, 0));
	float s10 = ObjectHeights.Load(int3(t1.x, t0.y, 0));
	float s01 = ObjectHeights.Load(int3(t0.x, t1.y, 0));
	float s11 = ObjectHeights.Load(int3(t1.x, t1.y, 0));

	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// Fully-bare test. ShellSurfaceZ's depth is rampDepth + (-8) * bare, so -8 is
// the floor: it is what ground reaches when EVERY layer under it is a non-snow
// class, and no blend can go below it. Anything above -8 is still on the ramp
// that climbs to the +14..+30 of a snow class, and culling there would cut the
// transition and leave a gap where the layer rises out of the ground - so the
// test is against the floor and not a threshold part-way up.
static const float kBareDepthFloor = -7.9;

// The one thing in ShellSurfaceZ that can raise depth back off the floor:
//   float liftForce = smoothstep(6.0, 20.0, field - terrainHeight);
//   rampDepth = max(rampDepth, liftForce * 6.0);
// so a patch is only safe to cull while the object field stands less than 6
// units proud. Tested with margin.
//
// NOT a test for "the field has data": t4 is terrain run through the cone
// transform, so it holds a real height across the whole object window and a
// data test vetoes every near patch - which is where the pixels are.
static const float kBareLiftMargin = 2.0;

// Conservative over the whole patch, which point sampling is NOT: the shell's
// surface is the BILINEAR blend of the terrain texels, so a patch whose corners
// all land in one bare texel can still rise to a snowy neighbour across its
// span. Truncating to the nearest texel misses exactly those, and culling them
// punched holes in open snow.
//
// So take the MAX effective depth over every texel the patch's bilinear can
// reach - floor(min) through floor(max)+1 - and cull only if even that is on
// the floor. Cheap where it matters: patches in the fine bands sit inside one
// texel and cost a single load, and the coarsest band is 128 units against a
// 128-unit texel, so the range is 2x2. The clamp is a loop bound, not a limit
// any real patch reaches.
bool ShellTerrainAllBare(float2 lo, float2 hi)
{
	float2 tLo = (GridToTerrainOffset + lo) / TerrainTexelSize;
	float2 tHi = (GridToTerrainOffset + hi) / TerrainTexelSize;
	float maxTexel = (float)(TerrainDim - 1);
	int2 i0 = (int2)clamp(floor(tLo), 0.0, maxTexel);
	int2 i1 = (int2)clamp(floor(tHi) + 1.0, 0.0, maxTexel);
	i1 = min(i1, i0 + 3);

	float maxDepth = -1e9;
	[loop] for (int y = i0.y; y <= i1.y; ++y)
	{
		[loop] for (int x = i0.x; x <= i1.x; ++x)
		{
			float3 t = TerrainWindowWet.Load(int3(x, y, 0)).xyz;
			// Sentinel texels carry no data; leave them to the full evaluation.
			[branch] if (t.x < -50000.0)
				return false;
			maxDepth = max(maxDepth, t.y + (-8.0) * saturate(1.0 - saturate(t.z)));
		}
	}
	return maxDepth <= kBareDepthFloor;
}

// Object-lift veto, sampled at corners and centre. The cone field is slope
// limited and changes slowly, and the margin sits well under the 6 units where
// the lift actually starts, so a few taps carry it.
bool ShellObjectLiftsAt(float2 gridLocal)
{
	float field = SampleObjectHeight(GridOrigin + gridLocal);
	return field > -50000.0 && field - SampleTerrainBilinearHeight(gridLocal) > kBareLiftMargin;
}


// SampleExclusionField lives in SnowFields.hlsli.

float2 SampleObjectBottom(float2 worldXY)
{
	float2 dims;
	bool valid;
	float2 t = ObjectMapTexel(worldXY, dims, valid);
	// t5: x = coverage suppression, y = melt fraction. Independent channels, so
	// a door's influence tail cannot discard the melt around it. Outside the
	// window nothing is suppressed or melted - a raw-height sentinel here
	// zeroes VS coverage on every out-of-window vertex instead.
	if (!valid)
		return float2(0.0, 0.0);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float2 s00 = ObjectBottoms.Load(int3(t0.x, t0.y, 0));
	float2 s10 = ObjectBottoms.Load(int3(t1.x, t0.y, 0));
	float2 s01 = ObjectBottoms.Load(int3(t0.x, t1.y, 0));
	float2 s11 = ObjectBottoms.Load(int3(t1.x, t1.y, 0));

	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// Near mask unioned with the wide field. The near one wins where it has data -
// it alone carries shelter (roofs, tents, walkways) and it resolves clearings
// at four times the field's resolution - and the field fills in beyond its
// reach, which is where every clearing used to vanish.
float2 SampleExclusionMask(float2 worldXY)
{
	float2 nearMask = 0.0;
	[branch] if (ObjectLiftCap > 0.0)
		nearMask = SampleObjectBottom(worldXY);
	return max(nearMask, SampleExclusionField(worldXY));
}

// Mirrors of SnowDeformation.h kNoRoadTop / SnowStaticsShell.hlsl kRoadOwnsTop.
static const float kNoRoadTop = -1000000.0;
static const float kRoadOwnsTop = 8.0;

// The object-layer depth cap: the skin depth (max of 4 texels; the raster is
// sentinel-free, 0 where nothing wrote) where a captured object covers the
// texel, or a huge no-cap value where none does or the window does not reach.
//
// Road-owned columns cap to ZERO: the trench patch owns road snow outright
// (ROAD-HEIGHTFIELD-PLAN), and a shell layer drawn there at its own class
// depth is a second surface through one snow layer - Josef's depth sweep
// caught it slicing across the patch's trench walls as a bright ledge
// wherever the two depths disagree. Cap 0 takes the shell's existing
// step-aside path (coverage dies with the depth), which the patch then fills
// by construction. ALL FOUR texels must be road-owned - one-texel erosion, so
// the verge keeps today's overlap and a jitter-shaped gap cannot open at the
// road edge. The G channel is written only while the road heightfield is on,
// so this path is inert with the feature off.
float SampleObjectDepthCap(float2 worldXY)
{
	float2 dims;
	bool valid;
	float2 t = ObjectMapTexel(worldXY, dims, valid);
	if (!valid)
		return 1e6;
	int2 t0 = (int2)t;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	float4 tops = float4(
		ObjectTopsRaw.Load(int3(t0.x, t0.y, 0)), ObjectTopsRaw.Load(int3(t1.x, t0.y, 0)),
		ObjectTopsRaw.Load(int3(t0.x, t1.y, 0)), ObjectTopsRaw.Load(int3(t1.x, t1.y, 0)));
	[flatten] if (all(tops < -50000.0))
		return 1e6;
	float2 sd00 = ObjectSkinDepthMap.Load(int3(t0.x, t0.y, 0));
	float2 sd10 = ObjectSkinDepthMap.Load(int3(t1.x, t0.y, 0));
	float2 sd01 = ObjectSkinDepthMap.Load(int3(t0.x, t1.y, 0));
	float2 sd11 = ObjectSkinDepthMap.Load(int3(t1.x, t1.y, 0));
	float4 roadTops = float4(sd00.y, sd10.y, sd01.y, sd11.y);
	[branch] if (all(roadTops > kNoRoadTop * 0.5) && all(tops - roadTops < kRoadOwnsTop))
		return 0.0;
	return max(max(sd00.x, sd10.x), max(sd01.x, sd11.x));
}

// Bilinear raw object top at world XY; sentinel where a texel is empty.
// a_skipRoads also returns it over road-owned columns (the patch's ground).
float SampleObjectTopRaw(float2 worldXY, bool a_skipRoads)
{
	float2 dims;
	bool valid;
	float2 t = ObjectMapTexel(worldXY, dims, valid);
	float top = -100000.0;
	[branch] if (valid)
	{
		int2 t0 = (int2)t;
		float2 f = t - t0;
		int2 t1 = min(t0 + 1, int2(dims) - 1);
		float4 tops = float4(
			ObjectTopsRaw.Load(int3(t0.x, t0.y, 0)), ObjectTopsRaw.Load(int3(t1.x, t0.y, 0)),
			ObjectTopsRaw.Load(int3(t0.x, t1.y, 0)), ObjectTopsRaw.Load(int3(t1.x, t1.y, 0)));
		[branch] if (all(tops > -50000.0))
		{
			bool road = false;
			[branch] if (a_skipRoads)
				road = max(max(ObjectSkinDepthMap.Load(int3(t0.x, t0.y, 0)).y, ObjectSkinDepthMap.Load(int3(t1.x, t0.y, 0)).y),
						   max(ObjectSkinDepthMap.Load(int3(t0.x, t1.y, 0)).y, ObjectSkinDepthMap.Load(int3(t1.x, t1.y, 0)).y)) > kNoRoadTop * 0.5;
			[flatten] if (!road)
				top = lerp(lerp(tops.x, tops.y, f.x), lerp(tops.z, tops.w, f.x), f.y);
		}
	}
	return top;
}

// Where the sheet lies over a captured object: x = meeting weight (1 on the
// junction, 0 from half of ObjectMeetBand of perpendicular snow thickness
// up; each side takes half the band), yz = the object's macro slope. Both
// from the raw top raster at the pixel's own world XY: no object texture
// detail, nothing a camera can move. The 8-unit step spans whole texels,
// which cancels the MAX raster's stair. An upper deck (gap well below zero)
// and faces too steep to hold a coat weigh nothing.
float3 ShellObjectMeet(float2 worldXY, float sheetZ)
{
	float3 result = float3(0.0, 0.0, 0.0);
	float halfBand = ObjectMeetBand * 0.5;
	[branch] if (halfBand > 0.0)
	{
		float meetTop = SampleObjectTopRaw(worldXY, true);
		float meetGap = sheetZ - meetTop;
		[branch] if (meetTop > -50000.0 && meetGap < halfBand * 3.0 && meetGap > -16.0)
		{
			const float mStep = 8.0;
			float mXP = SampleObjectTopRaw(worldXY + float2(mStep, 0.0), false);
			float mXN = SampleObjectTopRaw(worldXY - float2(mStep, 0.0), false);
			float mYP = SampleObjectTopRaw(worldXY + float2(0.0, mStep), false);
			float mYN = SampleObjectTopRaw(worldXY - float2(0.0, mStep), false);
			[flatten] if (min(min(mXP, mXN), min(mYP, mYN)) > -50000.0)
			{
				float2 objectGrad = float2(mXP - mXN, mYP - mYN) / (2.0 * mStep);
				float objectNz = rsqrt(1.0 + dot(objectGrad, objectGrad));
				float meet = (1.0 - smoothstep(0.0, halfBand, meetGap * objectNz)) *
				             (1.0 - smoothstep(8.0, 16.0, -meetGap)) * smoothstep(0.35, 0.6, objectNz);
				result = float3(meet, objectGrad);
			}
		}
	}
	return result;
}

// ---- Surface undulation: wind-settled dunes ----
// Two octaves of world-anchored value noise, added as real geometry via
// ShellSurfaceZ and shaded through its gradient. Amplitude scales with local
// depth, so thin snow, class boundaries and carved floors stay flat.
// Undulation, CarveProfile and kFireMeltFloor live in SnowFields.hlsli, so the
// surface, the shading gradient and both shells' self-shadow marches see one
// shape. Trenches carve to the ground.

// Edge berm: displaced snow piles along the trench rim, taller from a deeper
// layer. Shape comes from the BLURRED deformation (BermField) - two rings
// reaching ~40 units (x BermReach) past the trail edge, the outer ring's small per-tap
// weight giving a long tail instead of a knife along the stamp falloff.
// BermShape and the tap ring are in SnowFields.hlsli.

float BermFieldTapped(float2 gridLocal)
{
	float b = SampleDisplacedFast(gridLocal);
	[unroll] for (int i = 0; i < 16; i++)
		b += SampleDisplacedFast(gridLocal + kBermTaps[i] * BermReach);
	return saturate(b / 17.0);
}

// The berm field dominated the shell's pixel cost at 68 loads a call, and the
// normal alone needs four. BermBakeActive routes to the bake; the tapped path
// stays compiled so the two can be A/B'd from the debug menu.
float BermField(float2 gridLocal)
{
	float field = 0.0;
	[branch] if (BermBakeActive > 0.5)
		field = BermFieldBaked(gridLocal);
	else
		field = BermFieldTapped(gridLocal);
	return field;
}

// ---- Trench churn: chunky broken snow in disturbed zones ----
// ~10-unit lumps of short-wavelength noise on carved or piled snow, weighted
// by the same carve/berm terms the profile uses, so churn dies at the
// untouched surface with no boundary. The self-shadow march skips it - a few
// units is under its step resolution.
float ChurnNoise(float2 worldXY)
{
	return ChurnNoiseScaled(worldXY, ChurnSizeScale);
}

// Water level over the texels a point touches, from the raw raster: the max,
// so a shore texel answers with its body's level and the sentinel never
// blends in. .y = bilinear presence of the four texels, so the cap can fade
// across the raster's last texel instead of stopping on its edge.
float2 SampleWaterHeight(float2 gridLocal)
{
	float2 t = (GridToTerrainOffset + gridLocal) / TerrainTexelSize;
	t = clamp(t, 0.0, (float)(TerrainDim - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDim - 1, TerrainDim - 1));
	int r0 = (int)TerrainDim - 1 - t0.y;
	int r1 = (int)TerrainDim - 1 - t1.y;
	float w00 = WaterRawWindow.Load(int3(t0.x, r0, 0));
	float w10 = WaterRawWindow.Load(int3(t1.x, r0, 0));
	float w01 = WaterRawWindow.Load(int3(t0.x, r1, 0));
	float w11 = WaterRawWindow.Load(int3(t1.x, r1, 0));
	float4 present = step(-50000.0, float4(w00, w10, w01, w11));
	return float2(max(max(w00, w10), max(w01, w11)), lerp(lerp(present.x, present.y, f.x), lerp(present.z, present.w, f.x), f.y));
}

// Texels from a point to the nearest vertex under the water, bilinear over
// the near field (rows flipped like the water window).
float SampleWaterNear(float2 gridLocal)
{
	float2 t = (GridToTerrainOffset + gridLocal) / TerrainTexelSize;
	t = clamp(t, 0.0, (float)(TerrainDim - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDim - 1, TerrainDim - 1));
	int r0 = (int)TerrainDim - 1 - t0.y;
	int r1 = (int)TerrainDim - 1 - t1.y;
	float n00 = WaterNear.Load(int3(t0.x, r0, 0));
	float n10 = WaterNear.Load(int3(t1.x, r0, 0));
	float n01 = WaterNear.Load(int3(t0.x, r1, 0));
	float n11 = WaterNear.Load(int3(t1.x, r1, 0));
	return lerp(lerp(n00, n10, f.x), lerp(n01, n11, f.x), f.y);
}

// Touch-down toe: positive depth eases to the ground below 9 units, tangent
// at 3, so a thinning layer lands ON the ground instead of ending 1-2 units
// up as a lip. Applied to geometry, the PS effective depth and the shading
// depth alike.
float TouchDownToe(float depth)
{
	return depth > 0.0 ? depth * smoothstep(3.0, 9.0, depth) : depth;
}

// min(a, b) through a quadratic knee of radius k, so a cap meeting a ramp
// leaves no crease.
float KneeMin(float a, float b, float k)
{
	float e = a - b;
	float knee = e > k ? e : (e < -k ? 0.0 : (e + k) * (e + k) / (4.0 * k));
	return a - knee;
}

// Bilinear land height from the 128-texel window, -1e5 when a corner is
// unbaked.
float TerrainHeightAt(float2 gridLocal)
{
	float2 t = (GridToTerrainOffset + gridLocal) / TerrainTexelSize;
	t = clamp(t, 0.0, (float)(TerrainDim - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDim - 1, TerrainDim - 1));
	float s00 = TerrainWindow.Load(int3(t0.x, t0.y, 0)).x;
	float s10 = TerrainWindow.Load(int3(t1.x, t0.y, 0)).x;
	float s01 = TerrainWindow.Load(int3(t0.x, t1.y, 0)).x;
	float s11 = TerrainWindow.Load(int3(t1.x, t1.y, 0)).x;
	[flatten] if (min(min(s00, s10), min(s01, s11)) <= -50000.0)
		return -100000.0;
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// Land-blended edges: on mixed land the landscape's own height blend decides
// snow or ground (Lighting WriteLandSnowOdds), the snow lies as a film where
// the land has any snow weight, and trenches reach the ground.
// LANDSCAPE-SNOW-PLAN Phase 5.
static const float kLandFilm = 0.5;
// Remaining depth under which the snow is a film: the land's own verdict
// decides it pixel by pixel, objects included.
static const float kLandFilmDepth = 1.0;

// The land's snow-texture weight, bilinear like TerrainHeightAt.
float LandSnowWeight(float2 gridLocal)
{
	float2 t = (GridToTerrainOffset + gridLocal) / TerrainTexelSize;
	t = clamp(t, 0.0, (float)(TerrainDim - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDim - 1, TerrainDim - 1));
	float b00 = TerrainBare.Load(int3(t0.x, t0.y, 0));
	float b10 = TerrainBare.Load(int3(t1.x, t0.y, 0));
	float b01 = TerrainBare.Load(int3(t0.x, t1.y, 0));
	float b11 = TerrainBare.Load(int3(t1.x, t1.y, 0));
	return 1.0 - lerp(lerp(b00, b10, f.x), lerp(b01, b11, f.x), f.y);
}

// No land snow weight on any texel a bilinear tap over [lo, hi] can reach
// (ShellTerrainAllBare's texel range).
bool LandRectNoSnow(float2 lo, float2 hi)
{
	float2 tLo = (GridToTerrainOffset + lo) / TerrainTexelSize;
	float2 tHi = (GridToTerrainOffset + hi) / TerrainTexelSize;
	float maxTexel = (float)(TerrainDim - 1);
	int2 i0 = (int2)clamp(floor(tLo), 0.0, maxTexel);
	int2 i1 = (int2)clamp(floor(tHi) + 1.0, 0.0, maxTexel);
	i1 = min(i1, i0 + 3);
	float bare = 1.0;
	[loop] for (int y = i0.y; y <= i1.y; ++y)
	{
		[loop] for (int x = i0.x; x <= i1.x; ++x)
			bare = min(bare, TerrainBare.Load(int3(x, y, 0)));
	}
	return bare >= 0.998;
}

// The land's snow odds in Masks.y; -16 where something else drew there (an
// object's base, grass), so thin snow never claims it.
float LandSnowOdds(float masksY)
{
	return masksY > 0.002 && masksY < 1.5 ? saturate((masksY - 0.004) * (1.0 / 0.996)) * 32.0 - 16.0 : -16.0;
}

// The snow's side of the contest: remaining depth buries the land's grain
// (CompactLook.y per unit), a carve pushes thin snow away (CompactLook.z).
float LandSnowSide(float remaining, float carve)
{
	return CompactLook.y * remaining - CompactLook.z * smoothstep(0.3, 1.0, carve);
}

// Rise above the waterline over which the cap runs the class ramp's 38
// units (-8 to +30): the snow is full about 8 units above the water on any
// bank. Keyed on height alone so it is monotone away from the water; a
// horizontal-distance estimate (rise / local slope) fell again wherever a
// bank steepened inland, a trough on a straight texel edge.
static const float kShoreRise = 10.0;

// The snow ends AT the waterline with the class border's own depth ramp.
// The wet texels (-8) give the shore a class ramp, but that ramp is anchored
// to the land lattice, so its zero can fall either side of the line; this
// caps the depth by rise above the line from the 128-texel bilinear alone
// (the same from every range), zero at the line, negative past it. The cap
// is the larger of that and a horizontal term, the class ramp run from one
// texel past the nearest vertex under the water: ground two texels from
// the line is free however little it stands above the level (a cave floor
// 5 units over a lake plane was thinned everywhere, in heightmap-quantised
// rectangles), and both terms only grow away from the water. Rounded knee
// where the cap meets the class ramp. Once per consumer. waterCap is the
// height the sheet may stand above the ground here (1e9 = no water), so
// the object lift can be held to it as well.
float3 EndSnowAtWater(float3 terrain, float2 gridLocal, out float waterCap)
{
	waterCap = 1e9;
	[branch] if (CompactLook.x > 0.5 && terrain.x > -50000.0 && terrain.y > -7.9)
	{
		float2 waterAt = SampleWaterHeight(gridLocal);
		float water = waterAt.x;
		float waterWeight = waterAt.y;
		[branch] if (waterWeight > 0.001)
		{
			float h = TerrainHeightAt(gridLocal);
			[branch] if (h > -50000.0)
			{
				float rise = h - water + kWaterSkimDepth;
				float capRise = rise * (38.0 / kShoreRise);
				float capNear = (SampleWaterNear(gridLocal) - 1.0) * 38.0;
				// Past the raster's last texel the cap lifts clear of the ramp.
				float cap = max(capRise, capNear) + (1.0 - waterWeight) * 60.0;
				waterCap = cap;
				terrain.y = max(KneeMin(terrain.y, cap, 4.0), -8.0);
			}
		}
	}
	return terrain;
}


// The shell surface: per-texture-class snow depth carved by deformation.
// Class depths blend by their baked weights on the CPU (window rebuild),
// so boundaries between differently-deep snows are geometric depth ramps;
// negative depths (roads) submerge the shell below the surface.
// Shared by the geometry and the prepass so both agree.

float ShellSurfaceZ(float2 gridLocal, out float coverage, out float terrainHeight)
{
	// Beyond the loaded-cell seam the recolored LOD terrain owns the ground:
	// park the vertex under it and skip the field work entirely. Single
	// return keeps fxc's X4000 quiet (early return + out params misfires).
	float edgeFade = ShellEdgeFade(gridLocal);
	float surfaceZ;
	[branch] if (edgeFade <= 0.0)
	{
		terrainHeight = SampleTerrain(gridLocal).x;
		coverage = 0.0;
		surfaceZ = terrainHeight - 32.0;
	}
	else
	{
		float3 terrain = SampleTerrain(gridLocal);

		float waterCap;
		terrain = EndSnowAtWater(terrain, gridLocal, waterCap);

		terrainHeight = terrain.x;
		float rampDepth = terrain.y;
		coverage = saturate(terrain.z);

		// The mask carries two independent channels. x = coverage suppression,
		// smooth 0-1, so doorstep clearings fade at their edges instead of
		// cutting. y = melt fraction: depth thins toward kFireMeltFloor
		// (coverage untouched), so melted ground keeps a thin snow floor
		// instead of fading to bare ground. Sampled BEFORE the object lift:
		// the wall-drift banks must respect clearings too (see below).
		float2 shelterMask = SampleExclusionMask(GridOrigin + gridLocal);

		// Object height field: t4 holds the SLOPE-LIMITED snow-height field
		// (terrain run through the angle-of-repose cone transform), t5 the
		// shelter mask; 1 under floating structures, so walkways, roofs and
		// bridges keep the ground beneath them bare.
		[branch] if (ObjectLiftCap > 1.0)
		{
			float2 worldXY = GridOrigin + gridLocal;
			float field = SampleObjectHeight(worldXY);
			[flatten] if (field > -50000.0)
			{
				// The lift never crosses the shell's own boundary: banks and
				// cones rise only from ground showing shell snow (class
				// coverage minus clearings). Gating on the exclusion mask alone
				// misses bare landscape classes and raises a rim where the
				// shell meets dirt.
				float groundSnow = smoothstep(0.1, 0.45, coverage) * (1.0 - saturate(shelterMask.x));
				// The lift is the field's excess over its OWN base, the window's
				// bilinear height - not over the height the shell stands on.
				// With the land-exact layer those differ by the whole chord
				// error, and max(exact, bilinear) is an envelope creased along
				// the 128-unit lattice (stripes, lifted hollows, forced snow).
				float lift = max(field - SampleTerrainBilinearHeight(gridLocal), 0.0) * groundSnow;
				// A drift bank ends at the water like the sheet does: lift plus
				// depth stays under the shore cap.
				lift = max(KneeMin(lift, max(waterCap - max(rampDepth, 0.0), 0.0), 4.0), 0.0);
				// Where a captured object defines the surface, the layer wears
				// the object's own skin depth instead of the landscape class
				// depth (a thin-skinned rock must not carry a deep landscape
				// layer). Blend by how far the object stands proud of the
				// un-lifted base, so buried objects and the aprons around them
				// keep landscape depth.
				float capT = smoothstep(0.25, 1.0, lift / max(rampDepth, 1.0));
				rampDepth = lerp(rampDepth, min(rampDepth, SampleObjectDepthCap(worldXY)), capT);
				terrainHeight += lift;
				// Drift failsafe: a field standing well proud IS snow. Force
				// coverage and a minimum depth so banks raised over bare or
				// low-coverage ground (dirt patches at walls) never dither into
				// holes or submerge out of the surface.
				float liftForce = smoothstep(6.0, 20.0, lift);
				coverage = max(coverage, liftForce);
				rampDepth = max(rampDepth, liftForce * 6.0);
			}
		}

		coverage *= saturate(1.0 - shelterMask.x);

		// Bare ground contributes negative depth so the shell submerges toward
		// uncovered terrain as well; edgeFade (computed above) melts the shell
		// into the ground across the seam ramp.
		float bare = saturate(1.0 - coverage);
		float depth = rampDepth + (-8.0) * bare;
		depth = lerp(-8.0, depth, edgeFade);

		// Deep trench floors pass unchanged; a floor set below ~9 (a low
		// Trench Floor fraction) compresses with the toe.
		depth = TouchDownToe(depth);
		// Melt AFTER the toe: the floor stands kFireMeltFloor over the
		// ground. Melted first, the toe took it to exactly 0 and the sheet
		// fought the land for every pixel of the basin.
		depth = lerp(depth, min(depth, kFireMeltFloor), saturate(shelterMask.y));

		// Carves only where the layer is raised; the negative-depth submerge at
		// class edges is untouched. Undulation rides on top, scaled by
		// remaining depth.
		[flatten] if (depth > 0.0)
		{
			float deformation = saturate(SampleDeformation(gridLocal));
			float bermD = BermField(gridLocal);
			float uncarved = depth;
			// The berm is spoil piled on snow that was NOT dug. Unmasked, the
			// blurred field also lifted the trench floor and walls - a narrow
			// trail's disc average is well above zero at its own centre, so
			// raising Berm Height raised the whole trench with it.
			depth = CarveProfile(deformation, uncarved, GridOrigin + gridLocal) +
			        BermShape(bermD) * saturate(1.0 - deformation) * uncarved * BermHeightAmp * BermDepthGate(uncarved);
			depth += UndulationSampled(GridOrigin + gridLocal) * saturate(depth / 8.0);
			// Churn scales away on thin cover: the /10 keeps the dig under 80% of
			// local depth even at the slider's 8-unit maximum.
			[branch] if ((ShellFlags.x & 16) == 0)
				depth += ChurnNoise(GridOrigin + gridLocal) * ChurnHeightAmp * ChurnWeight(deformation, bermD, GridOrigin + gridLocal) * saturate(depth / 10.0);
			// Berm clods: a coarser octave weighted by BermShape rather than
			// the churn weight, since spoil lands on the crest and churn peaks
			// in the trench. Masked like the berm and gated on material; the
			// self-shadow march skips it, as it skips churn.
			[branch] if (RimStyle.z > 0.01 && (ShellFlags.x & 16) == 0)
				depth += ChurnNoiseScaled(GridOrigin + gridLocal, kClodSizeScale) * RimStyle.z *
				         BermShape(bermD) * saturate(1.0 - deformation) * BermDepthGate(uncarved);
		}

		surfaceZ = terrainHeight + depth;
	}
	return surfaceZ;
}

// The vertex normal's two height differences: wide 32-unit differences bridge
// the 128-unit data texels, and interpolation removes the faceting of the
// per-pixel piecewise-constant gradient.
float2 ShellVertexSlope(float2 gridLocal)
{
	float hxp = SampleTerrain(gridLocal + float2(32.0, 0.0)).x;
	float hxn = SampleTerrain(gridLocal - float2(32.0, 0.0)).x;
	float hyp = SampleTerrain(gridLocal + float2(0.0, 32.0)).x;
	float hyn = SampleTerrain(gridLocal - float2(0.0, 32.0)).x;
	return float2(hxp - hxn, hyp - hyn);
}

// Shared with the raised landscape (LandscapeSnow.hlsl). Macros: as called
// functions fxc schedules the shell's vertex stages differently.
#define SHELL_TERRAIN_NORMAL(slopeX, slopeY) normalize(float3(-(slopeX) / 64.0, -(slopeY) / 64.0, 1.0))

// Slope drape (Josef's sketch, 2026-09-05): on steep ground the layer
// stands off along the surface normal instead of straight up, so a
// near-vertical face keeps the full depth of cover in front of it. A
// vertical-only offset thins to depth*cos(slope) and the face shows
// through. The foot stays on the lattice; only the output position moves,
// so every sample still reads the vertex's own ground. footXY is the
// foot in position's frame (absolute world for the shell).
#define SHELL_DRAPE(position, footXY, z, terrainHeight, terrainNormal)                                      \
	[branch] if (SlopeDrape.x > 0.001 && (z) > (terrainHeight))                                             \
	{                                                                                                       \
		float drapeAbove = (z) - (terrainHeight);                                                           \
		float drapeTilt = SlopeDrape.x * (1.0 - smoothstep(SlopeDrape.y, SlopeDrape.z, (terrainNormal).z)); \
		float3 drapeDir = normalize(lerp(float3(0.0, 0.0, 1.0), (terrainNormal), drapeTilt));               \
		position = float3((footXY), (terrainHeight)) + drapeDir * drapeAbove;                               \
	}

// The raised landscape's sun-cascade visibility (LandscapeSnow.hlsl); below
// 0.35 nothing casts. Its terms:
// - The far anti-pinhole pass dilates coverage AND height (4-tap max + ridge
//   pad), so beyond ~3000 units a ring around every patch passes both castVis
//   terms while the main view discards those pixels per-texel — and this
//   depth-only caster has no PS to do the same. Gating coverage on the
//   UNDILATED texel under the vertex keeps true snowfields casting and
//   silences the dilation skirt over bare coast.
// - Melt pits do not cast: snow the melt removed casts nothing, and the pit
//   dissolves at texture resolution while the caster rim collapses at vertex
//   resolution, printing a blocky shadow on the revealed ground. Stamped
//   (spell) melt only - static clearings are the exclusion field's. Keep the
//   thresholds well above the residue floor, or decaying .y lands on the
//   castVis NaN cliff and caster triangles flip once a second.
// - The far field keeps printing blotches from residual data/geometry
//   mismatches the near gates cannot see, and the shell's crisp shadows only
//   matter near the camera anyway — the far field is diffuse-dominated and
//   terrain shadows carry the rest. Fade the caster out entirely over ~40-70 m.
#define SHELL_CAST_VIS(castVis, gridLocal, z, coverage)                                                                               \
	float3 rawTerrainCast = SampleTerrain(gridLocal);                                                                                 \
	float castVis = smoothstep(2.0, 5.0, (z) - rawTerrainCast.x) * smoothstep(0.2, 0.5, min((coverage), saturate(rawTerrainCast.z))); \
	castVis *= 1.0 - smoothstep(0.35, 0.6, SampleMelted(gridLocal));                                                                  \
	castVis *= 1.0 - smoothstep(2800.0, 4900.0, length((gridLocal) - WarpedHalfSpan));

#ifndef SNOW_SURFACE_ONLY
// The replay domain shader's vertex tail: terrain normal, slope drape,
// camera-relative transform and the few outputs the prepass reads. The two
// height differences arrive as separate scalars: as a float2 the compiler
// vectorises the normalize below, the same arithmetic but not the same
// bytecode.
VS_OUTPUT FinishShellVertexSloped(float2 gridLocal, float z, float coverage, float terrainHeight, float slopeX, float slopeY)
{
	float3 terrainNormal = SHELL_TERRAIN_NORMAL(slopeX, slopeY);

	float3 absolutePos = float3(GridOrigin + gridLocal, z);
	SHELL_DRAPE(absolutePos, GridOrigin + gridLocal, z, terrainHeight, terrainNormal)

	// Shader world space is camera-relative.
	float3 rel = absolutePos - ShellCameraPosAdjust.xyz;

	VS_OUTPUT vsout;
	vsout.Position = mul(CameraViewProj, float4(rel, 1.0));
	vsout.WorldPos = rel;
	vsout.GridLocal = gridLocal;
	// The prepass PS reads CurrentClip.w and the normal's xyz of the rest.
	vsout.CurrentClip = float4(0.0, 0.0, 0.0, mul(CameraViewProjUnjittered, float4(rel, 1.0)).w);
	vsout.PreviousClip = 0.0;
	vsout.Snowness = 0.0;
	vsout.DebugHeight = 0.0;
	vsout.TerrainNormalAlpha = float4(terrainNormal, 0.0);
	return vsout;
}

VS_OUTPUT FinishShellVertex(float2 gridLocal, float z, float coverage, float terrainHeight)
{
	float hxp = SampleTerrain(gridLocal + float2(32.0, 0.0)).x;
	float hxn = SampleTerrain(gridLocal - float2(32.0, 0.0)).x;
	float hyp = SampleTerrain(gridLocal + float2(0.0, 32.0)).x;
	float hyn = SampleTerrain(gridLocal - float2(0.0, 32.0)).x;
	return FinishShellVertexSloped(gridLocal, z, coverage, terrainHeight, hxp - hxn, hyp - hyn);
}
#endif

#if defined(HULLSHADER) || defined(SNOW_SURFACE_ONLY)
// Detail reach (LandscapeSnowTess.hlsli LsDetailFactor): full kTessMax within
// kTessNear/kTessMax units, factor 1 by kTessNear. Deformed edges stretch the
// reach by kTessReachBoost; past the BOOSTED reach the factor clamps to 1 for
// every deformation value, so the map taps cannot change the answer.
static const float kTessNear = 1600.0;
static const float kTessMax = 8.0;
static const float kTessReachBoost = 3.0;
static const float kTessCutoff = kTessNear * kTessReachBoost;
#endif

#include "SnowDeformation/SnowParallax.hlsli"

#if defined(DOMAINSHADER) || defined(SNOW_SURFACE_ONLY)
// The vertex surface: ShellSurfaceZ plus the skirt descent and the
// displacement relief. One function for the replay's domain shader and the
// raised landscape, so both stand on the same bits.
float ShellVertexZFrom(float2 gridLocal, float z, float coverage, float terrainHeight)
{
	float camDist = length(GridOrigin + gridLocal - ShellCameraPosAdjust.xy);
	float2 snowUV = (SnowUVOffset + gridLocal) / kSnowUVTile;
	// Coarser mips with distance: vertex density falls below texel density
	// out there and full-res sampling shimmers.
	float snowMip = clamp(log2(max(camDist, 64.0) / 128.0), 0.0, 6.0);

	// Grain-driven skirt descent (HEIGHT-BLEND-PLAN): across the coverage edge
	// band the surface descends in grain-shaped tongues rather than a knife-cut
	// hover. Half the PS alpha's sharpness - the alpha cuts the outline, while
	// near-binary geometry raises walls that smear the top-projected texture.
	// Deterministic per position, so shared patch-edge vertices agree.
	// The border's raggedness is HORIZONTAL (Josef's sketch): the grain also
	// advances/retards WHERE along the band the sheet lands, so the landing
	// line billows in plan. The retired vertical edge grain (z +- grain) made
	// the same billows by lifting the rim, and every raised bump hovered with
	// its underside exposed; the fringe now touches ground everywhere. Carved
	// trenches and melt floors keep the vertical grain's old exemptions.
	float descentBlend = SnowHeightBlendSharpness(camDist);
	float wEdge = smoothstep(0.0, 0.6, coverage);
	[branch] if (HasSnowHeight > 0.5 && descentBlend > 1.0 && z > terrainHeight && wEdge < 0.999)
	{
		float hDescent = SampleSnowHeight(ComputeSnowTapsNoGrad(snowUV, GridOrigin + gridLocal), 0.0.xx, snowMip);
		float billowW = (1.0 - smoothstep(0.05, 0.4, saturate(SampleDeformation(gridLocal)))) *
		                (1.0 - saturate(SampleExclusionMask(GridOrigin + gridLocal).y * 2.0));
		const float kEdgeBillowShift = 0.35;
		float wBillow = saturate(wEdge + (hDescent - 0.5) * kEdgeBillowShift * billowW);
		float descent = SnowHeightBlendOneSided(wBillow, hDescent, 1.0 + (descentBlend - 1.0) * 0.5);
		// Josef's border spec: the sheet ends BELOW the ground, not on it. As
		// the descent runs out the sheet keeps going to terrain - 8
		// (ShellSurfaceZ's own submerge floor), so the visible border is the
		// sheet's INTERSECTION with the terrain - the depth test clips it -
		// and a hovering rim with an exposed underside cannot exist. The
		// grain still decides WHERE each tongue dives, so the intersection
		// line keeps the billows.
		z = terrainHeight + (z - terrainHeight) * descent - 8.0 * (1.0 - descent);
	}

	// Real relief from the PBR displacement map, through the SAME anti-tiling
	// taps the PS shades with, so the normal map's shading and the geometry
	// describe one surface. (A single un-offset tap here against the PS's
	// three random per-cell offsets decorrelates the two
	// fields were decorrelated, so geometry bumps sat where the texture had
	// none. That is what made the relief read wrong against its own shading.)
	// Gated by local depth (thin cover and carved floors stay flat), by the
	// deformation (compressed snow is smooth), and faded with the same
	// distance band as the micro-normal.
	[branch] if (HasSnowHeight > 0.5 && SnowReliefDepth > 0.01)
	{
		float reliefFade = 1.0 - smoothstep(600.0, 2200.0, camDist);
		float depthAbove = z - terrainHeight;
		[branch] if (reliefFade > 0.001 && depthAbove > 0.5)
		{
			// Same uv and same world XY the PS feeds ComputeSnowTaps, so the
			// tap set here is the one that will shade this point.
			SnowTaps reliefTaps = ComputeSnowTapsNoGrad(snowUV, GridOrigin + gridLocal);
			float h = SampleSnowHeight(reliefTaps, 0.0.xx, snowMip);
			float carve = saturate(SampleDeformation(gridLocal));
			z += (h - 0.5) * SnowReliefDepth * reliefFade * saturate(depthAbove / 6.0) * (1.0 - carve);
		}
	}

	return z;
}

float ShellVertexZ(float2 gridLocal, out float coverage, out float terrainHeight)
{
	float z = ShellSurfaceZ(gridLocal, coverage, terrainHeight);
	return ShellVertexZFrom(gridLocal, z, coverage, terrainHeight);
}
#endif  // DOMAINSHADER || SNOW_SURFACE_ONLY



#ifdef PSHADER

struct PS_OUTPUT
{
	float4 Diffuse : SV_Target0;
	float4 MotionVectors : SV_Target1;
	float4 NormalGlossiness : SV_Target2;
	float4 Albedo : SV_Target3;
	float4 Specular : SV_Target4;
	float4 Reflectance : SV_Target5;
	float4 Masks : SV_Target6;
	float4 Masks2 : SV_Target7;
};

// Depth prepass (SNOW_SHELL_DEPTH_PREPASS): the alpha cut and the export
// clamp, nothing else. DepthLE is the shell's real depth (clamped); the
// colour target receives the RASTER depth of every fragment that wins, which
// the fill pass turns into the shading pass's EQUAL test buffer - a hardware
// EQUAL against the clamped depth would drop every clamped pixel.
struct PS_PREPASS_OUTPUT
{
	float RasterDepth : SV_Target0;
#	ifdef SNOW_PREPASS_ATTR
	// Deferred shading's inputs for each winning pixel: GridLocal and the
	// terrain normal's xy; GridLocal's coarse derivatives; the view depth and
	// WorldPos.z's coarse derivatives.
	float4 AttrGrid : SV_Target1;
	float4 AttrGridDeriv : SV_Target2;
	float4 AttrDepth : SV_Target3;
#		ifdef SNOW_PREPASS_EARLY
	// A pending pixel's inputs for the resolve: y = the snow's side of the land
	// contest; w = 2 if the export clamped + 8 if the land contest runs.
	float4 Pending : SV_Target4;
#		endif
#	endif
	float DepthLE : SV_DepthLessEqual;
};

// The depth the shell writes: the raster depth, pulled to just in front of
// the scene inside the two clamp windows. Shared by the prepass and the
// export tail so both agree bit for bit. FAR clamp: the z-fight/pinhole
// class loses at the source without moving geometry; far field only.
// NEAR micro-clamp: the edge zone and carved floors ride within window error
// of the mesh; 0.75 units is too thin to overdraw feet or props.
//
// sceneAbove tells a coincident terrain surface from an occluder: the scene
// point on this pixel's ray, in world height over THE SNOW LINE AT ITS OWN
// FOOTPRINT (ShellSceneAboveSnowLine). Terrain the shell z-fights sits on
// that line; a wall, a rock or a roof edge stands above it.
// Without this the far clamp pulled the ground behind a wall in front of the
// wall, the object's skin drawn after lost the depth test, and the ground in
// the wall's own shadow showed where the cap was - per object, past 4000
// units, moving with the camera, deaf to every shading toggle, and tipped
// into view by two units of bump octave (Josef, 2026-09-08).
static const float kShellClampOccluderSlack = 8.0;
// Is the scene surface on this pixel's ray the TERRAIN this shell covers, or
// something standing on it? Take the scene point along the ray and read the
// snow line at ITS OWN footprint: terrain the shell z-fights sits on that
// line, a wall top or a rock cap stands well above it. Measuring against the
// shell fragment's own height instead is not enough - ground banked up behind
// a low wall reads at the wall's height, so the clamp fired anyway and the
// caps stayed dark (Josef, 2026-09-08). Outside the terrain window, and over
// missing data, this says nothing and the clamp keeps its old behaviour.
//
// A projected-snow static (Masks.y >= 2; land keeps (0, 1]) is an occluder at
// any height: its foot stands inside the slack, and a sheet clamped over it
// reads the cascades from inside the object - a dark rim past 4000 units.
float ShellSceneAboveSnowLine(float3 worldPosRel, float sceneZ, float shellZ, float2 pixel)
{
	float3 sceneRel = worldPosRel * (sceneZ / max(shellZ, 1e-3));
	float2 sceneLocal = sceneRel.xy + ShellCameraPosAdjust.xy - GridOrigin;
	float2 tt = (GridToTerrainOffset + sceneLocal) / TerrainTexelSize;
	bool inWindow = all(tt >= 0.0) && all(tt <= (float)(TerrainDim - 1));
	float3 st = SampleTerrain(sceneLocal);
	float above = (sceneRel.z + ShellCameraPosAdjust.z) - (st.x + max(st.y, 0.0));
	above = (inWindow && st.x > -50000.0) ? above : 0.0;
#ifdef SNOW_PREPASS_EARLY
	// No land masks yet: a clamp a static could cancel is left to the resolve.
	return above;
#else
	bool sceneIsStatic = LandMasksCopy.Load(int3(pixel, 0)).y >= 1.5;
	return sceneIsStatic ? 1e6 : above;
#endif
}

bool ShellFarClampWindow(float shellZ, float sceneZ)
{
	float clampWindow = min(8.0 + shellZ * 0.008, 48.0);
	return shellZ > 4000.0 && shellZ > sceneZ && shellZ - sceneZ < clampWindow;
}

bool ShellNearClampWindow(float shellZ, float sceneZ)
{
	return shellZ > sceneZ && shellZ - sceneZ < 0.75;
}

float ShellExportDepth(float rasterZ, float rawSceneDepth, float shellZ, float sceneZ, float pixelEffDepth, float pixelCarve, float sceneAbove)
{
	float depth = rasterZ;
	bool clampMode = sceneAbove < kShellClampOccluderSlack;
	[branch] if (clampMode && ShellFarClampWindow(shellZ, sceneZ))
		depth = min(rasterZ, rawSceneDepth - 1e-5);
	else if (clampMode && (pixelEffDepth < 4.0 || pixelCarve > 0.5) && ShellNearClampWindow(shellZ, sceneZ))
		depth = min(rasterZ, rawSceneDepth - 1e-5);
	return depth;
}

// Out of both windows the export is rasterZ whatever sceneAbove says.
float ShellExportDepthAt(float rasterZ, float rawSceneDepth, float shellZ, float sceneZ, float pixelEffDepth, float pixelCarve, float3 worldPosRel, float2 pixel)
{
	float sceneAbove = 0.0;
	[branch] if (ShellFarClampWindow(shellZ, sceneZ) || ShellNearClampWindow(shellZ, sceneZ))
		sceneAbove = ShellSceneAboveSnowLine(worldPosRel, sceneZ, shellZ, pixel);
	return ShellExportDepth(rasterZ, rawSceneDepth, shellZ, sceneZ, pixelEffDepth, pixelCarve, sceneAbove);
}

// Out of both windows the export is the raster depth; behind the scene (the
// blended depth is the main depth to within rounding) it fails LESS_EQUAL.
bool ShellPrepassHidden(float rasterZ, float rawSceneDepth, float shellZ, float sceneZ)
{
	return !ShellFarClampWindow(shellZ, sceneZ) && !ShellNearClampWindow(shellZ, sceneZ) && rasterZ > rawSceneDepth + 1e-6;
}

// Every lane of the quad is hidden, so the exit is quad-uniform and the
// derivatives fxc sinks below it stay valid.
bool ShellPrepassQuadHidden(float rasterZ, float rawSceneDepth, float shellZ, float sceneZ)
{
	float hidden = ShellPrepassHidden(rasterZ, rawSceneDepth, shellZ, sceneZ) ? 1.0 : 0.0;
	float hx = ddx_fine(hidden);
	return hidden > 0.5 && hx == 0.0 && ddy_fine(hidden) == 0.0 && ddy_fine(hx) == 0.0;
}


#if defined(SNOW_SHELL_DEFERRED)
// LLF cluster lookup coordinates for a camera-relative position.
void ShellClusterCoords(float3 relPos, out float viewZ, out float2 clusterUV)
{
	viewZ = mul(CameraView, float4(relPos, 1.0)).z;
	float4 clip = mul(CameraViewProj, float4(relPos, 1.0));
	clusterUV = clip.xy / max(clip.w, 1e-4) * float2(0.5, -0.5) + 0.5;
}

// Detail grain from the diffuse's luminance, for snow sets without a normal map.
float3 ShellLuminanceBump(float3 normalWS, float2 worldXY, float bumpFade)
{
	const float kBumpTile = 64.0;
	const float kBumpHeight = 0.55;
	float2 texDims;
	SnowDiffuse.GetDimensions(texDims.x, texDims.y);
	float e = 1.5 / texDims.x;
	float2 detailUV = worldXY / kBumpTile;
	const float3 kLum = float3(0.30, 0.45, 0.25);
	const float2 detailDx = gDeferredGridDx / kBumpTile;
	const float2 detailDy = gDeferredGridDy / kBumpTile;
	float h0 = dot(SnowDiffuse.SampleGrad(SnowSampler, detailUV, detailDx, detailDy).rgb, kLum);
	float hx = dot(SnowDiffuse.SampleGrad(SnowSampler, detailUV + float2(e, 0.0), detailDx, detailDy).rgb, kLum);
	float hy = dot(SnowDiffuse.SampleGrad(SnowSampler, detailUV + float2(0.0, e), detailDx, detailDy).rgb, kLum);
	float2 bumpGrad = float2(hx - h0, hy - h0) * (kBumpHeight / (e * kBumpTile));
	return normalize(normalWS + float3(-bumpGrad * bumpFade, 0.0));
}
#endif

#if defined(SNOW_S1_RESOLVE)
// S1 resolve (DrawShell, before the deferred pass): finishes the early
// prepass's pending pixels with what the game's opaque draws left - the land's
// verdict, the clamp against a static - and writes every winner's depth into
// the main buffer under LESS_EQUAL, so a pixel something drew in front of
// since the claim loses. The marks are DepthSync's and the private fill's.
Texture2D<float> EarlyRasterDepth : register(t61);
Texture2D<float4> EarlyPending : register(t62);

struct S1_RESOLVE_OUTPUT
{
	float Marks : SV_Target0;
	float Depth : SV_Depth;
};

S1_RESOLVE_OUTPUT main(float4 pos : SV_Position)
{
	const int3 px = int3(pos.xy, 0);
	const float raster = EarlyRasterDepth.Load(px);
	if (raster == 0.0)
		discard;
	float depth = abs(raster);
	[branch] if (raster < 0.0)
	{
		const float4 pending = EarlyPending.Load(px);
		const uint flags = (uint)(pending.w * 0.5);
		const float hLandRaw = LandMasksCopy.Load(px).y;
		// The land's own verdict against the snow's side.
		[branch] if ((flags & 4) != 0)
		{
			if (LandSnowOdds(hLandRaw) + pending.y < 0.0)
				discard;
		}
		if ((flags & 1) != 0 && hLandRaw < 1.5)
			depth = min(depth, SceneDepth.Load(px) - 1e-5);
	}
	S1_RESOLVE_OUTPUT o;
	o.Marks = abs(raster);
	o.Depth = depth;
	return o;
}
#else
#if defined(SNOW_SHELL_DEFERRED)
PS_OUTPUT ShellShade(VS_OUTPUT input)
#else
PS_PREPASS_OUTPUT main(VS_OUTPUT input)
#endif
{
	// Same convention as MotionBlur::GetSSMotionVector.
	float2 motionVector = float2(-0.5, 0.5) * (input.CurrentClip.xy / input.CurrentClip.w - input.PreviousClip.xy / input.PreviousClip.w);

	float2 gridLocal = input.GridLocal;
	// Depth reads shared by the hidden-fragment exit, the edge dissolve, the
	// proximity fade and the conservative depth clamp at the tail.
	float rawSceneDepth = SceneDepth.Load(int3(input.Position.xy, 0));
	float sceneZ = SharedData::GetScreenDepth(rawSceneDepth);
	float shellZ = input.CurrentClip.w;
	// The prepass's only derivatives.
#ifdef SNOW_PREPASS_ATTR
	const float4 attrGridDeriv = float4(ddx_coarse(input.GridLocal), ddy_coarse(input.GridLocal));
	const float2 attrZDeriv = float2(ddx_coarse(input.WorldPos.z), ddy_coarse(input.WorldPos.z));
#endif
#if defined(SNOW_SHELL_DEPTH_PREPASS) && !defined(SNOW_PREPASS_KEEP_HIDDEN)
	// The depth export denies early-Z; a hidden quad skips the alpha cut.
	[branch] if (ShellPrepassQuadHidden(input.Position.z, rawSceneDepth, shellZ, sceneZ))
	{
		discard;
		return (PS_PREPASS_OUTPUT)0;
	}
#endif

	// The terrain field per PIXEL: the ramp the deferred attributes carry and
	// the inputs of the cut below.
#ifdef SNOW_SHELL_DEFERRED
	// The prepass computed the ramp from the same GridLocal bits; with the
	// cut trusted, nothing else of this block reaches the outputs.
	float pixelRampDepth = gDeferredRampDepth;
	float pixelWaterCap = 0.0;
	float3 pixelTerrain = 0.0;
	float pixelCoverage = 1.0;
	float psEdgeFade = 1.0;
	float pixelClassDepth = 0.0;
#else
	float pixelWaterCap;
	float3 pixelTerrain = EndSnowAtWater(SampleTerrain(gridLocal), gridLocal, pixelWaterCap);
	float pixelCoverage = saturate(pixelTerrain.z);
	float psEdgeFade = ShellEdgeFade(gridLocal);

	// Un-carved class depth ramp; negative means the snow is submerged.
	// Mirrors ShellSurfaceZ's object-depth cap so the cut agrees with the
	// capped geometry.
	float pixelClassDepth = pixelTerrain.y;
	[branch] if (ObjectLiftCap > 1.0)
	{
		float2 capWorldXY = GridOrigin + gridLocal;
		float capField = SampleObjectHeight(capWorldXY);
		[flatten] if (capField > -50000.0)
		{
			// Boundary gate mirroring ShellSurfaceZ: no lift from bare ground.
			float capGround = smoothstep(0.1, 0.45, pixelCoverage) * (1.0 - saturate(SampleExclusionMask(capWorldXY).x));
			capField = lerp(min(capField, pixelTerrain.x), capField, capGround);
			float capLift = capField - pixelTerrain.x;
			capLift = max(KneeMin(capLift, max(pixelWaterCap - max(pixelClassDepth, 0.0), 0.0), 4.0), 0.0);
			float capT = smoothstep(0.25, 1.0, capLift / max(pixelClassDepth, 1.0));
			pixelClassDepth = lerp(pixelClassDepth, min(pixelClassDepth, SampleObjectDepthCap(capWorldXY)), capT);
			// Drift failsafe, mirroring ShellSurfaceZ: raised banks are snow
			// regardless of the ground class beneath.
			float liftForce = smoothstep(6.0, 20.0, capLift);
			pixelCoverage = max(pixelCoverage, liftForce);
			pixelClassDepth = max(pixelClassDepth, liftForce * 6.0);
		}
	}
	float pixelBare = saturate(1.0 - pixelCoverage);
	float pixelRampDepth = pixelClassDepth + (-8.0) * pixelBare;
	pixelRampDepth = lerp(-8.0, pixelRampDepth, psEdgeFade);
#endif

	// Mirror of the geometry's touch-down toe, so the cut reads the sheet's
	// ACTUAL height above ground.
	float pixelEffDepth = TouchDownToe(pixelRampDepth);
#ifdef SNOW_SHELL_DEFERRED
	float coverageAlpha = 1.0;
#else
	// Wherever the land has snow weight the pixel is a candidate; on mixed
	// land the land's own verdict decides it below.
	const float pixelSnowWeight = LandSnowWeight(gridLocal);
	float coverageAlpha = pixelSnowWeight > 0.002 ? psEdgeFade : 0.0;
#endif
	// Josef's border spec: the alpha may not cut geometry still standing
	// above its ground - the sheet dives below the terrain (the descent's
	// undershoot) and the depth test against the terrain IS the border.
	// Keyed on the RENDERED height above the terrain data, not the ramp. The
	// window-edge fade rides along; the overrides and the land contest below
	// still multiply.
	float sheetAboveGround = smoothstep(0.25, 1.5, input.WorldPos.z + ShellCameraPosAdjust.z - pixelTerrain.x);
	coverageAlpha = max(coverageAlpha, sheetAboveGround * psEdgeFade);

	// Object blending (Terrain Blending-style depth proximity): the shell only
	// knows terrain heights, so this is what makes it meet statics softly
	// instead of slicing across them at the depth test. The gap is measured
	// along the view ray and shifts with the camera; mild distance scaling
	// turns that wobble into a soft fade. Keep the base short and the scaling
	// mild - a long or view-Z-proportional band reads as a translucent margin.
	float objectFadeBand = 5.0 + shellZ * 0.004;
	float proximityFade = saturate((sceneZ - shellZ) / objectFadeBand);
	// The fade softens where the shell CUTS THROUGH something standing above
	// the snow; a backdrop lying below the snow surface is buried and the shell
	// over it is opaque. Reconstruct the scene surface's world height along
	// the ray and compare it with the shell's own surface here, not with the
	// terrain: a rock top a few units under the shell is under the snow, and
	// measured against the terrain it read as an object and dithered into a
	// tint that never occluded it. Objects standing proud keep the soft
	// contact; the rim's descent onto its own terrain is below the shell too.
	[branch] if (shellZ < 8192.0)
	{
		float sceneSurfaceZ = ShellCameraPosAdjust.z + input.WorldPos.z * (sceneZ / max(shellZ, 1e-3));
		float shellSurfaceZ = ShellCameraPosAdjust.z + input.WorldPos.z;
		float objectness = smoothstep(-1.0, 2.0, sceneSurfaceZ - shellSurfaceZ);
		proximityFade = max(proximityFade, 1.0 - objectness);
	}
	// Two cases hug the geometry behind them and must override the fade, or
	// they dither away view-dependently: carved trench floors, and the shell
	// riding a raised height field.
	const bool pixelUntouched = ShellUntouched(gridLocal);
	float pixelCarve = 0.0;
	[branch] if (!pixelUntouched)
		pixelCarve = saturate(SampleDeformation(gridLocal));
	float pixelLift = 0.0;
	float2 pixelShelter = SampleExclusionMask(GridOrigin + gridLocal);
	[branch] if (ObjectLiftCap > 1.0)
	{
		float fieldHeight = SampleObjectHeight(GridOrigin + gridLocal);
		[flatten] if (fieldHeight > -50000.0)
		{
			// Boundary gate mirroring ShellSurfaceZ: a lift the geometry no
			// longer takes must not hold the alpha override either.
			float liftGround = smoothstep(0.1, 0.45, saturate(pixelTerrain.z)) * (1.0 - saturate(pixelShelter.x));
			fieldHeight = lerp(min(fieldHeight, pixelTerrain.x), fieldHeight, liftGround);
			pixelLift = max(KneeMin(fieldHeight - pixelTerrain.x, max(pixelWaterCap - max(pixelTerrain.y, 0.0), 0.0), 4.0), 0.0);
		}
	}
	// Fire-melted floors hug the terrain BY DESIGN (kFireMeltFloor above it);
	// without an override the proximity fade dithers them into translucency
	// like any other near-coincident surface.
	float pixelMelt = saturate(pixelShelter.y);
	// Trampled Border Fade retired: its default-0 behavior is the
	// keeper, so the window is the constant it resolved to.
	float carveOverride = smoothstep(0.1, 0.5, pixelCarve) * smoothstep(0.5, 1.0, pixelTerrain.y);
	// Land-blended edges: a film is the land's own snow, so the land's verdict,
	// not the view-dependent proximity fade, decides it next to objects.
	bool landFilm = false;
	float landRemaining = 0.0;
#ifndef SNOW_SHELL_DEFERRED
	[branch] if (coverageAlpha > 0.001)
	{
		landRemaining = CarveProfile(pixelCarve, max(pixelEffDepth, 0.0), GridOrigin + gridLocal);
		landFilm = landRemaining < kLandFilmDepth;
	}
#endif
	coverageAlpha *= landFilm ? 1.0 : max(proximityFade, saturate(carveOverride + smoothstep(2.0, 10.0, pixelLift) + smoothstep(0.1, 0.4, pixelMelt)));

#ifndef SNOW_SHELL_DEFERRED
#	ifdef SNOW_PREPASS_EARLY
	// The land's verdict is drawn after this pass: the snow's side is recorded
	// and the resolve runs the contest. It only lowers the alpha, so the cut
	// below on the uncontested alpha discards nothing it would keep.
	bool landContest = false;
	float landSide = 0.0;
	[branch] if (coverageAlpha > 0.001 && (landFilm || pixelSnowWeight < 0.998))
	{
		landSide = LandSnowSide(landRemaining, pixelCarve);
		landContest = landSide < 16.0;
	}
#	else
	[branch] if (coverageAlpha > 0.001 && (landFilm || pixelSnowWeight < 0.998))
	{
		[flatten] if (LandSnowOdds(LandMasksCopy.Load(int3(input.Position.xy, 0)).y) + LandSnowSide(landRemaining, pixelCarve) < 0.0)
			coverageAlpha = 0.0;
	}
#	endif
#endif
	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);
	float3 objectMeet = float3(0.0, 0.0, 0.0);
	[branch] if (shellZ < 2048.0)
	{
		objectMeet = ShellObjectMeet(GridOrigin + gridLocal, input.WorldPos.z + ShellCameraPosAdjust.z);
		objectMeet.x *= 1.0 - smoothstep(1024.0, 2048.0, shellZ);
	}
#ifdef SNOW_SHELL_DEFERRED
	// Only the prepass's winners reach this pass, and it already ran the cut.
	coverageAlpha = 1.0;
#else
	// Hard alpha test plus a whisker of stochastic dust just BEYOND the cut
	// (the 0.2..0.5 alpha tail), never upward into the committed sheet.
	// Survivors write fully opaque.
	{
		float dust = saturate((coverageAlpha - 0.2) * (1.0 / 0.3));
		if (screenNoise * screenNoise >= dust)
			discard;
		coverageAlpha = 1.0;
	}
#endif

	// A melted floor is as thin as an edge: it takes the near micro-clamp
	// too, which the unmelted depth alone never asked for.
	float exportEffDepth = lerp(pixelEffDepth, min(pixelEffDepth, kFireMeltFloor), pixelMelt);
#ifdef SNOW_SHELL_DEPTH_PREPASS
	PS_PREPASS_OUTPUT prepassOut;
	prepassOut.RasterDepth = input.Position.z;
#	ifdef SNOW_PREPASS_ATTR
	prepassOut.AttrGrid = float4(input.GridLocal, normalize(input.TerrainNormalAlpha.xyz).xy);
	prepassOut.AttrGridDeriv = attrGridDeriv;
	prepassOut.AttrDepth = float4(input.CurrentClip.w, attrZDeriv, pixelRampDepth);
#	endif
	prepassOut.DepthLE = ShellExportDepthAt(input.Position.z, rawSceneDepth, shellZ, sceneZ, exportEffDepth, pixelCarve, input.WorldPos, input.Position.xy);
#	ifdef SNOW_PREPASS_EARLY
	// Pending: the land contest to run, or a clamp a static behind would
	// cancel. Marked by a negative raster depth; the claim fill skips it.
	const bool clampPending = prepassOut.DepthLE < input.Position.z;
	prepassOut.Pending = 0.0;
	[branch] if (clampPending || landContest)
	{
		prepassOut.RasterDepth = -input.Position.z;
		prepassOut.Pending = float4(0.0, landSide, 0.0, (clampPending ? 2.0 : 0.0) + (landContest ? 8.0 : 0.0));
	}
#	endif
	return prepassOut;
#endif
#ifndef SNOW_SHELL_DEPTH_PREPASS

	// Normal = smooth interpolated terrain normal + per-pixel gradient of
	// the shared carve profile (central differences at the deformation
	// map's resolution), so trench walls AND the edge berm shade by the
	// same shape the geometry displaces.
	float3 terrainNormal = normalize(input.TerrainNormalAlpha.xyz);
	// Shading depth mirrors ShellSurfaceZ's uncarved depth: the touch-down
	// toe, then melt thins toward kFireMeltFloor. Every relief gradient below
	// scales by it, so a print in a melt basin shades as flat as it is built.
	float pixelDepth = max(TouchDownToe(pixelRampDepth), 0.0);
	pixelDepth = lerp(pixelDepth, min(pixelDepth, kFireMeltFloor), pixelMelt);
	// Untouched: every profile tap is pixelDepth and every berm tap zero, so
	// both gradients and the centre are +0.
	float2 profileGrad = 0.0;
	float2 bermGrad = 0.0;
	float bermCenter = 0.0;
	[branch] if (!pixelUntouched)
	{
		const float step = 4.0;
		float dXP = SampleDeformation(gridLocal + float2(step, 0.0));
		float dXN = SampleDeformation(gridLocal - float2(step, 0.0));
		float dYP = SampleDeformation(gridLocal + float2(0.0, step));
		float dYN = SampleDeformation(gridLocal - float2(0.0, step));
		profileGrad = float2(
			CarveProfile(saturate(dXP), pixelDepth, GridOrigin + gridLocal + float2(step, 0.0)) - CarveProfile(saturate(dXN), pixelDepth, GridOrigin + gridLocal - float2(step, 0.0)),
			CarveProfile(saturate(dYP), pixelDepth, GridOrigin + gridLocal + float2(0.0, step)) - CarveProfile(saturate(dYN), pixelDepth, GridOrigin + gridLocal - float2(0.0, step))) / (2.0 * step);
		// Berm shading: numerical gradient of the SAME blurred hill the
		// geometry displaces by, so the light/shadow break sits on the hill's
		// true flanks (the analytic shortcut put the terminator on the crest).
		// A steeper berm can shrink its step below the profile's; the mask then
		// needs its own taps at the same points.
		const float bStep = BermGradStep();
		float mXP = dXP, mXN = dXN, mYP = dYP, mYN = dYN;
		[branch] if (bStep < step - 0.01)
		{
			mXP = SampleDeformation(gridLocal + float2(bStep, 0.0));
			mXN = SampleDeformation(gridLocal - float2(bStep, 0.0));
			mYP = SampleDeformation(gridLocal + float2(0.0, bStep));
			mYN = SampleDeformation(gridLocal - float2(0.0, bStep));
		}
		float bermXP = BermField(gridLocal + float2(bStep, 0.0));
		float bermXN = BermField(gridLocal - float2(bStep, 0.0));
		float bermYP = BermField(gridLocal + float2(0.0, bStep));
		float bermYN = BermField(gridLocal - float2(0.0, bStep));
		// Differenced WITH the (1 - deformation) mask, so the shading gradient is
		// of the exact surface the vertex path displaces - an unmasked gradient
		// here shades a hill the geometry no longer has.
		bermGrad = float2(
			BermShape(bermXP) * saturate(1.0 - mXP) - BermShape(bermXN) * saturate(1.0 - mXN),
			BermShape(bermYP) * saturate(1.0 - mYP) - BermShape(bermYN) * saturate(1.0 - mYN)) / (2.0 * bStep);
		bermCenter = 0.25 * (bermXP + bermXN + bermYP + bermYN);
	}
	// Meeting a captured object: the sheet's MACRO slope goes halfway to the
	// object's (ShellObjectMeet); the coat comes the other half
	// (SnowStaticsShell.hlsl). Relief and grain stay the sheet's own.
	float2 macroGrad = -terrainNormal.xy / max(terrainNormal.z, 0.1);
	macroGrad = lerp(macroGrad, objectMeet.yz, 0.5 * objectMeet.x);
	float2 gradZ = macroGrad + profileGrad + bermGrad * pixelDepth * BermHeightAmp * BermDepthGate(pixelDepth);

	// Undulation gradient (same field the VS displaced by) shades the dunes.
	float2 worldXYPS = GridOrigin + gridLocal;
	float undScale = saturate(pixelDepth / 8.0);
	[branch] if (undScale > 0.001)
	{
		gradZ += UndulationGradSampled(worldXYPS) * undScale;
	}

	// Churn gradient (same field the geometry displaces by). Untouched, carve
	// and berm are 0 and so are both weights.
	float churnW = 0.0;
	float clodW = 0.0;
	[branch] if (!pixelUntouched)
	{
		churnW = ChurnWeight(pixelCarve, bermCenter, GridOrigin + gridLocal) * saturate(pixelDepth / 10.0);
		clodW = RimStyle.z > 0.01 ? BermShape(bermCenter) * saturate(1.0 - pixelCarve) * BermDepthGate(pixelDepth) : 0.0;
	}
	// Churn by parallax (ShellFlags bit 4): the geometry leaves the churn and
	// clod heights out, so the ray is marched through them here and the point
	// it meets is shaded. Depth-buffer work stays on the sheet.
	float3 shadePos = input.WorldPos;
	bool gridShifted = false;
	[branch] if ((ShellFlags.x & 16) != 0)
	{
		float fade = 1.0 - smoothstep(1200.0, 2000.0, shellZ);
		float churnAmp = (ChurnHeightAmp > 0.01 ? ChurnHeightAmp * churnW : 0.0) * fade;
		float clodAmp = RimStyle.z * clodW * fade;
		[branch] if (churnAmp + clodAmp > 0.05)
		{
			float3 dir = normalize(input.WorldPos);
			float t = ChurnParallaxT(dir, gradZ, churnAmp, clodAmp, worldXYPS, ChurnSizeScale);
			shadePos = input.WorldPos + dir * t;
			worldXYPS += dir.xy * t;
			gridLocal += dir.xy * t;
			gridShifted = true;
		}
	}
	[branch] if (churnW > 0.001 && ChurnHeightAmp > 0.01)
	{
		const float cStep = 3.0;
		float cXP = ChurnNoise(worldXYPS + float2(cStep, 0.0));
		float cXN = ChurnNoise(worldXYPS - float2(cStep, 0.0));
		float cYP = ChurnNoise(worldXYPS + float2(0.0, cStep));
		float cYN = ChurnNoise(worldXYPS - float2(0.0, cStep));
		gradZ += float2(cXP - cXN, cYP - cYN) / (2.0 * cStep) * ChurnHeightAmp * churnW;
	}

	// P6 clod gradient (same field the VS displaces by; centre-weighted
	// like the churn gradient - the clod frequency is far above the berm
	// field's, so the noise gradient dominates).
	[branch] if (clodW > 0.001)
	{
		const float kStep = 4.0;
		float kXP = ChurnNoiseScaled(worldXYPS + float2(kStep, 0.0), kClodSizeScale);
		float kXN = ChurnNoiseScaled(worldXYPS - float2(kStep, 0.0), kClodSizeScale);
		float kYP = ChurnNoiseScaled(worldXYPS + float2(0.0, kStep), kClodSizeScale);
		float kYN = ChurnNoiseScaled(worldXYPS - float2(0.0, kStep), kClodSizeScale);
		gradZ += float2(kXP - kXN, kYP - kYN) / (2.0 * kStep) * RimStyle.z * clodW;
	}

	float3 normalWS = normalize(float3(gradZ * -1.0, 1.0));

	// Snow texture taps, shared by albedo, normal and RMAOS so every map
	// agrees on the same anti-tiling offsets. Micro-relief fades with
	// distance, where the grain frequency aliases instead of detailing.
	float bumpFade = 1.0 - smoothstep(600.0, 2200.0, shellZ);
	// The distance fade WITHOUT the crust flattening applied. The flattening
	// exists to take the powder grain away; the frost crystal replacing it
	// must not be taken away by the same term.
	const float bumpFadeRaw = bumpFade;

	// Crust, sampled once and used three times. Flattening the normal map is
	// the strongest of the three by a distance: powder reads as grain, and ice
	// reads as a SHEET, so smoothing the surface says "frozen over" far louder
	// than any change to reflectance or colour can. Roughness and specular
	// alone were nearly invisible without it.
	// One quad fetch for both: the scorch read further down wants the same
	// four texels of the same map at the same place, and both sites are
	// unconditional, so sharing costs nothing and skips four loads.
	float scorchRaw = 0.0, crustRaw = 0.0;
	float crustAmount = 0.0;
	[branch] if (!pixelUntouched)
	{
		SampleScorchCrust(gridLocal, scorchRaw, crustRaw);
		crustAmount = saturate(crustRaw * SpellShading.y);
		bumpFade *= lerp(1.0, 1.0 - saturate(SpellShading.w), crustAmount);
	}
	float2 snowUV = (SnowUVOffset + gridLocal) / kSnowUVTile;
	SnowTaps snowTaps = ComputeSnowTapsNoGrad(snowUV, worldXYPS);
	snowTaps.duvdx = gDeferredGridDx / kSnowUVTile;
	snowTaps.duvdy = gDeferredGridDy / kSnowUVTile;
	float snowHeightMip = SnowHeightMipGrad(snowTaps.duvdx, snowTaps.duvdy);
	// Two-plane projection: top-down uv smears down near-vertical trench walls,
	// so steep pixels blend in a side-plane sample. Keyed on the per-pixel
	// trench normal, captured BEFORE the normal map perturbs normalWS - the
	// side POM march must resolve the view into the same plane these uvs use.
	// Earlier and narrower than the statics ramp: landscape walls live at 40-65
	// degrees, where any top-projection share boils as the camera moves. Side
	// takes over fully by ~57 degrees.
	float snowSteepness = smoothstep(0.75, 0.55, abs(normalWS.z));
	float snowWorldZAbs = shadePos.z + ShellCameraPosAdjust.z;
	bool snowSideDropsX = abs(normalWS.x) > abs(normalWS.y);
	float2 snowSidePlane = snowSideDropsX ? float2(worldXYPS.y, snowWorldZAbs) : float2(worldXYPS.x, snowWorldZAbs);
	// NO SnowUVOffset on the side plane, and a STATIC 4096-unit fold (exactly
	// 24 tiles). SnowUVOffset compensates gridLocal's rebase, but the side
	// plane is absolute and never rebases, so adding it slides the wall texture
	// every time the grid advances. The fold keeps the uv small for float
	// precision.
	float2 snowUVSide = (snowSidePlane - 4096.0 * floor(snowSidePlane / 4096.0)) / kSnowUVTile;
	SnowTaps snowTapsSide = ComputeSnowTapsNoGrad(snowUVSide, snowSidePlane);
	snowTapsSide.duvdx = (snowSideDropsX ? float2(gDeferredGridDx.y, gDeferredZDx) : float2(gDeferredGridDx.x, gDeferredZDx)) / kSnowUVTile;
	snowTapsSide.duvdy = (snowSideDropsX ? float2(gDeferredGridDy.y, gDeferredZDy) : float2(gDeferredGridDy.x, gDeferredZDy)) / kSnowUVTile;
	float snowHeightMipSide = SnowHeightMipGrad(snowTapsSide.duvdx, snowTapsSide.duvdy);
	// Tangent basis for the snow maps. The snow uv is a world-XY planar
	// projection, so the frame is axis-aligned by construction: bumpT is
	// world +X (uv.x), bumpB world +Y (uv.y). Built from the geometric normal
	// BEFORE the normal map perturbs it, matching Lighting.hlsl's use of the
	// interpolated TBN. Shared with the parallax shadow below.
	float3 bumpT = normalize(cross(float3(0.0, 1.0, 0.0), normalWS) + float3(1e-5, 0.0, 0.0));
	float3 bumpB = cross(normalWS, bumpT);

	float3 V = -normalize(input.WorldPos);

	// Pre-parallax derivatives for the glint grid, mirroring Lighting.hlsl's
	// uvOriginal: the POM offset below is view-dependent, and glints must not
	// ride it. (The glint uv itself is rebuilt world-anchored at the call.)
	// Steep pixels take the side plane, like the albedo: the top plane's
	// footprint on a wall is a needle, and the glint NDF then resolves single
	// microfacets per pixel.
	const bool glintSide = snowSteepness > 0.5;
	const float2 glintDuvdx = glintSide ? snowTapsSide.duvdx : snowTaps.duvdx;
	const float2 glintDuvdy = glintSide ? snowTapsSide.duvdy : snowTaps.duvdy;

	// Parallax occlusion: the depth the shell was missing. The normal map
	// only tilts the lighting; this moves the texture itself, so grain
	// occludes grain and the surface reads as thick. Runs BEFORE every snow
	// fetch, and shifts the tap set rather than rebuilding it, so albedo,
	// normal, RMAOS and the parallax shadow all ride the displaced position.
	[branch] if (HasSnowHeight > 0.5 && SnowParallax.z > 0.001 && bumpFade > 0.001)
	{
		// bumpT/bumpB ARE the uv axes (world-XY planar projection); EM's
		// marcher builds tangent-space view from this frame itself.
		float3x3 snowTbn = float3x3(bumpT, bumpB, normalWS);
		DisplacementParams pomParams = SnowDisplacementParams();
		pomParams.HeightScale *= SnowParallax.z;
		float2 pomOffset = SnowParallaxOffset(snowTaps, snowUV, V, snowTbn, shellZ, snowHeightMip, screenNoise, pomParams);
		// Trench walls march NOTHING: a march is only honest when the surface
		// lies in its projection plane, and a 40-60 degree wall lies in neither,
		// so both marches redraw the wall as the camera moves. A steepness scale
		// still leaves most of the top march live there. Hard tilt cut instead,
		// dead by ~40 degrees, side plane sampled unmarched - at a 21-unit wall
		// stability beats parallax.
		pomOffset *= 1.0 - smoothstep(0.15, 0.35, 1.0 - abs(normalWS.z));
		snowUV += pomOffset;
		snowTaps = OffsetSnowTaps(snowTaps, pomOffset);
	}

	[branch] if (HasSnowNormal > 0.5 && bumpFade > 0.001)
	{
		float3 texN = SampleSnowPlanar(SnowNormalMap, snowTaps, snowTapsSide, snowSteepness).xyz * 2.0 - 1.0;
		texN.z = sqrt(saturate(1.0 - dot(texN.xy, texN.xy)));
		texN.y = -texN.y;  // DDS v grows down; our uv v grows with world +Y
		normalWS = normalize(normalWS + (bumpT * texN.x + bumpB * texN.y) * bumpFade);
	}
	else if (HasSnowTexture != 0 && bumpFade > 0.001)
		normalWS = ShellLuminanceBump(normalWS, worldXYPS, bumpFade);

	// Frost crystal, laid on top of the flattened powder grain rather than
	// instead of it: the flattening is what reads as frozen over, this puts the
	// rime back as its own structure. Fetched once and used three times below
	// (normal, albedo, polish) - they must agree about where a crystal is.
	FrostTaps frost;
	frost.normal = float3(0.0, 0.0, 1.0);
	frost.crystal = 0.0;
	frost.valid = false;
	const float frostAmount = (CrustLook2.w > 0.5) ? crustAmount * saturate(CrustLook2.y) : 0.0;
	[branch] if (frostAmount > 0.001)
	{
		frost = SampleFrostPattern(worldXYPS, CrustLook2.z);
		normalWS = normalize(normalWS +
							 (bumpT * frost.normal.x + bumpB * frost.normal.y) * frostAmount * bumpFadeRaw);
	}

	float3 viewNormal = normalize(mul((float3x3)CameraView, normalWS));

	// Snow material: the modlist's snow diffuse when available, otherwise a
	// bright, slightly blue constant.
	float3 kSnowAlbedo = float3(0.82, 0.84, 0.88);
	[branch] if (HasSnowTexture != 0)
	{
		kSnowAlbedo = SampleSnowPlanar(SnowDiffuse, snowTaps, snowTapsSide, snowSteepness).rgb;
		// The colour space the ground reads this texture in: TRUE_PBR's
		// Color::Diffuse for a PBR set, ColorToLinear for a vanilla one.
		kSnowAlbedo = SnowTextureIsLinear != 0.0 ? (ENABLE_LL ? kSnowAlbedo : Color::LinearToSrgb(kSnowAlbedo)) : Color::ColorToLinear(kSnowAlbedo);
	}
	// Scorch: a shock discharge leaves the snow burnt where it struck. Darkened
	// rather than recoloured, and biased slightly warm, so it reads as fouled
	// snow instead of a grey decal painted over it.
	{
		float scorch = scorchRaw * SpellShading.x;
		[branch] if (scorch > 0.001)
			kSnowAlbedo = lerp(kSnowAlbedo, kSnowAlbedo * float3(0.30, 0.27, 0.26), saturate(scorch));
	}
	// Blood lies on the snow the way the game's decal lies on the ground: its
	// colour over the albedo by its alpha, from a detail tile where one lies
	// and from the blood map elsewhere, so a tile's edge does not show. Drying
	// turns it maroon.
	float bloodFresh = 0.0;
	float4 blood = SampleBlood(input.GridLocal, bloodFresh);
	float4 bloodPaint;
	SampleBloodTiles(worldXYPS, gDeferredGridDx, gDeferredGridDy, blood, bloodFresh, bloodPaint);
	// Under a tile the map's reading is zero, elsewhere the tile's.
	blood += bloodPaint;
	[branch] if (blood.a > 0.002)
	{
		float3 paintRgb = Color::LinearToSrgb(blood.rgb);
		paintRgb = lerp(paintRgb, paintRgb * float3(0.75, 0.55, 0.55), 1.0 - bloodFresh);
		kSnowAlbedo = lerp(kSnowAlbedo, paintRgb, saturate(blood.a * BloodLook.x));
	}
	const float bloodCover = blood.a;
	// A rune's glyph lies on the snow as the game's decal lies on the ground:
	// its colour over the albedo by its alpha, its glow added after lighting.
	RuneGlyph rune = SampleRunes(worldXYPS, gDeferredGridDx, gDeferredGridDy);
	float3 runeEmission = rune.emission;
	[branch] if (rune.colour.a > 0.004)
	{
		kSnowAlbedo = lerp(kSnowAlbedo, rune.colour.rgb, rune.colour.a);
		float3 runeNormal = normalize(rune.tangent * rune.normalTS.x + rune.bitangent * rune.normalTS.y + normalWS * rune.normalTS.z);
		normalWS = normalize(lerp(normalWS, runeNormal, rune.colour.a));
	}
	// Thin-snow print: a boot through a dusting (thin classes) has no wall
	// to light, so it reads by material - pressed wet, darker and smoother.
	// Gone by ~6 units of cover, where relief takes over. Not on melted
	// floors: a walked fire ring went dark as a whole.
	float wetPrint = smoothstep(0.1, 0.5, pixelCarve) * (1.0 - smoothstep(1.5, 6.0, pixelDepth)) * (1.0 - smoothstep(0.1, 0.4, pixelMelt));
	[branch] if (wetPrint > 0.001)
		kSnowAlbedo = lerp(kSnowAlbedo, kSnowAlbedo * float3(0.76, 0.77, 0.80), wetPrint);
	// PBR snow material: GGX microfacet specular with Fresnel and energy-
	// conserving lobes. Light and ambient stay in the frame's units
	// (DirLightColor is already pi-scaled by pipeline convention, so no
	// Lambert 1/pi on diffuse); the indirect specular lobe goes to the
	// Reflectance RT where the composite applies cubemap and ambient
	// specular like any TruePBR surface.
	float kSnowRoughness = 0.6;
	float3 kSnowF0 = float3(0.028, 0.028, 0.028);

	// Crust: refrozen snow shades as ice, not powder. All three cues are on
	// sliders - physically honest values are far too subtle to see.
	// ORDER: only the colour cast belongs here. Roughness and reflectance must
	// be applied AFTER the RMAOS block below, which overwrites both from the
	// snow material whenever a PBR set is installed.
	[branch] if (crustAmount > 0.001)
		kSnowAlbedo = lerp(kSnowAlbedo, kSnowAlbedo * float3(CrustLook.y, CrustLook.z, CrustLook2.x), crustAmount);
	// A whisper of the pattern in the albedo too, so the crystal reads even
	// where nothing is catching a highlight. Kept faint on purpose: snow is
	// already near white, so this can only ever darken the gaps between
	// crystals rather than brighten the crystals themselves.
	[branch] if (frost.valid)
		kSnowAlbedo *= lerp(1.0, lerp(0.94, 1.0, frost.crystal), frostAmount);

	// Per-pixel PBR response from the RMAOS map (TruePBR channel layout:
	// roughness / metallic / AO / specular level), with the landscape
	// config's authored scales.
	float snowRoughness = kSnowRoughness;
	float3 snowF0 = kSnowF0;
	float snowAO = 1.0;
	[branch] if (HasSnowRmaos > 0.5)
	{
		float4 rmaos = SampleSnowPlanar(SnowRmaosMap, snowTaps, snowTapsSide, snowSteepness);
		snowRoughness = clamp(rmaos.x * SnowRoughnessScale, 0.05, 1.0);
		snowAO = rmaos.z;
		snowF0 = rmaos.w * SnowSpecularLevel;
	}


	// Crust polishes whatever the material ended up being, PBR set or not. It
	// has to come after the block above rather than before it, or an installed
	// RMAOS map silently discards both.
	[branch] if (crustAmount > 0.001)
	{
		snowRoughness = lerp(snowRoughness, SpellShading.z, crustAmount);
		snowF0 = lerp(snowF0, CrustLook.xxx, crustAmount);
	}
	// Wet print: same ordering rule as crust, after the RMAOS overwrite.
	snowRoughness = lerp(snowRoughness, snowRoughness * 0.76, wetPrint);
	// Fresh blood is wet and glossy; dried blood is matte like the snow.
	snowRoughness = lerp(snowRoughness, 0.22, saturate(bloodCover * 2.0) * bloodFresh * BloodLook2.y);

	// The crystal has to be the part that shines. A normal map alone tilts
	// facets away from the light and puts the highlight in the gaps between
	// them - matte structure on glossy ground. Modulating polish by the same
	// tap that shaped the normal keeps the highlight on the ice.
	[branch] if (frost.valid)
	{
		snowRoughness = saturate(snowRoughness * lerp(1.0, lerp(1.35, 0.45, frost.crystal), frostAmount));
		snowF0 = snowF0 * lerp(1.0, lerp(0.75, 1.7, frost.crystal), frostAmount);
	}
	// The glyph shines where its normal map's alpha says, as the decal does.
	[branch] if (rune.colour.a > 0.004)
	{
		float runeGloss = rune.colour.a * rune.gloss;
		snowRoughness = lerp(snowRoughness, 0.28, runeGloss);
		snowF0 = lerp(snowF0, float3(0.06, 0.06, 0.06), runeGloss);
	}

	float3 L = SharedData::DirLightDirection.xyz;
	float satNdotL = saturate(dot(normalWS, L));
	float satNdotV = saturate(abs(dot(normalWS, V)) + 1e-5);

	float worldShadow = ShadowSampling::GetWorldShadow(shadePos, ShellCameraPosAdjust.xyz);
	// Distant shadow softening: the far cascade's texels quantize into hard
	// blocky patches on distant snow. The cascades must NOT be faded out;
	// LOD trees cast into them and bare ground keeps their shadows at range;
	// so the crisp path instead WIDENS its PCF ring with distance: same
	// shadows, soft penumbra blobs instead of blocks.
	float farShadowT = smoothstep(6000.0, 15000.0, length(input.WorldPos));
	float sunShadow;
	[branch] if (CrispShadows > 0.5)
	{
		// Full-resolution comparison PCF against the game's raw cascade
		// atlas: the same crisp tree/actor shadows bare ground receives.
		sunShadow = worldShadow * SnowShadow::GetCascadeShadow(shadePos, normalWS, lerp(1.0, 6.0, farShadowT), uint2((uint)BorderStyle.z, (uint)BorderStyle.w));
	}
	else
	{
		// Fallback: the Volumetric Shadows 512px VSM moments copy (blurry).
		float detailedShadow;
		float dynamicShadow = ShadowSampling::GetLightingShadow(shadePos, detailedShadow);
		sunShadow = worldShadow * min(dynamicShadow, detailedShadow);
	}

	// Heightfield self-shadowing: the shell is a heightfield, so march it
	// toward the sun and find the horizon this pixel must clear. Hills,
	// mounds, field raises and the dune undulation all cast soft shadows
	// onto the snow behind them; contact detail the game's cascades cannot
	// hold. Geometric growth in the tap distances gives sharp close shadows
	// and long soft ones at low sun angles.
	float pixelVertexAO = -1.0;
	[branch] if ((ShellFlags.x & 1) != 0 && sunShadow > 0.01 && satNdotL > 0.001 && L.z > 0.01)
	{
		// Redistributed toward the NEAR field. The first tap set the finest
		// boundary the horizon can resolve, so at 28 units every shadow edge
		// was smeared over at least that distance and the softness below had
		// to be wide enough to hide it. Same tap COUNT, same 1000-unit reach.
		static const float kMarchDist[5] = { 12.0, 32.0, 90.0, 300.0, 1000.0 };
		float sunLen2D = max(length(L.xy), 1e-4);
		float sunTan = L.z / sunLen2D;
		float2 stepDir = L.xy / sunLen2D;
		float surfZ = shadePos.z + ShellCameraPosAdjust.z;
		// The receiver REBUILT the way the taps are, so what the reconstruction
		// gets wrong - bilinear terrain against the triangulated mesh, the
		// max-of-four object raster - cancels instead of standing as a false
		// rise over the nearest tap: flat snow shadowed itself in texel-sized
		// blotches at a low sun. One unit of bias absorbs the residue.
		float marchRef = surfZ;
		{
			float3 st0 = SampleTerrain(gridLocal);
			// Same four texels: the vertex AO below takes them from here.
			pixelVertexAO = SampleTerrainVertexAO(gridLocal);
			[branch] if (st0.x > -50000.0)
			{
				float depth0 = max(st0.y, 0.0);
				// pixelMelt is this read at the unshifted gridLocal.
				float melt0 = pixelMelt;
				[branch] if (gridShifted)
					melt0 = saturate(SampleExclusionMask(GridOrigin + gridLocal).y);
				depth0 = lerp(depth0, min(depth0, kFireMeltFloor), melt0);
				// Untouched: CarveProfile of a zero carve is depth0, the berm +0.
				[branch] if (!pixelUntouched)
				{
					float deform0 = SampleDeformationMarch(gridLocal);
					float berm0 = BermBakeActive > 0.5 ? BermFieldBaked(gridLocal) : 0.0;
					depth0 = CarveProfile(deform0, depth0, GridOrigin + gridLocal) +
					         BermShape(berm0) * saturate(1.0 - deform0) * depth0 * BermHeightAmp * BermDepthGate(depth0);
				}
				marchRef = st0.x + depth0 + Undulation(GridOrigin + gridLocal) * saturate(depth0 / 8.0);
			}
		}
		marchRef = max(marchRef, surfZ - 2.0);
		float horizonTan = -10.0;
		[unroll] for (uint marchI = 0; marchI < 5; marchI++)
		{
			float d = kMarchDist[marchI];
			float2 sampleLocal = gridLocal + stepDir * d;
			float3 st = SampleTerrain(sampleLocal);
			float sampleDepth = max(st.y, 0.0);
			// The march must see the CARVED surface (same floor rule as the
			// geometry): without the carve, a wide trench reads as ringed by
			// full-height snow and sits in permanent shadow even facing the
			// sun. Same rule for MELT bowls (fires, workspace clearings):
			// without the melt, every bowl-floor pixel reads as ringed by
			// full-height snow and the whole bowl darkens.
			float2 sampleMask = SampleExclusionMask(GridOrigin + sampleLocal);
			{
				float sampleMelt = saturate(sampleMask.y);
				sampleDepth = lerp(sampleDepth, min(sampleDepth, kFireMeltFloor), sampleMelt);
			}
			[branch] if (!ShellUntouched(sampleLocal))
			{
				float sampleDeform = SampleDeformationMarch(sampleLocal);
				// The berm occluder reads the berm FIELD, as the geometry does.
				// BermShape of a raw deformation value peaks across the trench's
				// sloping wall, which rings every trail with a phantom ridge. One
				// bilinear tap when the bake is live; the unbaked A/B path skips
				// the term rather than paying 17 taps per march step.
				float sampleBerm = BermBakeActive > 0.5 ? BermFieldBaked(sampleLocal) : 0.0;
				sampleDepth = CarveProfile(sampleDeform, sampleDepth, GridOrigin + sampleLocal) +
				              BermShape(sampleBerm) * saturate(1.0 - sampleDeform) * sampleDepth * BermHeightAmp * BermDepthGate(sampleDepth);
			}
			// Live undulation ON PURPOSE: in the march, ALU is free and loads
			// are the bottleneck, so the bake's 4 loads per tap were a
			// regression here (2026-08-30). Geometry and shading keep the bake.
			// No object term: every object under the shell is geometry the
			// cascades already shadow with its real silhouette, and the shell
			// draped over it casts through its own caster.
			float sh = st.x + sampleDepth + Undulation(GridOrigin + sampleLocal) * saturate(sampleDepth / 8.0);
			horizonTan = max(horizonTan, (sh - marchRef - 1.0) / d);
		}
		// Near: a crisp penumbra band. Far: a much wider penumbra plus
		// attenuated strength; the march's per-texel horizon steps stop
		// reading as hard-edged blocks on distant snow.
		// Near softness halved: it existed to hide the tap quantisation the
		// finer first taps now resolve. Far end untouched.
		float soft = lerp(0.03, 0.35, farShadowT);
		// Penumbra CENTRED on the horizon: the old band was the same total
		// width (3*soft) but sat entirely on the LIT side of
		// sunTan == horizonTan, so the shadow always over-reached its
		// geometric edge - worse the lower the sun. Kept identical to the
		// statics march; the two shells must agree across the seam.
		// Fades out toward the horizon: a 4-unit raster and a 32-unit terrain
		// texel cannot resolve contact shadows at a grazing sun, every bump
		// and draped stone became a blotch at sunset, and the cascades own
		// the long shadows there anyway. Same gate on the statics march.
		float lowSun = smoothstep(0.04, 0.12, sunTan);
		sunShadow *= lerp(1.0, lerp(smoothstep(-1.5 * soft, 1.5 * soft, sunTan - horizonTan), 1.0, 0.7 * farShadowT), lowSun);
	}

	// 2: this frame's mask was marched on the snow surface; applied as
	// Lighting.hlsl applies it, without the gates below.
	const bool sssSeesSnow = ScreenSpaceShadowsActive > 1.5;
	[branch] if (sssSeesSnow && !SharedData::InInterior && dot(normalWS, L) >= 0.0)
		sunShadow *= ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, float2(0.0, 0.0), 0.0);

	// A mask that did not see the snow was marched on the ground UNDER the
	// shell, so applying it near paints buried objects through the snow.
	// Cascades own the near field; SSS blends in only beyond them, where it
	// carries the distant LOD tree shadows and the shell hugs the ground the
	// march ran on.
	// A zero handoff leaves sunShadow exactly as it is.
	[branch] if (!sssSeesSnow && ScreenSpaceShadowsActive > 0.5 && SnowShadow::GetSssHandoff(shellZ) > 0.0)
	{
		// Depth agreement is the gate: trust the mask only where the shell hugs
		// the surface the march saw. Measured VERTICALLY, not along the view
		// ray - the along-ray gap is depth / sin(elevation) and explodes at far
		// grazing views, which forces a distance override that then paints
		// buried shadows through distant drifts. One vertical rule covers every
		// range. The along-ray term survives as a backstop for steep faces seen
		// edge-on. Thresholds cannot separate grass shadows from buried prints;
		// their gaps overlap, so the caster discriminator below does it.
		float sssRayGap = sceneZ - shellZ;
		float sssVertGap = abs(input.WorldPos.z) * sssRayGap / max(shellZ, 1e-3);
		float sssBlend = (1.0 - smoothstep(8.0, 24.0, sssVertGap)) *
		                 (1.0 - smoothstep(150.0, 400.0, sssRayGap));
		// Buried-caster discriminator: three sunward taps of the object-top
		// raster; any captured surface standing above this pixel's shell
		// means the mask's darkness here is that object's buried shadow.
		[branch] if (ObjectLiftCap > 0.0 && sssBlend > 0.001)
		{
			float2 sunXY = L.xy / max(length(L.xy), 1e-4);
			float sssSurfZ = input.WorldPos.z + ShellCameraPosAdjust.z;
			// Taps start at 15 units, not 40: a caster one plank-width away
			// (the Dawnstar stair boards) slipped BETWEEN the pixel and the
			// old first tap, so all three overshot it and its buried shadow
			// printed in the trench dips - where the hug gate passes by
			// design, because a carved floor hugs the ground. Same far reach.
			[unroll] for (uint sssI = 0; sssI < 4; sssI++)
			{
				float2 tapWorldXY = GridOrigin + gridLocal + sunXY * (15.0 + 55.0 * sssI);
				float2 tapDims;
				bool tapValid;
				float2 tapTexel = ObjectMapTexel(tapWorldXY, tapDims, tapValid);
				[flatten] if (tapValid)
				{
					float tapTop = ObjectTopsRaw.Load(int3((int2)tapTexel, 0));
					// A buried caster is by definition LOW - planks and rails
					// poke barely above the snow, so a "stands well above the
					// shell" trigger misses exactly the casters that print.
					// Any captured surface from slightly-buried upward counts;
					// grass is never captured, so it cannot be touched here.
					[flatten] if (tapTop > -50000.0 && tapTop > sssSurfZ - 16.0)
						sssBlend = 0.0;
				}
			}
		}
		// The near field takes no SSS at all: even a 35% contact version
		// still reads as shadows from under the snow. Casters' true shadows
		// come from the cascades near; the mask only earns its keep past
		// 2500, where it is the sole source of LOD tree shadows. Known
		// trade: near-field grass shadows on the shell die with it, since
		// grass casts only via this march. This ramp is the dial.
		// Distance gate pinned to the cascades themselves rather than a
		// fixed band: cascades own everything until
		// their own distance fade, the mask ramps in as its exact complement
		// (GetSssHandoff), so the handoff tracks the user's shadow distance
		// with no gap and no ghost overlap. The vertical hug metric and the
		// caster probe above still guard the buried-print cases.
		sssBlend *= SnowShadow::GetSssHandoff(shellZ);
		float sssMask = ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, float2(0.0, 0.0), 0.0);
		sunShadow *= lerp(1.0, sssMask, sssBlend);
	}

	// Parallax self-shadow on the snow's own grain: EM's
	// GetParallaxSoftShadowMultiplier, four fixed taps along the light in
	// tangent space, no march. Distinct from the heightfield march above, which
	// shadows at terrain scale; this shadows WITHIN one texture repeat. The
	// fetches are ours, but every constant and the occlusion formula are EM's,
	// so the response matches the ground by construction.
	[branch] if (HasSnowHeight > 0.5 && SnowParallax.y > 0.001 && bumpFade > 0.001 &&
		sunShadow > 0.01 && satNdotL > 0.001)
	{
		// Light into the snow uv's own frame. bumpT/bumpB ARE the uv axes, so
		// this is the planar-projection equivalent of mul(DirLightDirection, tbn).
		float2 lightUV = float2(dot(L, bumpT), dot(L, bumpB));
		float2 lightUVSide = snowSideDropsX ? float2(L.y, L.z) : float2(L.x, L.z);
		float occlusion = SnowParallaxOcclusionPlanar(snowTaps, snowTapsSide, snowSteepness,
			lightUV, lightUVSide, snowHeightMip, snowHeightMipSide,
			SnowParallaxQuality(shellZ), screenNoise, SnowDisplacementParams());

		float parallaxShadow = 1.0 - saturate(occlusion * SnowParallax.y);
		// Faded on the same band as the normal map it occludes, and faded out
		// on walls: the march is jittered by screen-anchored noise, which on a
		// wall's high-contrast side-projected grain crawls as the camera moves.
		// Walls take the trench self-shadow march and the cascades instead.
		sunShadow *= lerp(1.0, parallaxShadow, bumpFade * (1.0 - snowSteepness));
	}

	float3 sunLight = SharedData::DirLightColor.xyz * sunShadow;

	// Sun BRDF + indirect lobes through CS's own PBR path (SnowShading.hlsli,
	// ROUTING-ROADMAP M1): glints, energy conservation and every future
	// TruePBR lobe ride the shared code. Outputs are Lighting-internal units;
	// Color::PBRLightingScale is applied at the write tail below.
	// World-anchored glint uv: snowUV's fmod(GridOrigin) fold shifts by whole
	// tiles as the grid advances - a no-op for the periodic texture, but the
	// glint hash is NOT tile-periodic, so it re-rolls the sparkle field every
	// few metres. Fold on a STATIC 4096-unit block (an exact tile multiple).
	const float2 glintUV = glintSide ? (snowSidePlane - 4096.0 * floor(snowSidePlane / 4096.0)) / kSnowUVTile :
	                                   fmod(GridOrigin + gridLocal, 4096.0) / kSnowUVTile;
	// Built once, shared by the sun and every point light (M3). Soaked snow
	// does not sparkle. Glints reach only the direct lobes: the sun's, zero
	// without sunShadow, and the cluster's lights.
	bool glintsLit = sunShadow != 0.0;
	[branch] if (!glintsLit && PointLightsActive > 0.5)
	{
		float glintViewZ;
		float2 glintClusterUV;
		ShellClusterCoords(input.WorldPos, glintViewZ, glintClusterUV);
		glintsLit = SnowLights::ClusterLightCount(glintClusterUV, glintViewZ) > 0;
	}
	const float glintsHere = glintsLit ? EnableGlints * (1.0 - saturate(bloodCover * 3.0)) : 0.0;
	SnowMaterialCtx snowMtl = SnowBuildMaterial(normalWS, kSnowAlbedo, snowRoughness, snowF0, snowAO,
		SnowGlintParams, glintsHere, glintUV, glintDuvdx, glintDuvdy, input.Position.xy);
	SnowSunLighting sunLit = SnowEvaluateSunPBR(snowMtl, normalWS, V, shadePos, ShellCameraPosAdjust.xyz, sunShadow,
		glintUV, glintDuvdx, glintDuvdy);
	float3 specularLobe = sunLit.specularLobe;
	float3 diffuseLobe = sunLit.diffuseLobe;
	float3 directDiffuse = sunLit.directDiffuse;
	float3 directSpecular = sunLit.directSpecular;

	// Ice reads at GRAZING angles, where a sheet catches the sky and powder
	// does not. Snow is already near-white, so a specular lobe has almost no
	// headroom left above it and the honest BRDF response is swallowed - this
	// adds the one thing white snow cannot already be doing.
	[branch] if (crustAmount > 0.001)
	{
		float grazing = pow(1.0 - satNdotV, 4.0);
		directSpecular += grazing * crustAmount * CrustLook.w * sunLight;
	}

	// Placed lights (fires, lanterns): the clustered LLF list, with each
	// shadow-casting light's own map sampled at the shell surface.
	[branch] if (PointLightsActive > 0.5)
	{
		float viewZ;
		float2 clusterUV;
		ShellClusterCoords(input.WorldPos, viewZ, clusterUV);
		SnowLights::AccumulatePointLights(snowMtl, shadePos, shadePos + ShellCameraPosAdjust.xyz,
			normalWS, V, viewZ, clusterUV, glintUV, glintDuvdx, glintDuvdy, float4(-1.0, 0.0, 0.0, 0.0), directDiffuse, directSpecular);
	}

	// No AO here: the routed GetIndirectLobeWeights already folds snowAO into
	// diffuseLobe via MultiBounceAO (PBR.hlsli:276), exactly as ground does.
	// Multiplying again was double-counting - it read as darker nights and
	// less-blue days, ambient being where the sky blue lives.
	float3 ambientColor = SnowAmbientColor(normalWS);
	float3 ambientPart = ambientColor * diffuseLobe;
	// The land's real baked vertex AO under this pixel, by ground's recipe
	// (Lighting.hlsl:2633-2636): linearized max component, VertexAOStrength
	// lerp. Skylighting darkens only BEYOND it (the function divides), and
	// the composite's SSGI reads its complement from Masks2.
	[branch] if (pixelVertexAO < 0.0)
		pixelVertexAO = SampleTerrainVertexAO(gridLocal);
	float landVertexAO = Color::ColorToLinear(pixelVertexAO.xxx).x;
	landVertexAO = lerp(1.0, landVertexAO, SharedData::truePBRSettings.VertexAOStrength);
	// Skylighting parity with Lighting.hlsl's deferred tail: the ambient is
	// darkened by the probe volume with the same multi-bounce term terrain
	// uses (ApplySkylighting passes the SCALED albedo).
	[branch] if (SkylightingActive > 0.5)
	{
		sh2 skylightingSH = Skylighting::Sample(shadePos, normalWS);
		float skylightingDiffuse = Skylighting::GetSkylightingDiffuse(skylightingSH, shadePos, normalWS, landVertexAO);
		ambientPart = Color::IrradianceToGamma(Color::IrradianceToLinear(ambientPart) * MultiBounceAO(diffuseLobe * Color::PBRLightingScale, skylightingDiffuse));
	}
	// TruePBR G-buffer units (Lighting.hlsl:2766-2774): diffuse, specular,
	// ambient and the Albedo payload carry PBRLightingScale; the Reflectance
	// lobe does not - the composite assumes exactly this split.
	ambientPart *= Color::PBRLightingScale;
	directDiffuse *= Color::PBRLightingScale;
	directSpecular *= Color::PBRLightingScale;
	diffuseLobe *= Color::PBRLightingScale;
	float3 preLit = ambientPart + directDiffuse + runeEmission * Color::PBRLightingScale;

	// Terrain Blending-style output: alpha rides every .w and the stochastic
	// blend mask goes to NormalGlossiness.w, exactly as Lighting.hlsl's
	// deferred tail encodes it for the temporal resolve.
	float alpha = coverageAlpha;
	float stochasticBlend = (screenNoise * screenNoise) < alpha ? 1.0 : 0.0;

	PS_OUTPUT psout;
	psout.Diffuse = float4(preLit, alpha);
	psout.MotionVectors = float4(motionVector, 0.0, alpha);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(viewNormal), 1.0 - snowRoughness, stochasticBlend);
	// Albedo carries the diffuse lobe (Lighting's PBR tail writes the same),
	// Specular the direct GGX lobe, Reflectance the environment lobe weight.
	psout.Albedo = float4(diffuseLobe, alpha);
	psout.Specular = float4(directSpecular, alpha);
	psout.Reflectance = float4(specularLobe, alpha);
	// Masks.z carries the final ambient luma for the composite. Lighting's
	// masksZ is albedo-multiplied and skylit (directionalAmbientColor *=
	// outputAlbedo, then ApplySkylighting); ambientPart matches that.
	psout.Masks = float4(0.0, 0.0, Color::RGBToYCoCg(ambientPart).x, alpha);
	// Stored as 1 - vertexAO, matching Lighting's convention (the composite
	// divides SSGI's AO by it).
	psout.Masks2 = float4(1.0 - landVertexAO, 0.0, 0.0, alpha);

	return psout;
#endif  // !SNOW_SHELL_DEPTH_PREPASS
}

#ifdef SNOW_SHELL_DEFERRED
// Deferred shading: one full-screen triangle at depth 0, tested LESS against
// the fill's private depth, so it shades exactly the prepass's winners and
// every quad is whole. The prepass's attributes stand in for the interpolants.
Texture2D<float4> DeferredGrid : register(t91);
Texture2D<float4> DeferredGridDeriv : register(t92);
Texture2D<float4> DeferredDepth : register(t93);

PS_OUTPUT main(float4 pos : SV_Position)
{
	const int3 px = int3(pos.xy, 0);
	const float4 grid = DeferredGrid.Load(px);
	const float4 gridDeriv = DeferredGridDeriv.Load(px);
	const float4 depth = DeferredDepth.Load(px);
	gDeferredGridDx = gridDeriv.xy;
	gDeferredGridDy = gridDeriv.zw;
	gDeferredZDx = depth.y;
	gDeferredZDy = depth.z;
	gDeferredRampDepth = depth.w;

	// The raster point back from its view depth: rows 0, 1 and 3 of the
	// jittered view-projection solved for rel (Cramer), the pixel centre
	// mapped through the render viewport.
	const float w = depth.x;
	const float2 ndc = float2(pos.x / TessScreen.y - 1.0, 1.0 - pos.y / TessScreen.z);
	const float3 r0 = CameraViewProj[0].xyz;
	const float3 r1 = CameraViewProj[1].xyz;
	const float3 r3 = CameraViewProj[3].xyz;
	const float3 c0 = cross(r1, r3);
	const float3 c1 = cross(r3, r0);
	const float3 c2 = cross(r0, r1);
	const float3 b = float3(ndc.x * w - CameraViewProj[0].w, ndc.y * w - CameraViewProj[1].w, w - CameraViewProj[3].w);
	const float3 rel = (b.x * c0 + b.y * c1 + b.z * c2) / dot(r0, c0);

	VS_OUTPUT input = (VS_OUTPUT)0;
	input.Position = float4(pos.xy, 0.0, 1.0);
	input.CurrentClip = mul(CameraViewProjUnjittered, float4(rel, 1.0));
	input.PreviousClip = mul(CameraPreviousViewProjUnjittered, float4(rel + ShellCameraPosAdjust.xyz - ShellCameraPreviousPosAdjust.xyz, 1.0));
	input.WorldPos = rel;
	input.GridLocal = grid.xy;
	input.TerrainNormalAlpha = float4(grid.zw, sqrt(saturate(1.0 - dot(grid.zw, grid.zw))), 1.0);
	return ShellShade(input);
}
#endif
#endif  // !SNOW_S1_RESOLVE
#endif

#ifdef COMPUTESHADER
#ifdef SNOW_TERRAIN_CUT_CS
// TerrainWindowWet's builder: one WaterBareTexel per texel, run when the
// terrain or the water window changes rather than in every terrain tap.
RWTexture2D<float4> TerrainWetOut : register(u4);

[numthreads(8, 8, 1)] void TerrainCutCS(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= TerrainDim))
		return;
	float4 texel = TerrainWindow.Load(int3(id.xy, 0));
	texel.xyz = WaterBareTexel(texel.xyz, int2(id.xy));
	TerrainWetOut[id.xy] = texel;
}
#endif
#endif
