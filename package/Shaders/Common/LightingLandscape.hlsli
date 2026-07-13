/**
 * @file LightingLandscape.hlsli
 * @brief Landscape layer helpers and six-way blend macros for Lighting.hlsl.
 * @details PBR tile bits must match @c PBR::TerrainFlags in PBRMath.hlsli.
 *          Texture registers cannot be indexed by loop variable; use the X-macros.
 */

#ifndef __LIGHTING_LANDSCAPE_HLSLI__
#define __LIGHTING_LANDSCAPE_HLSLI__

#if defined(LANDSCAPE)

#	if !defined(__PERMUTATION_DEPENDENCY_HLSL__)
#		include "Common/Permutation.hlsli"
#	endif

namespace LandscapeLayers
{
#	if defined(TRUE_PBR)
	/** @brief True if tile uses full PBR material. */
	inline bool PbrTileUsesFullPBR(uint tileIndex)
	{
		return (PBRFlags & (1u << tileIndex)) != 0;
	}
	/** @brief True if tile has a displacement map. */
	inline bool PbrTileHasDisplacement(uint tileIndex)
	{
		return (PBRFlags & (1u << (tileIndex + 6u))) != 0;
	}
	/** @brief True if tile has glint. */
	inline bool PbrTileHasGlint(uint tileIndex)
	{
		return (PBRFlags & (1u << (tileIndex + 12u))) != 0;
	}
#	else
	/** @brief True if TH land tile has displacement. */
	inline bool ThTileHasDisplacement(uint tileIndex)
	{
		return (Permutation::ExtraFeatureDescriptor & (1u << tileIndex)) != 0;
	}
#	endif
}

/** @brief X-macro args: tileIndex, displacementTexture, diffuseOrAlphaHeightTexture. */
#	if defined(TRUE_PBR)
#		define LANDSCAPE_PBR_LAYER_FOREACH(X)                      \
			X(0, TexLandDisplacement0Sampler, TexColorSampler)      \
			X(1, TexLandDisplacement1Sampler, TexLandColor2Sampler) \
			X(2, TexLandDisplacement2Sampler, TexLandColor3Sampler) \
			X(3, TexLandDisplacement3Sampler, TexLandColor4Sampler) \
			X(4, TexLandDisplacement4Sampler, TexLandColor5Sampler) \
			X(5, TexLandDisplacement5Sampler, TexLandColor6Sampler)
#	else
#		define LANDSCAPE_TH_LAYER_FOREACH(X)                 \
			X(0, TexLandTHDisp0Sampler, TexColorSampler)      \
			X(1, TexLandTHDisp1Sampler, TexLandColor2Sampler) \
			X(2, TexLandTHDisp2Sampler, TexLandColor3Sampler) \
			X(3, TexLandTHDisp3Sampler, TexLandColor4Sampler) \
			X(4, TexLandTHDisp4Sampler, TexLandColor5Sampler) \
			X(5, TexLandTHDisp5Sampler, TexLandColor6Sampler)
#	endif

