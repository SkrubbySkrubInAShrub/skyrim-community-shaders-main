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
/** @brief Fade width for c1/c2 rank-swap seams in @ref ComputeStochasticOffsets. */
static const float TAP_SWAP_FADE_WIDTH = 0.1;
/** @brief Narrow guard that disables the rank-swap fade near c0/c1 ties. */
static const float DOMINANCE_GUARD_WIDTH = 0.02;
/** @brief w2/w1 ratio below which the 2nd tap collapses onto tap 1. */
static const float TAP2_COLLAPSE_RATIO_LO = 0.02;
/** @brief w2/w1 ratio where the 2nd-tap collapse fade begins. */
static const float TAP2_COLLAPSE_RATIO_HI = 0.04;
/** @brief Golden ratio for LOD sample jitter. */
static const float STOCHASTIC_LOD_PHI = 1.618;
/** @brief LOD two-tap blend weight (callers gate on enableLODTerrainTilingFix). */
static const float STOCHASTIC_LOD_BLEND = 0.65;

/**
 * @brief Per-pixel stochastic UV offsets and blend weights for near terrain.
 * @details Lives across the POM ray march; keep fields minimal.
 */
struct StochasticOffsets
{
	float2 offset1;
	float2 offset2;
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

/**
 * @brief Landscape UV gradients for SampleGrad.
 * @details Scales by @c exp2(MipBias) to match SampleBias sharpening under upscaling.
 */
inline TerrainGradients ComputeTerrainGradients(float2 uv)
{
	TerrainGradients g;
	float biasScale = exp2(SharedData::MipBias);
	g.gradDx = ddx(uv) * biasScale;
	g.gradDy = ddy(uv) * biasScale;
	return g;
}

/**
 * @brief Builds 2-tap stochastic offsets from landscape UV (triangular grid).
 * @details Keeps the two highest barycentric corners. Fades w2 near c1/c2 ties
 *          (@ref TAP_SWAP_FADE_WIDTH) so the discarded corner cannot pop identity;
 *          @ref DOMINANCE_GUARD_WIDTH disables that fade near c0/c1 ties so the
 *          primary swap stays symmetric. Collapses the 2nd tap when w2/w1 is tiny
 *          (@ref TAP2_COLLAPSE_RATIO_LO / @ref TAP2_COLLAPSE_RATIO_HI).
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
	o.w1Contrast = pow(saturate(c0.w), STOCHASTIC_WEIGHT_POWER);
	float rankFade = smoothstep(0.0, TAP_SWAP_FADE_WIDTH, c1.w - c2.w);
	float dominanceGuard = smoothstep(0.0, DOMINANCE_GUARD_WIDTH, c0.w - c1.w);
	float w2Contrast = pow(saturate(c1.w), STOCHASTIC_WEIGHT_POWER) * lerp(1.0, rankFade, dominanceGuard);
	w2Contrast *= smoothstep(TAP2_COLLAPSE_RATIO_LO, TAP2_COLLAPSE_RATIO_HI, w2Contrast / max(o.w1Contrast, 1e-6));
	o.w2Contrast = w2Contrast;
	o.offset2 = w2Contrast > 0.0 ? hash2D2D(c1.cell) : o.offset1;
	return o;
}

/**
 * @brief Height-aware blend of two stochastic taps.
 * @details At w2Contrast == 0 returns exactly s1.
 */
inline float4 StochasticBlendTwoSamples(float4 s1, float4 s2, float w1Contrast, float w2Contrast, float blendFactor1, float blendFactor2)
{
	float w1 = w1Contrast * (1.0 + HEIGHT_INFLUENCE * blendFactor1);
	float w2 = w2Contrast * (1.0 + HEIGHT_INFLUENCE * blendFactor2);
	float denom = max(w1 + w2, 1e-8);
	return lerp(s2, s1, w1 / denom);
}

/**
 * @brief Two-tap SampleBias LOD terrain sampling.
 * @details Call only when enableLODTerrainTilingFix is on; builds cell hashes and jitter internally.
 * @param rnd Screen noise in [0,1] for low-discrepancy UV jitter.
 */
inline float4 StochasticSampleLOD(float rnd, Texture2D tex, SamplerState samp, float2 uv)
{
	float2 cellID = floor(uv * 255437.0);
	float2 offset1 = hashLOD(cellID) * 0.08;
	float2 offset2 = hashLOD(cellID + 127.0) * 0.08;
	float2 jitter = float2(rnd - 0.5, frac(rnd * STOCHASTIC_LOD_PHI) - 0.5);
	float2 j1 = (offset1 + jitter) * 0.01;
	float2 j2 = (offset2 + float2(jitter.y, -jitter.x)) * 0.01;
	float4 s1 = tex.SampleBias(samp, uv + j1, SharedData::MipBias);
	float4 s2 = tex.SampleBias(samp, uv + j2, SharedData::MipBias);
	return lerp(s2, s1, STOCHASTIC_LOD_BLEND);
}

/**
 * @brief Two-tap SampleGrad stochastic sampling for near terrain albedo/normals.
 * @details Uses @ref g_terrainStochasticGrad. Skips the 2nd fetch when w2Contrast == 0.
 */
inline float4 StochasticEffect(Texture2D tex, SamplerState samp, float2 uv, StochasticOffsets offsets)
{
	TerrainGradients g = g_terrainStochasticGrad;
	float4 s1 = tex.SampleGrad(samp, uv + offsets.offset1, g.gradDx, g.gradDy);
	float4 result;

	[branch] if (offsets.w2Contrast <= 0.0)
		result = s1;
	else {
		float4 s2 = tex.SampleGrad(samp, uv + offsets.offset2, g.gradDx, g.gradDy);
		float h1 = lerp(dot(s1.rgb, LUMINANCE_WEIGHTS), s1.a, step(0.001, s1.a));
		float h2 = lerp(dot(s2.rgb, LUMINANCE_WEIGHTS), s2.a, step(0.001, s2.a));
		result = StochasticBlendTwoSamples(s1, s2, offsets.w1Contrast, offsets.w2Contrast, h1, h2);
	}

	return result;
}

/**
 * @brief Two-tap SampleLevel stochastic sampling for parallax/height.
 * @details Branchless for FXC (many inlines on the POM path). When collapsed,
 *          offset2 == offset1 so the 2nd fetch is a cache hit. Same offsets as
 *          @ref StochasticEffect so height stays aligned with albedo.
 */
inline float4 StochasticEffectParallax(Texture2D tex, SamplerState samp, float2 uv, float mipLevel, StochasticOffsets offsets)
{
	float4 s1 = tex.SampleLevel(samp, uv + offsets.offset1, mipLevel);
	float4 s2 = tex.SampleLevel(samp, uv + offsets.offset2, mipLevel);
	return StochasticBlendTwoSamples(s1, s2, offsets.w1Contrast, offsets.w2Contrast, s1.a, s2.a);
}

#endif  // TERRAIN_VARIATION_HLSLI
