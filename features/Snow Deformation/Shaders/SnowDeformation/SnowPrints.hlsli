// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Snow Trenches on Objects, the readers: Lighting's bare floor
// (SnowLighting.hlsli), the drape's print twin (SnowStaticsShell.hlsl,
// SNOW_OBJECT_PRINTS), and the volume's PS (VOXEL), which has them for when
// it follows the projected snow onto objects (VOLUME-SNOW.md). The map is
// DeformationUpdateCS.hlsl's PrintMap; its frame this frame is SharedData's
// PrintRect. The rim (SNOW_PRINT_RIM) is PrintRimCS's.

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

	// Bilinear print depth, 0-1, at a point on a surface (absolute world): the
	// dent shadow's taps.
	float DepthFast(float3 a_world)
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

	// Cubic B-spline weights at a fraction, for the texels -1 to +2 around it.
	float4 BSpline(float a_f)
	{
		const float f2 = a_f * a_f;
		const float f3 = f2 * a_f;
		return float4(1.0 - 3.0 * a_f + 3.0 * f2 - f3, 4.0 - 6.0 * f2 + 3.0 * f3, 1.0 + 3.0 * a_f + 3.0 * f2 - 3.0 * f3, f3) * (1.0 / 6.0);
	}

	// The weights' derivatives.
	float4 BSplineSlope(float a_f)
	{
		const float f2 = a_f * a_f;
		return float4(-0.5 + a_f - 0.5 * f2, -2.0 * a_f + 1.5 * f2, 0.5 + a_f - 1.5 * f2, 0.5 * f2);
	}

	// Row a_y of the 4 x 4 block around a_t0: texels a_t0 + (-1 to 2, a_y).
	float4 BlockRow(int2 a_t0, int a_y, int a_mask, float a_z)
	{
		return float4(TexelDepth(a_t0 + int2(-1, a_y), a_mask, a_z), TexelDepth(a_t0 + int2(0, a_y), a_mask, a_z),
			TexelDepth(a_t0 + int2(1, a_y), a_mask, a_z), TexelDepth(a_t0 + int2(2, a_y), a_mask, a_z));
	}

	// Print depth, 0-1, at a point on a surface, through a cubic B-spline over
	// the 4 x 4 height-tested texels, as the ground's trench map is read: the
	// grid's steps come out as smooth curves. The inner 2 x 2 goes first; when
	// it holds nothing the outer ring gives at most 0.31 of its deepest texel,
	// under Bare's lowest threshold, so it is skipped.
	float Depth(float3 a_world)
	{
		const float2 tf = a_world.xy / kTexel - 0.5;
		const float2 t0f = floor(tf);
		const int2 t0 = int2(t0f);
		const int mask = Mask(t0 - 1, 3);
		float d = 0.0;
		[branch] if (mask >= 0)
		{
			const float2 inner0 = float2(TexelDepth(t0, mask, a_world.z), TexelDepth(t0 + int2(1, 0), mask, a_world.z));
			const float2 inner1 = float2(TexelDepth(t0 + int2(0, 1), mask, a_world.z), TexelDepth(t0 + int2(1, 1), mask, a_world.z));
			[branch] if (any(max(inner0, inner1) > 0.0))
			{
				const float2 f = tf - t0f;
				const float4 wx = BSpline(f.x);
				const float4 r1 = float4(TexelDepth(t0 + int2(-1, 0), mask, a_world.z), inner0, TexelDepth(t0 + int2(2, 0), mask, a_world.z));
				const float4 r2 = float4(TexelDepth(t0 + int2(-1, 1), mask, a_world.z), inner1, TexelDepth(t0 + int2(2, 1), mask, a_world.z));
				const float4 across = float4(dot(wx, BlockRow(t0, -1, mask, a_world.z)), dot(wx, r1), dot(wx, r2), dot(wx, BlockRow(t0, 2, mask, a_world.z)));
				d = dot(BSpline(f.y), across);
			}
		}
		return d;
	}

	// Print depth at a surface point as a share of a full print's (Snow
	// Resistance's); 0 where none.
	float Share(float3 a_world)
	{
		const float full = SharedData::snowDeformationSettings.PrintFullDepth;
		float share = 0.0;
		[branch] if (full > 0.001)
			share = Depth(a_world) / full;
		return share;
	}

	// How much of the game's projected snow a print takes off at a depth
	// share, a_noise the game's own projection noise there: the edge breaks
	// up in the noise's flecks within a few units of the sole's outline, the
	// last snow on low noise as at the projection's own edge. None as the
	// resistance nears 100 %.
	float Bare(float a_share, float a_noise)
	{
		// The threshold: noise at its 5th percentile (0.05) keeps its snow to a
		// share of 0.85, at its 95th (0.45) gives it up at 0.38. Never below
		// 0.36, clear of the 0.31 Depth's early-out can drop.
		const float noiseLo = 0.05;
		const float noiseHi = 0.45;
		const float soft = 0.03;
		const float thr = max(lerp(0.85, 0.38, (a_noise - noiseLo) / (noiseHi - noiseLo)), 0.36);
		return smoothstep(thr - soft, thr + soft, a_share) * saturate(4.0 * SharedData::snowDeformationSettings.PrintFullDepth);
	}

	// Depth at a surface point and its world slope, x = depth, yz = d/dx,
	// d/dy: Depth's B-spline and its exact gradient, smooth across texel edges.
	float3 DepthSlope(float3 a_world)
	{
		const float2 tf = a_world.xy / kTexel - 0.5;
		const float2 t0f = floor(tf);
		const int2 t0 = int2(t0f);
		const int mask = Mask(t0 - 1, 3);
		float3 r = 0.0;
		[branch] if (mask >= 0)
		{
			const float2 f = tf - t0f;
			const float4 wx = BSpline(f.x);
			const float4 sx = BSplineSlope(f.x);
			const float4 r0 = BlockRow(t0, -1, mask, a_world.z);
			const float4 r1 = BlockRow(t0, 0, mask, a_world.z);
			const float4 r2 = BlockRow(t0, 1, mask, a_world.z);
			const float4 r3 = BlockRow(t0, 2, mask, a_world.z);
			const float4 across = float4(dot(wx, r0), dot(wx, r1), dot(wx, r2), dot(wx, r3));
			r.x = dot(BSpline(f.y), across);
			r.y = dot(BSpline(f.y), float4(dot(sx, r0), dot(sx, r1), dot(sx, r2), dot(sx, r3))) / kTexel;
			r.z = dot(BSplineSlope(f.y), across) / kTexel;
		}
		return r;
	}

