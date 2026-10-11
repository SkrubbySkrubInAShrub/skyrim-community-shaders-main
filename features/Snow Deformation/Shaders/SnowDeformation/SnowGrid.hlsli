// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Distance bands around the camera: the trench patch places its vertices on
// them (WarpAxisT), and the ground snow's replay tessellates to their spacing
// (SnowShell.hlsl ShellBandStep).
//
// Reads GridSpacing from ShellCB, which both SnowShell.hlsl and
// SnowStaticsShell.hlsl mirror.

// Distance warp, power-of-two bands: band b holds kWarpBandVerts[b] vertices
// spaced kWarpBandMul[b] * GridSpacing apart, reaching ~17k units per side.
// Must match SnowDeformation.h (kShellWarpBandVerts / kShellWarpBandMul).
//
// INVARIANT: every step is an exact power of two of the base step and every
// band start is a multiple of both its own step and the origin snap, so a
// vertex lands on its band's lattice with no rounding left over. Break it and
// quad widths flip as the camera moves (distant up/down jumping).
#define kWarpBands 5
static const float kWarpBandVerts[kWarpBands] = { 192.0, 8.0, 8.0, 8.0, 104.0 };
static const float kWarpBandMul[kWarpBands] = { 1.0, 2.0, 4.0, 8.0, 16.0 };

// Band lookup for vertex |u|: x = step multiplier in GridSpacing units,
// y = fraction through the band (0 at its inner edge, 1 at its outer), z = band
// index. Vertices past the table extend at the coarsest step.
float3 WarpBand(float au)
{
	float3 found = float3(kWarpBandMul[kWarpBands - 1], 1.0, (float)(kWarpBands - 1));
	bool done = false;
	float acc = 0.0;
	[unroll] for (int band = 0; band < kWarpBands; ++band)
	{
		float prev = acc;
		acc += kWarpBandVerts[band];
		[flatten] if (!done && au < acc)
		{
			found = float3(kWarpBandMul[band], saturate((au - prev) / max(kWarpBandVerts[band], 1.0)), (float)band);
			done = true;
		}
	}
	return found;
}

// Maps a vertex coordinate relative to the grid center (in vertex units) to a
// world-unit offset from the center, on a caller-supplied band table and base
// step. The trench patch runs its own table (a fine core sized to the object
// raster rather than to the horizon).
float WarpAxisT(float u, float bandVerts[kWarpBands], float bandMul[kWarpBands], float spacing)
{
	float a = abs(u);
	float off = 0.0;
	[unroll] for (int band = 0; band < kWarpBands; ++band)
	{
		float take = min(a, bandVerts[band]);
		off += take * bandMul[band];
		a -= take;
	}
	off += a * bandMul[kWarpBands - 1];
	return sign(u) * off * spacing;
}

// World-unit offset from the grid center back to vertex units, on the
// shell's own table.
float InverseWarpAxis(float w)
{
	float a = abs(w) / GridSpacing;
	float u = 0.0;
	[unroll] for (int band = 0; band < kWarpBands; ++band)
	{
		float span = kWarpBandVerts[band] * kWarpBandMul[band];
		float take = min(a, span);
		u += take / kWarpBandMul[band];
		a -= take;
	}
	u += a / kWarpBandMul[kWarpBands - 1];
	return sign(w) * u;
}
