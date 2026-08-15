#pragma once

#include <string_view>

#include "SceneSettingsManager.h"

struct Feature;

/// Intercepts ImGui widget calls so a Scene Manager replica of a feature's DrawSettings can bind
/// each control to a scene entry. Detours are installed once and stay inert until a Scope is alive.
namespace SceneWidgetInterceptor
{
	/// What the armed replica is editing. Copied into the interceptor for the scope's lifetime.
	struct Context
	{
		Feature* feature = nullptr;
		SceneSettingsManager::SceneContextId contextId;
		/// False writes every edit to all six periods, which is what "no time of day" means.
		bool perPeriod = true;
	};

	/** @brief Installs the detours. Idempotent; call from the render thread before the first frame. */
	bool Install();

	bool IsInstalled();

	/// Empty while healthy; otherwise names the entry point whose attach failed.
	std::string_view GetInstallError();

	/// Arms interception for the duration of one replica's DrawSettings call.
	class Scope
	{
	public:
		explicit Scope(const Context& context);
		~Scope();

		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		Context previous;
		bool previousArmed;
	};

	/// The armed context, or nullptr when unarmed. For SceneWidgetBinding only.
	const Context* GetArmedContext();
}
