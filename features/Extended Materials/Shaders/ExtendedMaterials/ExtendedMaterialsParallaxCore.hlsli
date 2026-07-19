/**
 * @file ExtendedMaterialsParallaxCore.hlsli
 * @brief POM ray march / secant refine (included inside namespace ExtendedMaterials).
 */

#ifndef EXTENDED_MATERIALS_PARALLAX_CORE_HLSLI
#define EXTENDED_MATERIALS_PARALLAX_CORE_HLSLI

/**
 * @brief Parallax-occlusion UV offset.
 * @param pixelOffset Normalized hit depth along the height slab [0,1].
 * @param weights [LANDSCAPE] Updated layer weights when height blending is enabled.
 * @return Displaced texture coordinates.
 */
#if defined(LANDSCAPE)
	float2 GetParallaxCoords(PS_INPUT input, float2 coords, float mipLevels[6], float3 viewDir, float3x3 tbn, float noise, DisplacementParams params[6],
		StochasticOffsets sharedOffset,
		out float pixelOffset,
		out float weights[6])
#else
	float2 GetParallaxCoords(float2 coords, float mipLevel, float3 viewDir, float3x3 tbn, float noise, Texture2D<float4> tex, SamplerState texSampler, uint channel, DisplacementParams params, out float pixelOffset)
