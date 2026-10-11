// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Landscape snow: the game's own landscape draws, raised onto the ground
// shell's surface (SnowShell.hlsl in surface-only mode). CS's landscape VS
// (Lighting.hlsl or Utility.hlsl, main renamed) runs per vertex: LS_VSRaise on
// the draw's own vertices, LS_DS on the vertices LS_HS tessellates. Compiled
// per (shader type, VS descriptor) with CS's defines; LS_LIGHTING selects
// Lighting.hlsl.

#ifndef VSHADER
#	define VSHADER
#endif

#define main LS_GameVS
#if defined(LS_LIGHTING)
#	include "Lighting.hlsl"
#else
#	include "Utility.hlsl"
#endif
#undef main

#if defined(SKINNED) || defined(EYE)
#	error "landscape draws are never skinned"
#endif

#define SNOW_SURFACE_ONLY
#define SNOW_SHELL_CB_REGISTER b13
#include "SnowDeformation/SnowShell.hlsl"

cbuffer LandscapeSnowCB : register(b11)
{
	// xy: the draw's translate less GridOrigin, z: its translate z, w: the
	// height the view cull lifts a patch by.
	float4 LsGeometry;
	// x: shell-parity factors (trench reach boost, screen cap) rather than the
	// base lattice alone, y: max factor, z: content gate, the checks that can
	// call an edge detailed (0 = every edge is; 1 map tiles, 2 class borders,
	// 4 water, 8 object lift, 16 exclusions), w: drop patches outside the view.
	float4 LsTess;
	// x: view cull guard band, a fraction of clip w, y: units the raised
	// surface sits under the ground shell while the shell still draws.
	float4 LsCull;
};

#if defined(LS_LIGHTING)
#	define LS_POS(v) (v).Position
#else
#	define LS_POS(v) (v).PositionMS
#endif

// Model space is the draw's translate away from absolute world (landscape
// draws carry no rotation or scale; the CPU checks).
float3 LsRaise(float3 p)
{
	float2 gridLocal = p.xy + LsGeometry.xy;
	float landZ = p.z + LsGeometry.z;
	float coverage;
	float terrainHeight;
	float z = ShellVertexZ(gridLocal, coverage, terrainHeight);
	[branch] if (z - LsCull.y > landZ)
	{
		float3 raised = float3(0.0, 0.0, z);
		[branch] if (SlopeDrape.x > 0.001 && z > terrainHeight)
		{
			float2 slope = ShellVertexSlope(gridLocal);
			float3 terrainNormal = SHELL_TERRAIN_NORMAL(slope.x, slope.y);
			SHELL_DRAPE(raised, float2(0.0, 0.0), z, terrainHeight, terrainNormal)
		}
		p += float3(raised.xy, raised.z - LsCull.y - landZ);
	}
	return p;
}

VS_OUTPUT LS_VSRaise(VS_INPUT input)
{
	LS_POS(input).xyz = LsRaise(LS_POS(input).xyz);
	return LS_GameVS(input);
}

struct LS_CP
{
#if defined(LS_LIGHTING)
	float4 Position : LSPOS;
	float2 TexCoord0 : LSUV;
#	if !defined(MODELSPACENORMALS)
	float4 Normal : LSNRM;
	float4 Bitangent : LSBIN;
#	endif
#	if defined(VC)
	float4 Color : LSCOL;
#		if defined(LANDSCAPE)
	float4 LandBlendWeights1 : LSLB1;
	float4 LandBlendWeights2 : LSLB2;
#		endif
#	endif
#else
	float4 PositionMS : LSPOS;
#	if defined(TEXTURE)
	float2 TexCoord : LSUV;
#	endif
#	if defined(NORMALS)
	float4 Normal : LSNRM;
	float4 Bitangent : LSBIN;
#	endif
#	if defined(VC)
	float4 Color : LSCOL;
#	endif
#endif
};

