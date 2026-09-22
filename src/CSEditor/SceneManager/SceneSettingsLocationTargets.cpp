#include "SceneSettingsLocationTargets.h"

#include "Globals.h"
#include "Utils/Format.h"

#include <algorithm>
#include <set>

using namespace SceneSettingsInternal;

namespace SceneSettingsLocationTargets
{
	bool IsValidLocationTargetType(SceneSettingsManager::LocationTargetType type)
	{
		return type == SceneSettingsManager::LocationTargetType::Region ||
		       type == SceneSettingsManager::LocationTargetType::Location ||
		       type == SceneSettingsManager::LocationTargetType::Cell;
	}

	std::string GetRegionTargetName(const RE::TESRegion* region)
	{
		auto name = Util::PrettifyIdentifier(Util::GetFormEditorID(region));
		return name.empty() ? Util::GetFormDisplayName(region->GetFormID()) : name;
	}

	std::vector<SceneSettingsManager::LocationTarget> BuildLocationTargetChain(
		RE::BGSLocation* location, RE::TESObjectCELL* cell)
	{
		using LocationTargetType = SceneSettingsManager::LocationTargetType;
		const auto cocCode = cell ? Util::GetFormEditorID(cell) : std::string{};

		std::vector<RE::BGSLocation*> locationChain;
		std::set<RE::FormID> visited;
		for (auto* current = location; current && visited.insert(current->GetFormID()).second; current = current->parentLoc)
			locationChain.push_back(current);
		std::reverse(locationChain.begin(), locationChain.end());

		std::vector<SceneSettingsManager::LocationTarget> targets;
		// The sky knows which of an exterior cell's overlapping regions actually won, but only for the
		// cell the player is standing in; anywhere else the cell's own first region is the best guess.
		if (cell && cell->IsExteriorCell()) {
			RE::TESRegion* region = nullptr;
			if (auto* player = RE::PlayerCharacter::GetSingleton();
				player && player->GetParentCell() == cell && globals::game::sky)
				region = globals::game::sky->region;
			if (!region) {
				if (auto* regions = cell->GetRegionList(false)) {
					auto regionIt = std::find_if(regions->begin(), regions->end(),
						[](auto* candidate) { return candidate != nullptr; });
					if (regionIt != regions->end())
						region = *regionIt;
				}
			}
			if (region) {
				targets.push_back({
					.type = LocationTargetType::Region,
					.formKey = Util::GetFormFileKey(region),
					.name = GetRegionTargetName(region),
					.editorId = Util::GetFormEditorID(region),
					.cocCode = cocCode,
					.formId = region->GetFormID(),
				});
			}
		}

		for (auto* current : locationChain) {
			targets.push_back({
				.type = LocationTargetType::Location,
				.formKey = Util::GetFormFileKey(current),
				.name = GetLocationTargetDisplayName(current),
				.editorId = Util::GetFormEditorID(current),
				.cocCode = cocCode,
				.formId = current->GetFormID(),
			});
		}

		if (cell) {
			targets.push_back({
				.type = LocationTargetType::Cell,
				.formKey = Util::GetFormFileKey(cell),
				.name = GetLocationTargetDisplayName(cell),
				.editorId = cocCode,  // The coc code is the cell's own editor ID.
				.cocCode = cocCode,
				.formId = cell->GetFormID(),
			});
		}
		return targets;
	}

	RE::TESForm* ResolveLocationTargetForm(std::string_view formKey)
	{
		const auto parsed = Util::ParseSpid(std::string(formKey));
		if (parsed.localFormId == 0)
			return nullptr;
		const auto formId = parsed.pluginName.empty() ? parsed.localFormId : Util::SpidToFormId(std::string(formKey));
		return formId != 0 ? RE::TESForm::LookupByID(formId) : nullptr;
	}

	std::vector<SceneSettingsManager::LocationTarget> ResolveLocationTargetChain(
		SceneSettingsManager::LocationTargetType type, std::string_view formKey)
	{
		if (auto* manager = SceneSettingsManager::GetSingleton()) {
			const auto& currentTargets = manager->GetCurrentLocationTargets();
			const auto normalizedKey = NormalizeLocationFormKey(formKey);
			if (std::any_of(currentTargets.begin(), currentTargets.end(), [&](const auto& target) {
					return target.type == type && NormalizeLocationFormKey(target.formKey) == normalizedKey;
				}))
				return currentTargets;
		}
		auto* form = ResolveLocationTargetForm(formKey);
		if (!form)
			return {};
		// A region is reached through the cells it covers, so away from them it is a chain of itself.
		if (type == SceneSettingsManager::LocationTargetType::Region) {
			auto* region = form->As<RE::TESRegion>();
			if (!region)
				return {};
			return { {
				.type = SceneSettingsManager::LocationTargetType::Region,
				.formKey = Util::GetFormFileKey(region),
				.name = GetRegionTargetName(region),
				.editorId = Util::GetFormEditorID(region),
				.formId = region->GetFormID(),
			} };
		}
		if (type == SceneSettingsManager::LocationTargetType::Location)
			return BuildLocationTargetChain(form->As<RE::BGSLocation>(), nullptr);
		auto* cell = form->As<RE::TESObjectCELL>();
		return cell ? BuildLocationTargetChain(cell->GetLocation(), cell) :
		              std::vector<SceneSettingsManager::LocationTarget>{};
	}

	std::optional<bool> GetLocationChainInteriorState(
		const std::vector<SceneSettingsManager::LocationTarget>& targets)
	{
		for (const auto& target : targets) {
			// Regions only ever cover exterior cells.
			if (target.type == SceneSettingsManager::LocationTargetType::Region)
				return false;
			if (target.type == SceneSettingsManager::LocationTargetType::Cell)
				if (auto* form = ResolveLocationTargetForm(target.formKey))
					if (auto* cell = form->As<RE::TESObjectCELL>())
						return cell->IsInteriorCell();
		}
		return std::nullopt;
	}
}
