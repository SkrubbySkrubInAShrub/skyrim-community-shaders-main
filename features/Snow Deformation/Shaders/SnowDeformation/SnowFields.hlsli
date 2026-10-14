// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#ifndef __SNOW_FIELDS_DEPENDENCY_HLSL__
#define __SNOW_FIELDS_DEPENDENCY_HLSL__

// Trench-detail shaping, spell-mark readers and shared field surfaces,
// verbatim-identical in SnowShell.hlsl and SnowStaticsShell.hlsl
// (ROUTING-ROADMAP M8), so landscape and
// object snow cannot drift apart. Relies on the including shell's ShellCB
// (GridToDeformOffset, DeformInvWorldSize, ExclusionFieldWindow,
// UndulationAmp/Scale, UndulationFieldWindow, TrampleStyle,
// GridOrigin, DeformMapOrigin), DeformationMap
// (t1) with the shell's DeformTexel torus helper, BermFieldMap
// (t14), UntouchedTiles (t57), ExclusionFieldMap (t15), the frost patterns (t16/t17),
// UndulationFieldMap (t29), SnowSampler
// (PS) and the TerrainVariation include - all declared before this include.
// Deliberately NOT shared: the deformation .x samplers (the landscape
// smooths bicubic, statics stays bilinear by cost) and BermFieldTapped,
// which rides each shell's own sampler.

// ShapeNoiseHash / ShapeNoise / UndulationNorm live in SnowNoise.hlsli so
// the undulation bake CS shares the exact functions without the ShellCB
// context. Callers here are unchanged.
#include "SnowDeformation/SnowNoise.hlsli"

// The rim teeth's value noise and the churn's octave sum. Pixel passes that
// bind the hash table (t109) and the churn tile (t111, DrawShell) read each in
// one fetch; every other stage evaluates the same torus noise from the hash.
#if defined(SNOW_NOISE_TABLE)
Texture2D<float> SnowNoiseHash : register(t109);
Texture2D<float> SnowChurnTile : register(t111);

float SnowDetailNoise(float2 p)
{
	float2 i = floor(p);
	float2 f = frac(p);
	return SnowNoiseHash.SampleLevel(SnowSampler, (i + f * f * (3.0 - 2.0 * f) + 0.5) / kNoiseTorusCells, 0.0);
}

// The octave sum in [0, 1] at a churn tile uv (ChurnTileUVs).
float ChurnNoiseUV(float2 uv)
{
	return SnowChurnTile.SampleLevel(SnowSampler, uv, 0.0);
}
#else
float SnowDetailNoise(float2 p)
{
	return ShapeNoiseTorus(p, kNoiseTorusCells);
}

float ChurnNoiseUV(float2 uv)
{
	return ChurnNoiseTorus(uv * kChurnTileTexels - 0.5);
}
#endif

// Churn tile uv per world unit for a size knob: q = worldXY / sizeScale, one
// texel per q-unit, texel centres on whole q.
float ChurnTileScale(float sizeScale)
{
	return 1.0 / (kChurnTileTexels * max(sizeScale, 0.05));
}

// Tiles the lattice input wraps after: the stochastic hash multiplies the
// cell index by ~3300 and needs it small to keep a fraction (the frost
// lattice below, same reason).
static const float kChurnLatticeWrap = 32.0;

// Field map sizes. The landscape shell (SNOW_FIELD_DIMS) takes them from
// ShellCB.FieldDims, GetDimensions of what DrawShell binds (0 where unbound);
// elsewhere the macros expand to the query itself, token for token.
#ifdef SNOW_FIELD_DIMS
#	define SNOW_DEFORM_DIMS(d) float2 d = FieldDims.xx
#	define SNOW_BERM_DIMS(d) float2 d = FieldDims.yy
#	define SNOW_EXCLUSION_DIMS(d) float2 d = FieldDims.ww
#else
#	define SNOW_DEFORM_DIMS(d) 		float2 d;               		DeformationMap.GetDimensions(d.x, d.y)
#	define SNOW_BERM_DIMS(d) 		float2 d;             		BermFieldMap.GetDimensions(d.x, d.y)
#	define SNOW_EXCLUSION_DIMS(d) 		float2 d;                  		ExclusionFieldMap.GetDimensions(d.x, d.y)
#endif

// Two-octave domain warp for a boundary, a_noise world units strong: a capped
// 37-unit coarse wander plus a fine 8-unit octave for raggedness. Moves WHERE
// a border falls without touching what is sampled there, so it stops following
// the data lattice and reads as an organic edge instead of a staircase. The
// trench patch's silhouette.
float2 BorderJitter(float2 worldXY, float a_noise)
{
	float coarseAmp = min(a_noise, 16.0);
	return float2(
			   ShapeNoise(worldXY / 37.0) - 0.5,
			   ShapeNoise(worldXY / 37.0 + 111.7) - 0.5) *
	           (2.0 * coarseAmp) +
	       float2(
			   ShapeNoise(worldXY / 8.0) - 0.5,
			   ShapeNoise(worldXY / 8.0 + 57.3) - 0.5) *
	           (1.2 * a_noise);
}

// Saturates EARLY (full height once the disc is a third carved), and the old
// high-field cut is gone - both for the same reported reason. The cut's job,
// keeping berms out of carved interiors, is now done exactly by the explicit
// (1 - deformation) mask at every call site, which also lets the strip
// between two adjacent trails pile a proper ridge (the cut used to kill it).
// And a rise that kept climbing to 0.6 meant a rim point's berm grew for as
// long as the trail kept extending within the 40-unit disc, so ground already
// passed appeared to morph upward, which snow does not do. With
// the early plateau, the berm is at full height by the time the trail REACHES
// a point, and the walker meets a full lip ahead of the leading edge instead
// of raising one behind. Tail still reaches zero with zero slope.
float BermShape(float bermDeform)
{
	return smoothstep(0.02, 0.32, bermDeform);
}

// 17 taps on two staggered 8-point rings. Tap COUNT is the anti-seam: for
// a straight trail edge each tap's projection crosses zero at a different
// distance, so the field climbs in 1/17 steps instead of the 2/9 ledge a
// sparse ring printed as a visible contour line ~34 units out. Bilinear
// taps: the 17-way average supplies the smoothness bicubic would.
static const float2 kBermTaps[16] = {
	float2(18.0, 0.0), float2(12.73, 12.73), float2(0.0, 18.0), float2(-12.73, 12.73),
	float2(-18.0, 0.0), float2(-12.73, -12.73), float2(0.0, -18.0), float2(12.73, -12.73),
	float2(36.96, 15.31), float2(15.31, 36.96), float2(-15.31, 36.96), float2(-36.96, 15.31),
	float2(-36.96, -15.31), float2(-15.31, -36.96), float2(15.31, -36.96), float2(36.96, -15.31)
};

