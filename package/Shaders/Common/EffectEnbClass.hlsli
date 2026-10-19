#ifndef EFFECT_ENB_CLASS_HLSLI
#define EFFECT_ENB_CLASS_HLSLI

#include "Common/Permutation.hlsli"

// Sorts Effect.hlsl draws the way ENB does for its [FIRE], [LIGHTSPRITE] and [PARTICLE] settings.
// Reads the Effect shader's permutation defines (ADDBLEND, SOFT, LIGHTING, TEXTURE, ...), so include it from Effect.hlsl only, under EFFECTS11.
namespace EnbEffectClass
{
	static const uint None = 0;
	static const uint Fire = 1;
	static const uint LightSprite = 2;
	static const uint Particle = 3;
}

uint GetEnbEffectClass()
{
	const bool grayscaleToColor = Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToColor;
	const bool grayscaleToAlpha = Permutation::PixelShaderDescriptor & Permutation::EffectFlags::GrayscaleToAlpha;

	if (Permutation::VertexShaderDescriptor & Permutation::EffectFlags::SkyObject) {
#if defined(ADDBLEND)
#	if defined(SOFT)
		return grayscaleToColor && grayscaleToAlpha ? EnbEffectClass::Fire : EnbEffectClass::LightSprite;
#	elif defined(PARTICLES) && defined(TEXCOORD_INDEX) && defined(INDEXED_TEXTURE)
		return EnbEffectClass::Fire;
#	else
		return EnbEffectClass::LightSprite;
#	endif
#else
		return EnbEffectClass::None;
#endif
	}

#if defined(MULTBLEND) || defined(MULTBLEND_DECAL) || defined(MEMBRANE) || defined(BLOOD) || defined(PROJECTED_UV)
	return EnbEffectClass::None;
#else
#	if defined(TEXTURE)
	const bool plainTexture = !grayscaleToColor && !grayscaleToAlpha && !(Permutation::PixelShaderDescriptor & Permutation::EffectFlags::IgnoreTexAlpha);
#	else
	const bool plainTexture = false;
#	endif

#	if defined(LIGHTING)
#		if defined(ADDBLEND) && !defined(SOFT)
	return EnbEffectClass::None;
#		else
	return plainTexture ? EnbEffectClass::Particle : EnbEffectClass::None;
#		endif
#	else
	if (grayscaleToColor && grayscaleToAlpha)
		return EnbEffectClass::Fire;
#		if defined(ADDBLEND)
#			if defined(SOFT)
	return plainTexture ? EnbEffectClass::LightSprite : EnbEffectClass::None;
#			else
	return plainTexture ? EnbEffectClass::Fire : EnbEffectClass::None;
#			endif
#		elif defined(SOFT)
#			if !defined(IS_VOLUMETRIC_FOG)
	if (grayscaleToAlpha && !grayscaleToColor)
		return EnbEffectClass::Fire;
#			endif
	return plainTexture ? EnbEffectClass::Particle : EnbEffectClass::None;
#		else
	return EnbEffectClass::None;
#		endif
#	endif
#endif
}

#endif  // EFFECT_ENB_CLASS_HLSLI
