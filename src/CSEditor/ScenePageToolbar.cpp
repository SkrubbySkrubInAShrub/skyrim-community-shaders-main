#include "ScenePageToolbar.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../I18n/I18n.h"
#include "EditorWindow.h"
#include "Menu.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	using SceneContextId = SceneSettingsManager::SceneContextId;
	using SceneContextType = SceneSettingsManager::SceneContextType;
	using CopyCandidate = SceneSettingsManager::CopyCandidate;
	using CopyConflictPolicy = SceneSettingsManager::CopyConflictPolicy;
	using CopyRejection = SceneSettingsManager::CopyRejection;
	using CopySource = SceneSettingsManager::CopySource;
	using PeriodScope = SceneSettingsManager::PeriodScope;

	/// Keeps the toolbar off the window's scrollbar, like the widget gutter does.
	constexpr float kRightMargin = 8.0f;

	/// The preview lists every candidate, so it scrolls rather than growing past the screen.
	constexpr float kPreviewWidth = 520.0f;
	constexpr float kPreviewHeight = 300.0f;
	constexpr ImGuiTableFlags kPreviewTableFlags =
		ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;

	constexpr const char* kCopyPopupId = "##ScenePageCopy";

	/// Height the From/To submenus scroll within once their list outgrows it, most notably To, which
	/// walks every weather period rather than only the ones already authored.
	constexpr float kCopyListHeight = 260.0f;

	/// Source/destination enumeration walks every weather period and every location, so each direction is
	/// cached until the entries it counts change. The dropdown recomputes on open, so a stale count never picks.
	struct CopyListCache
	{
		std::uint64_t revision = 0;
		int lastUsedFrame = 0;
		bool valid = false;
		std::vector<CopySource> entries;
	};
	std::map<SceneContextId, CopyListCache> sourceCaches;
	std::map<SceneContextId, CopyListCache> destinationCaches;
	/// A page that has not drawn for this long has been closed or scrolled out of the tree. Evicting on
	/// size instead would drop caches pages are still using, which costs a rebuild every frame.
	constexpr int kSourceCacheRetentionFrames = 120;

	/// A copy whose conflicts the user has yet to resolve. One flow at a time, keyed by its page.
	struct CopyFlow
	{
		SceneContextId source;
		SceneContextId destination;
		std::string sourceName;
		std::vector<CopyCandidate> candidates;
		PeriodScope periodScope = PeriodScope::ActivePeriod;
		bool active = false;
		bool pendingOpen = false;
	};
	CopyFlow copyFlow;

	/// The page a pending clear belongs to, so only that page's toolbar draws the confirmation.
	SceneContextId clearContext;
	bool clearRequested = false;
	Util::ConfirmationPopup clearConfirmation;

	/// Shared cache/eviction logic for both copy directions; only what fetches the list differs.
	template <typename Fetch>
	const std::vector<CopySource>& GetCachedCopyList(
		std::map<SceneContextId, CopyListCache>& caches, const SceneContextId& context, bool forceRefresh, Fetch fetch)
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		const auto revision = manager->GetEntryPresentationRevision();
		const auto frame = ImGui::GetFrameCount();
		std::erase_if(caches, [&](const auto& entry) {
			return entry.first != context && frame - entry.second.lastUsedFrame > kSourceCacheRetentionFrames;
		});

		auto& cache = caches[context];
		cache.lastUsedFrame = frame;
		if (forceRefresh || !cache.valid || cache.revision != revision) {
			cache.entries = fetch(manager, context);
			cache.revision = revision;
			cache.valid = true;
		}
		return cache.entries;
	}

	const std::vector<CopySource>& GetCopySources(const SceneContextId& context, bool forceRefresh)
	{
		return GetCachedCopyList(sourceCaches, context, forceRefresh,
			[](auto* manager, const auto& ctx) { return manager->GetCopySources(ctx); });
	}

	const std::vector<CopySource>& GetCopyDestinations(const SceneContextId& context, bool forceRefresh)
	{
		return GetCachedCopyList(destinationCaches, context, forceRefresh,
			[](auto* manager, const auto& ctx) { return manager->GetCopyDestinations(ctx); });
	}

	/// Heading the source list groups under, matching the type-first order GetCopySources returns.
	const char* GetContextTypeLabel(SceneContextType type)
	{
		switch (type) {
		case SceneContextType::Interior:
			return T(TKEY("scene_page_group_interior"), "Interior");
		case SceneContextType::Weather:
			return T(TKEY("scene_page_group_weather"), "Weather");
		case SceneContextType::Location:
			return T(TKEY("scene_page_group_location"), "Location");
		default:
			return T(TKEY("scene_page_group_time_of_day"), "Time of Day");
		}
	}

	/// Why one candidate is not going to be copied. Empty when it is.
	const char* GetRejectionText(CopyRejection rejection)
	{
		switch (rejection) {
		case CopyRejection::NotInCatalog:
			return T(TKEY("scene_page_copy_reject_catalog"), "Not a setting a scene can override.");
		case CopyRejection::NotAllowedInLayer:
			return T(TKEY("scene_page_copy_reject_layer"), "This page cannot hold this setting.");
		case CopyRejection::ValueRejected:
			return T(TKEY("scene_page_copy_reject_value"), "The value is not valid here.");
		case CopyRejection::BlockedByOverwrite:
			return T(TKEY("scene_page_copy_reject_overwrite"), "A mod override already holds this setting.");
		case CopyRejection::GroupCompanionRejected:
			return T(TKEY("scene_page_copy_reject_companion"), "Another part of the same control cannot be copied.");
		default:
			return "";
		}
	}

	/// Width one text button occupies, so the toolbar can right-align before drawing anything.
	float ButtonWidth(const char* label)
	{
		return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	}

	void RunCopy(const SceneContextId& source, const SceneContextId& destination, CopyConflictPolicy policy,
		PeriodScope periodScope)
	{
		const auto result = SceneSettingsManager::GetSingleton()->CopySettingsAcrossPeriods(
			source, destination, policy, periodScope);
		auto copied = result.copied;
		auto overwritten = result.overwritten;
		// The preview already explained the incompatible rows; the toast only counts what landed.
		auto skipped = result.skipped + result.incompatible;
		EditorWindow::GetSingleton()->ShowNotification(
			std::vformat(T(TKEY("scene_page_copy_result"), "{} copied, {} overwritten, {} skipped"),
				std::make_format_args(copied, overwritten, skipped)),
			result.Changed() ? Util::Colors::GetSuccess() : Util::Colors::GetWarning());
	}

	/// Dry run first: a copy with nothing to overwrite needs no preview and runs on the click.
	void StartCopy(const SceneContextId& source, const SceneContextId& destination, const std::string& sourceName,
		PeriodScope periodScope)
	{
		auto candidates = SceneSettingsManager::GetSingleton()->GetCopyCandidates(source, destination,
			SceneSettingsManager::CopyScope::EntireContext, std::nullopt, periodScope);
		if (std::ranges::none_of(candidates, [](const auto& candidate) { return candidate.conflicts; })) {
			RunCopy(source, destination, CopyConflictPolicy::SkipExisting, periodScope);
			return;
		}
		copyFlow = { .source = source,
			.destination = destination,
			.sourceName = sourceName,
			.candidates = std::move(candidates),
			.periodScope = periodScope,
			.active = true,
			.pendingOpen = true };
	}

	/// One scrollable, type-grouped list shared by the From and To submenus.
	void DrawCopyList(std::span<const CopySource> entries, const std::function<void(const CopySource&)>& onPick)
	{
		if (ImGui::BeginChild("##ScenePageCopyList", ImVec2(0.0f, kCopyListHeight * Util::GetUIScale()))) {
			auto lastType = std::optional<SceneContextType>{};
			for (const auto& entry : entries) {
				if (lastType != entry.context.type) {
					lastType = entry.context.type;
					ImGui::SeparatorText(GetContextTypeLabel(*lastType));
				}
				if (ImGui::Selectable(std::format("{} ({})", entry.displayName, entry.settingCount).c_str()))
					onPick(entry);
			}
		}
		ImGui::EndChild();
	}

	void DrawCopyPopup(const SceneContextId& context, PeriodScope periodScope)
	{
		if (!ImGui::BeginPopup(kCopyPopupId))
			return;

		auto* manager = SceneSettingsManager::GetSingleton();
		const auto& sources = GetCopySources(context, false);
		ImGui::BeginDisabled(sources.empty());
		// IsItemHovered() must run right after BeginMenu(), before the submenu's own items are drawn:
		// once EndMenu() returns, the "last item" is whatever was hovered inside the submenu, not the header.
		const bool fromOpen = ImGui::BeginMenu(T(TKEY("scene_page_copy_from"), "From"));
		Util::AddTooltip(T(TKEY("scene_page_copy_from_tooltip"), "Copies another context's settings into this page."),
			ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled);
		if (fromOpen) {
			DrawCopyList(sources, [&](const CopySource& source) {
				StartCopy(source.context, context, source.displayName, periodScope);
				ImGui::CloseCurrentPopup();
			});
			ImGui::EndMenu();
		}
		ImGui::EndDisabled();

		const auto& destinations = GetCopyDestinations(context, false);
		ImGui::BeginDisabled(destinations.empty());
		const bool toOpen = ImGui::BeginMenu(T(TKEY("scene_page_copy_to"), "To"));
		Util::AddTooltip(T(TKEY("scene_page_copy_to_tooltip"), "Copies this page's settings into another context."),
			ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled);
		if (toOpen) {
			const auto sourceName = manager->GetSceneContextDisplayName(context);
			DrawCopyList(destinations, [&](const CopySource& destination) {
				StartCopy(context, destination.context, sourceName, periodScope);
				ImGui::CloseCurrentPopup();
			});
			ImGui::EndMenu();
		}
		ImGui::EndDisabled();

		ImGui::EndPopup();
	}

	void DrawCandidateRow(const CopyCandidate& candidate)
	{
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		if (candidate.compatible)
			ImGui::TextUnformatted(candidate.displayName.c_str());
		else
			Util::Text::Disabled("%s", candidate.displayName.c_str());

		ImGui::TableNextColumn();
		const auto value = candidate.value.dump();
		if (!candidate.compatible) {
			// No arrow: nothing is going to happen to this row.
			Util::Text::Disabled("%s", value.c_str());
			ImGui::SameLine();
			Util::Text::Warning("%s", GetRejectionText(candidate.rejection));
		} else if (candidate.conflicts) {
			ImGui::Text("%s -> %s", candidate.destinationValue->dump().c_str(), value.c_str());
		} else {
			ImGui::TextUnformatted(value.c_str());
		}
	}

	/// Draws wherever the destination page's own toolbar happens to render, which may not be the page
	/// the user is currently looking at (e.g. copying to a period other than the one on screen). The
	/// flow is global, so this only needs to run once per frame even if several toolbars call it.
	void DrawCopyPreview()
	{
		if (!copyFlow.active)
			return;

		static int lastDrawnFrame = -1;
		const int frame = ImGui::GetFrameCount();
		if (lastDrawnFrame == frame)
			return;
		lastDrawnFrame = frame;

		const char* title = T(TKEY("scene_page_copy_title"), "Copy settings");
		if (copyFlow.pendingOpen) {
			ImGui::OpenPopup(title);
			copyFlow.pendingOpen = false;
		}

		auto* manager = SceneSettingsManager::GetSingleton();
		bool open = true;
		std::optional<CopyConflictPolicy> decision;
		if (auto popup = Util::CenteredPopupModal(title, &open)) {
			const auto destinationName = manager->GetSceneContextDisplayName(copyFlow.destination);
			ImGui::TextWrapped("%s", std::vformat(T(TKEY("scene_page_copy_intro"), "Copying {} into {}."),
										std::make_format_args(copyFlow.sourceName, destinationName))
										.c_str());

			size_t conflicting = 0;
			size_t rejected = 0;
			for (const auto& candidate : copyFlow.candidates) {
				conflicting += candidate.conflicts ? 1 : 0;
				rejected += candidate.compatible ? 0 : 1;
			}
			Util::Text::Disabled("%s",
				std::vformat(T(TKEY("scene_page_copy_counts"), "{} already set here, {} cannot be copied."),
					std::make_format_args(conflicting, rejected))
					.c_str());
			ImGui::Spacing();

			const float scale = Util::GetUIScale();
			if (ImGui::BeginTable("##ScenePageCopyPreview", 2, kPreviewTableFlags,
					ImVec2(kPreviewWidth * scale, kPreviewHeight * scale))) {
				ImGui::TableSetupColumn(T(TKEY("scene_page_copy_column_setting"), "Setting"));
				ImGui::TableSetupColumn(T(TKEY("scene_page_copy_column_change"), "Change"));
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableHeadersRow();
				for (const auto& candidate : copyFlow.candidates)
					DrawCandidateRow(candidate);
				ImGui::EndTable();
			}

			ImGui::Spacing();
			if (ImGui::Button(T(TKEY("scene_page_copy_skip"), "Skip existing")))
				decision = CopyConflictPolicy::SkipExisting;
			Util::AddTooltip(T(TKEY("scene_page_copy_skip_tooltip"),
				"Copies only the settings this page does not already hold."));
			ImGui::SameLine();
			if (Util::WarningButton(T(TKEY("scene_page_copy_overwrite"), "Overwrite existing")))
				decision = CopyConflictPolicy::OverwriteExisting;
			Util::AddTooltip(T(TKEY("scene_page_copy_overwrite_tooltip"),
				"Replaces the values this page already holds with the ones listed above."));
			ImGui::SameLine();
			if (decision || ImGui::Button(T(TKEY("cancel"), "Cancel")))
				ImGui::CloseCurrentPopup();
		}

		if (decision)
			RunCopy(copyFlow.source, copyFlow.destination, *decision, copyFlow.periodScope);
		// The preview is discarded once its popup is gone, whether it was acted on or dismissed.
		if (!ImGui::IsPopupOpen(title))
			copyFlow = {};
	}

	void DrawClearConfirmation(const SceneContextId& context, PeriodScope periodScope)
	{
		if (!clearRequested || clearContext != context)
			return;
		if (clearConfirmation.Draw()) {
			SceneSettingsManager::GetSingleton()->ClearContextEntries(context, periodScope);
			clearRequested = false;
		} else if (!clearConfirmation.IsOpen()) {
			clearRequested = false;
		}
	}
}

