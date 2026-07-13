/**
 * @file TerrainVariation.hlsli
 * @brief Stochastic terrain tiling (Deliot & Heitz).
 * @see https://eheitzresearch.wordpress.com/722-2/
 */

#ifndef TERRAIN_VARIATION_HLSLI
#define TERRAIN_VARIATION_HLSLI

#include "Common/SharedData.hlsli"

static const float2x2 SKEW_MATRIX = float2x2(1.0, 0.0, -0.57735027, 1.15470054);
static const float WORLD_SCALE = 332.54;
static const float2 HASH_MULTIPLIER = float2(1271.5151, 3337.8237);
/** @brief Mix of stochastic vs height-driven tap weights in @ref StochasticBlendTwoSamples. */
static const float HEIGHT_INFLUENCE = 0.3;
static const float3 LUMINANCE_WEIGHTS = float3(0.2126, 0.7152, 0.0722);
/** @brief Barycentric contrast for the 2-tap path (milder than classic Heitz ~8). */
static const float STOCHASTIC_WEIGHT_POWER = 2.0;
/** @brief Width (in barycentric weight units) of the fade zone used to hide the 2nd/3rd corner rank-swap seam in @ref ComputeStochasticOffsets. */
static const float TAP_SWAP_FADE_WIDTH = 0.1;
/** @brief Width of the guard band around the c0/c1 tie where the rank-swap fade must stay disabled (see @ref ComputeStochasticOffsets); kept narrow so it only shrinks the (mathematically unavoidable) residual at the hexagon corners without weakening the fade elsewhere on the median. */
static const float DOMINANCE_GUARD_WIDTH = 0.02;
/** @brief Golden ratio used by @ref StochasticSampleLODJitter. */
static const float STOCHASTIC_LOD_PHI = 1.618;

/** @brief Per-pixel stochastic UV offsets and blend weights. */
struct StochasticOffsets
{
	float2 offset1;
	float2 offset2;
	float2 offset3;
	float3 weights;
	float w1Contrast;
	float w2Contrast;
};

/** @brief Shared ddx/ddy for landscape UVs (set once before the six-way blend). */
struct TerrainGradients
{
	float2 gradDx;
	float2 gradDy;
};

static TerrainGradients g_terrainStochasticGrad;
static float g_terrainStochasticSecondSampleFade[6];
static float g_terrainStochasticHeightInfluence[6];
static float g_terrainParallaxSecondSampleFade[6];
static float g_terrainParallaxHeightInfluence[6];

/** @brief Triangle corner (cell id + barycentric weight) for sorting. */
struct StochasticCorner
{
	float2 cell;
	float w;
};

/** @brief 2D hash for near-terrain stochastic offsets. */
inline float2 hash2D2D(float2 s)
{
	s = frac(s * HASH_MULTIPLIER);
	s += dot(s, s.yx + 19.19);
	return frac((s.xx + s.yy) * s.yx);
}

/** @brief 2D hash for LOD terrain stochastic offsets. */
inline float2 hashLOD(float2 p)
{
	p = frac(p * 0.318);
	return frac(float2(dot(p, float2(1.0, 17.0)), dot(p, float2(1.0, 23.0))));
}

/** @brief Raises a barycentric weight to @ref STOCHASTIC_WEIGHT_POWER. */
inline float StochasticContrastWeight(float weight)
{
	return pow(saturate(weight), STOCHASTIC_WEIGHT_POWER);
}

/** @brief Returns a zeroed @ref StochasticOffsets. */
inline StochasticOffsets ZeroStochasticOffsets()
{
	StochasticOffsets o;
	o.offset1 = 0;
	o.offset2 = 0;
	o.offset3 = 0;
	o.weights = 0;
	o.w1Contrast = 0;
	o.w2Contrast = 0;
	return o;
}

/** @brief Computes landscape UV gradients for SampleGrad. */
inline TerrainGradients ComputeTerrainGradients(float2 uv)
{
	TerrainGradients g;
	g.gradDx = ddx(uv);
	g.gradDy = ddy(uv);
	return g;
}

/**
 * @brief Initializes per-tile albedo stochastic blend parameters.
 * @param tile Landscape layer index [0,5].
 * @param tex Layer color texture (unused; kept for call-site symmetry).
 * @param extraLandMipBias Unused; reserved for mip-driven fade.
 */
inline void InitTerrainStochasticMip(uint tile, Texture2D tex, float extraLandMipBias)
{
	g_terrainStochasticSecondSampleFade[tile] = 1.0;
	g_terrainStochasticHeightInfluence[tile] = HEIGHT_INFLUENCE;
}

