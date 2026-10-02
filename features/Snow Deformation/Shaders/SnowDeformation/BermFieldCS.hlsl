// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Berm field bake.
//
// The berm field is a 17-tap disc average of the deformation map over a ~40
// unit radius times BermReach (see BermField in SnowShell.hlsl). It is a pure function of the
// map, yet the shells recomputed it per PIXEL - four times over, for the
// central difference the normal needs, plus once more per vertex. At 68 loads
// a call that dominated the shell's pixel cost.
//
// Baking it once per map texel here turns every one of those calls into a
// single bilinear tap. The kernel, the tap order and the zero-outside-window
// rule below mirror BermField exactly, so the baked field equals what the
// per-pixel path produced at texel centres; between centres the consumers'
// bilinear filtering stands in for the 17-way average, which is already
// smooth at a far coarser scale than one texel.

Texture2D<float4> DeformationMap : register(t0);
RWTexture2D<float> OutBermField : register(u0);
// Tiles (x | y<<16) whose berm output is stale, from ScanBermCS: the map
// changed within the tap reach. Consumed by BermTiledCS via indirect args.
StructuredBuffer<uint> BermTilesIn : register(t1);

// Trample extent bake (ExtentCS): reads the finished berm field, writes one
// texel per 8x8 map tile. See TRENCH-MORPHOLOGY-RESEARCH.md P1.
Texture2D<float> BermFieldIn : register(t2);
RWTexture2D<float> OutExtentField : register(u1);

// Per-tile "a berm texel is non-zero", written by every bake group for its own
// tile, so a stale bake still reads as touched.
RWTexture2D<uint> OutBermOccupancy : register(u2);
groupshared uint gBermNonZero;

// UntouchedCS: the map's tile occupancy and the berm's, dilated one tile.
Texture2D<uint> DeformOccupancyIn : register(t3);
Texture2D<uint> BermOccupancyIn : register(t4);
RWTexture2D<uint> OutUntouched : register(u3);

// Shares DeformationUpdateCS's PerFrame buffer; only TexelSize is read, but
// the leading layout must match it exactly.
cbuffer PerFrame : register(b0)
{
	float2 WindowOrigin;
	// Toroidal map addressing (see DeformationUpdateCS.hlsl).
	int2 MapOrigin;

	float TexelSize;
	uint StampCount;
	float RefillAmount;
	uint ClearMap;

	float StampFalloffStart;
	// 1 / Berm Steepness: scales the taps.
	float BermReach;
	float2 WindBias;

	float DeltaTime;
	float MeltPersistence;
	float MeltFloorStart;
	float MeltEdgeNoise;
}

// Must match kBermTaps in SnowShell.hlsl / SnowStaticsShell.hlsl.
static const float2 kBermTaps[16] = {
	float2(18.0, 0.0), float2(12.73, 12.73), float2(0.0, 18.0), float2(-12.73, 12.73),
	float2(-18.0, 0.0), float2(-12.73, -12.73), float2(0.0, -18.0), float2(12.73, -12.73),
	float2(36.96, 15.31), float2(15.31, 36.96), float2(-15.31, 36.96), float2(-36.96, 15.31),
	float2(-36.96, -15.31), float2(-15.31, -36.96), float2(15.31, -36.96), float2(36.96, -15.31)
};

// Displaced (dug) depth of a map texel: total minus the MELTED portion only.
// Channel y is signed - negative is scorch - and scorched snow was thrown
// aside rather than removed, so it keeps every bit of its berm.
float Displaced(float4 texel)
{
	return saturate(texel.x - max(texel.y, 0.0));
}

// The berm field is toroidal like the map it is baked from (same MapOrigin),
// so a partial rebuild stays valid across scrolls - texels never move.
// Consumers translate through the shells' DeformTexel, exactly as for the
// map.
int3 DeformTexel(int2 t, int2 dims)
{
	return int3((t + MapOrigin) & (dims - 1), 0);
}

// Bilinear tap in texel coordinates, clamped to the edge - the Load-based
// filtering SampleDeformationBilinear performs in the shells.
float TapBilinear(float2 t, float2 dims)
{
	t = clamp(t, 0.0, dims - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	// DISPLACED depth only (total minus the melted portion). A berm is snow
	// that had to go somewhere; melted snow leaves no spoil, so it must not
	// reach the field at all. Subtracting here rather than scaling the berm
	// down afterwards also keeps a boot print through a melt basin throwing
	// its own proper ridge.
	float s00 = Displaced(DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims))));
	float s10 = Displaced(DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims))));
	float s01 = Displaced(DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims))));
	float s11 = Displaced(DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims))));

	return saturate(lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y));
}

// A tap whose window UV leaves [0,1] contributes 0, matching
// SampleDeformationFast: the field must decay to nothing at the window border
// rather than smearing the edge texels inward.
float Tap(float2 texel, float2 dims)
{
	float2 uv = (texel + 0.5) / dims;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;
	return TapBilinear(texel, dims);
}

