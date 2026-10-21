Texture2D<float4> ShadowMask : register(t0);
#if defined(TERRAIN_BLENDING)
Texture2D<float> DepthTexture : register(t1);
#else
Texture2D<unorm float> DepthTexture : register(t1);
#endif

#if defined(SHADOW_MASK_UNORM)
RWTexture2D<unorm float4> FilteredShadowMask : register(u0);
#else
RWTexture2D<float4> FilteredShadowMask : register(u0);
#endif

cbuffer ShadowMaskFilterCB : register(b0)
{
	float4 CameraData;
	float2 DepthScale;
	float2 RenderSize;
	float FarDepthValue;
	float3 pad0;
};

static const int2 PinwheelCorners[4] = {
	int2(1, 2),
	int2(-1, 1),
	int2(0, -1),
	int2(2, 0)
};

static const int2 CornerTexels[4] = {
	int2(-1, -1),
	int2(0, -1),
	int2(-1, 0),
	int2(0, 0)
};

int2 ClampToRender(int2 texel)
{
	return clamp(texel, 0, int2(RenderSize) - 1);
}

float LoadLinearDepth(int2 texel, out bool isSky)
{
	float depth = DepthTexture.Load(int3(int2((texel + 0.5) * DepthScale), 0));
	isSky = depth == FarDepthValue;
	return CameraData.w / (-depth * CameraData.z + CameraData.x);
}

float GetDepthGradient(float centerDepth, int2 texel, int2 axis)
{
	bool unusedSky;
	float backward = centerDepth - LoadLinearDepth(ClampToRender(texel - axis), unusedSky);
	float forward = LoadLinearDepth(ClampToRender(texel + axis), unusedSky) - centerDepth;
	return abs(backward) < abs(forward) ? backward : forward;
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= (uint2)RenderSize))
		return;

	int2 texel = int2(dispatchID.xy);
	float4 center = ShadowMask[texel];

	bool centerIsSky;
	float centerDepth = LoadLinearDepth(texel, centerIsSky);
	if (centerIsSky) {
		FilteredShadowMask[texel] = center;
		return;
	}

	float2 depthGradient = float2(GetDepthGradient(centerDepth, texel, int2(1, 0)), GetDepthGradient(centerDepth, texel, int2(0, 1)));

	float4 filtered = center;
	float weightSum = 1.0;

	[unroll] for (uint i = 0; i < 4; i++)
	{
		[unroll] for (uint j = 0; j < 4; j++)
		{
			int2 sampleTexel = ClampToRender(texel + PinwheelCorners[i] + CornerTexels[j]);

			bool sampleIsSky;
			float sampleDepth = LoadLinearDepth(sampleTexel, sampleIsSky);
			float predictedDepth = centerDepth + dot(float2(sampleTexel - texel), depthGradient);
			float residual = sampleDepth - predictedDepth;
			float tolerance = (residual < 0.0 ? 0.02 : 0.01) * centerDepth;
			float weight = sampleIsSky ? 0.0 : 0.4375 * saturate(1.0 - abs(residual) / max(tolerance, 1e-4));

			filtered += ShadowMask[sampleTexel] * weight;
			weightSum += weight;
		}
	}

	FilteredShadowMask[texel] = filtered / weightSum;
}