// Berm Steepness scales the rings by BermReach. The shading step follows it,
// floored at half a map texel (below that the bilinear bake shades in
// facets); exactly 4 at the default reach.
float BermGradStep()
{
	SNOW_DEFORM_DIMS(dims);
	return min(4.0, max(4.0 * BermReach, 0.5 / (DeformInvWorldSize * dims.x)));
}

// The carve's short blur (BermFieldCS .y): the centre and a ring of eight
// this far out, world units. CarveProfile lays walls back toward it by the
// slough (CompactLook.w). Mirror in BermFieldCS.hlsl.
static const float kWallBlurReach = 12.0;

// One bilinear tap of the baked field, addressed exactly like the deformation
// map it was baked from (BermFieldCS writes texel-for-texel, same toroidal
// layout and MapOrigin - so partial rebuilds survive scrolls). x = berm,
// y = the carve's short blur.
float2 BermFieldBaked(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	SNOW_BERM_DIMS(dims);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims - 1.001);
#ifdef SNOW_DEFORM_SAMPLER
	return BermFieldMap.SampleLevel(SnowSampler, (t + 0.5 + float2(DeformMapOrigin)) / dims, 0.0);
#else
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float2 s00 = BermFieldMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims)));
	float2 s10 = BermFieldMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims)));
	float2 s01 = BermFieldMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims)));
	float2 s11 = BermFieldMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims)));

	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
#endif
}

// The sun heightfield march's tap distances, both shells.
static const float kSunMarchDist[5] = { 12.0, 32.0, 90.0, 300.0, 1000.0 };

#if defined(PSHADER)
// The sun marches' occluder heights, baked (MarchCache.cpp): per deformation
// texel in the map's own torus, the ground's tap height (MarchHeightCS) and
// the road's where the statics march takes a road tap, sentinel elsewhere
// (RoadMarchHeightCS); past the trail map, the ground's per land-layer
// vertex over the FineWindow (MarchHeightFarCS).
Texture2D<float> MarchHeightGround : register(t106);
Texture2D<float> MarchHeightRoad : register(t107);
Texture2D<float> MarchHeightFar : register(t108);

// A cache's four texels around a grid-local point (s00, s10, s01, s11), one
// gather on s0 (it wraps like DeformTexel); a_f = the bilinear weights.
float4 MarchCacheTexels(Texture2D<float> a_map, float2 a_gridLocal, out float2 a_f)
{
	SNOW_DEFORM_DIMS(dims);
	const float2 t = (GridToDeformOffset + a_gridLocal) * DeformInvWorldSize * dims - 0.5;
	const float2 t0 = floor(t);
	a_f = t - t0;
	return a_map.GatherRed(SnowSampler, (t0 + float2(DeformMapOrigin) + 1.0) / dims).wzxy;
}

float MarchCacheBilinear(float4 a_s, float2 a_f)
{
	return lerp(lerp(a_s.x, a_s.y, a_f.x), lerp(a_s.z, a_s.w, a_f.x), a_f.y);
}

// Every tap of a march from a_gridLocal along a_stepDir, out to a_reach, lies
// at least a texel inside the map's window.
bool MarchCacheCovers(float2 a_gridLocal, float2 a_stepDir, float a_reach)
{
	SNOW_DEFORM_DIMS(dims);
	const float2 margin = 1.5 / dims;
	const float2 uv0 = (GridToDeformOffset + a_gridLocal) * DeformInvWorldSize;
	const float2 uv1 = uv0 + a_stepDir * (a_reach * DeformInvWorldSize);
	return all(min(uv0, uv1) >= margin) && all(max(uv0, uv1) <= 1.0 - margin);
}

// The same for the far cache, whose texel i lies on land-layer vertex i.
bool MarchFarCovers(float2 a_gridLocal, float2 a_stepDir, float a_reach)
{
	const float2 t0 = (FineWindow.xy + a_gridLocal) / FineWindow.w;
	const float2 t1 = t0 + a_stepDir * (a_reach / FineWindow.w);
	return FineWindow.z > 0.5 && all(min(t0, t1) >= 1.0) && all(max(t0, t1) <= FineWindow.z - 2.0);
}

float MarchHeightNear(float2 a_gridLocal)
{
	float2 f;
	return MarchCacheBilinear(MarchCacheTexels(MarchHeightGround, a_gridLocal, f), f);
}

float MarchHeightFarAt(float2 a_gridLocal)
{
	const float2 t = (FineWindow.xy + a_gridLocal) / FineWindow.w;
	const float2 t0 = floor(t);
	return MarchCacheBilinear(MarchHeightFar.GatherRed(SnowSampler, (t0 + 1.0) / FineWindow.z).wzxy, t - t0);
}

// The receiver's height and the five taps', all from one cache, so the six
// gathers issue together.
void MarchCachedHeights(float2 a_gridLocal, float2 a_stepDir, bool a_near, out float a_h[6])
{
	[branch] if (a_near)
	{
		a_h[0] = MarchHeightNear(a_gridLocal);
		[unroll] for (uint i = 0; i < 5; i++)
			a_h[i + 1] = MarchHeightNear(a_gridLocal + a_stepDir * kSunMarchDist[i]);
	}
	else
	{
		a_h[0] = MarchHeightFarAt(a_gridLocal);
		[unroll] for (uint i = 0; i < 5; i++)
			a_h[i + 1] = MarchHeightFarAt(a_gridLocal + a_stepDir * kSunMarchDist[i]);
	}
}
#endif

// Every map and berm read within 8 units of here returns exactly zero, so the
// carve, berm and surface-state terms equal their zero-input results. Just
// outside the window a tap can still reach in, so only far outside skips.
bool ShellUntouched(float2 gridLocal)
{
#ifdef SNOW_UNTOUCHED_SKIP_OFF
	return false;
#else
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	float2 pad = 8.0 * DeformInvWorldSize;
	bool farOutside = any(uv < -pad) || any(uv > 1.0 + pad);
	SNOW_DEFORM_DIMS(dims);
	int2 logical = clamp(int2(uv * dims), 0, int2(dims) - 1);
	bool zeroTile = UntouchedTiles.Load(int3(((logical + DeformMapOrigin) & (int2(dims) - 1)) >> 3, 0)) != 0;
	bool inside = all(uv >= 0.0) && all(uv <= 1.0);
	return BermBakeActive > 0.5 && (farOutside || (inside && zeroTile));
#endif
}