#endif
	{
		pixelOffset = 0.0;
		// viewDirTS is already unit length; its z is the view-vs-surface cosine directly.
		float3 viewDirTS = normalize(mul(tbn, viewDir));
		float ndotv = saturate(viewDirTS.z);

#if defined(LANDSCAPE)
		// Soft denom (same idea as meshes): true xy/z tracks tan(θ) and makes tall silhouettes
		// bob when the camera pans. FlattenAmount from EnableParallaxWarpingFix feeds in here.
		// UV-span stepping + contact refine still cover grazing; do not drop back to undersampling.
		float parallaxZ = max(abs(viewDirTS.z) * 0.7 + 0.3 + params[0].FlattenAmount, 0.0625);
		float2 parallaxDir = viewDirTS.xy / parallaxZ;
#else
		viewDirTS.xy /= viewDirTS.z * 0.7 + 0.3 + params.FlattenAmount;
		float2 parallaxDir = viewDirTS.xy;
#endif

#if defined(LANDSCAPE)
		// Dev softens vertex land weights when height blending is on (smoothstep), which reduces
		// hard triangle borders even when height maps are flat/invalid. Fade the soften with
		// distance like Dev; do not fade parallax UV itself (full-distance POM).
		float viewDist = length(input.WorldPosition.xyz);
		float nearBlendToFar = smoothstep(1024.0, 2048.0, viewDist);
		float blendFactor = SharedData::extendedMaterialSettings.EnableHeightBlending ? sqrt(saturate(1.0 - nearBlendToFar)) : 0.0;
		float4 w1 = lerp(input.LandBlendWeights1, smoothstep(0.0, 1.0, input.LandBlendWeights1), blendFactor);
		float2 w2 = lerp(input.LandBlendWeights2.xy, smoothstep(0.0, 1.0, input.LandBlendWeights2.xy), blendFactor);
		const float marchHeightBlendFactor = 0.0;

		// Default out weights; ray-march / secant paths overwrite when height blending is enabled.
		weights[0] = w1.x;
		weights[1] = w1.y;
		weights[2] = w1.z;
		weights[3] = w1.w;
		weights[4] = w2.x;
		weights[5] = w2.y;

#	if defined(TRUE_PBR)
		float scale = TerrainMaxWeightedHeightScaleW(w1, w2, params);
		float terrainHeightNormMul = rcp(max(scale, 1e-4));
		float maxHeight = 0.1 * scale;
#	else
		float scale = 1;
		float terrainHeightNormMul = 1.0;
		float maxHeight = 0.1 * scale;
#	endif
#else
		float scale = params.HeightScale;
		float maxHeight = 0.1 * scale;
#endif
		float minHeight = maxHeight * 0.5;

#if defined(LANDSCAPE) && defined(TRUE_PBR)
		if (scale <= 0.001) {
			pixelOffset = 0.0;
			if (SharedData::extendedMaterialSettings.EnableHeightBlending) {
				float unusedHeight;
				unusedHeight = GetTerrainHeight(noise, input, coords, mipLevels, params, blendFactor, w1, w2, sharedOffset, weights);
			}
			return coords;
		}
#elif !defined(LANDSCAPE)
		if (scale <= 0.001) {
			pixelOffset = 0.0;
			return coords;
		}
#endif

		{
			const uint minSteps = 4;
#if defined(LANDSCAPE)
			const uint maxStepsCap = 64;
#else
			const uint maxStepsCap = 64;
			const float baseMaxSteps = 8;
#endif

			// Quadratic grazing: head-on ~0, only steep angles pay for extra steps.
			float grazing = (1.0 - ndotv);
			grazing *= grazing;

#if defined(LANDSCAPE)
			// March height mips: +1, strong distance bias, far floor, floor(m).
			// Contact/secant use unbiased mipLevels (asymmetric — refine stays sharp).
			float marchMip = ComputeParallaxMarchMip(mipLevels[0], viewDist);
			float marchMipLevels[6];
			float parallaxLODMip = mipLevels[0];
			[unroll] for (uint mi = 0; mi < 6; mi++)
				marchMipLevels[mi] = marchMip;
#else
			float parallaxLODMip = mipLevel;
			float marchMipLevel = ComputeParallaxMarchMip(mipLevel, 0.0);
#endif
			float distStepScale = lerp(0.25, 1.0, saturate((3.0 - parallaxLODMip) * (1.0 / 3.0)));

#if defined(LANDSCAPE)
			// Close grazing UV gaps: size steps so each sample spans a few texels, not a cliff-sized jump.
			// Bigger depth steps make ovals worse — |parallaxDir| grows as 1/N·V, so we need more steps.
			float2 texDim;
			TexColorSampler.GetDimensions(texDim.x, texDim.y);
			float uvMarchSpan = dot(abs(parallaxDir), maxHeight + minHeight);
			float texelsPerStep = lerp(3.5, 1.75, saturate(grazing));
			uint uvSteps = (uint)(uvMarchSpan * max(texDim.x, texDim.y) * rcp(texelsPerStep) * distStepScale + 0.5);
			uint angleSteps = (uint)(lerp((float)minSteps, (float)maxStepsCap, grazing) * distStepScale + 0.5);
			uint numSteps = max(minSteps, max(uvSteps, angleSteps));
			numSteps = min(numSteps, maxStepsCap);
			numSteps = (numSteps + 2) & ~3;
#else
			float grazingStepBoost = lerp(1.0, 1.65, grazing);
			float angleStepMul = clamp(0.5 * rcp(max(ndotv, 0.0625)), 0.5, 2.5);
			uint numSteps = max(minSteps, (uint)(scale * baseMaxSteps * angleStepMul * distStepScale * grazingStepBoost));
			numSteps = min(numSteps, maxStepsCap);
			numSteps = (numSteps + 2) & ~3;
#endif

			// Binary contact refine + secant: continuous hit within the bracket (no dither).
			uint contactIters = grazing > 0.2 ? 4u : 2u;
			uint secantIters = grazing > 0.25 ? 2u : 1u;

			float stepSize = rcp((float)numSteps);

			float2 offsetPerStep = parallaxDir * maxHeight * stepSize;
			float2 prevOffset = parallaxDir * minHeight + coords.xy;
			float prevBound = 1.0;
			float prevHeight = 1.0;

			float2 pt1 = 0;
			float2 pt2 = 0;
			bool intersectionFound = false;

			[loop] while (numSteps > 0)
			{
				float4 currentOffset[2];
				currentOffset[0] = prevOffset.xyxy - float4(1, 1, 2, 2) * offsetPerStep.xyxy;
				currentOffset[1] = prevOffset.xyxy - float4(3, 3, 4, 4) * offsetPerStep.xyxy;
				float4 currentBound = prevBound.xxxx - float4(1, 2, 3, 4) * stepSize;

				float4 currHeight;
#if defined(LANDSCAPE)
				currHeight = GetTerrainHeightQuadRayMarch(noise, input, currentOffset[0].xy, currentOffset[0].zw, currentOffset[1].xy, currentOffset[1].zw, marchMipLevels, params, marchHeightBlendFactor, w1, w2, sharedOffset, weights) * terrainHeightNormMul + 0.5;
#else
				currHeight.x = tex.SampleLevel(texSampler, currentOffset[0].xy, marchMipLevel)[channel];
				currHeight.y = tex.SampleLevel(texSampler, currentOffset[0].zw, marchMipLevel)[channel];
				currHeight.z = tex.SampleLevel(texSampler, currentOffset[1].xy, marchMipLevel)[channel];
				currHeight.w = tex.SampleLevel(texSampler, currentOffset[1].zw, marchMipLevel)[channel];

				currHeight = AdjustDisplacementNormalized(currHeight, params);
#endif

				bool4 testResult = currHeight >= currentBound;
				[branch] if (any(testResult))
				{
					intersectionFound = true;
					float2 outOffset;
					[branch] if (testResult.x)
					{
						outOffset = prevOffset;
						pt1 = float2(currentBound.x, currHeight.x);
						pt2 = float2(prevBound, prevHeight);
					}
					else if (testResult.y)
					{
						outOffset = currentOffset[0].xy;
						pt1 = float2(currentBound.y, currHeight.y);
						pt2 = float2(currentBound.x, currHeight.x);
					}
					else if (testResult.z)
					{
						outOffset = currentOffset[0].zw;
						pt1 = float2(currentBound.z, currHeight.z);
						pt2 = float2(currentBound.y, currHeight.y);
					}
					else
					{
						outOffset = currentOffset[1].xy;
						pt1 = float2(currentBound.w, currHeight.w);
						pt2 = float2(currentBound.z, currHeight.z);
					}
					prevOffset = outOffset;
					break;
				}

				prevOffset = currentOffset[1].zw;
				prevBound = currentBound.w;
				prevHeight = currHeight.w;
				numSteps -= 4;
			}

			float parallaxAmount = 0.0;
			[branch] if (intersectionFound)
			{
				float tNear = pt1.x;
				float hNear = pt1.y;
				float fNear = hNear - tNear;
				float tFar = pt2.x;
				float hFar = pt2.y;
				float fFar = hFar - tFar;

				// Binary contact refinement: shrink the coarse step bracket before secant.
				[loop] for (uint c = 0; c < contactIters; c++)
				{
					float tMid = 0.5 * (tNear + tFar);
					float2 midCoords = coords.xy + parallaxDir * (((1.0 - tMid) * -maxHeight) + minHeight);
					float hMid = 0.0;
#if defined(LANDSCAPE)
					hMid = GetTerrainHeight(noise, input, midCoords, mipLevels, params, marchHeightBlendFactor, w1, w2, sharedOffset, weights) * terrainHeightNormMul + 0.5;
#else
					hMid = tex.SampleLevel(texSampler, midCoords, mipLevel)[channel];
					hMid = AdjustDisplacementNormalized(hMid, params);
#endif
					float fMid = hMid - tMid;
					[branch] if (fMid >= 0.0)
					{
						tNear = tMid;
						hNear = hMid;
						fNear = fMid;
					}
					else
					{
						tFar = tMid;
						hFar = hMid;
						fFar = fMid;
					}
				}

				[loop] for (uint i = 0; i < secantIters; i++)
				{
					float denominator = fNear - fFar;
					float r = abs(denominator) > EPSILON_DIVISION ? saturate(fNear / denominator) : 0.5;
					float tSecant = lerp(tNear, tFar, r);
					float2 secantCoords = coords.xy + parallaxDir * (((1.0 - tSecant) * -maxHeight) + minHeight);

					float hSecant = 0.0;
#if defined(LANDSCAPE)
					hSecant = GetTerrainHeight(noise, input, secantCoords, mipLevels, params, marchHeightBlendFactor, w1, w2, sharedOffset, weights) * terrainHeightNormMul + 0.5;
#else
					hSecant = tex.SampleLevel(texSampler, secantCoords, mipLevel)[channel];
					hSecant = AdjustDisplacementNormalized(hSecant, params);
#endif

					float fSecant = hSecant - tSecant;
					[branch] if (fSecant >= 0.0)
					{
						tNear = tSecant;
						hNear = hSecant;
						fNear = fSecant;
					}
					else
					{
						tFar = tSecant;
						hFar = hSecant;
						fFar = fSecant;
					}
				}

				float denominator = fNear - fFar;
				float r = abs(denominator) > EPSILON_DIVISION ? saturate(fNear / denominator) : 0.5;
				parallaxAmount = lerp(tNear, tFar, r);
			}

			float offset = (1.0 - parallaxAmount) * -maxHeight + minHeight;
			pixelOffset = saturate(parallaxAmount);
			float2 finalCoords = parallaxDir * offset + coords.xy;
#if defined(LANDSCAPE)
			if (SharedData::extendedMaterialSettings.EnableHeightBlending) {
				float unusedHeight;
				// Softened w1/w2 + distance-scaled blendFactor (Dev behaviour). Height sharpening
				// still runs when maps have real variation; flat maps keep the smoothstep soften.
				unusedHeight = GetTerrainHeight(noise, input, finalCoords, mipLevels, params, blendFactor, w1, w2, sharedOffset, weights);
			}
#endif
			return finalCoords;
		}
	}

