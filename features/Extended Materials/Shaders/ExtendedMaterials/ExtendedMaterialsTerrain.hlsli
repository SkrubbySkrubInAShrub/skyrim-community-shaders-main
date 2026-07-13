/**
 * @file ExtendedMaterialsTerrain.hlsli
 * @brief Landscape height sampling / height blending (inside namespace ExtendedMaterials, LANDSCAPE only).
 */

#ifndef EXTENDED_MATERIALS_TERRAIN_HLSLI
#define EXTENDED_MATERIALS_TERRAIN_HLSLI

	/** @brief Fills per-layer mip levels for terrain parallax. */
	void InitializeTerrainMipLevels(float2 coords, out float mipLevels[6])
	{
		mipLevels[0] = GetMipLevel(coords, TexColorSampler);
		mipLevels[1] = GetMipLevel(coords, TexLandColor2Sampler);
		mipLevels[2] = GetMipLevel(coords, TexLandColor3Sampler);
		mipLevels[3] = GetMipLevel(coords, TexLandColor4Sampler);
		mipLevels[4] = GetMipLevel(coords, TexLandColor5Sampler);
		mipLevels[5] = GetMipLevel(coords, TexLandColor6Sampler);
	}

	/**
	 * @brief Samples a terrain height/displacement texel (stochastic when TERRAIN_VARIATION is set).
	 */
	inline float4 TerrainParallaxTexSample(Texture2D tex, float2 uv, float mipLevel, StochasticOffsets sharedOffset, uint layerIndex)
	{
#	if defined(TERRAIN_VARIATION)
		return StochasticEffectParallax(tex, SampTerrainParallaxSampler, uv, mipLevel, sharedOffset);
#	else
		return tex.SampleLevel(SampTerrainParallaxSampler, uv, mipLevel);
#	endif
	}