// P6 clod cells: ~2.8x the default churn rubble (24/10-unit cells at
// scale 1.5), so berm clods and trench rubble read at visibly different
// frequencies - the two-scale contrast of RDR2's O3.
static const float kClodSizeScale = 1.5;

// The churn tile's uvs at a world point, xy for the churn at its size knob
// (ChurnSizeScale / ObjChurnSizeScale, the only way the two shells differ
// here), zw for the clods. Both move by the stochastic offset of the churn's
// tile cell there (CS's sampler, one lattice cell per tile): one offset per
// cell keeps every read one fetch, and churn and clods cut together at the
// cell borders.
float4 ChurnTileUVs(float2 worldXY, float churnSize)
{
	float4 uv = worldXY.xyxy * float4(ChurnTileScale(churnSize).xx, ChurnTileScale(kClodSizeScale).xx);
	uv -= kChurnLatticeWrap * floor(uv / kChurnLatticeWrap);
	return uv + ComputeStochasticOffsetsMesh(uv.xy).offset1.xyxy + 0.5 / kChurnTileTexels;
}

// Churn and clod lump noise in [-1, 1] at a world point.
float ChurnNoiseScaled(float2 worldXY, float churnSize)
{
	return ChurnNoiseUV(ChurnTileUVs(worldXY, churnSize).xy) * 2.0 - 1.0;
}

float ClodNoise(float2 worldXY, float churnSize)
{
	return ChurnNoiseUV(ChurnTileUVs(worldXY, churnSize).zw) * 2.0 - 1.0;
}

// Central difference of the lump noise around a churn tile uv (ChurnTileUVs;
// a_scale its ChurnTileScale), a_step world units either side. The taps share
// the centre's cell offset.
float2 ChurnNoiseGradUV(float2 uv, float a_scale, float a_step)
{
	const float d = a_step * a_scale;
	return float2(ChurnNoiseUV(uv + float2(d, 0.0)) - ChurnNoiseUV(uv - float2(d, 0.0)),
			   ChurnNoiseUV(uv + float2(0.0, d)) - ChurnNoiseUV(uv - float2(0.0, d))) /
	       a_step;
}

// Churn plus clod heights at a_t along a ray whose churn tile uvs are
// a_uv0 + a_duv * a_t (xy churn, zw clods).
float ChurnHeightsAlong(float4 a_uv0, float4 a_duv, float a_t, float churnAmp, float clodAmp)
{
	float h = churnAmp * (ChurnNoiseUV(a_uv0.xy + a_duv.xy * a_t) * 2.0 - 1.0);
	[branch] if (clodAmp > 0.001)
		h += clodAmp * (ChurnNoiseUV(a_uv0.zw + a_duv.zw * a_t) * 2.0 - 1.0);
	return h;
}

// Distance along the unit view ray (dir) from the sheet point to where it first
// meets the churn and clod heights laid over the pixel's tangent plane (slope
// g), searched from +h to -h around the plane and refined by secant. uv0: the
// churn tile uvs at the sheet point (ChurnTileUVs), a_scale their
// ChurnTileScale (x churn, y clods).
float ChurnParallaxT(float3 dir, float2 g, float churnAmp, float clodAmp, float4 uv0, float2 a_scale)
{
	float rise = dir.z - dot(dir.xy, g);
	float hMax = churnAmp + clodAmp;
	// Single exit: a return inside [branch] trips X4000.
	float t = 0.0;
	[branch] if (rise <= -0.02)
	{
		float span = -2.0 * hMax / rise;
		float steps = clamp(ceil(span / 2.5), 4.0, 24.0);
		float dt = span / steps;
		const float4 duv = dir.xyxy * a_scale.xxyy;
		float prevT = hMax / rise;
		float prevGap = hMax - ChurnHeightsAlong(uv0, duv, prevT, churnAmp, clodAmp);
		t = prevT;
		[loop] for (float i = 1.0; i <= steps; i += 1.0)
		{
			float ti = hMax / rise + dt * i;
			float gap = rise * ti - ChurnHeightsAlong(uv0, duv, ti, churnAmp, clodAmp);
			[branch] if (gap <= 0.0)
			{
				t = lerp(prevT, ti, prevGap / max(prevGap - gap, 1e-6));
				break;
			}
			prevT = ti;
			prevGap = gap;
			t = ti;
		}
	}
	return t;
}

// ---- Deformation surface-state readers (spell marks). One bilinear tap;
// channel meanings in DeformationUpdateCS.hlsl. ----

// Melted depth at a point: the positive half of the surface-state channel.
// The shadow caster reads it to keep melt pits from casting.
float SampleMelted(float2 gridLocal)
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
	return saturate(v.y);
}

// Scorch at a point: burnt snow left by a shock discharge, the negative half
// of the same channel.
float SampleScorch(float2 gridLocal)
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
	return saturate(-v.y);
}

// Crust at a point: refrozen snow, the map's third channel.
// Scorch and crust read the SAME quad of the SAME texture at the same place -
// one reads the negative part of .y, the other .z - so the shells fetch once
// and split. Same texels, same weights, same order of operations as the two
// helpers below, which are kept for any caller that needs only one.
void SampleScorchCrust(float2 gridLocal, out float scorch, out float crust)
{
	scorch = 0.0;
	crust = 0.0;
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return;

	SNOW_DEFORM_DIMS(dims);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float4 s00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims)));
	float4 s10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims)));
	float4 s01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims)));
	float4 s11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims)));

	// Interpolate the quad ONCE, then split. Both originals interpolate
	// before their saturate, and scorch negates after interpolating - doing
	// it the other way round changes the answer wherever the quad straddles
	// zero, which is exactly the rim of a scorch mark.
	float4 v = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
	scorch = saturate(-v.y);
	crust = saturate(v.z);
}

float SampleCrust(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	SNOW_DEFORM_DIMS(dims);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float c00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims))).z;
	float c10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims))).z;
	float c01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims))).z;
	float c11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims))).z;
	return saturate(lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y));
}

// A fire or shock mark's weight (either half of .y): it churns whatever the
// climate. Magnitudes per tap, so a melt beside a scorch cannot cancel.
// Mirrored by DeformationUpdateCS.hlsl SPELL_MARK_CHURN_START.
static const float kSpellMarkChurnStart = 0.02;
float SampleSpellMark(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	SNOW_DEFORM_DIMS(dims);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float m00 = abs(DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims))).y);
	float m10 = abs(DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims))).y);
	float m01 = abs(DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims))).y);
	float m11 = abs(DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims))).y);
	return smoothstep(kSpellMarkChurnStart, 0.15, lerp(lerp(m00, m10, f.x), lerp(m01, m11, f.x), f.y));
}