LS_CP LS_VS(VS_INPUT input)
{
	LS_CP o;
#if defined(LS_LIGHTING)
	o.Position = input.Position;
	o.TexCoord0 = input.TexCoord0;
#	if !defined(MODELSPACENORMALS)
	o.Normal = input.Normal;
	o.Bitangent = input.Bitangent;
#	endif
#	if defined(VC)
	o.Color = input.Color;
#		if defined(LANDSCAPE)
	o.LandBlendWeights1 = input.LandBlendWeights1;
	o.LandBlendWeights2 = input.LandBlendWeights2;
#		endif
#	endif
#else
	o.PositionMS = input.PositionMS;
#	if defined(TEXTURE)
	o.TexCoord = input.TexCoord;
#	endif
#	if defined(NORMALS)
	o.Normal = input.Normal;
	o.Bitangent = input.Bitangent;
#	endif
#	if defined(VC)
	o.Color = input.Color;
#	endif
#endif
	return o;
}

struct LS_HSConst
{
	float Edges[3] : SV_TessFactor;
	float Inside : SV_InsideTessFactor;
};

// Every point of the box is ShellUntouched: no map or berm read within 8
// units. A box straddling the window edge counts as touched.
bool LsBoxUntouched(float2 lo, float2 hi)
{
	float2 uvLo = (GridToDeformOffset + lo) * DeformInvWorldSize;
	float2 uvHi = (GridToDeformOffset + hi) * DeformInvWorldSize;
	float2 pad = 8.0 * DeformInvWorldSize;
	bool farOutside = any(uvHi < -pad) || any(uvLo > 1.0 + pad);
	bool inside = all(uvLo >= 0.0) && all(uvHi <= 1.0);
	bool untouched = farOutside;
	[branch] if (!farOutside && inside)
	{
		SNOW_DEFORM_DIMS(dims);
		int2 i0 = clamp(int2(uvLo * dims), 0, int2(dims) - 1);
		int2 i1 = clamp(int2(uvHi * dims), 0, int2(dims) - 1);
		untouched = all(i1 - i0 <= 16);
		[unroll] for (int y = 0; y < 3; y++)
		{
			[unroll] for (int x = 0; x < 3; x++)
			{
				int2 t = min(i0 + int2(x, y) * 8, i1);
				untouched = untouched && UntouchedTiles.Load(int3(((t + DeformMapOrigin) & (int2(dims) - 1)) >> 3, 0)) != 0;
			}
		}
	}
	return BermBakeActive > 0.5 && untouched;
}

// Class depth moves the surface less than this across the box: the land's
// own triangles carry it.
static const float kLsBorderTolerance = 1.0;

// The class-depth term can change by at most the spread of depth (and of the
// bare blend) over every texel a shaped tap from the box reaches: the jitter
// and the smoothing ring, then the bilinear's next texel.
bool LsBoxPlain(float2 lo, float2 hi)
{
	float reach = 0.0;
	[flatten] if (BorderNoise >= 0.01 || BorderSmooth >= 0.01)
		reach = min(BorderNoise, 16.0) + 0.6 * BorderNoise + BorderSmooth;
	float maxT = (float)(TerrainDim - 1);
	int2 t0 = (int2)clamp(floor((GridToTerrainOffset + lo - reach) / TerrainTexelSize), 0.0, maxT);
	int2 t1 = (int2)clamp(floor((GridToTerrainOffset + hi + reach) / TerrainTexelSize) + 1.0, 0.0, maxT);
	bool plain = all(t1 - t0 <= 5);
	float2 lowest = 1e9;
	float2 highest = -1e9;
	[unroll] for (int y = 0; y < 6; y++)
	{
		[unroll] for (int x = 0; x < 6; x++)
		{
			float2 depthCoverage = TerrainWindowWet.Load(int3(min(t0 + int2(x, y), t1), 0)).yz;
			depthCoverage.x = max(depthCoverage.x, 0.0) * RimStyle.w + min(depthCoverage.x, 0.0);
			lowest = min(lowest, depthCoverage);
			highest = max(highest, depthCoverage);
		}
	}
	float spread = (highest.x - lowest.x) + 8.0 * (saturate(highest.y) - saturate(lowest.y));
	return plain && spread < kLsBorderTolerance;
}