#ifdef SNOW_PRINT_RIM
	// The rim map (DeformationUpdateCS.hlsl PrintRimCS): how strongly the snow
	// around a print is disturbed, packed and addressed as the map, one
	// surface a texel.
	Texture2D<uint> PrintRim : register(t116);
	// Mirror PRINT_RIM_APRON: texels a rim reaches past its print.
	static const int kRimApron = 5;

	float RimTexel(int2 a_texel, int a_mask, float a_z)
	{
		const uint v = PrintRim.Load(int3(a_texel & a_mask, 0));
		const float z = float(v >> 10) * 0.25 - kZOffset;
		return float(v & 1023u) * saturate(2.0 - abs(a_z - z) / 6.0) * (1.0 / 1023.0);
	}

	// Rim strength, 0-1 as a print's depth, at a point on a surface: bilinear
	// over this surface's rim texels. The tile mask is read where the rim's
	// farthest print could lie: that tile and the three after it cover them all.
	float Rim(float3 a_world)
	{
		const float2 tf = a_world.xy / kTexel - 0.5;
		const float2 t0f = floor(tf);
		const int2 t0 = int2(t0f);
		const int4 rect = SharedData::snowDeformationSettings.PrintRect;
		float r = 0.0;
		[branch] if (all(t0 >= rect.xy) && all(t0 + 1 < rect.zw))
		{
			uint3 dims;
			PrintMap.GetDimensions(dims.x, dims.y, dims.z);
			const int m = int(dims.x) - 1;
			[branch] if (PrintTiles.Load(int3(((t0 - kRimApron) >> kTileShift) & (m >> kTileShift), 0)) != 0u)
			{
				const float2 f = tf - t0f;
				r = lerp(lerp(RimTexel(t0, m, a_world.z), RimTexel(t0 + int2(1, 0), m, a_world.z), f.x),
					lerp(RimTexel(t0 + int2(0, 1), m, a_world.z), RimTexel(t0 + int2(1, 1), m, a_world.z), f.x), f.y);
			}
		}
		return r;
	}
#endif

	// Sun visibility through a print's dent at a point on it: the dent lies
	// -a_dentDepth x depth under the surface's tangent plane (slope a_g). Four
	// taps toward the sun until the ray clears the untouched surface, each
	// tap's overshoot weighed against its distance as in TrenchDetailSunShadow,
	// the worst scaled by a_strength. The point and the taps read one filter,
	// so the dent cannot shadow itself where two filters disagree. 1 = lit.
	float DentSunShadow(float3 a_world, float3 L, float2 a_g, float a_dentDepth, float a_strength, float a_noise)
	{
		const float climb = L.z - dot(L.xy, a_g);
		const float h0 = -a_dentDepth * DepthFast(a_world);
		float lit = 1.0;
		[branch] if (climb > 0.01 && h0 < -0.01)
		{
			const float reach = min(-h0 / climb, 16.0);
			float occlusion = 0.0;
			[loop] for (float i = 0.0; i < 4.0; i += 1.0)
			{
				const float s = reach * (i + 0.1 + 0.9 * a_noise) / 4.0;
				const float2 step = L.xy * s;
				const float h = -a_dentDepth * DepthFast(float3(a_world.xy + step, a_world.z + dot(step, a_g)));
				occlusion = max(occlusion, (h - (h0 + climb * s)) / (0.2 * s + 0.05));
			}
			lit = 1.0 - saturate(occlusion * a_strength);
		}
		return lit;
	}
}

#endif  // __SNOW_PRINTS_HLSLI__