#	define HEIGHT_POWER 2
#	define HEIGHT_MULT 8

	/**
	 * @brief Relative per-layer gate for height sampling: layers whose weight x HeightScale falls
	 *        below this fraction of the dominant layer's cannot move the blended height perceptibly
	 *        (their linear-blend contribution is bounded by this fraction of the max displacement),
	 *        so their fetches are skipped. Vertex blend weights vary smoothly, so a layer fades
	 *        through sub-threshold contribution before being gated — no popping.
	 */
	static const float TERRAIN_LAYER_GATE_EPS = 0.02;

	/** @brief Max of weight x HeightScale across the six layers for the given weights. */
	inline float TerrainMaxWeightedHeightScaleW(float4 w1, float2 w2, DisplacementParams params[6])
	{
		return max(params[0].HeightScale * w1.x, max(params[1].HeightScale * w1.y, max(params[2].HeightScale * w1.z,
																						  max(params[3].HeightScale * w1.w, max(params[4].HeightScale * w2.x, params[5].HeightScale * w2.y)))));
	}

	/**
	 * @brief Gate threshold for one height evaluation.
	 * @details Only linear blends (heightBlend <= 1: ray march, secant, shadows) are gated. The
	 *          height-sharpened weight pass (heightBlend > 1) keeps every layer: sharpening can
	 *          legitimately boost a small linear weight, so a gated-out height there could skew
	 *          the final albedo blend weights.
	 */
	inline float TerrainLayerGateThreshold(float heightBlend, float4 w1, float2 w2, DisplacementParams params[6])
	{
		return heightBlend <= 1.0 ? TERRAIN_LAYER_GATE_EPS * TerrainMaxWeightedHeightScaleW(w1, w2, params) : 0.0;
	}

	/** @brief Dot product of per-layer heights and weights. */
	float TerrainWeightedHeightSum(float heights[6], float weights[6])
	{
		float totalHeight = 0;
		[loop] for (int i = 0; i < 6; i++)
		{
			totalHeight += heights[i] * weights[i];
		}
		return totalHeight;
	}

	/**
	 * @brief Normalizes or height-sharpens landscape blend weights and returns weighted height.
	 * @param heightBlend 1 = linear weights; >1 applies height power blending.
	 */
	void ProcessTerrainHeightWeights(float heightBlend, float4 w1, float2 w2, float heights[6], inout float weights[6], out float totalHeight)
	{
		totalHeight = 0.0;
		weights[0] = w1.x;
		weights[1] = w1.y;
		weights[2] = w1.z;
		weights[3] = w1.w;
		weights[4] = w2.x;
		weights[5] = w2.y;

		if (heightBlend <= 1.0) {
			float wsum = 0;
			[loop] for (int j = 0; j < 6; j++)
			{
				wsum += weights[j];
			}

			float invwsum = rcp(wsum);
			[loop] for (int k = 0; k < 6; k++)
			{
				weights[k] *= invwsum;
				totalHeight += heights[k] * weights[k];
			}
		} else {
			float logHeightBlend = log2(max(abs(heightBlend), 0.0001));
			[loop] for (int hbIdx = 0; hbIdx < 6; hbIdx++)
			{
				weights[hbIdx] *= exp2((HEIGHT_MULT * heights[hbIdx]) * logHeightBlend);
			}

			[loop] for (int j = 0; j < 6; j++)
			{
				weights[j] = min(100, pow(abs(weights[j]), max(abs(heightBlend), 0.0001)));
			}

			float wsum = 0;
			[loop] for (int k = 0; k < 6; k++)
			{
				wsum += weights[k];
			}

			float invwsum = rcp(wsum);
			[loop] for (int l = 0; l < 6; l++)
			{
				weights[l] *= invwsum;
				totalHeight += heights[l] * weights[l];
			}
		}
	}

	/**
	 * @brief Blends four height vectors like four @ref GetTerrainHeight calls.
	 * @param weights Output weights from the last UV (tap 3).
	 */
	float4 FinishTerrainHeightQuadBlend(float heightBlend, float4 w1, float2 w2,
		float qh0[6], float qh1[6], float qh2[6], float qh3[6], out float weights[6])
	{
		float4 result = 0.0;
		if (heightBlend <= 1.0) {
			float t3 = 0.0;
			ProcessTerrainHeightWeights(heightBlend, w1, w2, qh3, weights, t3);
			result = float4(TerrainWeightedHeightSum(qh0, weights), TerrainWeightedHeightSum(qh1, weights), TerrainWeightedHeightSum(qh2, weights), t3);
		} else {
			float wTmp[6];
			float t0 = 0.0, t1 = 0.0, t2 = 0.0, t3 = 0.0;
			ProcessTerrainHeightWeights(heightBlend, w1, w2, qh0, wTmp, t0);
			ProcessTerrainHeightWeights(heightBlend, w1, w2, qh1, wTmp, t1);
			ProcessTerrainHeightWeights(heightBlend, w1, w2, qh2, wTmp, t2);
			ProcessTerrainHeightWeights(heightBlend, w1, w2, qh3, weights, t3);
			result = float4(t0, t1, t2, t3);
		}
		return result;
	}

#	if defined(TRUE_PBR)

/** @note Pass full scoped PBR::TerrainFlags values; FXC will not expand macros inside `::`. */
#define EM_PBR_DISP_LAYER_SCALAR(N, TILEFLAG, TEX, WGT) \
		[branch] if ((PBRFlags & (TILEFLAG)) != 0 && (WGT) > 0.01 && (WGT)*params[N].HeightScale >= layerGateThreshold) \
		{ \
			heights[N] = ScaleDisplacement(TerrainParallaxTexSample(TEX, coords, mipLevels[N], sharedOffset, N).x, params[N]); \
		}

#define EM_PBR_DISP_LAYER_QUAD(N, TILEFLAG, TEX, WGT) \
		[branch] if ((PBRFlags & (TILEFLAG)) != 0 && (WGT) > 0.01 && (WGT)*params[N].HeightScale >= layerGateThreshold) \
		{ \
			[loop] for (uint k = 0; k < 4; k++) \
				h4[k][N] = ScaleDisplacement(TerrainParallaxTexSample(TEX, uvs[k], mipLevels[N], sharedOffset, N).x, params[N]); \
		}

