Texture2DArray<float> InputTexture : register(t0);
Texture2DArray<float> ESRAMShadow : register(t1);
RWTexture2D<float2> OutputTexture : register(u0);
SamplerState LinearSampler : register(s0);

float2 GetVSMMoments(in float depth)
{
	return float2(depth, depth * depth);
}

float2 ReduceMoments(float2 a, float2 b, float2 c, float2 d)
{
	return (a + b + c + d) * 0.25;
}

groupshared float2 g_scratchDepths[8][8];

#if defined(DOWNSAMPLE_SHADOW_MIP0)
static const uint CASCADE = 1;
#elif defined(DOWNSAMPLE_SHADOW_MIP1)
static const uint CASCADE = 0;
#endif

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 groupThreadID : SV_GroupThreadID) {
	uint inputW, inputH, inputSlices;
	InputTexture.GetDimensions(inputW, inputH, inputSlices);

	uint outputW, outputH;
	OutputTexture.GetDimensions(outputW, outputH);

	// Determine reduction levels from input/output ratio
	// Each thread covers threadSpan x threadSpan texels in 2x2 gathers, each group reduction handles another 2x.
	// A group reduces at most 8x, so past 16x each thread gathers more texels, keeping one group per output texel.
	// Must match the dispatch size in VolumetricShadows.cpp.
	uint totalReduction = inputW / outputW;
	uint threadSpan = max(totalReduction / 8, 2);
	uint groupSpan = totalReduction / threadSpan;
	uint groupReductions = 0;
	if (groupSpan >= 2)
		groupReductions = 1;
	if (groupSpan >= 4)
		groupReductions = 2;
	if (groupSpan >= 8)
		groupReductions = 3;

	uint2 pixCoord = dispatchThreadID.xy * threadSpan;

	// Gather from shadow cascades and mix with ESRAM shadow
	float2 vsmDepth = 0;
	for (uint gy = 0; gy < threadSpan; gy += 2) {
		for (uint gx = 0; gx < threadSpan; gx += 2) {
			float2 uv = (pixCoord + uint2(gx, gy) + 0.5) / float2(inputW, inputH);
			float4 depths = InputTexture.GatherRed(LinearSampler, float3(uv, CASCADE));
			float4 esramDepths = ESRAMShadow.GatherRed(LinearSampler, float3(uv, CASCADE));
			depths = min(depths, esramDepths);

			float2 gatherDepth = 0;
			for (uint i = 0; i < 4; i++)
				gatherDepth += GetVSMMoments(depths[i]);
			vsmDepth += gatherDepth * 0.25;
		}
	}
	if (threadSpan > 2)
		vsmDepth *= 4.0 / float(threadSpan * threadSpan);

	g_scratchDepths[groupThreadID.x][groupThreadID.y] = vsmDepth;

	GroupMemoryBarrierWithGroupSync();

	// First reduction: 2x2
	if (groupReductions >= 1) {
		if (all((groupThreadID.xy % 2) == 0)) {
			uint2 tid = groupThreadID.xy;
			g_scratchDepths[tid.x][tid.y] = ReduceMoments(
				g_scratchDepths[tid.x + 0][tid.y + 0],
				g_scratchDepths[tid.x + 1][tid.y + 0],
				g_scratchDepths[tid.x + 0][tid.y + 1],
				g_scratchDepths[tid.x + 1][tid.y + 1]);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	// Second reduction: 4x4
	if (groupReductions >= 2) {
		if (all((groupThreadID.xy % 4) == 0)) {
			uint2 tid = groupThreadID.xy;
			g_scratchDepths[tid.x][tid.y] = ReduceMoments(
				g_scratchDepths[tid.x + 0][tid.y + 0],
				g_scratchDepths[tid.x + 2][tid.y + 0],
				g_scratchDepths[tid.x + 0][tid.y + 2],
				g_scratchDepths[tid.x + 2][tid.y + 2]);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	// Third reduction: 8x8
	if (groupReductions >= 3) {
		if (all(groupThreadID.xy == 0)) {
			g_scratchDepths[0][0] = ReduceMoments(
				g_scratchDepths[0][0],
				g_scratchDepths[4][0],
				g_scratchDepths[0][4],
				g_scratchDepths[4][4]);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	// Write output - only threads aligned to the output grid
	uint outputDiv = max(groupSpan, 1);
	if (all((groupThreadID.xy % outputDiv) == 0)) {
		OutputTexture[dispatchThreadID.xy / outputDiv] = g_scratchDepths[groupThreadID.x][groupThreadID.y];
	}
}