// Trampled ground: snow walked deeper than one pass (TrampleStyle.z) is
// packed, and its floor churn scales toward TrampleStyle.y (1 = off).
// Melted depth is not packing.
float TrampleChurnScale(float2 worldXY, float deformation)
{
	float scale = 1.0;
	[branch] if (TrampleStyle.y < 0.999 && deformation > TrampleStyle.z + 0.001)
	{
		const float packed = saturate((deformation - SampleMelted(worldXY - GridOrigin) - TrampleStyle.z) / max(1.0 - TrampleStyle.z, 1e-3));
		scale = lerp(1.0, saturate(TrampleStyle.y), packed);
	}
	return scale;
}

// Trampled ground flattens the churn by how far INTO the trench a point is:
// the floor takes the full scale, the berm outside it none.
float ChurnWeight(float deformation, float bermDeform, float2 worldXY)
{
	float trench = smoothstep(0.05, 0.5, deformation);
	return max(trench, BermShape(bermDeform)) * lerp(1.0, TrampleChurnScale(worldXY, deformation), trench);
}

// ---- Snow Climate per pixel (TRACK-VARIATION-RESEARCH.md 3.3) ----
// The CPU's climate rows lerped by the point's temperature offset from the
// player's (height x lapse, clamped at the outer rows' 2 C). Fixed indices:
// a per-pixel index into the constant buffer serialises the warp's reads.
float4 SnowClimateRow(float worldZ)
{
	const float dT = clamp(SnowClimate.y * (SnowClimate.z - worldZ), -2.0, 2.0);
	return lerp(SnowClimateLUT[1], dT < 0.0 ? SnowClimateLUT[0] : SnowClimateLUT[2], abs(dT) * 0.5);
}

#ifdef PSHADER
// Thin-snow print: a boot through a dusting (thin classes) has no wall to
// light, so it reads by material - pressed wet, darker and smoother. Gone by
// ~6 units of cover, where relief takes over. Not on melted floors: a walked
// fire ring went dark as a whole. Wet snow (Snow Climate) presses darker and
// glossier at any depth; the warmest row says whether any snow in view is
// wet. Under Snow Climate the thin print is the wet look too, so it follows
// the wetness. Darkens a_albedo; returns the roughness cut.
float SnowWetPrint(inout float3 a_albedo, float a_carve, float a_melt, float a_depth, float a_worldZ)
{
	float wetRough = 0.0;
	const float wetPress = smoothstep(0.1, 0.5, a_carve) * (1.0 - smoothstep(0.1, 0.4, a_melt));
	[branch] if (wetPress > 0.001 && (a_depth < 6.0 || SnowClimateLUT[2].w > 0.001))
	{
		const float climateWet = SnowClimateRow(a_worldZ).w;
		const float thinPrint = (1.0 - smoothstep(1.5, 6.0, a_depth)) * saturate(SnowClimate2.w + climateWet);
		const float wetPrint = wetPress * max(thinPrint, 0.8 * climateWet);
		a_albedo = lerp(a_albedo, a_albedo * float3(0.76, 0.77, 0.80) * (1.0 - 0.25 * climateWet), wetPrint);
		wetRough = wetPrint * (0.24 + 0.26 * climateWet);
	}
	return wetRough;
}
#endif

#ifdef SNOW_CONDITION_VIEW
// Debugging Options > Snow Condition. Mirrors SnowDeformation::GetSnowCondition
// (Accumulation.cpp): the player's values shifted by height. xyzw =
// temperature (C), wetness, stickiness, firmness.
float4 SnowConditionAt(float a_worldZ)
{
	const float playerT = SnowClimate.x;
	const float t = playerT - SnowClimate.y * (a_worldZ - SnowClimate.z);
	const float fresh = SnowClimate.w;
	const float wind = SnowClimate2.x;
	const float wet = SnowClimateOverride.x >= 0.0 ? saturate(SnowClimateOverride.x) :
	                                                 saturate(SnowClimate2.y + smoothstep(-1.5, 1.5, t) - smoothstep(-1.5, 1.5, playerT));
	const float freshDensity = t <= 0.0 ? 67.9 + 51.3 * exp(t / 2.6) : 125.0 + 40.0 * min(t, 3.0);
	const float density = clamp(lerp(freshDensity, 290.0, 1.0 - fresh) + 110.0 * wet + 150.0 * wind, 50.0, 560.0);
	const float sticky = SnowClimateOverride.y >= 0.0 ? saturate(SnowClimateOverride.y) :
	                                                    saturate(0.1 + 0.62 * smoothstep(-20.0, -1.5, t) * (0.3 + 0.7 * (1.0 - fresh)) +
																 0.45 * wet * (1.0 - smoothstep(0.75, 1.0, wet)) + 0.25 * wind);
	const float firm = SnowClimateOverride.z >= 0.0 ? saturate(SnowClimateOverride.z) : smoothstep(150.0, 460.0, density);
	return float4(t, wet, sticky, firm);
}

// 1 temperature (blue below freezing, white at it, orange above, a line
// every 2 C), 2 wetness, 3 stickiness, 4 firmness, 5 all three as RGB.
float3 SnowConditionColor(float4 a_condition, float a_view)
{
	const float t = a_condition.x;
	const float band = t / 2.0;
	const float bandWidth = fwidth(band);
	float3 color;
	[branch] if (a_view < 1.5)
	{
		const float3 cold = lerp(float3(1.0, 1.0, 1.0), float3(0.05, 0.25, 1.0), saturate(-t / 20.0));
		const float3 warm = lerp(float3(1.0, 1.0, 1.0), float3(1.0, 0.25, 0.0), saturate(t / 4.0));
		const float isotherm = 1.0 - smoothstep(0.0, 1.5 * bandWidth, abs(frac(band + 0.5) - 0.5));
		color = (t < 0.0 ? cold : warm) * (1.0 - 0.6 * isotherm);
	}
	else if (a_view < 2.5)
		color = lerp(float3(0.95, 0.95, 0.95), float3(0.1, 0.35, 1.0), a_condition.y);
	else if (a_view < 3.5)
		color = lerp(float3(0.95, 0.95, 0.95), float3(1.0, 0.45, 0.05), a_condition.z);
	else if (a_view < 4.5)
		color = lerp(float3(0.95, 0.95, 0.95), float3(0.55, 0.1, 0.85), a_condition.w);
	else
		color = a_condition.yzw;
	return color;
}
#endif

