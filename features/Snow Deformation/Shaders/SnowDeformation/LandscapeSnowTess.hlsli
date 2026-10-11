// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Landscape tessellation shared by the raised landscape (LandscapeSnow.hlsl)
// and the ground snow drawn on the landscape's meshes (LandscapeSnowShell.hlsl):
// the per-draw constants and the shell-parity edge factor. Include after
// SnowShell.hlsl.

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

// The hull's checks; the tessellation constants they read are hull-only.
#if defined(HULLSHADER) || defined(SNOW_SURFACE_ONLY)
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
#endif