/**
 * @brief Initializes per-tile parallax stochastic blend parameters.
 * @param tile Landscape layer index [0,5].
 * @param mipLevel Layer mip (unused; reserved for mip-driven fade).
 */
inline void InitTerrainParallaxStochasticFade(uint tile, float mipLevel)
{
	g_terrainParallaxSecondSampleFade[tile] = 1.0;
	g_terrainParallaxHeightInfluence[tile] = HEIGHT_INFLUENCE;
}

/**
 * @brief Builds 2-tap stochastic offsets from landscape UV (triangular grid).
 * @details Sorts barycentric corners and keeps the two highest weights. The kept second corner
 *          (c1) and discarded third corner (c2) swap identity wherever their weights are equal;
 *          since c1/c2 are unrelated hashed UV offsets, that swap is a discontinuity in *which*
 *          texel is sampled. @ref TAP_SWAP_FADE_WIDTH fades w2Contrast to 0 as (c1.w - c2.w) -> 0
 *          so the ambiguous tap contributes negligible weight right where its identity would pop,
 *          which is what causes visible faceted seams on high-contrast textures otherwise.
 *
 *          That fade must not fire near a c0/c1 tie (the hexagon-cell boundary, where c0 and c1
 *          swap instead): that swap is already continuous on its own because both c0 and c1 are
 *          sampled and symmetrically exchange roles at equal weight -- but suppressing w2 alone
 *          (never w1) breaks that symmetry and re-introduces a pop. The two tie lines cross only
 *          at the triangle centroids (hexagon corners, where all 3 barycentric weights -> 1/3),
 *          so @c dominanceGap gates the fade off whenever c0/c1 are close, keeping every other
 *          point on the c1/c2 median fixed while leaving the (unfixable in exactly 2 taps) corner
 *          points to fall back to the original, still-safe c0/c1-symmetric behavior. The guard
 *          band (@ref DOMINANCE_GUARD_WIDTH) is intentionally much narrower than the rank-swap
 *          fade band (@ref TAP_SWAP_FADE_WIDTH): only the region where *both* bands overlap (near
 *          each corner) is imperfect, so shrinking the guard band shrinks that residual without
 *          weakening the median fade anywhere else.
 */
inline StochasticOffsets ComputeStochasticOffsets(float2 landscapeUV)
{
	float2 skewUV = mul(SKEW_MATRIX, landscapeUV * WORLD_SCALE);
	float2 vxID = floor(skewUV);
	float2 f = frac(skewUV);
	float bz = 1.0 - f.x - f.y;

	StochasticCorner c0, c1, c2;
	if (bz > 0) {
		c0.cell = vxID;
		c0.w = bz;
		c1.cell = vxID + float2(0, 1);
		c1.w = f.y;
		c2.cell = vxID + float2(1, 0);
		c2.w = f.x;
	} else {
		c0.cell = vxID + 1.0;
		c0.w = -bz;
		c1.cell = vxID + float2(1, 0);
		c1.w = 1.0 - f.y;
		c2.cell = vxID + float2(0, 1);
		c2.w = 1.0 - f.x;
	}

	if (c1.w > c0.w) {
		StochasticCorner t = c0;
		c0 = c1;
		c1 = t;
	}
	if (c2.w > c0.w) {
		StochasticCorner t = c0;
		c0 = c2;
		c2 = t;
	}
	if (c2.w > c1.w) {
		StochasticCorner t = c1;
		c1 = c2;
		c2 = t;
	}

	StochasticOffsets o;
	o.offset1 = hash2D2D(c0.cell);
	o.offset2 = hash2D2D(c1.cell);
	o.offset3 = 0;
	o.weights = float3(c0.w, c1.w, c2.w);
	o.w1Contrast = StochasticContrastWeight(c0.w);
	float dominanceGap = c0.w - c1.w;
	float safeToSuppress = smoothstep(0.0, DOMINANCE_GUARD_WIDTH, dominanceGap);
	float rankFade = smoothstep(0.0, TAP_SWAP_FADE_WIDTH, c1.w - c2.w);
	float rankConfidence = lerp(1.0, rankFade, safeToSuppress);
	o.w2Contrast = StochasticContrastWeight(c1.w) * rankConfidence;
	return o;
}

/**
 * @brief Builds LOD-terrain stochastic offsets (disabled when LOD tiling fix is off).
 */
