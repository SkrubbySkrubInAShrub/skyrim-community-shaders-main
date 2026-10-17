
#include "Common/SharedData.hlsli"

#include "ScreenSpaceShadows/bend_sss_gpu.hlsli"

// Match bend_sss_gpu.hlsli's struct field types for assignment compatibility.
// TERRAIN_BLENDING ON  -> R32_FLOAT (no unorm). OFF -> R24_UNORM_X8_TYPELESS (unorm).
#if defined(TERRAIN_BLENDING)
Texture2D<float> DepthTexture : register(t0);  // Depth Buffer Texture (R32_FLOAT)
#else
Texture2D<unorm float> DepthTexture : register(t0);  // Depth Buffer Texture (R24_UNORM_X8_TYPELESS)
#endif
RWTexture2D<unorm float2> OutputTexture : register(u0);  // Front- and back-facing screen-space shadow visibility (R8G8_UNORM)
SamplerState PointBorderSampler : register(s0);          // A point sampler, with Wrap Mode set to Clamp-To-Border-Color (D3D12_TEXTURE_ADDRESS_MODE_BORDER), and Border Color set to "FarDepthValue" (typically zero), or some other far-depth value out of DepthBounds.
														 // If you have issues where invalid shadows are appearing from off-screen, it is likely that this sampler is not correctly setup
cbuffer PerFrame : register(b1)
{
	// Runtime data returned from BuildDispatchList():
	float4 LightCoordinate;  // Values stored in DispatchList::LightCoordinate_Shader by BuildDispatchList()
	int2 WaveOffset;         // Values stored in DispatchData::WaveOffset_Shader by BuildDispatchList()

	// Renderer Specific Values:
	float FarDepthValue;   // Set to the Depth Buffer Value for the far clip plane, as determined by renderer projection matrix setup (typically 0).
	float NearDepthValue;  // Set to the Depth Buffer Value for the near clip plane, as determined by renderer projection matrix setup (typically 1).

	// Sampling data:
	float2 InvDepthTextureSize;  // Inverse of the texture dimensions for 'DepthTexture' (used to convert from pixel coordinates to UVs)
								 // If 'PointBorderSampler' is an Unnormalized sampler, then this value can be hard-coded to 1.
								 // The 'USE_HALF_PIXEL_OFFSET' macro might need to be defined if sampling at exact pixel coordinates isn't precise (e.g., if odd patterns appear in the shadow).

	float2 DynamicRes;

	float SurfaceThickness;
	float BilinearThreshold;
	float ShadowContrast;
};

#if defined(SSS_CHANGED_TILES)
// A repeat march over a depth that differs from the frame's first march only
// in the 8x8 tiles marked here (each mark covering its neighbours): a group
// whose depth reads all land on unmarked tiles writes what the first march
// already wrote, so it returns.
Texture2D<uint> ChangedTiles : register(t1);
groupshared uint gReadsChanged;

// The group's reads are one line, pixel_xy(thread step 0) - xy_delta * w over
// READ_COUNT wave lengths of w. Samples at most 7.2 px apart per axis leave
// every read, bilinear neighbour included, within 8 texels of one: inside
// the marks' one-tile reach.
bool ReadsChangedDepth(DispatchParameters inParameters, int3 inGroupID, int inGroupThreadID)
{
	half2 xy_delta;
	half2 start_xy;
	half pixel_distance;
	bool x_axis_major;
	ComputeWavefrontExtents(inParameters, inGroupID, inParameters.LightCoordinate.w > 0.0f ? 0 : WAVE_SIZE - 1, xy_delta, start_xy, pixel_distance, x_axis_major);
#	if defined(RIGHT)
	start_xy.x += 1.0 / inParameters.InvDepthTextureSize.x;
#	endif
	uint2 dims;
	DepthTexture.GetDimensions(dims.x, dims.y);
	uint2 tiles;
	ChangedTiles.GetDimensions(tiles.x, tiles.y);
	const float2 toTexel = inParameters.InvDepthTextureSize * inParameters.DynamicRes * float2(dims);
	if (any(toTexel > 1.001))
		return true;
	const float2 toTile = toTexel * 0.125;
	const float span = READ_COUNT - 1.0 / WAVE_SIZE;
	const float lo = -inParameters.LightCoordinate.w < 0.0 ? 0.0 : 1.0 - READ_COUNT;
	const float step = 7.2 / max(abs(xy_delta.x), abs(xy_delta.y));
	const uint count = (uint)ceil(span / step) + 1;
	bool changed = false;
	[loop] for (uint k = inGroupThreadID; k < count; k += WAVE_SIZE)
	{
		const float2 read_xy = floor(start_xy - xy_delta * (lo + min(k * step, span))) + 0.5;
		const int2 tile = clamp(int2(floor(read_xy * toTile)), 0, int2(tiles) - 1);
		changed = changed || ChangedTiles.Load(int3(tile, 0)) != 0;
	}
	return changed;
}
#endif

[numthreads(WAVE_SIZE, 1, 1)] void main(
	int3 groupID : SV_GroupID,
	int groupThreadID : SV_GroupThreadID) {
	DispatchParameters parameters;
	parameters.SetDefaults();

	parameters.LightCoordinate = LightCoordinate;
	parameters.WaveOffset = WaveOffset;
	parameters.FarDepthValue = 1;
	parameters.NearDepthValue = 0;
	parameters.InvDepthTextureSize = InvDepthTextureSize;
	parameters.DepthTexture = DepthTexture;
	parameters.OutputTexture = OutputTexture;
	parameters.PointBorderSampler = PointBorderSampler;

	parameters.SurfaceThickness = SurfaceThickness;
	parameters.BilinearThreshold = BilinearThreshold;
	parameters.ShadowContrast = ShadowContrast;

	parameters.DynamicRes = DynamicRes;

	parameters.UsePrecisionOffset = true;

#if defined(SSS_CHANGED_TILES)
	if (groupThreadID == 0)
		gReadsChanged = 0;
	GroupMemoryBarrierWithGroupSync();
	if (ReadsChangedDepth(parameters, groupID, groupThreadID))
		InterlockedOr(gReadsChanged, 1u);
	GroupMemoryBarrierWithGroupSync();
	if (gReadsChanged == 0)
		return;
#endif

	WriteScreenSpaceShadow(parameters, groupID, groupThreadID);
}