#define EM_PBR_DISP_FOREACH(M) \
		M(0, PBR::TerrainFlags::LandTile0HasDisplacement, TexLandDisplacement0Sampler, w1.x) \
		M(1, PBR::TerrainFlags::LandTile1HasDisplacement, TexLandDisplacement1Sampler, w1.y) \
		M(2, PBR::TerrainFlags::LandTile2HasDisplacement, TexLandDisplacement2Sampler, w1.z) \
		M(3, PBR::TerrainFlags::LandTile3HasDisplacement, TexLandDisplacement3Sampler, w1.w) \
		M(4, PBR::TerrainFlags::LandTile4HasDisplacement, TexLandDisplacement4Sampler, w2.x) \
		M(5, PBR::TerrainFlags::LandTile5HasDisplacement, TexLandDisplacement5Sampler, w2.y)

	/** @brief Weighted terrain height at coords (PBR displacement maps). */
	float GetTerrainHeight(float screenNoise, PS_INPUT input, float2 coords, float mipLevels[6], DisplacementParams params[6], float blendFactor, float4 w1, float2 w2,
		StochasticOffsets sharedOffset,
		out float weights[6])
	{
		float heightBlend = 1 + blendFactor * HEIGHT_POWER;
		float layerGateThreshold = TerrainLayerGateThreshold(heightBlend, w1, w2, params);
		float heights[6] = { 0, 0, 0, 0, 0, 0 };

		EM_PBR_DISP_FOREACH(EM_PBR_DISP_LAYER_SCALAR)

		float total = 0.0;
		ProcessTerrainHeightWeights(heightBlend, w1, w2, heights, weights, total);
		return total;
	}

	/** @brief Four-UV height sample for coarse ray-march steps (same result as four GetTerrainHeight calls). */
	float4 GetTerrainHeightQuadRayMarch(float screenNoise, PS_INPUT input,
		float2 u0, float2 u1, float2 u2, float2 u3,
		float mipLevels[6], DisplacementParams params[6], float blendFactor, float4 w1, float2 w2,
		StochasticOffsets sharedOffset,
		out float weights[6])
	{
		float heightBlend = 1 + blendFactor * HEIGHT_POWER;
		float layerGateThreshold = TerrainLayerGateThreshold(heightBlend, w1, w2, params);
		float2 uvs[4] = { u0, u1, u2, u3 };
		float h4[4][6];
		[loop] for (uint qi = 0; qi < 4; qi++)
			[loop] for (uint lj = 0; lj < 6; lj++)
				h4[qi][lj] = 0;

		EM_PBR_DISP_FOREACH(EM_PBR_DISP_LAYER_QUAD)

		return FinishTerrainHeightQuadBlend(heightBlend, w1, w2, h4[0], h4[1], h4[2], h4[3], weights);
	}

#undef EM_PBR_DISP_LAYER_SCALAR
#undef EM_PBR_DISP_LAYER_QUAD
#undef EM_PBR_DISP_FOREACH

#	else

#define EM_LEGACY_LAYER_SCALAR(N, THFLAG, THSAMPLER, COLSAMPLER, WGT) \
		if ((WGT) > 0.01 && (WGT)*params[N].HeightScale >= layerGateThreshold) { \
			[branch] if ((Permutation::ExtraFeatureDescriptor & (THFLAG)) != 0) \
			{ \
				heights[N] = ScaleDisplacement(TerrainParallaxTexSample(THSAMPLER, coords, mipLevels[N], sharedOffset, N).x, params[N]); \
			} \
			else \
			{ \
				heights[N] = ScaleDisplacement(TerrainParallaxTexSample(COLSAMPLER, coords, mipLevels[N], sharedOffset, N).w, params[N]); \
			} \
		}

#define EM_LEGACY_LAYER_QUAD(N, THFLAG, THSAMPLER, COLSAMPLER, WGT) \
		if ((WGT) > 0.01 && (WGT)*params[N].HeightScale >= layerGateThreshold) { \
			[branch] if ((Permutation::ExtraFeatureDescriptor & (THFLAG)) != 0) \
			{ \
				[loop] for (uint k = 0; k < 4; k++) \
					h4[k][N] = ScaleDisplacement(TerrainParallaxTexSample(THSAMPLER, uvs[k], mipLevels[N], sharedOffset, N).x, params[N]); \
			} \
			else \
			{ \
				[loop] for (uint k = 0; k < 4; k++) \
					h4[k][N] = ScaleDisplacement(TerrainParallaxTexSample(COLSAMPLER, uvs[k], mipLevels[N], sharedOffset, N).w, params[N]); \
			} \
		}