inline StochasticOffsets ComputeStochasticOffsetsLOD(float2 landscapeUV)
{
	float lodOn = SharedData::terrainVariationSettings.enableLODTerrainTilingFix ? 1.0 : 0.0;

	float2 cellID = floor(landscapeUV * 255437.0);
	float2 h1 = hashLOD(cellID);
	float2 h2 = hashLOD(cellID + 127.0);

	StochasticOffsets o;
	o.offset1 = h1 * 0.08 * lodOn;
	o.offset2 = h2 * 0.08 * lodOn;
	o.offset3 = 0;
	o.weights = float3(0.65, 0.35, 0.0) * lodOn;
	o.w1Contrast = StochasticContrastWeight(o.weights.x);
	o.w2Contrast = StochasticContrastWeight(o.weights.y);
	return o;
}

/** @brief Low-discrepancy jitter for @ref StochasticSampleLOD. */
inline float2 StochasticSampleLODJitter(float rnd)
{
	return float2(rnd - 0.5, frac(rnd * STOCHASTIC_LOD_PHI) - 0.5);
}

/**
 * @brief Height-aware blend of two stochastic taps.
 * @param secondSampleScale Scales the second tap (1 = full contribution).
 */
inline float4 StochasticBlendTwoSamples(float4 s1, float4 s2, float w1Contrast, float w2Contrast, float heightInfluence, float blendFactor1, float blendFactor2, float secondSampleScale)
{
	float w1 = w1Contrast * (1.0 + heightInfluence * blendFactor1);
	float w2 = w2Contrast * secondSampleScale * (1.0 + heightInfluence * blendFactor2);
	float denom = max(w1 + w2, 1e-8);
	return lerp(s2, s1, w1 / denom);
}

/**
 * @brief Two-tap SampleBias LOD terrain sampling.
 * @param jitter From @ref StochasticSampleLODJitter.
 */
inline float4 StochasticSampleLOD(float2 jitter, Texture2D tex, SamplerState samp, float2 uv, StochasticOffsets offsetsLOD)
{
	float lodOn = SharedData::terrainVariationSettings.enableLODTerrainTilingFix ? 1.0 : 0.0;
	float2 j1 = (offsetsLOD.offset1 + jitter) * 0.01;
	float2 j2 = (offsetsLOD.offset2 + float2(jitter.y, -jitter.x)) * 0.01;
	float4 s1 = tex.SampleBias(samp, uv + j1 * lodOn, SharedData::MipBias);
	float4 s2 = tex.SampleBias(samp, uv + j2 * lodOn, SharedData::MipBias);
	float blendW = lerp(0.5, offsetsLOD.weights.x, lodOn);
	return lerp(s2, s1, blendW);
}

/**
 * @brief Two-tap SampleGrad stochastic sampling for near terrain albedo/normals.
 * @details Uses @ref g_terrainStochasticGrad for both taps.
 */
inline float4 StochasticEffect(Texture2D tex, SamplerState samp, float2 uv, StochasticOffsets offsets, float secondSampleFade, float heightInfluence)
{
	TerrainGradients g = g_terrainStochasticGrad;
	float4 s1 = tex.SampleGrad(samp, uv + offsets.offset1, g.gradDx, g.gradDy);
	float4 s2 = tex.SampleGrad(samp, uv + offsets.offset2, g.gradDx, g.gradDy);

	float h1 = lerp(dot(s1.rgb, LUMINANCE_WEIGHTS), s1.a, step(0.001, s1.a));
	float h2 = lerp(dot(s2.rgb, LUMINANCE_WEIGHTS), s2.a, step(0.001, s2.a));

	return StochasticBlendTwoSamples(s1, s2, offsets.w1Contrast, offsets.w2Contrast, heightInfluence, h1, h2, secondSampleFade);
}

/**
 * @brief Two-tap SampleLevel stochastic sampling for parallax/height.
 * @details Branchless; must match @ref StochasticEffect offsets/weights so height stays aligned with albedo.
 */
inline float4 StochasticEffectParallax(Texture2D tex, SamplerState samp, float2 uv, float mipLevel, StochasticOffsets offsets, float secondSampleFade, float heightInfluence)
{
	float4 s1 = tex.SampleLevel(samp, uv + offsets.offset1, mipLevel);
	float4 s2 = tex.SampleLevel(samp, uv + offsets.offset2, mipLevel);
	return StochasticBlendTwoSamples(s1, s2, offsets.w1Contrast, offsets.w2Contrast, heightInfluence, s1.a, s2.a, secondSampleFade);
}

/**
 * @brief Landscape layer sample via @ref StochasticEffect and per-tile fade/influence.
 */
inline float4 SampleTerrain(Texture2D tex, SamplerState samp, float2 uv, StochasticOffsets offsets, uint tileIndex)
{
	return StochasticEffect(tex, samp, uv, offsets, g_terrainStochasticSecondSampleFade[tileIndex], g_terrainStochasticHeightInfluence[tileIndex]);
}

#endif  // TERRAIN_VARIATION_HLSLI