// Blood at a point (BloodMap t30 / BloodClock t31, the deformation map's
// torus): rgb = pigment (linear), a = concentration after burial. Each texel
// is dated by the snowfall clock and the game hour it was deposited; the
// fade is computed per tap and the faded taps are interpolated, so a never-
// written neighbour (clock 0) reads as nothing rather than as ancient blood.
// fresh: 1 just spilled, 0 once BloodLook2.x hours have passed.
float4 SampleBlood(float2 gridLocal, out float fresh)
{
	// No early return inside a [branch]: fxc reads it as an uninitialised
	// out (X4000), the trap CLAUDE.md records.
	fresh = 0.0;
	float4 acc = 0.0;
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	const bool live = BloodLook2.z > 0.5 && all(uv >= 0.0) && all(uv <= 1.0);
	[branch] if (live)
	{
		float2 dims;
		BloodMap.GetDimensions(dims.x, dims.y);
		float2 t = clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
		int2 t0 = (int2)t;
		float2 f = t - t0;
		int2 t1 = min(t0 + 1, int2(dims) - 1);

		const int2 taps[4] = { int2(t0.x, t0.y), int2(t1.x, t0.y), int2(t0.x, t1.y), int2(t1.x, t1.y) };
		const float weights[4] = { (1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y };
		float freshAcc = 0.0;
		[unroll] for (int i = 0; i < 4; i++)
		{
			float4 b = BloodMap.Load(DeformTexel(taps[i], int2(dims)));
			[branch] if (b.a > 0.002)
			{
				float2 clock = BloodClock.Load(DeformTexel(taps[i], int2(dims)));
				float buried = saturate((BloodLook.y - clock.x) / BloodLook.w);
				float age = saturate((BloodLook.z - clock.y) / BloodLook2.x);
				float conc = b.a * (1.0 - buried) * weights[i];
				acc += float4(b.rgb * conc, conc);
				freshAcc += (1.0 - age) * conc;
			}
		}
		[branch] if (acc.a > 1e-4)
		{
			acc.rgb /= acc.a;
			fresh = freshAcc / acc.a;
		}
	}
	return acc;
}

// Wide exclusion field, bilinear: x = door suppression, y = melt. Returns 0
// outside the window (nothing claimed where nothing was baked). The
// landscape unions this with its near object-bottoms mask; the skin reads
// it alone.
float2 SampleExclusionField(float2 worldXY)
{
	float2 result = 0.0;
	[branch] if (ExclusionFieldWindow.w > 0.5)
	{
		float2 local = (worldXY - ExclusionFieldWindow.xy) * ExclusionFieldWindow.z;
		[branch] if (all(abs(local) < 0.995))
		{
			SNOW_EXCLUSION_DIMS(dims);
			float2 uv = local * 0.5 + 0.5;
			float2 t = clamp(uv * dims - 0.5, 0.0, dims - 1.001);
			int2 t0 = (int2)t;
			float2 f = t - t0;
			int2 t1 = min(t0 + 1, int2(dims) - 1);

			float2 s00 = ExclusionFieldMap.Load(int3(t0.x, t0.y, 0));
			float2 s10 = ExclusionFieldMap.Load(int3(t1.x, t0.y, 0));
			float2 s01 = ExclusionFieldMap.Load(int3(t0.x, t1.y, 0));
			float2 s11 = ExclusionFieldMap.Load(int3(t1.x, t1.y, 0));

			result = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
		}
	}
	return result;
}

// ---- Shared field surface pieces: the carve profile, the dune undulation
// and the melt floor, seen identically by geometry, shading and both
// shells' self-shadow marches. ----

// Melted fire basins keep this much snow above the terrain: the floor stays
// shell snow, never bare ground, never below the terrain mesh.
static const float kFireMeltFloor = 1.0;

float Undulation(float2 worldXY)
{
	return UndulationNorm(worldXY, UndulationScale) * UndulationAmp;
}

// ---- Baked undulation (UndulationFieldCS) ----
// Amp-free height (x) and its +-kUndulationGradStep gradient (yz), both
// multiplied by UndulationAmp on read so the Strength slider needs no
// rebake, in a camera-snapped world window: UndulationFieldWindow = (centre XY,
// 1/half-extent, bake live > 0.5). Same manual-bilinear convention as
// BermFieldBaked, so every stage reads it without a sampler. The live
// Undulation() path stays compiled underneath: the fallback for taps
// outside the window (distant statics), and the debug A/B.

float3 UndulationBakedHG(float2 worldXY)
{
	float2 uv = (worldXY - UndulationFieldWindow.xy) * UndulationFieldWindow.z * 0.5 + 0.5;
	float2 dims;
	UndulationFieldMap.GetDimensions(dims.x, dims.y);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	float3 s00 = UndulationFieldMap.Load(int3(t0, 0)).xyz;
	float3 s10 = UndulationFieldMap.Load(int3(t0 + int2(1, 0), 0)).xyz;
	float3 s01 = UndulationFieldMap.Load(int3(t0 + int2(0, 1), 0)).xyz;
	float3 s11 = UndulationFieldMap.Load(int3(t0 + int2(1, 1), 0)).xyz;
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

bool UndulationBakedCovers(float2 worldXY)
{
	return UndulationFieldWindow.w > 0.5 &&
	       max(abs(worldXY.x - UndulationFieldWindow.x), abs(worldXY.y - UndulationFieldWindow.y)) * UndulationFieldWindow.z < 0.999;
}

// Drop-in for Undulation(): one bilinear read instead of two octaves.
// Single-exit like BermField - a return inside [branch] trips X4000.
float UndulationSampled(float2 worldXY)
{
	float result;
	[branch] if (UndulationBakedCovers(worldXY))
		result = UndulationBakedHG(worldXY).x * UndulationAmp;
	else
		result = Undulation(worldXY);
	return result;
}

// Drop-in for the shading blocks' four-tap central difference (kUndulationGradStep,
// the operator the bake stored).
float2 UndulationGradSampled(float2 worldXY)
{
	float2 result;
	[branch] if (UndulationBakedCovers(worldXY))
		result = UndulationBakedHG(worldXY).yz * UndulationAmp;
	else {
		const float uStep = kUndulationGradStep;
		result = float2(
					 Undulation(worldXY + float2(uStep, 0.0)) - Undulation(worldXY - float2(uStep, 0.0)),
					 Undulation(worldXY + float2(0.0, uStep)) - Undulation(worldXY - float2(0.0, uStep))) /
		         (2.0 * uStep);
	}
	return result;
}

// Deformation carves the layer to the ground.
//
// P7 (trench plan Stage 3): DEPTH PICKS THE PROFILE. The map's carve
// gradient is depth-blind, so a bootprint in 5 units of cover wore the same
// normalized cliff walls as a knee-deep trench, miniaturized. The
// smootherstep remap flattens the response at rim and floor - the rim rolls
// over, the floor entry rounds, and a shallow print reads as a soft dimple -
// while deep snow keeps the map-authored cut untouched. Living HERE rather
// than in the stamp falloff (where the plan first pointed) because the map
// update has no depth data and every consumer of the shape - geometry,
// finite-difference normals, both shells, the self-shadow march - already
// routes through this one function.
// P5 (trench plan Stage 3) rides in here too, for the same reason P7 does:
// one function, every consumer agrees. Teeth first, then the depth remap,
// then the lip on the notched value so the teeth break the lip into blocks
// (RDR2 O1: "irregular teeth and broken blocks" at the rim).
// Slough (Snow Climate) comes before all of them: loose snow falls back into
// the print, toward the carve's short blur (deformBlur, the berm field's .y)
// but never past the print, so the hole keeps its outline and its floor
// rounds and narrows.
float CarveSlough(float deformation, float deformBlur)
{
	return saturate(lerp(deformation, min(deformation, deformBlur), CompactLook.w));
}

float CarveProfile(float deformation, float deformBlur, float uncarvedDepth, float2 worldXY)
{
	float depthT = smoothstep(6.0, 18.0, uncarvedDepth);
	float d = CarveSlough(deformation, deformBlur);

	// P5 teeth: the border work's own two-octave recipe (37-unit wander +
	// 8-unit raggedness, HEIGHT-BLEND-PLAN - reused, not reinvented),
	// applied to the carve value inside the rim band only.
	// The contour breaks into teeth; floors (d high) and open snow (d = 0)
	// sit outside the band and never move. Faded by depthT: teeth are
	// cut-wall vocabulary, dimples stay smooth. The churn-free shading
	// draws only when they are off.
#ifndef SNOW_NO_CHURN
	[branch] if (RimStyle.y > 0.001)
	{
		float rimBand = smoothstep(0.02, 0.12, d) * (1.0 - smoothstep(0.30, 0.55, d));
		// Outside the band the notch adds a signed zero.
#	ifndef SNOW_TEETH_UNGATED
		[branch] if (rimBand > 0.0 && depthT > 0.0)
#	endif
		{
			float notch = (SnowDetailNoise(worldXY / 37.0) - 0.5) * 0.8 +
			              (SnowDetailNoise(worldXY / 8.0) - 0.5) * 1.2;
			d = saturate(d + notch * RimStyle.y * 0.35 * rimBand * depthT);
		}
	}
#endif

	float soft = d * d * d * (d * (d * 6.0 - 15.0) + 10.0);
	d = lerp(soft, d, depthT);
	float profile = uncarvedDepth * (1.0 - d);

	// P5 lip: the rim rolls UP before it drops - a small cornice bulge on
	// the carve skirt, peaking at d ~ 0.1 and gone by mid-wall. Berm
	// guardrail (the plan's double-ridge warning): at the default 0.10 the
	// lip is ~1/4 berm height and sits on the berm's INNER flank, so it
	// reads as the rim rolling into the berm rather than a second ridge -
	// RimStyle.x is the dial if it ever stacks. Faded by depthT with the
	// rest of the cut-wall vocabulary.
	[branch] if (RimStyle.x > 0.001)
	{
		float hump = smoothstep(0.02, 0.10, d) * (1.0 - smoothstep(0.10, 0.35, d));
		profile += RimStyle.x * uncarvedDepth * hump * depthT;
	}
	return profile;
}

// CarveProfile without the rim teeth, for the sun march's occluders. The tail
// mirrors CarveProfile's: keep the two in step.
float CarveProfileNoTeeth(float deformation, float deformBlur, float uncarvedDepth)
{
	float depthT = smoothstep(6.0, 18.0, uncarvedDepth);
	float d = CarveSlough(deformation, deformBlur);
	float soft = d * d * d * (d * (d * 6.0 - 15.0) + 10.0);
	d = lerp(soft, d, depthT);
	float profile = uncarvedDepth * (1.0 - d);
	[branch] if (RimStyle.x > 0.001)
	{
		float hump = smoothstep(0.02, 0.10, d) * (1.0 - smoothstep(0.10, 0.35, d));
		profile += RimStyle.x * uncarvedDepth * hump * depthT;
	}
	return profile;
}

// P7's berm half: spoil needs material. Below ~3 units of cover there is
// nothing to throw and the ridge vanishes; full berms only from ~12 up.
// Multiply this in wherever BermShape scales by a local depth - geometry,
// the shading gradient, the shadow-march occluder and the statics ridge
// must all agree or shape, light and shadow drift apart.
float BermDepthGate(float depth)
{
	return smoothstep(3.0, 12.0, depth);
}

#if defined(PSHADER)
// Frost pattern taps, shared by the normal, the albedo and the polish so the
// texture is fetched once and the three always agree about where a crystal
// is.
struct FrostTaps
{
	float3 normal;   // tangent-space, x along +X, y along +Y
	float crystal;   // 0 in the gaps, 1 on the crystal
	bool valid;
};

// The lattice the stochastic sampler scatters over, kept inside float
// precision. ComputeStochasticOffsets multiplies by WORLD_SCALE (332.54) and
// the hash then multiplies by another 1271, both tuned for landscape UVs that
// live in 0-1. World coordinates are five digits, so the product lands past
// 1e8 - far beyond the ~1.6e7 where a float32 still has a fraction to take -
// and frac() returns the same number across whole regions, which is a
// stochastic sampler that has quietly stopped scattering.
//
// So the tile index is wrapped before it ever reaches the hash. The scatter
// pattern then repeats every WRAP tiles, which at any sane crystal size is
// tens of thousands of units away. DeformationUpdateCS guards its own noise
// the same way and for the same reason.
#define FROST_LATTICE_WRAP 512.0

FrostTaps SampleFrostPattern(float2 worldXY, float tileSize)
{
	FrostTaps taps;
	taps.normal = float3(0.0, 0.0, 1.0);
	taps.crystal = 0.0;
	taps.valid = false;

	float2 tileUV = worldXY / max(tileSize, 4.0);
	// Derivatives from the UNWRAPPED coordinate: the wrap below is a cliff one
	// pixel wide, and a mip level chosen across it would band there.
#if defined(SNOW_SHELL_DEFERRED) || defined(SNOW_PATCH_DEFERRED)
	// ComputeTerrainStochasticLodBase's rule on the handed-in derivatives.
	const float2 tileDx = gDeferredGridDx / max(tileSize, 4.0);
	const float2 tileDy = gDeferredGridDy / max(tileSize, 4.0);
	g_terrainStochasticLodBase = 0.5 * log2(max(min(dot(tileDx, tileDx), dot(tileDy, tileDy)), 1e-12)) + SharedData::MipBias;
#else
	g_terrainStochasticLodBase = ComputeTerrainStochasticLodBase(tileUV);
#endif
	float2 wrapped = tileUV - FROST_LATTICE_WRAP * floor(tileUV / FROST_LATTICE_WRAP);

	// Divided back out because the sampler expects a landscape UV and converts
	// it to lattice cells itself; this hands it one cell per texture tile.
	StochasticOffsets offsets = ComputeStochasticOffsets(wrapped / WORLD_SCALE);
	float3 n = StochasticEffect(FrostPatternNormal, SnowSampler, wrapped, offsets).xyz * 2.0 - 1.0;
	n.z = sqrt(saturate(1.0 - dot(n.xy, n.xy)));
	taps.normal = n;
	taps.crystal = saturate(StochasticEffect(FrostPatternDiffuse, SnowSampler, wrapped, offsets).x);
	taps.valid = true;
	return taps;
}
#endif  // PSHADER (frost)

#if defined(PSHADER)
// Land vertex AO, packed by the CPU window build into the terrain window's
// w channel: baked texels carry maxComponent(vertex color) * 0.499, so the
// LOD-fill provenance codes (>= 0.5) stay distinguishable. Texels without
// vertex data (LOD fill, sentinel) read as 1 = no baked occlusion.
float DecodeTerrainVertexAO(float4 texel)
{
	return (texel.x > -50000.0 && texel.w < 0.5) ? saturate(texel.w * (1.0 / 0.499)) : 1.0;
}

float SampleTerrainVertexAO(float2 gridLocal)
{
	float2 t = clamp((GridToTerrainOffset + gridLocal) / TerrainTexelSize, 0.0, (float)(TerrainDim - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDim, TerrainDim) - 1);
	float s00 = DecodeTerrainVertexAO(TerrainWindow.Load(int3(t0.x, t0.y, 0)));
	float s10 = DecodeTerrainVertexAO(TerrainWindow.Load(int3(t1.x, t0.y, 0)));
	float s01 = DecodeTerrainVertexAO(TerrainWindow.Load(int3(t0.x, t1.y, 0)));
	float s11 = DecodeTerrainVertexAO(TerrainWindow.Load(int3(t1.x, t1.y, 0)));
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}
#endif  // PSHADER

// EM's landscape height blending (ExtendedMaterialsTerrain.hlsli::
// ProcessTerrainHeightWeights) specialized to two surfaces: an edge fade
// contests by height instead of cross-fading through translucency. Same
// log2-space formula and near/far sharpness ramp; constants mirror EM's
// HEIGHT_MULT/HEIGHT_POWER. EM-checkbox-INDEPENDENT (the Snow
// Borders dials are the only owners of the snow border; the checkbox
// consult here silently disabled the statics rim/ground shaping and the
// DS skirt descent).
static const float kHeightBlendMult = 8.0;
static const float kHeightBlendPower = 2.0;

float SnowHeightBlendSharpness(float viewDist)
{
	float nearBlendToFar = smoothstep(1024.0 * 1024.0, 2048.0 * 2048.0, viewDist * viewDist);
	float blendFactor = sqrt(saturate(1.0 - nearBlendToFar));
	return 1.0 + blendFactor * kHeightBlendPower;
}

// w' = normalize(pow(w * B^(MULT*h), B)) over {w, 1-w}. Preserves 0 and 1,
// so gates and overrides composed around it keep their meaning. EM's skip
// when the heights carry no signal is kept: without it pow() sharpens the
// fade alone and hardens it into a contour.
float SnowHeightBlend(float w, float hSnow, float hOther, float heightBlend)
{
	if (heightBlend <= 1.0 || abs(hSnow - hOther) <= 1e-3)
		return w;
	float logHeightBlend = log2(heightBlend);
	float wSnow = min(100, exp2(heightBlend * (log2(abs(w)) + kHeightBlendMult * hSnow * logHeightBlend)));
	float wOther = min(100, exp2(heightBlend * (log2(abs(1.0 - w)) + kHeightBlendMult * hOther * logHeightBlend)));
	return wSnow * rcp(max(wSnow + wOther, 1e-6));
}

// One-sided form: the neighbor surface the shell cannot sample. NOT a
// constant reference - grain near any fixed value gets no push, and "grain
// near the reference" is a spatially coherent contour, so a constant prints
// a dithered ribbon at one particular height. Instead the bar sweeps with
// the fade itself (1-w): tall grain survives to the outer edge, low grain
// dies early, and the crossing isoline moves with the ramp - no height is
// special, so partial alpha survives only on a thin moving line.
float SnowHeightBlendOneSided(float w, float hSnow, float heightBlend)
{
	return SnowHeightBlend(w, hSnow, 1.0 - w, heightBlend);
}

#endif  //__SNOW_FIELDS_DEPENDENCY_HLSL__

#if defined(PSHADER) && defined(SNOW_BLOOD_TILES)
// Blood detail tiles (BloodTiles.cpp), landscape shell only. Where a tile
// lies it replaces the blood map's reading: the pigment (premultiplied, own
// mip chain), dated per clock block. The mark comes back as PAINT - the
// decal's colour over the snow by its alpha, as a rune's glyph lies on it.
// The cell and the texel come from BloodLook3.zw (the
// detail level sets them); the rest mirrors SnowDeformation.h.
Texture2D<uint> BloodTileIndex : register(t66);
Texture2D<float4> BloodTilePigment : register(t67);
Texture2D<float4> BloodTileClock : register(t69);

static const int kBloodTileDim = 512;
// A clock block is 8 texels, 4 units.
static const int kBloodTileBlockShift = 3;

// worldDx / worldDy: screen derivatives of worldXY, taken outside any branch.
void SampleBloodTiles(float2 worldXY, float2 worldDx, float2 worldDy, inout float4 blood, inout float fresh, out float4 paint)
{
	paint = 0.0;
	[branch] if (BloodLook2.w > 0.5)
	{
		const float kBloodTileCell = BloodLook3.z;
		const float kBloodTileTexel = BloodLook3.w;
		const float kBloodTileApron = 0.5 * (float(kBloodTileDim) * kBloodTileTexel - kBloodTileCell);
		const uint kBloodTilesAcross = 8u;
		const int2 cell = (int2)floor(worldXY / kBloodTileCell);
		const uint slot1 = BloodTileIndex.Load(int3(cell & 127, 0));
		[branch] if (slot1 != 0u)
		{
			const uint slot = slot1 - 1u;
			const int2 tileTexel = int2(slot % kBloodTilesAcross, slot / kBloodTilesAcross) * kBloodTileDim;
			const float2 local = (worldXY - (float2(cell) * kBloodTileCell - kBloodTileApron)) / kBloodTileTexel;
			const float atlasDim = float(kBloodTileDim) * float(kBloodTilesAcross);
			const float uvPerUnit = 1.0 / (kBloodTileTexel * atlasDim);
			float4 own = BloodTilePigment.SampleGrad(ShellLinearSampler, (float2(tileTexel) + local) / atlasDim, worldDx * uvPerUnit, worldDy * uvPerUnit);

			const int2 texel = clamp((int2)local, 0, kBloodTileDim - 1);
			const int2 blockOrigin = tileTexel >> kBloodTileBlockShift;
			float4 clock = BloodTileClock.Load(int3(blockOrigin + (texel >> kBloodTileBlockShift), 0));

			const float ownKeep = step(1e-4, clock.y) * (1.0 - saturate((BloodLook.y - clock.x) / BloodLook.w));
			const float ownConc = own.a * ownKeep;
			blood = 0.0;
			paint = float4(own.rgb / max(own.a, 1e-3), saturate(ownConc));
			fresh = 1.0 - saturate((BloodLook.z - clock.y) / BloodLook2.x);
		}
	}
}
#endif

#if defined(PSHADER) && defined(SNOW_RUNE_GLYPHS)
// Rune glyphs (Blood.cpp, RenderRuneCapture), for the shells that define
// SNOW_RUNE_GLYPHS - the landscape shell and the road patch; object skins
// get theirs from the decal overlay. A 2x2 atlas, one tile per live
// rune, redrawn every frame from the rune's own ground decal. A tile is the
// decal's uv field (rg = uv, b = covered, a = material alpha); the glyph
// itself comes from the rune's own diffuse and normal map through that uv.
Texture2D<float4> RuneAtlas : register(t90);
Texture2D<float4> RuneDiffuse[4] : register(t80);
Texture2D<float4> RuneNormal[4] : register(t84);
cbuffer RuneCB : register(b7)
{
	// x = live tiles, y = glow
	float4 RuneParams;
	// xy = tile's world min, z = 1 / world size; tile i at (i & 1, i >> 1)
	float4 RuneRects[4];
	// rgb = the decal material's emissive, w = 1 with a normal map
	float4 RuneTints[4];
}

struct RuneGlyph
{
	float4 colour;     // rgb = the decal's diffuse, a = its alpha here
	float3 normalTS;   // its normal map
	float gloss;       // the normal map's alpha, the game's specular mask
	float3 tangent;    // world direction of +u
	float3 bitangent;  // world direction of +v
	float3 emission;
};

// worldDx / worldDy: screen derivatives of worldXY, taken outside any branch.
RuneGlyph SampleRunes(float2 worldXY, float2 worldDx, float2 worldDy)
{
	RuneGlyph g;
	g.colour = 0.0;
	g.normalTS = float3(0.0, 0.0, 1.0);
	g.gloss = 0.0;
	g.tangent = float3(1.0, 0.0, 0.0);
	g.bitangent = float3(0.0, 1.0, 0.0);
	g.emission = 0.0;
	const uint count = min((uint)RuneParams.x, 4u);
	[unroll] for (uint i = 0; i < 4; i++)
	{
		float2 tileUV = (worldXY - RuneRects[i].xy) * RuneRects[i].z;
		[branch] if (i < count && all(tileUV > 0.0) && all(tileUV < 1.0))
		{
			// In from the tile's edge: the neighbour tile is another rune.
			float2 inTile = clamp(tileUV, 1.5 / 512.0, 1.0 - 2.5 / 512.0);
			float2 atlasUV = (float2(i & 1u, i >> 1u) + inTile) * 0.5;
			const float texel = 0.5 / 512.0;
			float4 c = RuneAtlas.SampleLevel(ShellLinearSampler, atlasUV, 0);
			float4 cx = RuneAtlas.SampleLevel(ShellLinearSampler, atlasUV + float2(texel, 0.0), 0);
			float4 cy = RuneAtlas.SampleLevel(ShellLinearSampler, atlasUV + float2(0.0, texel), 0);
			// All three taps inside the decal: a tap straddling its border
			// blends uv toward 0. The decal's own border is transparent.
			[branch] if (min(c.b, min(cx.b, cy.b)) > 0.999)
			{
				// The uv field is planar, so its world derivative is one
				// texel's difference, and the screen gradient follows from it.
				float texelWorld = 1.0 / (RuneRects[i].z * 512.0);
				float2 uvDx = (cx.rg - c.rg) / texelWorld;
				float2 uvDy = (cy.rg - c.rg) / texelWorld;
				float2 gradX = uvDx * worldDx.x + uvDy * worldDx.y;
				float2 gradY = uvDx * worldDy.x + uvDy * worldDy.y;
				float4 d = RuneDiffuse[i].SampleGrad(SnowSampler, c.rg, gradX, gradY);
				float a = d.a * c.a;
				[branch] if (a > g.colour.a)
				{
					g.colour = float4(d.rgb, a);
					// Lighting adds a material's emissive to the LIGHT, before the texture colour multiplies it: a dark rim glows dark.
					g.emission = RuneTints[i].rgb * d.rgb * a * RuneParams.y;
					float det = uvDx.x * uvDy.y - uvDy.x * uvDx.y;
					[branch] if (RuneTints[i].w > 0.5 && abs(det) > 1e-12)
					{
						float4 n = RuneNormal[i].SampleGrad(SnowSampler, c.rg, gradX, gradY);
						g.normalTS = normalize(n.xyz * 2.0 - 1.0);
						g.gloss = n.a;
						// Inverse of [uvDx uvDy]: world xy per unit u, per unit v.
						g.tangent = normalize(float3(uvDy.y, -uvDx.y, 0.0) / det);
						g.bitangent = normalize(float3(-uvDy.x, uvDx.x, 0.0) / det);
					}
				}
			}
		}
	}
	return g;
}
#endif