bool BermTexel(uint2 phys)
{
	float2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
	if (any(float2(phys) >= dims))
		return false;

	// Tap offsets are authored in world units; the window resizes with the
	// Trenches range slider, so convert through the live texel size. Tap
	// math runs in LOGICAL texel space (the window-border zeroing is a world
	// rule); only the fetches and the output translate.
	float invTexel = 1.0 / max(TexelSize, 1e-4);
	float2 centre = float2((int2(phys) - MapOrigin) & (int2(dims) - 1));

	float b = Tap(centre, dims);
	[unroll] for (int i = 0; i < 16; i++)
		b += Tap(centre + kBermTaps[i] * (BermReach * invTexel), dims);

	const float berm = saturate(b / 17.0);
	OutBermField[phys] = berm;
	return berm != 0.0;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint3 Gid : SV_GroupID, uint GIdx : SV_GroupIndex)
{
	if (GIdx == 0)
		gBermNonZero = 0;
	GroupMemoryBarrierWithGroupSync();
	if (BermTexel(dtid.xy))
		InterlockedOr(gBermNonZero, 1u);
	GroupMemoryBarrierWithGroupSync();
	if (GIdx == 0)
		OutBermOccupancy[Gid.xy] = gBermNonZero;
}

// Indirect over ScanBermCS's list: only tiles whose inputs changed (plus
// the tap-reach halo) rebuild; everything else keeps last frame's bake,
// which the toroidal layout keeps valid across scrolls.
[numthreads(8, 8, 1)]
void BermTiledCS(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID, uint GIdx : SV_GroupIndex)
{
	// Uniform per group, so the early-out never splits a barrier.
	const uint listIndex = Gid.y * 1024u + Gid.x;
	if (listIndex >= BermTilesIn[0])
		return;
	const uint packed = BermTilesIn[1 + listIndex];
	const uint2 tile = uint2(packed & 0xFFFFu, packed >> 16u);
	if (GIdx == 0)
		gBermNonZero = 0;
	GroupMemoryBarrierWithGroupSync();
	if (BermTexel(tile * 8u + GTid.xy))
		InterlockedOr(gBermNonZero, 1u);
	GroupMemoryBarrierWithGroupSync();
	if (GIdx == 0)
		OutBermOccupancy[tile] = gBermNonZero;
}

// 1 where neither the map nor the berm field holds a non-zero texel within one
// tile: every map or berm read the shells take there returns exactly zero.
// Wrapped neighbours across the window border only over-include.
[numthreads(8, 8, 1)]
void UntouchedCS(uint3 dtid : SV_DispatchThreadID)
{
	uint2 dims;
	DeformOccupancyIn.GetDimensions(dims.x, dims.y);
	if (any(dtid.xy >= dims))
		return;
	uint touched = 0;
	[unroll] for (int dy = -1; dy <= 1; dy++)
	{
		[unroll] for (int dx = -1; dx <= 1; dx++)
		{
			const uint2 t = (dtid.xy + uint2(int2(dx, dy) + int2(dims))) & (dims - 1);
			touched |= DeformOccupancyIn[t] | BermOccupancyIn[t];
		}
	}
	OutUntouched[dtid.xy] = touched == 0 ? 1u : 0u;
}

// ---- Trample extent ----
// How two-dimensional the worked ground around a tile is. Per axis, the berm
// field averaged along both directions; the result is the MINIMUM over axes.
// A trail is high along itself and ~0 across it; a trampled area is high
// every way. A plain mean cannot tell a trail from a sparse field.
#define EXTENT_AXES 4
#define EXTENT_RADII 3
static const float2 kExtentAxis[EXTENT_AXES] = {
	float2(1.0, 0.0), float2(0.7071, 0.7071), float2(0.0, 1.0), float2(-0.7071, 0.7071)
};
static const float kExtentRadius[EXTENT_RADII] = { 40.0, 84.0, 128.0 };

// Point tap of the baked berm field in LOGICAL texel space; zero outside the
// window, like Tap.
float ExtentTap(int2 t, int2 dims)
{
	float v = 0.0;
	if (all(t >= 0) && all(t < dims))
		v = BermFieldIn.Load(DeformTexel(t, dims));
	return v;
}

[numthreads(8, 8, 1)]
void ExtentCS(uint3 dtid : SV_DispatchThreadID)
{
	uint2 outDims;
	OutExtentField.GetDimensions(outDims.x, outDims.y);
	if (any(dtid.xy >= outDims))
		return;

	int2 dims = int2(outDims) * 8;
	float invTexel = 1.0 / max(TexelSize, 1e-4);
	// Tiles are PHYSICAL (they never move on a scroll); the taps are a world
	// rule, so they run in logical space from the tile's centre.
	// Integer centre plus integer offsets: a tap rounded AFTER the scroll is
	// added flips on exact ties as MapOrigin changes parity.
	int2 centre = (int2(dtid.xy) * 8 + 4 - MapOrigin) & (dims - 1);

	float extent = 1.0;
	[unroll] for (uint a = 0; a < EXTENT_AXES; a++)
	{
		float along = 0.0;
		[unroll] for (uint r = 0; r < EXTENT_RADII; r++)
		{
			int2 off = int2(floor(kExtentAxis[a] * (kExtentRadius[r] * invTexel) + 0.5));
			along += ExtentTap(centre + off, dims) + ExtentTap(centre - off, dims);
		}
		extent = min(extent, along / (2.0 * EXTENT_RADII));
	}
	OutExtentField[dtid.xy] = saturate(extent);
}
