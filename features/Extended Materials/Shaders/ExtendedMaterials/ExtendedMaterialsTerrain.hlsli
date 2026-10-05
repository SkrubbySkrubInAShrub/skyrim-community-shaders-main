// Landscape height sampling and blending (namespace ExtendedMaterials, LANDSCAPE).

#ifndef EXTENDED_MATERIALS_TERRAIN_HLSLI
#define EXTENDED_MATERIALS_TERRAIN_HLSLI

	// Landscape Seams adds four borrowed layers after the engine's six; it defines its own count.
#	if !defined(TERRAIN_LAYER_COUNT)
#		define TERRAIN_LAYER_COUNT 6
#		define TERRAIN_LAYER_ZEROS { 0, 0, 0, 0, 0, 0 }
#	endif

	// Shared mip for all six landscape layers from color-map UV derivatives.
	// maxTexDim is for POM step sizing (one GetDimensions).
	void InitializeTerrainMipLevels(float2 coords, out float mipLevels[6], out float maxTexDim)
	{
		float2 textureDims;
		TexColorSampler.GetDimensions(textureDims.x, textureDims.y);
		maxTexDim = max(textureDims.x, textureDims.y);
		float mip = GetMipLevelFromDims(coords, textureDims);
		[unroll] for (uint i = 0; i < 6; i++)
			mipLevels[i] = mip;
	}

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

	// Skip layers whose weight × HeightScale is below this fraction of the dominant layer.
	// Only used for linear height blends; vertex weights fade smoothly before the gate trips.
	static const float TERRAIN_LAYER_GATE_EPS = 0.02;

	inline float TerrainMaxWeightedHeightScaleW(float4 w1, float2 w2, DisplacementParams params[TERRAIN_LAYER_COUNT])
	{
		float scale = max(params[0].HeightScale * w1.x, max(params[1].HeightScale * w1.y, max(params[2].HeightScale * w1.z,
																							   max(params[3].HeightScale * w1.w, max(params[4].HeightScale * w2.x, params[5].HeightScale * w2.y)))));
#	if defined(LANDSCAPE_SEAMS)
		float4 extra = LandscapeSeams::ExtraWeights * float4(params[6].HeightScale, params[7].HeightScale, params[8].HeightScale, params[9].HeightScale);
		scale = max(scale, max(max(extra.x, extra.y), max(extra.z, extra.w)));
#	endif
		return scale;
	}

	// Gate for march / secant / shadows (heightBlend ≤ 1). Height-sharpened passes keep every
	// layer so small weights can still rise.
	inline float TerrainLayerGateThreshold(float heightBlend, float4 w1, float2 w2, DisplacementParams params[TERRAIN_LAYER_COUNT])
	{
		return heightBlend <= 1.0 ? TERRAIN_LAYER_GATE_EPS * TerrainMaxWeightedHeightScaleW(w1, w2, params) : 0.0;
	}

	float TerrainWeightedHeightSum(float heights[TERRAIN_LAYER_COUNT], float weights[TERRAIN_LAYER_COUNT])
	{
		float totalHeight = 0;
		[unroll] for (int i = 0; i < TERRAIN_LAYER_COUNT; i++)
		{
			totalHeight += heights[i] * weights[i];
		}
		return totalHeight;
	}

	// Normalize landscape weights; if heightBlend > 1, sharpen by height.
	// Sharpening is skipped when all heights are nearly equal, so pow() is not applied to
	// vertex weights alone (which hardens triangle borders).
	void ProcessTerrainHeightWeights(float heightBlend, float4 w1, float2 w2, float heights[TERRAIN_LAYER_COUNT], inout float weights[TERRAIN_LAYER_COUNT], out float totalHeight)
	{
		totalHeight = 0.0;
		weights[0] = w1.x;
		weights[1] = w1.y;
		weights[2] = w1.z;
		weights[3] = w1.w;
		weights[4] = w2.x;
		weights[5] = w2.y;
#	if defined(LANDSCAPE_SEAMS)
		weights[6] = LandscapeSeams::ExtraWeights.x;
		weights[7] = LandscapeSeams::ExtraWeights.y;
		weights[8] = LandscapeSeams::ExtraWeights.z;
		weights[9] = LandscapeSeams::ExtraWeights.w;
#	endif

		float wsum = 0;
		[unroll] for (int j = 0; j < TERRAIN_LAYER_COUNT; j++)
		{
			wsum += weights[j];
		}
		float invwsum = rcp(max(wsum, 1e-6));
		[unroll] for (int k = 0; k < TERRAIN_LAYER_COUNT; k++)
		{
			weights[k] *= invwsum;
		}

		bool sharpen = heightBlend > 1.0;
		[branch] if (sharpen)
		{
			float hMin = heights[0];
			float hMax = heights[0];
			[unroll] for (int hi = 1; hi < TERRAIN_LAYER_COUNT; hi++)
			{
				hMin = min(hMin, heights[hi]);
				hMax = max(hMax, heights[hi]);
			}
			sharpen = (hMax - hMin) > 1e-3;
		}

		[branch] if (sharpen)
		{
			// pow(w * heightBlend^(HEIGHT_MULT * h), heightBlend)
			// = exp2(heightBlend * (log2(w) + HEIGHT_MULT * h * log2(heightBlend)))
			float clampedHeightBlend = max(abs(heightBlend), 0.0001);
			float logHeightBlend = log2(clampedHeightBlend);
			[unroll] for (int hbIdx = 0; hbIdx < TERRAIN_LAYER_COUNT; hbIdx++)
			{
				float logWeight = log2(abs(weights[hbIdx])) + (HEIGHT_MULT * heights[hbIdx]) * logHeightBlend;
				weights[hbIdx] = min(100, exp2(clampedHeightBlend * logWeight));
			}

			wsum = 0;
			[unroll] for (int k = 0; k < TERRAIN_LAYER_COUNT; k++)
			{
				wsum += weights[k];
			}
			invwsum = rcp(max(wsum, 1e-6));
			[unroll] for (int l = 0; l < TERRAIN_LAYER_COUNT; l++)
			{
				weights[l] *= invwsum;
			}
		}

		[unroll] for (int t = 0; t < TERRAIN_LAYER_COUNT; t++)
		{
			totalHeight += heights[t] * weights[t];
		}
	}

	// Four per-layer height vectors → one float4, matching four GetTerrainHeight calls.
	// weights come from the last UV (tap 3).
	float4 FinishTerrainHeightQuadBlend(float heightBlend, float4 w1, float2 w2,
		float qh0[TERRAIN_LAYER_COUNT], float qh1[TERRAIN_LAYER_COUNT], float qh2[TERRAIN_LAYER_COUNT], float qh3[TERRAIN_LAYER_COUNT], out float weights[TERRAIN_LAYER_COUNT])
	{
		float4 result = 0.0;
		if (heightBlend <= 1.0) {
			float t3 = 0.0;
			ProcessTerrainHeightWeights(heightBlend, w1, w2, qh3, weights, t3);
			result = float4(TerrainWeightedHeightSum(qh0, weights), TerrainWeightedHeightSum(qh1, weights), TerrainWeightedHeightSum(qh2, weights), t3);
		} else {
			float wTmp[TERRAIN_LAYER_COUNT];
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

// Pass fully scoped PBR::TerrainFlags; FXC does not expand macros inside `::`.
#define EM_PBR_DISP_LAYER_SCALAR(N, TILEFLAG, TEX, WGT) \
		[branch] if ((PBRFlags & (TILEFLAG)) != 0 && (WGT) > 0.01 && (WGT)*params[N].HeightScale >= layerGateThreshold) \
		{ \
			heights[N] = ScaleDisplacement(TerrainParallaxTexSample(TEX, coords, mipLevels[N], sharedOffset, N).x, params[N]); \
		}

#define EM_PBR_DISP_LAYER_QUAD(N, TILEFLAG, TEX, WGT) \
		[branch] if ((PBRFlags & (TILEFLAG)) != 0 && (WGT) > 0.01 && (WGT)*params[N].HeightScale >= layerGateThreshold) \
		{ \
			[unroll] for (uint k = 0; k < 4; k++) \
				h4[k][N] = ScaleDisplacement(TerrainParallaxTexSample(TEX, uvs[k], mipLevels[N], sharedOffset, N).x, params[N]); \
		}

#define EM_PBR_DISP_FOREACH(M) \
		M(0, PBR::TerrainFlags::LandTile0HasDisplacement, TexLandDisplacement0Sampler, w1.x) \
		M(1, PBR::TerrainFlags::LandTile1HasDisplacement, TexLandDisplacement1Sampler, w1.y) \
		M(2, PBR::TerrainFlags::LandTile2HasDisplacement, TexLandDisplacement2Sampler, w1.z) \
		M(3, PBR::TerrainFlags::LandTile3HasDisplacement, TexLandDisplacement3Sampler, w1.w) \
		M(4, PBR::TerrainFlags::LandTile4HasDisplacement, TexLandDisplacement4Sampler, w2.x) \
		M(5, PBR::TerrainFlags::LandTile5HasDisplacement, TexLandDisplacement5Sampler, w2.y)

// Landscape Seams layers share layer 0's mip: every terrain layer uses the same mip.
#		if defined(LANDSCAPE_SEAMS)
#define EM_SEAMS_LAYER_SCALAR(N, INDEX, TEX, WGT) \
		[branch] if (LandscapeSeams::HasFlag(LandscapeSeams::FlagDisplacement, INDEX) && (WGT) > 0.01 && (WGT)*params[N].HeightScale >= layerGateThreshold) \
		{ \
			heights[N] = ScaleDisplacement(TerrainParallaxTexSample(TEX, coords, mipLevels[0], sharedOffset, N).x, params[N]); \
		}

#define EM_SEAMS_LAYER_QUAD(N, INDEX, TEX, WGT) \
		[branch] if (LandscapeSeams::HasFlag(LandscapeSeams::FlagDisplacement, INDEX) && (WGT) > 0.01 && (WGT)*params[N].HeightScale >= layerGateThreshold) \
		{ \
			[unroll] for (uint k = 0; k < 4; k++) \
				h4[k][N] = ScaleDisplacement(TerrainParallaxTexSample(TEX, uvs[k], mipLevels[0], sharedOffset, N).x, params[N]); \
		}

#define EM_SEAMS_FOREACH(M) \
		M(6, 0, LandscapeSeams::Displacement0, LandscapeSeams::ExtraWeights.x) \
		M(7, 1, LandscapeSeams::Displacement1, LandscapeSeams::ExtraWeights.y) \
		M(8, 2, LandscapeSeams::Displacement2, LandscapeSeams::ExtraWeights.z) \
		M(9, 3, LandscapeSeams::Displacement3, LandscapeSeams::ExtraWeights.w)
#		else
#define EM_SEAMS_FOREACH(M)
#		endif

	float GetTerrainHeight(float screenNoise, PS_INPUT input, float2 coords, float mipLevels[6], DisplacementParams params[TERRAIN_LAYER_COUNT], float blendFactor, float4 w1, float2 w2,
		StochasticOffsets sharedOffset,
		out float weights[TERRAIN_LAYER_COUNT])
	{
		float heightBlend = 1 + blendFactor * HEIGHT_POWER;
		float layerGateThreshold = TerrainLayerGateThreshold(heightBlend, w1, w2, params);
		float heights[TERRAIN_LAYER_COUNT] = TERRAIN_LAYER_ZEROS;

		EM_PBR_DISP_FOREACH(EM_PBR_DISP_LAYER_SCALAR)
		EM_SEAMS_FOREACH(EM_SEAMS_LAYER_SCALAR)

		float total = 0.0;
		ProcessTerrainHeightWeights(heightBlend, w1, w2, heights, weights, total);
		return total;
	}

	float4 GetTerrainHeightQuadRayMarch(float screenNoise, PS_INPUT input,
		float2 u0, float2 u1, float2 u2, float2 u3,
		float mipLevels[6], DisplacementParams params[TERRAIN_LAYER_COUNT], float blendFactor, float4 w1, float2 w2,
		StochasticOffsets sharedOffset,
		out float weights[TERRAIN_LAYER_COUNT])
	{
		float heightBlend = 1 + blendFactor * HEIGHT_POWER;
		float layerGateThreshold = TerrainLayerGateThreshold(heightBlend, w1, w2, params);
		float2 uvs[4] = { u0, u1, u2, u3 };
		float h4[4][TERRAIN_LAYER_COUNT];
		[unroll] for (uint qi = 0; qi < 4; qi++)
			[unroll] for (uint lj = 0; lj < TERRAIN_LAYER_COUNT; lj++)
				h4[qi][lj] = 0;

		EM_PBR_DISP_FOREACH(EM_PBR_DISP_LAYER_QUAD)
		EM_SEAMS_FOREACH(EM_SEAMS_LAYER_QUAD)

		return FinishTerrainHeightQuadBlend(heightBlend, w1, w2, h4[0], h4[1], h4[2], h4[3], weights);
	}

#undef EM_PBR_DISP_LAYER_SCALAR
#undef EM_PBR_DISP_LAYER_QUAD
#undef EM_PBR_DISP_FOREACH
#undef EM_SEAMS_LAYER_SCALAR
#undef EM_SEAMS_LAYER_QUAD
#undef EM_SEAMS_FOREACH

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
				[unroll] for (uint k = 0; k < 4; k++) \
					h4[k][N] = ScaleDisplacement(TerrainParallaxTexSample(THSAMPLER, uvs[k], mipLevels[N], sharedOffset, N).x, params[N]); \
			} \
			else \
			{ \
				[unroll] for (uint k = 0; k < 4; k++) \
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

// Without True PBR a borrowed layer reads Terrain Helper's height map when it has one, else its color alpha.
#		if defined(LANDSCAPE_SEAMS)
#define EM_SEAMS_LAYER_SCALAR(N, INDEX, THSAMPLER, COLSAMPLER, WGT) \
		if ((WGT) > 0.01 && (WGT)*params[N].HeightScale >= layerGateThreshold) { \
			[branch] if (LandscapeSeams::HasFlag(LandscapeSeams::FlagParallax, INDEX)) \
			{ \
				heights[N] = ScaleDisplacement(TerrainParallaxTexSample(THSAMPLER, coords, mipLevels[0], sharedOffset, N).x, params[N]); \
			} \
			else \
			{ \
				heights[N] = ScaleDisplacement(TerrainParallaxTexSample(COLSAMPLER, coords, mipLevels[0], sharedOffset, N).w, params[N]); \
			} \
		}

#define EM_SEAMS_LAYER_QUAD(N, INDEX, THSAMPLER, COLSAMPLER, WGT) \
		if ((WGT) > 0.01 && (WGT)*params[N].HeightScale >= layerGateThreshold) { \
			[branch] if (LandscapeSeams::HasFlag(LandscapeSeams::FlagParallax, INDEX)) \
			{ \
				[unroll] for (uint k = 0; k < 4; k++) \
					h4[k][N] = ScaleDisplacement(TerrainParallaxTexSample(THSAMPLER, uvs[k], mipLevels[0], sharedOffset, N).x, params[N]); \
			} \
			else \
			{ \
				[unroll] for (uint k = 0; k < 4; k++) \
					h4[k][N] = ScaleDisplacement(TerrainParallaxTexSample(COLSAMPLER, uvs[k], mipLevels[0], sharedOffset, N).w, params[N]); \
			} \
		}

#define EM_SEAMS_FOREACH(M) \
		M(6, 0, LandscapeSeams::Aux0, LandscapeSeams::Color0, LandscapeSeams::ExtraWeights.x) \
		M(7, 1, LandscapeSeams::Aux1, LandscapeSeams::Color1, LandscapeSeams::ExtraWeights.y) \
		M(8, 2, LandscapeSeams::Aux2, LandscapeSeams::Color2, LandscapeSeams::ExtraWeights.z) \
		M(9, 3, LandscapeSeams::Aux3, LandscapeSeams::Color3, LandscapeSeams::ExtraWeights.w)
#		else
#define EM_SEAMS_FOREACH(M)
#		endif

	float GetTerrainHeight(float screenNoise, PS_INPUT input, float2 coords, float mipLevels[6], DisplacementParams params[TERRAIN_LAYER_COUNT], float blendFactor, float4 w1, float2 w2,
		StochasticOffsets sharedOffset,
		out float weights[TERRAIN_LAYER_COUNT])
	{
		float heightBlend = 1 + blendFactor * HEIGHT_POWER;
		float layerGateThreshold = TerrainLayerGateThreshold(heightBlend, w1, w2, params);
		float heights[TERRAIN_LAYER_COUNT] = TERRAIN_LAYER_ZEROS;

		EM_LEGACY_FOREACH(EM_LEGACY_LAYER_SCALAR)
		EM_SEAMS_FOREACH(EM_SEAMS_LAYER_SCALAR)

		float total = 0.0;
		ProcessTerrainHeightWeights(heightBlend, w1, w2, heights, weights, total);
		return total;
	}

	float4 GetTerrainHeightQuadRayMarch(float screenNoise, PS_INPUT input,
		float2 u0, float2 u1, float2 u2, float2 u3,
		float mipLevels[6], DisplacementParams params[TERRAIN_LAYER_COUNT], float blendFactor, float4 w1, float2 w2,
		StochasticOffsets sharedOffset,
		out float weights[TERRAIN_LAYER_COUNT])
	{
		float heightBlend = 1 + blendFactor * HEIGHT_POWER;
		float layerGateThreshold = TerrainLayerGateThreshold(heightBlend, w1, w2, params);
		float2 uvs[4] = { u0, u1, u2, u3 };
		float h4[4][TERRAIN_LAYER_COUNT];
		[unroll] for (uint qi = 0; qi < 4; qi++)
			[unroll] for (uint lj = 0; lj < TERRAIN_LAYER_COUNT; lj++)
				h4[qi][lj] = 0;

		EM_LEGACY_FOREACH(EM_LEGACY_LAYER_QUAD)
		EM_SEAMS_FOREACH(EM_SEAMS_LAYER_QUAD)

		return FinishTerrainHeightQuadBlend(heightBlend, w1, w2, h4[0], h4[1], h4[2], h4[3], weights);
	}

#undef EM_LEGACY_LAYER_SCALAR
#undef EM_LEGACY_LAYER_QUAD
#undef EM_LEGACY_FOREACH
#undef EM_SEAMS_LAYER_SCALAR
#undef EM_SEAMS_LAYER_QUAD
#undef EM_SEAMS_FOREACH

#	endif
#	if defined(TRUE_PBR)
	static const uint TERRAIN_DISPLACEMENT_MASK = (1u << 6u) | (1u << 7u) | (1u << 8u) | (1u << 9u) | (1u << 10u) | (1u << 11u);
#	endif
#	define TERRAIN_HEIGHT_AT(COORDS, MIP, QUALITY, WEIGHTS) \
		GetTerrainHeight(noise, input, COORDS, MIP, params, 0.0, input.LandBlendWeights1, input.LandBlendWeights2.xy, sharedOffset, WEIGHTS)

	inline bool TerrainHasAnyDisplacement()
	{
#	if defined(TRUE_PBR)
		bool hasDisplacement = (PBRFlags & TERRAIN_DISPLACEMENT_MASK) != 0;
#		if defined(LANDSCAPE_SEAMS)
		hasDisplacement = hasDisplacement || LandscapeSeams::HasAnyFlag(LandscapeSeams::AnyDisplacementMask);
#		endif
#	else
		bool hasDisplacement = SharedData::extendedMaterialSettings.EnableTerrainParallax ||
		                       (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::THLandHasDisplacement) != 0;
#		if defined(LANDSCAPE_SEAMS)
		hasDisplacement = hasDisplacement || LandscapeSeams::HasAnyFlag(LandscapeSeams::AnyParallaxMask);
#		endif
#	endif
		return hasDisplacement;
	}

	inline float TerrainMaxWeightedHeightScale(PS_INPUT input, DisplacementParams params[TERRAIN_LAYER_COUNT])
	{
		return TerrainMaxWeightedHeightScaleW(input.LandBlendWeights1, input.LandBlendWeights2.xy, params);
	}

	inline uint TerrainDirectionalShadowTapCount(float quality)
	{
		return quality > 0.0 ? 1u : 0u;
	}

	bool ComputeTerrainParallaxShadowBaseHeight(PS_INPUT input, float2 coords, float mipLevels[6], float quality, float noise, DisplacementParams params[TERRAIN_LAYER_COUNT], StochasticOffsets sharedOffset, out float sh0)
	{
		sh0 = 0.0;
		if (!TerrainHasAnyDisplacement())
			return false;

		float weights[TERRAIN_LAYER_COUNT] = TERRAIN_LAYER_ZEROS;
		sh0 = TERRAIN_HEIGHT_AT(coords, mipLevels, quality, weights);
		return true;
	}

	// Soft shadow along light L. Strength matches the 4/tapCount object-shadow scale.
	float GetParallaxSoftShadowMultiplierTerrain(PS_INPUT input, float2 coords, float mipLevel[6], float3 L, float sh0, float quality, float noise, DisplacementParams params[TERRAIN_LAYER_COUNT], StochasticOffsets sharedOffset)
	{
		if (quality > 0.0) {
			float shadowStrength = ShadowIntensity * 4.0;
			float heights[TERRAIN_LAYER_COUNT] = TERRAIN_LAYER_ZEROS;
			float2 rayDir = L.xy * 0.1;
			float shi = TERRAIN_HEIGHT_AT(coords + rayDir * rcp(1.0 + noise), mipLevel, quality, heights);
			return 1.0 - saturate(max(0, shi - sh0) * shadowStrength);
		}
		return 1.0;
	}

	float EvaluateTerrainDirectionalParallaxShadowMultiplier(PS_INPUT input, float2 coords, float mipLevels[6], float3 lightDirection, float quality, float noise, DisplacementParams params[TERRAIN_LAYER_COUNT], StochasticOffsets sharedOffset, float sh0)
	{
		if (TerrainDirectionalShadowTapCount(quality) == 0)
			return 1.0;
		float shadowStrength = ShadowIntensity * 2.0;

		float heights[TERRAIN_LAYER_COUNT] = TERRAIN_LAYER_ZEROS;
		float2 rayDir = lightDirection.xy * 0.1;
		float shi = TERRAIN_HEIGHT_AT(coords + rayDir * rcp(1.0 + noise), mipLevels, quality, heights);
		return 1.0 - saturate(max(0, shi - sh0) * shadowStrength);
	}

#	undef TERRAIN_HEIGHT_AT

#endif  // EXTENDED_MATERIALS_TERRAIN_HLSLI
