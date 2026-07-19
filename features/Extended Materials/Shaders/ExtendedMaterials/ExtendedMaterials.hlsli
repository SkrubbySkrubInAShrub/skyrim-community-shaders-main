/**
 * @file ExtendedMaterials.hlsli
 * @brief Extended Materials parallax entry (EMAT).
 * @details Includes @ref ExtendedMaterialsTerrain.hlsli (LANDSCAPE) and
 *          @ref ExtendedMaterialsParallaxCore.hlsli.
 * @see https://github.com/tgjones/slimshader-cpp/blob/master/src/Shaders/Sdk/Direct3D11/DetailTessellation11/POM.hlsl
 * @see https://github.com/alandtse/SSEShaderTools/blob/main/shaders_vr/ParallaxEffect.h
 * @see https://github.com/marselas/Zombie-Direct3D-Samples/blob/5f53dc2d6f7deb32eb2e5e438d6b6644430fe9ee/Direct3D/ParallaxOcclusionMapping/ParallaxOcclusionMapping.fx
 * @see http://www.diva-portal.org/smash/get/diva2:831762/FULLTEXT01.pdf
 * @see https://bartwronski.files.wordpress.com/2014/03/ac4_gdc.pdf
 */

#ifndef EXTENDED_MATERIALS_HLSLI
#define EXTENDED_MATERIALS_HLSLI

/**
 * @brief Terrain Variation offsets, or a stub when TERRAIN_VARIATION is unset.
 */
#	if defined(LANDSCAPE)
#		if defined(TERRAIN_VARIATION)
#			include "TerrainVariation/TerrainVariation.hlsli"
#		else
struct StochasticOffsets
{
	float2 offset1;
	float2 offset2;
	float w1Contrast;
	float w2Contrast;
	float lodBlendWeight;
};
#		endif
#	endif

/** @brief Per-material displacement / parallax scale parameters. */
struct DisplacementParams
{
	float DisplacementScale;   /**< Multiplier around the 0.5 mid-level. */
	float DisplacementOffset;  /**< Additive offset after scale. */
	float HeightScale;         /**< Height slab scale for POM / shadows. */
	float FlattenAmount;       /**< Extra view-ray flatten (parallax warping fix). */
};

namespace ExtendedMaterials
{
	static const float ShadowIntensity = 2.0;
	static const float ParallaxCheapDistance = 1024.0;
	static const float ParallaxNearShadowQuality = 1.0;
	static const float ParallaxFarShadowQuality = 0.76;
	static const float TerrainParallaxShadowMaxMipLevel = 2.0;

	/**
	 * @brief Coarse height-map mip used during the POM ray march.
	 * @details Applies a distance bias and floors the result. Does not affect
	 *          contact / secant refine or height-blend samples (those use @ref GetMipLevel).
	 *          Grazing bias is intentionally omitted — it creates discontinuous hits.
	 * @param baseMip Floored mip from @ref GetMipLevel (includes MipBias).
	 * @param viewDist Camera distance; pass 0 to derive a far floor from @p baseMip.
	 */
	inline float ComputeParallaxMarchMip(float baseMip, float viewDist)
	{
		float m = baseMip + 1.0;
		m += min(2.0, baseMip * 0.5);
		float farFloor = viewDist > 0.0
			? lerp(0.0, 3.0, saturate((viewDist - 512.0) * rcp(1536.0)))
			: saturate(baseMip - 1.0);
		m = max(m, farFloor);
		return floor(m);
	}

	/**
	 * @brief Number of soft-shadow height taps for the given quality tier.
	 */
	inline uint ParallaxShadowTapCount(float quality)
	{
		uint taps = 1;
		if (quality > 0.25)
			taps++;
		if (quality > 0.5)
			taps++;
		if (quality > 0.75)
			taps++;
		return taps;
	}

	/** @brief Centers displacement around 0.5 and scales by HeightScale. */
	float ScaleDisplacement(float displacement, DisplacementParams params)
	{
		return (displacement - 0.5) * params.HeightScale;
	}

	/** @brief Applies DisplacementScale / DisplacementOffset in [0,1] height space. */
	float AdjustDisplacementNormalized(float displacement, DisplacementParams params)
	{
		return (displacement - 0.5) * params.DisplacementScale + 0.5 + params.DisplacementOffset;
	}

	/** @brief Per-component @ref AdjustDisplacementNormalized. */
	float4 AdjustDisplacementNormalized(float4 displacement, DisplacementParams params)
	{
		return float4(AdjustDisplacementNormalized(displacement.x, params), AdjustDisplacementNormalized(displacement.y, params), AdjustDisplacementNormalized(displacement.z, params), AdjustDisplacementNormalized(displacement.w, params));
	}

	/**
	 * @brief Floored anisotropic mip from UV derivatives, plus SharedData::MipBias.
	 */
	float GetMipLevel(float2 coords, Texture2D<float4> tex)
	{
		float2 textureDims;
		tex.GetDimensions(textureDims.x, textureDims.y);

#	if !defined(PARALLAX) && !defined(TRUE_PBR)
		textureDims /= 2.0;
#	endif

		float2 texCoordsPerSize = coords * textureDims;

		float2 dxSize = ddx(texCoordsPerSize);
		float2 dySize = ddy(texCoordsPerSize);

		float minTexCoordDelta = min(dot(dxSize, dxSize), dot(dySize, dySize));

		float mipLevel = max(0.5 * log2(minTexCoordDelta), 0);

#	if !defined(PARALLAX) && !defined(TRUE_PBR)
		mipLevel++;
#	endif

		return floor(max(mipLevel + SharedData::MipBias, 0));
	}

#	if defined(LANDSCAPE)
#		include "ExtendedMaterials/ExtendedMaterialsTerrain.hlsli"
#	endif
#	include "ExtendedMaterials/ExtendedMaterialsParallaxCore.hlsli"
}

#endif  // EXTENDED_MATERIALS_HLSLI
