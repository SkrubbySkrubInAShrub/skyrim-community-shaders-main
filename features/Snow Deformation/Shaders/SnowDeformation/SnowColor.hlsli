// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// The snow set's albedo convention, shared by the shells and Lighting's
// snow recolors so every surface wears the same snow.

#ifndef __SNOW_COLOR_HLSLI__
#define __SNOW_COLOR_HLSLI__

#include "Common/SharedData.hlsli"
#include "Common/ColorManagement.hlsli"

#if defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER)
namespace SnowColor
{
	// A snow set texel as the ground's own shader reads it: a PBR (linear) set
	// as TruePBR's albedo, a vanilla set as Lighting's diffuse.
	float3 SetAlbedo(float3 texel, bool isLinear)
	{
		return isLinear ? Color::LinearToPBRMaterial(ColorManagement::DiffuseToWorking(texel, true)) : ColorManagement::DiffuseToWorking(texel);
	}
}
#endif

#endif  // __SNOW_COLOR_HLSLI__
