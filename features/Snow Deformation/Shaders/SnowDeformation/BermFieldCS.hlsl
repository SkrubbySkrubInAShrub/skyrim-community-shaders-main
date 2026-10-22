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
// x = the berm field, y = the carve's short blur (CarveProfile's slough),
// z = the date of the carve within the short blur, w = the date of the snow
// the berm came from (TrackClock taps weighted by what they cut; 0 = none).
RWTexture2D<float4> OutBermField : register(u0);
// When each map texel was last cut (DeformationUpdateCS StampCS), on the
// ageing clock; read with the wrapping sampler, four texels a tap.
Texture2D<float> TrackClock : register(t2);
SamplerState TrackClockSampler : register(s0);
// Tiles (x | y<<16) whose berm output is stale, from ScanBermCS: the map
// changed within the tap reach. Consumed by BermTiledCS via indirect args.
StructuredBuffer<uint> BermTilesIn : register(t1);

// Per-tile "a berm texel is non-zero", written by every bake group for its own
// tile, so a stale bake still reads as touched.
RWTexture2D<uint> OutBermOccupancy : register(u2);
groupshared uint gBermNonZero;

// UntouchedTile: the map's tile occupancy and the berm's, dilated one tile.
Texture2D<uint> DeformOccupancyIn : register(t3);
Texture2D<uint> BermOccupancyIn : register(t4);
RWTexture2D<uint> OutUntouched : register(u3);

// DeformationUpdateCS's BermDirty grid: BermTiledCS consumes each rebuilt
// tile's mark. ScanBermCS lists every marked tile (its halo includes the tile
// itself), so no mark outlives the bake.
RWTexture2D<uint> OutBermDirty : register(u6);

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

	float CrustPrintDepth;
	float CrustThaw;
	float CrustBreakOnCarve;
	float SlumpRate;

	uint InjectValid;
	float GameDeltaTime;
	uint DebugActivityView;
	// The ageing clock now: older dates count as this many units old.
	float AgeingClock;
}

// Units a date can be older than the clock and still read as its own: past
// it, any date reads this old, so beside a fresh cut it averages to half
// aged, as the two would look, rather than to ancient. Past every Track
// Ageing span. Mirrored by kTrackAgeHorizon in SnowDeformation.h.
static const float kTrackAgeHorizon = 128.0;

// Must match kWallBlurReach in SnowFields.hlsli.
static const float kWallBlurReach = 12.0;

// 17 taps on two staggered 8-point rings. Tap COUNT is the anti-seam: for
// a straight trail edge each tap's projection crosses zero at a different
// distance, so the field climbs in 1/17 steps instead of the 2/9 ledge a
// sparse ring printed as a visible contour line ~34 units out.
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
// filtering SampleDeformationBilinear performs in the shells - and the dates
// of its four texels weighted by the same: x = the depth, y = depth x date.
// a_displaced: DISPLACED depth only (total minus the melted portion). A berm
// is snow that had to go somewhere; melted snow leaves no spoil, so it must
// not reach the field at all. Subtracting here rather than scaling the berm
// down afterwards also keeps a boot print through a melt basin throwing its
// own proper ridge. Otherwise the carve as CarveProfile reads it (all of it,
// melt included).
float2 DatedBilinear(float2 t, float2 dims, bool a_displaced)
{
	t = clamp(t, 0.0, dims - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float4 m00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims)));
	float4 m10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims)));
	float4 m01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims)));
	float4 m11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims)));
	float4 d = a_displaced ? float4(Displaced(m00), Displaced(m10), Displaced(m01), Displaced(m11)) :
	                         saturate(float4(m00.x, m10.x, m01.x, m11.x));

	// Physical texels t0..t0+1 in one gather (the sampler wraps like
	// DeformTexel), as s00 s10 s01 s11.
	float4 date = TrackClock.GatherRed(TrackClockSampler, (float2(t0 + MapOrigin) + 1.0) / dims).wzxy;
	date = max(date, AgeingClock - kTrackAgeHorizon);
	float4 w = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
	float depth = lerp(lerp(d.x, d.y, f.x), lerp(d.z, d.w, f.x), f.y);
	return float2(a_displaced ? saturate(depth) : depth, dot(w, d * date));
}

// A tap whose window UV leaves [0,1] contributes 0, matching
// SampleDeformationFast: the field must decay to nothing at the window border
// rather than smearing the edge texels inward.
float2 Tap(float2 texel, float2 dims, bool a_displaced)
{
	float2 uv = (texel + 0.5) / dims;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;
	return DatedBilinear(texel, dims, a_displaced);
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

	float2 b = Tap(centre, dims, true);
	[unroll] for (int i = 0; i < 16; i++)
		b += Tap(centre + kBermTaps[i] * (BermReach * invTexel), dims, true);

	// The short blur: the centre and the inner ring's eight directions at
	// kWallBlurReach, never closer than a texel. Inside ScanBermCS's halo,
	// whose reach is at least 20 units.
	const float blurReach = max(kWallBlurReach * invTexel, 1.0) / 18.0;
	float2 s = Tap(centre, dims, false);
	[unroll] for (int j = 0; j < 8; j++)
		s += Tap(centre + kBermTaps[j] * blurReach, dims, false);

	const float berm = saturate(b.x / 17.0);
	const float blur = s.x / 9.0;
	// Each date weighted by the depth it dated, so the snow cut deepest
	// speaks for the tap; nothing cut, no date. Stored times its own weight
	// (blur, berm) so filtering between texels stays a weighted mean;
	// BermFieldBaked divides it back out.
	OutBermField[phys] = float4(berm, blur, s.y / 9.0, b.y / 17.0);
	return berm != 0.0 || blur != 0.0;
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
	if (GIdx == 0) {
		OutBermOccupancy[tile] = gBermNonZero;
		OutBermDirty[tile] = 0u;
	}
}

// 1 where neither the map nor the berm field holds a non-zero texel within one
// tile: every map or berm read the shells take there returns exactly zero.
// Wrapped neighbours across the window border only over-include.
void UntouchedTile(uint2 tile)
{
	uint2 dims;
	DeformOccupancyIn.GetDimensions(dims.x, dims.y);
	if (any(tile >= dims))
		return;
	uint touched = 0;
	[unroll] for (int dy = -1; dy <= 1; dy++)
	{
		[unroll] for (int dx = -1; dx <= 1; dx++)
		{
			const uint2 t = (tile + uint2(int2(dx, dy) + int2(dims))) & (dims - 1);
			touched |= DeformOccupancyIn[t] | BermOccupancyIn[t];
		}
	}
	OutUntouched[tile] = touched == 0 ? 1u : 0u;
}

[numthreads(8, 8, 1)]
void UntouchedCS(uint3 dtid : SV_DispatchThreadID)
{
	UntouchedTile(dtid.xy);
}