// No drawn water over the box's texels: the shore cap shapes the edge there.
bool LsBoxDry(float2 lo, float2 hi)
{
	bool dry = true;
	[branch] if (CompactLook.x > 0.5)
	{
		float maxT = (float)(TerrainDim - 1) - 0.001;
		int2 t0 = (int2)clamp((GridToTerrainOffset + lo) / TerrainTexelSize, 0.0, maxT);
		int2 t1 = min((int2)clamp((GridToTerrainOffset + hi) / TerrainTexelSize, 0.0, maxT) + 1, int2(TerrainDim - 1, TerrainDim - 1));
		dry = all(t1 - t0 <= 3);
		[unroll] for (int y = 0; y < 4; y++)
		{
			[unroll] for (int x = 0; x < 4; x++)
			{
				int2 t = min(t0 + int2(x, y), t1);
				dry = dry && WaterRawWindow.Load(int3(t.x, (int)TerrainDim - 1 - t.y, 0)) < -50000.0;
			}
		}
	}
	return dry;
}

// Object lift (gate bit 8), and the clearings, shelter and melt basins of
// the exclusion mask (bit 16), at one point.
bool LsPointPlain(float2 gridLocal, uint gate)
{
	bool plain = true;
	[branch] if ((gate & 8) != 0 && ObjectLiftCap > 1.0)
		plain = !ShellObjectLiftsAt(gridLocal);
	[branch] if (plain && (gate & 16) != 0)
	{
		float2 mask = SampleExclusionMask(GridOrigin + gridLocal);
		plain = max(mask.x, mask.y) < 0.001;
	}
	return plain;
}

bool LsEdgeDetail(float2 a, float2 b, uint gate)
{
	float2 lo = min(a, b);
	float2 hi = max(a, b);
	bool detail = false;
	[branch] if ((gate & 1) != 0)
		detail = !LsBoxUntouched(lo, hi);
	[branch] if (!detail && (gate & 2) != 0)
		detail = !LsBoxPlain(lo, hi);
	[branch] if (!detail && (gate & 4) != 0)
		detail = !LsBoxDry(lo, hi);
	[branch] if (!detail && (gate & 24) != 0)
		detail = !(LsPointPlain(a, gate) && LsPointPlain(b, gate) && LsPointPlain(0.5 * (a + b), gate));
	return detail;
}

// The shell's vertex spacing along the edge: its base lattice step, divided
// in camera passes by its hull's trench factor (EdgeTessFactor) and capped
// at TessScreen.x pixels per segment. Reads the edge's endpoints only, so
// both patches on an edge agree.
float LsEdgeFactor(float3 a, float3 b)
{
	float2 mid = 0.5 * (a.xy + b.xy);
	float factor = 1.0;
	bool live = ShellEdgeFade(a.xy) > 0.0 || ShellEdgeFade(b.xy) > 0.0 || ShellEdgeFade(mid) > 0.0;
	[branch] if (live && LsTess.z > 0.5)
		live = LsEdgeDetail(a.xy, b.xy, (uint)LsTess.z);
	[branch] if (live)
	{
		float dist = length(mid - (ShellCameraPosAdjust.xy - GridOrigin));
		float spacing = ShellBandStep(mid);
		[branch] if (LsTess.x > 0.5)
		{
			float reliefBase = SnowReliefDepth > 0.01 ? 1.0 : 0.0;
			float reach = kTessNear * reliefBase;
			[branch] if (dist < kTessCutoff)
			{
				float deform = max(max(SampleDeformationFast(a.xy), SampleDeformationFast(b.xy)), SampleDeformationFast(mid));
				reach = kTessNear * lerp(reliefBase, kTessReachBoost, smoothstep(0.02, 0.25, deform));
			}
			spacing /= clamp(reach / max(dist, 32.0), 1.0, kTessMax);
		}
		factor = clamp(distance(a.xy, b.xy) / spacing, 1.0, LsTess.y);
		[branch] if (LsTess.x > 0.5 && TessScreen.x > 0.0 && factor > 1.0)
		{
			float3 camLocal = float3(GridOrigin, 0.0) - ShellCameraPosAdjust.xyz;
			float4 clipA = mul(CameraViewProj, float4(a + camLocal, 1.0));
			float4 clipB = mul(CameraViewProj, float4(b + camLocal, 1.0));
			[flatten] if (min(clipA.w, clipB.w) > 16.0)
				factor = min(factor, max(length((clipA.xy / clipA.w - clipB.xy / clipB.w) * TessScreen.yz) / TessScreen.x, 1.0));
		}
	}
	return factor;
}

