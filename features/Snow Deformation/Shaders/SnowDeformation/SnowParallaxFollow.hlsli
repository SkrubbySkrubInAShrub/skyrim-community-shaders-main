// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Snow Follows Parallax: where a pixel's parallax-shifted texel lies on the
// mesh. Lighting moves the projected paint there (SnowLighting.hlsli), the
// object coat its material (SnowStaticsShell.hlsl).

#ifndef SNOW_DEFORMATION_SNOWPARALLAXFOLLOW_HLSLI
#define SNOW_DEFORMATION_SNOWPARALLAXFOLLOW_HLSLI

namespace SnowParallaxFollow
{
	// A uv offset as a world offset across the mesh, through the inverse screen
	// Jacobian of the geometric uv; zero where the uv mapping degenerates. The
	// derivatives are the caller's, taken in uniform flow.
	float3 UVShiftToWorld(float2 uvShift, float2 uvDx, float2 uvDy, float3 posDx, float3 posDy)
	{
		const float det = uvDx.x * uvDy.y - uvDy.x * uvDx.y;
		const float2 px = float2(uvDy.y * uvShift.x - uvDy.x * uvShift.y, uvDx.x * uvShift.y - uvDx.y * uvShift.x) / det;
		return det * det > 1e-6 * dot(uvDx, uvDx) * dot(uvDy, uvDy) ? posDx * px.x + posDy * px.y : 0.0;
	}
}

#endif  // SNOW_DEFORMATION_SNOWPARALLAXFOLLOW_HLSLI
