Texture2D SourceTexture : register(t0);

cbuffer NightEyeParams : register(b0)
{
	float3 ColorBalance;
	float Amount;
	float Exposure;
	float Saturation;
	float Contrast;
	float Brightness;
	float LowClip;
	float3 Pad;
};

struct PS_INPUT
{
	float4 pos : SV_POSITION;
	float2 txcoord0 : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_TARGET
{
	float4 source = SourceTexture.Load(int3(input.pos.xy, 0));
	float3 color = source.rgb * Exposure;

	float3 removed = color - color * ColorBalance;
	color = color * ColorBalance + dot(removed, float3(1.0, 1.0, 1.0) / 3.0);

	float luminance = dot(color, float3(0.212656, 0.715158, 0.072186));
	color = lerp(luminance, color, Saturation);
	color = (color - 0.5) * Contrast + 0.5;
	color += Brightness * 0.1;

	float clip = LowClip * 0.1;
	color = max(color - clip, 0.0) / max(1.0 - clip, 0.0001);

	return float4(lerp(source.rgb, saturate(color), Amount), source.a);
}
