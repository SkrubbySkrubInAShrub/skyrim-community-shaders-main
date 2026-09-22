#pragma once

#include "SceneSettingsManager.h"

#include "SceneSettingsInternal.h"

/// Resolution of the broadest-to-narrowest location target chain a scene layer keys off.
namespace SceneSettingsLocationTargets
{
	bool IsValidLocationTargetType(SceneSettingsManager::LocationTargetType type);

	template <class Form>
	std::string GetLocationTargetDisplayName(const Form* form)
	{
		if (const char* fullName = form->GetFullName(); fullName && fullName[0] != '\0')
			return std::string(fullName);
		return Util::GetFormDisplayName(form->GetFormID());
	}

	/// Regions carry no full name, so their editor ID is the only readable label they have.
	std::string GetRegionTargetName(const RE::TESRegion* region);

	/// Build the broadest-to-narrowest target chain for a location and the cell that resolved it.
	std::vector<SceneSettingsManager::LocationTarget> BuildLocationTargetChain(
		RE::BGSLocation* location, RE::TESObjectCELL* cell);

	RE::TESForm* ResolveLocationTargetForm(std::string_view formKey);

	/// Resolve the chain an arbitrary target belongs to, preferring the player's own chain when it matches.
	std::vector<SceneSettingsManager::LocationTarget> ResolveLocationTargetChain(
		SceneSettingsManager::LocationTargetType type, std::string_view formKey);

	/// Whether a chain describes an interior, or nothing when no link in it settles the question.
	std::optional<bool> GetLocationChainInteriorState(
		const std::vector<SceneSettingsManager::LocationTarget>& targets);
}