// Clip position through the drawing pass's own constants (the copied VS b2/b12).
float4 LsClip(float3 p)
{
#if defined(LS_LIGHTING)
	return mul(ViewProj, float4(mul(World, float4(p, 1.0)), 1.0));
#else
	return mul(mul(FrameBuffer::CameraViewProj, World), float4(p, 1.0));
#endif
}

// Outside one clip plane at the land and at the fully raised height. Shadow
// maps pancake z (RENDER_SHADOWMAP_CLAMPED), so only x and y cull there.
bool LsOffView(float3 p0, float3 p1, float3 p2)
{
	const float3 lift = float3(0.0, 0.0, LsGeometry.w);
	const float k = 1.0 + LsCull.x;
	float3 pts[6] = { p0, p1, p2, p0 + lift, p1 + lift, p2 + lift };
	bool l = true, r = true, b = true, t = true, n = true;
	[unroll] for (uint i = 0; i < 6; i++)
	{
		float4 c = LsClip(pts[i]);
		l = l && (c.x + c.w * k < 0.0);
		r = r && (c.w * k - c.x < 0.0);
		b = b && (c.y + c.w * k < 0.0);
		t = t && (c.w * k - c.y < 0.0);
		n = n && (c.z < 0.0);
	}
#if defined(RENDER_SHADOWMAP)
	n = false;
#endif
	return l || r || b || t || n;
}

LS_HSConst LS_PatchConst(InputPatch<LS_CP, 3> p)
{
	LS_HSConst o;
	bool off = false;
	[branch] if (LsTess.w > 0.5)
		off = LsOffView(LS_POS(p[0]).xyz, LS_POS(p[1]).xyz, LS_POS(p[2]).xyz);
	o.Edges[0] = 0.0;
	o.Edges[1] = 0.0;
	o.Edges[2] = 0.0;
	o.Inside = 0.0;
	[branch] if (!off)
	{
		// Grid-local xy, camera-relative z off the land (screen cap only).
		float3 g0 = float3(LS_POS(p[0]).xy + LsGeometry.xy, LS_POS(p[0]).z + LsGeometry.z);
		float3 g1 = float3(LS_POS(p[1]).xy + LsGeometry.xy, LS_POS(p[1]).z + LsGeometry.z);
		float3 g2 = float3(LS_POS(p[2]).xy + LsGeometry.xy, LS_POS(p[2]).z + LsGeometry.z);
		o.Edges[0] = LsEdgeFactor(g1, g2);
		o.Edges[1] = LsEdgeFactor(g2, g0);
		o.Edges[2] = LsEdgeFactor(g0, g1);
		o.Inside = max(o.Edges[0], max(o.Edges[1], o.Edges[2]));
	}
	return o;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("LS_PatchConst")]
[maxtessfactor(64.0)]
LS_CP LS_HS(InputPatch<LS_CP, 3> p, uint i : SV_OutputControlPointID)
{
	return p[i];
}

#define LS_LERP(f) (p[0].f * uvw.x + p[1].f * uvw.y + p[2].f * uvw.z)

[domain("tri")]
VS_OUTPUT LS_DS(LS_HSConst c, float3 uvw : SV_DomainLocation, const OutputPatch<LS_CP, 3> p)
{
	VS_INPUT vin;
#if defined(LS_LIGHTING)
	vin.Position = LS_LERP(Position);
	vin.TexCoord0 = LS_LERP(TexCoord0);
#	if !defined(MODELSPACENORMALS)
	vin.Normal = LS_LERP(Normal);
	vin.Bitangent = LS_LERP(Bitangent);
#	endif
#	if defined(VC)
	vin.Color = LS_LERP(Color);
#		if defined(LANDSCAPE)
	vin.LandBlendWeights1 = LS_LERP(LandBlendWeights1);
	vin.LandBlendWeights2 = LS_LERP(LandBlendWeights2);
#		endif
#	endif
#else
	vin.PositionMS = LS_LERP(PositionMS);
#	if defined(TEXTURE)
	vin.TexCoord = LS_LERP(TexCoord);
#	endif
#	if defined(NORMALS)
	vin.Normal = LS_LERP(Normal);
	vin.Bitangent = LS_LERP(Bitangent);
#	endif
#	if defined(VC)
	vin.Color = LS_LERP(Color);
#	endif
#endif
	LS_POS(vin).xyz = LsRaise(LS_POS(vin).xyz);
	return LS_GameVS(vin);
}
