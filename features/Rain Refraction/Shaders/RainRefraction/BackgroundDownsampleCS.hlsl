Texture2D<float4> SourceTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

SamplerState LinearSampler : register(s0);

cbuffer DownsampleCB : register(b0)
{
	float2 InvSourceSize;
	uint2 OutputSize;
	float2 SourceUVMax;
	float2 pad0;
};

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= OutputSize))
		return;

	float2 uv = min((dispatchID.xy * 2.0 + 1.0) * InvSourceSize, SourceUVMax);
	OutputTexture[dispatchID.xy] = float4(max(SourceTexture.SampleLevel(LinearSampler, uv, 0).rgb, 0.0), 1.0);
}