#define EM_LEGACY_FOREACH(M) \
		M(0, Permutation::ExtraFeatureFlags::THLand0HasDisplacement, TexLandTHDisp0Sampler, TexColorSampler, w1.x) \
		M(1, Permutation::ExtraFeatureFlags::THLand1HasDisplacement, TexLandTHDisp1Sampler, TexLandColor2Sampler, w1.y) \
		M(2, Permutation::ExtraFeatureFlags::THLand2HasDisplacement, TexLandTHDisp2Sampler, TexLandColor3Sampler, w1.z) \
		M(3, Permutation::ExtraFeatureFlags::THLand3HasDisplacement, TexLandTHDisp3Sampler, TexLandColor4Sampler, w1.w) \
		M(4, Permutation::ExtraFeatureFlags::THLand4HasDisplacement, TexLandTHDisp4Sampler, TexLandColor5Sampler, w2.x) \
		M(5, Permutation::ExtraFeatureFlags::THLand5HasDisplacement, TexLandTHDisp5Sampler, TexLandColor6Sampler, w2.y)

	/** @brief Weighted terrain height at coords (legacy TH / color-alpha displacement). */
	float GetTerrainHeight(float screenNoise, PS_INPUT input, float2 coords, float mipLevels[6], DisplacementParams params[6], float blendFactor, float4 w1, float2 w2,
		StochasticOffsets sharedOffset,
		out float weights[6])
	{
		float heightBlend = 1 + blendFactor * HEIGHT_POWER;
		float layerGateThreshold = TerrainLayerGateThreshold(heightBlend, w1, w2, params);
		float heights[6] = { 0, 0, 0, 0, 0, 0 };

		EM_LEGACY_FOREACH(EM_LEGACY_LAYER_SCALAR)

		float total = 0.0;
		ProcessTerrainHeightWeights(heightBlend, w1, w2, heights, weights, total);
		return total;
	}

	/** @brief Four-UV height sample for coarse ray-march steps (legacy TH/color path). */
	float4 GetTerrainHeightQuadRayMarch(float screenNoise, PS_INPUT input,
		float2 u0, float2 u1, float2 u2, float2 u3,
		float mipLevels[6], DisplacementParams params[6], float blendFactor, float4 w1, float2 w2,
		StochasticOffsets sharedOffset,
		out float weights[6])
	{
		float heightBlend = 1 + blendFactor * HEIGHT_POWER;
		float layerGateThreshold = TerrainLayerGateThreshold(heightBlend, w1, w2, params);
		float2 uvs[4] = { u0, u1, u2, u3 };
		float h4[4][6];
		[loop] for (uint qi = 0; qi < 4; qi++)
			[loop] for (uint lj = 0; lj < 6; lj++)
				h4[qi][lj] = 0;

		EM_LEGACY_FOREACH(EM_LEGACY_LAYER_QUAD)

		return FinishTerrainHeightQuadBlend(heightBlend, w1, w2, h4[0], h4[1], h4[2], h4[3], weights);
	}

#undef EM_LEGACY_LAYER_SCALAR
#undef EM_LEGACY_LAYER_QUAD
#undef EM_LEGACY_FOREACH

#	endif
#	if defined(TRUE_PBR)
	static const uint TERRAIN_DISPLACEMENT_MASK = (1u << 6u) | (1u << 7u) | (1u << 8u) | (1u << 9u) | (1u << 10u) | (1u << 11u);