/** @brief Per-layer landscape diffuse/normal(/RMAOS) blend body used by Lighting.hlsl. */
#	if defined(TERRAIN_VARIATION)
#		define LANDSCAPE_SAMPLE_ARG(TILE) TILE
#	else
#		define LANDSCAPE_SAMPLE_ARG(TILE) landDistanceTexMipBias
#	endif
#	if defined(TRUE_PBR)
#		define LIGHTING_LANDSCAPE_BLEND_ONE_LAYER_PBR(TILE, COLOR_TEX, COLOR_SAMP, NORM_TEX, NORM_SAMP, RMAOS_TEX, RMAOS_SAMP, PBR_PARAMS3, GLINT_PARAMS, WEIGHT) \
			[branch] if ((WEIGHT) > 0.01)                                                                                                                          \
			{                                                                                                                                                      \
				float weight = WEIGHT;                                                                                                                             \
				float4 landColor = SampleTerrain(COLOR_TEX, COLOR_SAMP, uv, sharedOffset, LANDSCAPE_SAMPLE_ARG(TILE));                                                \
				float3 landColorRGB = landColor.rgb;                                                                                                               \
				[branch] if (!LandscapeLayers::PbrTileUsesFullPBR(TILE))                                                                                           \
				{                                                                                                                                                  \
					landColorRGB = Color::SrgbToLinear(landColorRGB / Color::PBRLightingScale);                                                                    \
				}                                                                                                                                                  \
				float landAlpha = landColor.a;                                                                                                                     \
				float4 landNormal = SampleTerrain(NORM_TEX, NORM_SAMP, uv, sharedOffset, LANDSCAPE_SAMPLE_ARG(TILE));                                                 \
				float3 landNormalRGB = landNormal.rgb;                                                                                                             \
				float landNormalAlpha = landNormal.a;                                                                                                              \
				float4 landRMAOS;                                                                                                                                  \
				[branch] if (LandscapeLayers::PbrTileUsesFullPBR(TILE))                                                                                            \
				{                                                                                                                                                  \
					landRMAOS = SampleTerrain(RMAOS_TEX, RMAOS_SAMP, uv, sharedOffset, LANDSCAPE_SAMPLE_ARG(TILE)) * float4((PBR_PARAMS3).x, 1, 1, (PBR_PARAMS3).z); \
					[branch] if (LandscapeLayers::PbrTileHasGlint(TILE))                                                                                           \
					{                                                                                                                                              \
						glintParameters += weight * (GLINT_PARAMS);                                                                                                \
					}                                                                                                                                              \
				}                                                                                                                                                  \
				else                                                                                                                                               \
				{                                                                                                                                                  \
					landRMAOS = float4(1 - glossiness.x, 0, 1, 0);                                                                                                   \
				}                                                                                                                                                  \
				blendedRMAOS += landRMAOS * weight;                                                                                                                \
				blendedRGB += landColorRGB * weight;                                                                                                               \
				blendedAlpha += landAlpha * weight;                                                                                                                \
				blendedNormalRGB += landNormalRGB * weight;                                                                                                        \
				blendedNormalAlpha += landNormalAlpha * weight;                                                                                                    \
			}
#	else
#		if defined(SNOW)
#			define LIGHTING_LAND_SNOW_ACCUM(SNOW_COMPONENT) \
				landSnowMask += (SNOW_COMPONENT) * weight * GetLandSnowMaskValue(landColor.w);
#		else
#			define LIGHTING_LAND_SNOW_ACCUM(SNOW_COMPONENT)
#		endif
#		define LIGHTING_LANDSCAPE_BLEND_ONE_LAYER(TILE, COLOR_TEX, COLOR_SAMP, NORM_TEX, NORM_SAMP, WEIGHT, SNOW_COMPONENT) \
			[branch] if ((WEIGHT) > 0.01)                                                                              \
			{                                                                                                          \
				float weight = WEIGHT;                                                                                 \
				float4 landColor = SampleTerrain(COLOR_TEX, COLOR_SAMP, uv, sharedOffset, LANDSCAPE_SAMPLE_ARG(TILE));     \
				float3 landColorRGB = landColor.rgb;                                                                   \
				float landAlpha = landColor.a;                                                                         \
				float4 landNormal = SampleTerrain(NORM_TEX, NORM_SAMP, uv, sharedOffset, LANDSCAPE_SAMPLE_ARG(TILE));      \
				float3 landNormalRGB = landNormal.rgb;                                                                 \
				float landNormalAlpha = landNormal.a;                                                                  \
				blendedRGB += landColorRGB * weight;                                                                   \
				blendedAlpha += landAlpha * weight;                                                                    \
				blendedNormalRGB += landNormalRGB * weight;                                                            \
				blendedNormalAlpha += landNormalAlpha * weight;                                                        \
				LIGHTING_LAND_SNOW_ACCUM(SNOW_COMPONENT)                                                               \
			}
#	endif

#endif  // LANDSCAPE

#endif  // __LIGHTING_LANDSCAPE_HLSLI__
