// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Snow Trenches on Objects, the readers: Lighting's bare floor
// (SnowLighting.hlsli) and the drape's print twin (SnowStaticsShell.hlsl,
// SNOW_OBJECT_PRINTS). The map is DeformationUpdateCS.hlsl's PrintMap; its
// frame this frame is SharedData's PrintRect.

#ifndef __SNOW_PRINTS_HLSLI__
#define __SNOW_PRINTS_HLSLI__

namespace SnowPrints
{
	// Two slots of (world Z in quarter units << 10 | depth in 1023rds) per
	// world texel, texel (x, y) at (x, y) & (dim - 1); 0 is empty.
	Texture2DArray<uint> PrintMap : register(t114);
	// One texel per 32 x 32 tile on the same world-anchored grid: nonzero where
	// that tile, or the next one along x, y or both, holds prints.
	Texture2D<uint> PrintTiles : register(t115);

	// Mirror PRINT_TEXEL, PRINT_Z_OFFSET and PRINT_TILE_SHIFT.
	static const float kTexel = 3.0;
	static const float kZOffset = 524288.0;
	static const int kTileShift = 5;

	// One texel's depth for a surface at a_z: a slot counts in full within 6
	// units of its own Z and not past 12, so a print on another level or
	// another object adds nothing.
	float TexelDepth(int2 a_texel, int a_mask, float a_z)
	{
		const int2 phys = a_texel & a_mask;
		const uint a = PrintMap.Load(int4(phys, 0, 0));
		const uint b = PrintMap.Load(int4(phys, 1, 0));
		const float za = float(a >> 10) * 0.25 - kZOffset;
		const float zb = float(b >> 10) * 0.25 - kZOffset;
		return max(float(a & 1023u) * saturate(2.0 - abs(a_z - za) / 6.0), float(b & 1023u) * saturate(2.0 - abs(a_z - zb) / 6.0)) * (1.0 / 1023.0);
	}

	// The map's address mask where texels a_t0 to a_t0 + a_span lie inside this
	// frame's rect and their tile holds prints; -1 where there is nothing to read.
	int Mask(int2 a_t0, int a_span)
	{
		const int4 rect = SharedData::snowDeformationSettings.PrintRect;
		int mask = -1;
		[branch] if (all(a_t0 >= rect.xy) && all(a_t0 + a_span < rect.zw))
		{
			uint3 dims;
			PrintMap.GetDimensions(dims.x, dims.y, dims.z);
			const int m = int(dims.x) - 1;
			[flatten] if (PrintTiles.Load(int3((a_t0 >> kTileShift) & (m >> kTileShift), 0)) != 0u)
				mask = m;
		}
		return mask;
	}

	// Bilinear print depth, 0-1, at a point on a surface (absolute world).
	float Depth(float3 a_world)
	{
		const float2 tf = a_world.xy / kTexel - 0.5;
		const float2 t0f = floor(tf);
		const int2 t0 = int2(t0f);
		const int mask = Mask(t0, 1);
		float d = 0.0;
		[branch] if (mask >= 0)
		{
			const float2 f = tf - t0f;
			d = lerp(lerp(TexelDepth(t0, mask, a_world.z), TexelDepth(t0 + int2(1, 0), mask, a_world.z), f.x),
				lerp(TexelDepth(t0 + int2(0, 1), mask, a_world.z), TexelDepth(t0 + int2(1, 1), mask, a_world.z), f.x), f.y);
		}
		return d;
	}

	// How much of the game's projected snow a print takes off a surface point:
	// the core of a print, judged against a full print's stored depth (Snow
	// Resistance's share), and none as the resistance nears 100 %.
	float Bare(float3 a_world)
	{
		const float full = SharedData::snowDeformationSettings.PrintFullDepth;
		float bare = 0.0;
		[branch] if (full > 0.001)
			bare = smoothstep(0.55, 0.85, Depth(a_world) / full) * saturate(4.0 * full);
		return bare;
	}

	// Bilinear on a 3 x 3 block of rows r0-r2 at a block position in [0, 2].
	float Bilinear3(float3 a_r0, float3 a_r1, float3 a_r2, float2 a_q)
	{
		const float2 i = min(floor(a_q), 1.0);
		const float2 f = a_q - i;
		const float3 lo = i.y < 0.5 ? a_r0 : a_r1;
		const float3 hi = i.y < 0.5 ? a_r1 : a_r2;
		const float2 a = i.x < 0.5 ? lo.xy : lo.yz;
		const float2 b = i.x < 0.5 ? hi.xy : hi.yz;
		return lerp(lerp(a.x, a.y, f.x), lerp(b.x, b.y, f.x), f.y);
	}

	// Depth at a surface point and its world slope, x = depth, yz = d/dx, d/dy:
	// central differences half a texel each way, which stay continuous across
	// texel edges where the bilinear's own slope steps. One height-tested 3 x 3
	// block serves all five reads.
	float3 DepthSlope(float3 a_world)
	{
		const float2 tf = a_world.xy / kTexel - 0.5;
		const float2 s0f = floor(tf - 0.5);
		const int2 s0 = int2(s0f);
		const int mask = Mask(s0, 2);
		float3 r = 0.0;
		[branch] if (mask >= 0)
		{
			float3 v[3];
			[unroll] for (int y = 0; y < 3; y++)
				v[y] = float3(TexelDepth(s0 + int2(0, y), mask, a_world.z), TexelDepth(s0 + int2(1, y), mask, a_world.z),
					TexelDepth(s0 + int2(2, y), mask, a_world.z));
			const float2 q = tf - s0f;
			r.x = Bilinear3(v[0], v[1], v[2], q);
			r.y = (Bilinear3(v[0], v[1], v[2], q + float2(0.5, 0.0)) - Bilinear3(v[0], v[1], v[2], q - float2(0.5, 0.0))) / kTexel;
			r.z = (Bilinear3(v[0], v[1], v[2], q + float2(0.0, 0.5)) - Bilinear3(v[0], v[1], v[2], q - float2(0.0, 0.5))) / kTexel;
		}
		return r;
	}

	// Sun visibility through a print's dent from a point a_h below the
	// surface's tangent plane (slope a_g; the dent lies -a_dentDepth x depth
	// under it): four taps toward the sun until the ray clears the untouched
	// surface, each tap's overshoot weighed against its distance as in
	// TrenchDetailSunShadow, the worst scaled by a_strength. 1 = lit.
	float DentSunShadow(float3 a_world, float a_h, float3 L, float2 a_g, float a_dentDepth, float a_strength, float a_noise)
	{
		const float climb = L.z - dot(L.xy, a_g);
		float lit = 1.0;
		[branch] if (climb > 0.01 && a_h < -0.01)
		{
			const float reach = min(-a_h / climb, 16.0);
			float occlusion = 0.0;
			[loop] for (float i = 0.0; i < 4.0; i += 1.0)
			{
				const float s = reach * (i + 0.1 + 0.9 * a_noise) / 4.0;
				const float2 step = L.xy * s;
				const float h = -a_dentDepth * Depth(float3(a_world.xy + step, a_world.z + dot(step, a_g)));
				occlusion = max(occlusion, (h - (a_h + climb * s)) / (0.2 * s + 0.05));
			}
			lit = 1.0 - saturate(occlusion * a_strength);
		}
		return lit;
	}
}

#endif  // __SNOW_PRINTS_HLSLI__