#	if !defined(LANDSCAPE)
	/**
	 * @brief Approximate soft shadow from a height map along light L.
	 * @see https://advances.realtimerendering.com/s2006/Tatarchuk-POM.pdf
	 */
	float GetParallaxSoftShadowMultiplier(float2 coords, float mipLevel, float3 L, float sh0, Texture2D<float4> tex, SamplerState texSampler, uint channel, float quality, float noise, DisplacementParams params)
	{
		[branch] if (quality > 0.0)
		{
			uint tapCount = ParallaxShadowTapCount(quality);
			float shadowStrength = ShadowIntensity * (4.0 / tapCount);
			float2 rayDir = L.xy * 0.1 * params.HeightScale;
			float4 multipliers = rcp((float4(1, 2, 3, 4) + noise));
			float4 sh = sh0.xxxx;
			sh.x = AdjustDisplacementNormalized(tex.SampleLevel(texSampler, coords + rayDir * multipliers.x, mipLevel)[channel], params);
			if (quality > 0.25)
				sh.y = AdjustDisplacementNormalized(tex.SampleLevel(texSampler, coords + rayDir * multipliers.y, mipLevel)[channel], params);
			if (quality > 0.5)
				sh.z = AdjustDisplacementNormalized(tex.SampleLevel(texSampler, coords + rayDir * multipliers.z, mipLevel)[channel], params);
			if (quality > 0.75)
				sh.w = AdjustDisplacementNormalized(tex.SampleLevel(texSampler, coords + rayDir * multipliers.w, mipLevel)[channel], params);
			return 1.0 - saturate(dot(max(0, sh - sh0), shadowStrength));
		}
		return 1.0;
	}

#	endif

#endif  // EXTENDED_MATERIALS_PARALLAX_CORE_HLSLI