#	endif
#	define TERRAIN_HEIGHT_AT(COORDS, MIP, QUALITY, WEIGHTS) \
		GetTerrainHeight(noise, input, COORDS, MIP, params, 0.0, input.LandBlendWeights1, input.LandBlendWeights2.xy, sharedOffset, WEIGHTS)

	/** @brief True when any landscape blend weight is significant. */
	inline bool TerrainHasSignificantBlend(float4 w1, float2 w2)
	{
		return (w1.x + w1.y + w1.z + w1.w + w2.x + w2.y) > 0.01;
	}

	/** @brief True when any landscape layer has displacement available. */
	inline bool TerrainHasAnyDisplacement()
	{
#	if defined(TRUE_PBR)
		return (PBRFlags & TERRAIN_DISPLACEMENT_MASK) != 0;
#	else
		return SharedData::extendedMaterialSettings.EnableTerrainParallax ||
		       (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::THLandHasDisplacement) != 0;
#	endif
	}

	/** @brief Max height scale weighted by current land blend weights. */
	inline float TerrainMaxWeightedHeightScale(PS_INPUT input, DisplacementParams params[6])
	{
		return TerrainMaxWeightedHeightScaleW(input.LandBlendWeights1, input.LandBlendWeights2.xy, params);
	}

	/** @brief Tap count for directional terrain parallax soft shadows. */
	inline uint TerrainDirectionalShadowTapCount(float quality)
	{
		if (quality > 0.7)
			return 2;
		if (quality > 0.0)
			return 1;
		return 0;
	}

	/** @brief Samples base height for terrain parallax shadows; returns false if skipped. */
	bool ComputeTerrainParallaxShadowBaseHeight(PS_INPUT input, float2 coords, float mipLevels[6], float quality, float noise, DisplacementParams params[6], StochasticOffsets sharedOffset, out float sh0)
	{
		sh0 = 0.0;
		if (!TerrainHasSignificantBlend(input.LandBlendWeights1, input.LandBlendWeights2.xy))
			return false;
		if (!TerrainHasAnyDisplacement())
			return false;

		float weights[6] = { 0, 0, 0, 0, 0, 0 };
		sh0 = TERRAIN_HEIGHT_AT(coords, mipLevels, quality, weights);
		return true;
	}

	/**
	 * @brief Soft shadow multiplier along light L for terrain parallax (point lights).
	 * @details Tap count is capped at 2, matching @ref TerrainDirectionalShadowTapCount; each
	 *          terrain tap is a full six-layer height blend, so extra taps are disproportionately
	 *          expensive. The 4/tapCount normalization already compensates intensity for the
	 *          reduced count (same scheme the quality tiers use).
	 */
	float GetParallaxSoftShadowMultiplierTerrain(PS_INPUT input, float2 coords, float mipLevel[6], float3 L, float sh0, float quality, float noise, DisplacementParams params[6], StochasticOffsets sharedOffset)
	{
		if (quality > 0.0) {
			uint tapCount = min(ParallaxShadowTapCount(quality), 2u);
			float shadowStrength = ShadowIntensity * (4.0 / tapCount);
			float heights[6] = { 0, 0, 0, 0, 0, 0 };
			float2 rayDir = L.xy * 0.1;
			float shadowScaleInv = 1.0;

#	if defined(TRUE_PBR)
			float scale = TerrainMaxWeightedHeightScale(input, params);
			if (scale < 0.01)
				return 1.0;
			rayDir *= scale;
			shadowScaleInv = rcp(scale);
#	endif
			float shadowAccum = 0.0;
			[loop] for (uint i = 0; i < tapCount; i++)
			{
				float shi = TERRAIN_HEIGHT_AT(coords + rayDir * rcp((float)(i + 1) + noise), mipLevel, quality, heights);
				shadowAccum += max(0, shi - sh0) * shadowScaleInv;
			}
			return 1.0 - saturate(shadowAccum * shadowStrength);
		}
		return 1.0;
	}

	/** @brief Directional-light terrain parallax soft shadow. */
	float EvaluateTerrainDirectionalParallaxShadowMultiplier(PS_INPUT input, float2 coords, float mipLevels[6], float3 lightDirection, float quality, float noise, DisplacementParams params[6], StochasticOffsets sharedOffset, float sh0)
	{
		uint tapCount = TerrainDirectionalShadowTapCount(quality);
		if (tapCount == 0)
			return 1.0;
		float shadowStrength = ShadowIntensity * (2.0 / tapCount);
		if (!TerrainHasSignificantBlend(input.LandBlendWeights1, input.LandBlendWeights2.xy))
			return 1.0;

		float heights[6] = { 0, 0, 0, 0, 0, 0 };
		float2 rayDir = lightDirection.xy * 0.1;
		float shadowScaleInv = 1.0;

#	if defined(TRUE_PBR)
		float scale = TerrainMaxWeightedHeightScale(input, params);
		if (scale < 0.01)
			return 1.0;
		rayDir *= scale;
		shadowScaleInv = rcp(scale);
#	endif

		float shadowAccum = 0.0;
		[loop] for (uint i = 0; i < tapCount; i++)
		{
			float shi = TERRAIN_HEIGHT_AT(coords + rayDir * rcp((float)(i + 1) + noise), mipLevels, quality, heights);
			shadowAccum += max(0, shi - sh0) * shadowScaleInv;
		}

		return 1.0 - saturate(shadowAccum * shadowStrength);
	}

#	undef TERRAIN_HEIGHT_AT

#endif  // EXTENDED_MATERIALS_TERRAIN_HLSLI