void ScenePageToolbar::Draw(const SceneContextId& context, SceneSettingsManager::PeriodScope periodScope)
{
	auto* manager = SceneSettingsManager::GetSingleton();
	if (!manager)
		return;

	const auto summary = manager->GetContextUserEntrySummary(context, periodScope);
	const bool hasEntries = summary.total != 0;
	// A mixed page pauses rather than resumes: the button is a way out of that state, not into it.
	const bool pauseTarget = !summary.AllPaused();

	const char* toggleLabel = pauseTarget ? T(TKEY("scene_page_pause_all"), "Pause All") :
	                                        T(TKEY("scene_page_resume_all"), "Resume All");
	const char* copyLabel = T(TKEY("scene_page_copy"), "Copy");
	const char* exportLabel = T(TKEY("scene_page_export"), "Export");
	const char* clearLabel = T(TKEY("scene_page_clear"), "Clear");

	const auto& style = ImGui::GetStyle();
	auto* menu = Menu::GetSingleton();
	const bool hasClearIcon = menu && menu->uiIcons.deleteSettings.texture;
	// An image button is the image plus the same frame padding, so a font-sized icon matches the row.
	const float clearIconSize = ImGui::GetFontSize();
	const float clearWidth = hasClearIcon ? clearIconSize + style.FramePadding.x * 2.0f : ButtonWidth(clearLabel);
	const float width = ButtonWidth(toggleLabel) + ButtonWidth(copyLabel) + ButtonWidth(exportLabel) +
	                    clearWidth + style.ItemSpacing.x * 3.0f;
	const float margin = kRightMargin * Util::GetUIScale();
	if (const auto avail = ImGui::GetContentRegionAvail().x; avail > width + margin)
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width - margin);

	ImGui::PushID("ScenePageToolbar");

	ImGui::BeginDisabled(!hasEntries);
	if (ImGui::Button(toggleLabel))
		manager->SetContextEntriesPaused(context, pauseTarget, periodScope);
	ImGui::EndDisabled();
	Util::AddTooltip(pauseTarget ?
						 T(TKEY("scene_page_pause_all_tooltip"),
							 "Holds back every override on this page without losing its value.") :
						 T(TKEY("scene_page_resume_all_tooltip"), "Applies every override on this page again."),
		ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled);

	ImGui::SameLine();
	// The lists are rebuilt on open, so what they offer is never a frame behind the page.
	const bool hasSources = !GetCopySources(context, false).empty();
	const bool hasDestinations = !GetCopyDestinations(context, false).empty();
	ImGui::BeginDisabled(!hasSources && !hasDestinations);
	if (ImGui::Button(copyLabel)) {
		GetCopySources(context, true);
		GetCopyDestinations(context, true);
		ImGui::OpenPopup(kCopyPopupId);
	}
	ImGui::EndDisabled();
	Util::AddTooltip(T(TKEY("scene_page_copy_tooltip"), "Copies settings between this page and another context."),
		ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled);
	DrawCopyPopup(context, periodScope);

	ImGui::SameLine();
	ImGui::BeginDisabled();
	ImGui::Button(exportLabel);
	ImGui::EndDisabled();
	Util::AddTooltip(T(TKEY("scene_page_export_tooltip"), "Exporting a page as overwrites is coming soon."),
		ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled);

	ImGui::SameLine();
	ImGui::BeginDisabled(!hasEntries);
	const bool clearClicked = hasClearIcon ?
	                              Util::ErrorImageButton("##ScenePageClear", menu->uiIcons.deleteSettings.texture,
	                                  ImVec2(clearIconSize, clearIconSize)) :
	                              Util::ErrorTextButton(clearLabel);
	if (clearClicked) {
		auto count = summary.total;
		auto pageName = manager->GetSceneContextDisplayName(context);
		clearConfirmation.title = T(TKEY("scene_page_clear_title"), "Clear page");
		clearConfirmation.message = std::vformat(T(TKEY("scene_page_clear_message"),
													"Remove all {} settings from {}? Mod overrides are left alone."),
			std::make_format_args(count, pageName));
		clearConfirmation.confirmLabel = clearLabel;
		clearConfirmation.cancelLabel = T(TKEY("cancel"), "Cancel");
		clearContext = context;
		clearRequested = true;
		clearConfirmation.Request();
	}
	ImGui::EndDisabled();
	Util::AddTooltip(T(TKEY("scene_page_clear_tooltip"), "Removes every override this page holds."),
		ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled);

	DrawClearConfirmation(context, periodScope);
	DrawCopyPreview();

	ImGui::PopID();
}

#undef I18N_KEY_PREFIX
