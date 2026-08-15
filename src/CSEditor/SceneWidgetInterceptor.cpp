#include "SceneWidgetInterceptor.h"

#include <algorithm>
#include <cassert>
#include <span>
#include <string>

#include <detours/detours.h>
#include <imgui.h>

#include "SceneSettingsCatalog.generated.h"
#include "SceneWidgetBinding.h"

namespace
{
	using namespace SceneWidgetInterceptor;

	bool installed = false;
	std::string installError;

	bool armed = false;
	Context armedContext;

	// --- Originals ---
	auto* RealSliderFloat = &ImGui::SliderFloat;
	auto* RealSliderFloat2 = &ImGui::SliderFloat2;
	auto* RealSliderInt = &ImGui::SliderInt;
	auto* RealSliderScalar = &ImGui::SliderScalar;
	auto* RealSliderAngle = &ImGui::SliderAngle;
	auto* RealCheckbox = &ImGui::Checkbox;
	auto* RealColorEdit3 = &ImGui::ColorEdit3;
	auto* RealColorEdit4 = &ImGui::ColorEdit4;
	// Both Combo overloads are used in src/Features, and RadioButton is overloaded too, so the
	// address of each has to be disambiguated by its exact signature.
	auto* RealComboArray = static_cast<bool (*)(const char*, int*, const char* const[], int, int)>(
		&ImGui::Combo);
	auto* RealComboZeroSeparated = static_cast<bool (*)(const char*, int*, const char*, int)>(
		&ImGui::Combo);
	auto* RealRadioButton = static_cast<bool (*)(const char*, int*, int)>(&ImGui::RadioButton);

	// ImGui delegates between entry points we also detour (SliderFloat -> SliderScalar,
	// ColorEdit3 -> ColorEdit4), and the gutter toggle is itself a Checkbox: only the outermost binds.
	bool insideInterceptedCall = false;

	struct InterceptedCall
	{
		InterceptedCall() { insideInterceptedCall = true; }
		~InterceptedCall() { insideInterceptedCall = false; }

		InterceptedCall(const InterceptedCall&) = delete;
		InterceptedCall& operator=(const InterceptedCall&) = delete;
	};

	bool ShouldIntercept()
	{
		return armed && !insideInterceptedCall;
	}
}

namespace
{
	bool DetouredSliderFloat(const char* label, float* v, float vMin, float vMax,
		const char* format, ImGuiSliderFlags flags)
	{
		if (!ShouldIntercept())
			return RealSliderFloat(label, v, vMin, vMax, format, flags);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::Float(v));
		return guard.Finish(RealSliderFloat(label, guard.Float(), vMin, vMax, format, flags));
	}

	bool DetouredSliderFloat2(const char* label, float v[2], float vMin, float vMax,
		const char* format, ImGuiSliderFlags flags)
	{
		if (!ShouldIntercept())
			return RealSliderFloat2(label, v, vMin, vMax, format, flags);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::FloatVector(v, 2));
		return guard.Finish(RealSliderFloat2(label, guard.Float(), vMin, vMax, format, flags));
	}

	bool DetouredSliderInt(const char* label, int* v, int vMin, int vMax,
		const char* format, ImGuiSliderFlags flags)
	{
		if (!ShouldIntercept())
			return RealSliderInt(label, v, vMin, vMax, format, flags);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::Int(v));
		return guard.Finish(RealSliderInt(label, guard.Int(), vMin, vMax, format, flags));
	}

	bool DetouredSliderScalar(const char* label, ImGuiDataType dataType, void* data,
		const void* min, const void* max, const char* format, ImGuiSliderFlags flags)
	{
		if (!ShouldIntercept())
			return RealSliderScalar(label, dataType, data, min, max, format, flags);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::Scalar(data, dataType));
		return guard.Finish(
			RealSliderScalar(label, dataType, guard.Raw(), min, max, format, flags));
	}

	bool DetouredSliderAngle(const char* label, float* radians, float degreesMin, float degreesMax,
		const char* format, ImGuiSliderFlags flags)
	{
		if (!ShouldIntercept())
			return RealSliderAngle(label, radians, degreesMin, degreesMax, format, flags);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::Float(radians));
		return guard.Finish(
			RealSliderAngle(label, guard.Float(), degreesMin, degreesMax, format, flags));
	}

	bool DetouredCheckbox(const char* label, bool* v)
	{
		if (!ShouldIntercept())
			return RealCheckbox(label, v);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::Bool(v));
		return guard.Finish(RealCheckbox(label, guard.Bool()));
	}

	bool DetouredColorEdit3(const char* label, float col[3], ImGuiColorEditFlags flags)
	{
		if (!ShouldIntercept())
			return RealColorEdit3(label, col, flags);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::FloatVector(col, 3));
		return guard.Finish(RealColorEdit3(label, guard.Float(), flags));
	}

	bool DetouredColorEdit4(const char* label, float col[4], ImGuiColorEditFlags flags)
	{
		if (!ShouldIntercept())
			return RealColorEdit4(label, col, flags);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::FloatVector(col, 4));
		return guard.Finish(RealColorEdit4(label, guard.Float(), flags));
	}

	bool DetouredComboArray(const char* label, int* current, const char* const items[],
		int itemCount, int popupMaxHeight)
	{
		if (!ShouldIntercept())
			return RealComboArray(label, current, items, itemCount, popupMaxHeight);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::Int(current));
		return guard.Finish(
			RealComboArray(label, guard.Int(), items, itemCount, popupMaxHeight));
	}

	bool DetouredComboZeroSeparated(const char* label, int* current, const char* items,
		int popupMaxHeight)
	{
		if (!ShouldIntercept())
			return RealComboZeroSeparated(label, current, items, popupMaxHeight);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::Int(current));
		return guard.Finish(RealComboZeroSeparated(label, guard.Int(), items, popupMaxHeight));
	}

	bool DetouredRadioButton(const char* label, int* v, int buttonValue)
	{
		if (!ShouldIntercept())
			return RealRadioButton(label, v, buttonValue);
		// A radio group is several calls against one address; only the first owns the gutter.
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::Int(v),
			SceneWidgetBinding::GutterPolicy::GroupMember);
		return guard.Finish(RealRadioButton(label, guard.Int(), buttonValue));
	}
}

namespace
{
	/// One detourable entry point, named so the catalog's coverage list can be checked against it.
	struct DetourEntry
	{
		std::string_view name;
		PVOID* original;
		PVOID replacement;
	};

	std::span<const DetourEntry> GetDetourTable()
	{
		// Function-local so the entries are built after the Real* pointers are initialised.
		static const DetourEntry table[] = {
			{ "SliderFloat", reinterpret_cast<PVOID*>(&RealSliderFloat), reinterpret_cast<PVOID>(&DetouredSliderFloat) },
			{ "SliderFloat2", reinterpret_cast<PVOID*>(&RealSliderFloat2), reinterpret_cast<PVOID>(&DetouredSliderFloat2) },
			{ "SliderInt", reinterpret_cast<PVOID*>(&RealSliderInt), reinterpret_cast<PVOID>(&DetouredSliderInt) },
			{ "SliderScalar", reinterpret_cast<PVOID*>(&RealSliderScalar), reinterpret_cast<PVOID>(&DetouredSliderScalar) },
			{ "SliderAngle", reinterpret_cast<PVOID*>(&RealSliderAngle), reinterpret_cast<PVOID>(&DetouredSliderAngle) },
			{ "Checkbox", reinterpret_cast<PVOID*>(&RealCheckbox), reinterpret_cast<PVOID>(&DetouredCheckbox) },
			{ "ColorEdit3", reinterpret_cast<PVOID*>(&RealColorEdit3), reinterpret_cast<PVOID>(&DetouredColorEdit3) },
			{ "ColorEdit4", reinterpret_cast<PVOID*>(&RealColorEdit4), reinterpret_cast<PVOID>(&DetouredColorEdit4) },
			{ "Combo", reinterpret_cast<PVOID*>(&RealComboArray), reinterpret_cast<PVOID>(&DetouredComboArray) },
			{ "Combo", reinterpret_cast<PVOID*>(&RealComboZeroSeparated), reinterpret_cast<PVOID>(&DetouredComboZeroSeparated) },
			{ "RadioButton", reinterpret_cast<PVOID*>(&RealRadioButton), reinterpret_cast<PVOID>(&DetouredRadioButton) },
		};
		return table;
	}

	/// Every widget kind the catalog needs must have a detour, or scene authoring silently loses it.
	bool VerifyCoverage()
	{
		for (const auto required : SceneSettingsCatalog::GetRequiredInterceptorEntryPoints()) {
			const auto matches = [required](const DetourEntry& entry) { return entry.name == required; };
			if (!std::ranges::any_of(GetDetourTable(), matches)) {
				installError = std::string{ required };
				assert(false && "SceneWidgetInterceptor detour table is missing a catalog entry point");
				return false;
			}
		}
		return true;
	}
}

bool SceneWidgetInterceptor::Install()
{
	if (installed)
		return true;

	installError.clear();
	if (!VerifyCoverage()) {
		logger::error(
			"SceneWidgetInterceptor: no detour for required entry point '{}'; "
			"scene authoring is off for this session",
			installError);
		return false;
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	for (const auto& entry : GetDetourTable()) {
		if (const auto result = DetourAttach(entry.original, entry.replacement); result != NO_ERROR) {
			installError = std::string{ entry.name };
			// Abort rolls back every attach in this transaction, so nothing is left half-hooked.
			DetourTransactionAbort();
			logger::error(
				"SceneWidgetInterceptor: DetourAttach failed for ImGui::{} ({}); "
				"scene authoring is off for this session",
				installError, result);
			return false;
		}
	}
	if (const auto result = DetourTransactionCommit(); result != NO_ERROR) {
		installError = "DetourTransactionCommit";
		logger::error(
			"SceneWidgetInterceptor: DetourTransactionCommit failed ({}); "
			"scene authoring is off for this session",
			result);
		return false;
	}

	installed = true;
	logger::info("SceneWidgetInterceptor: installed {} entry points", GetDetourTable().size());
	return true;
}

bool SceneWidgetInterceptor::IsInstalled()
{
	return installed;
}

std::string_view SceneWidgetInterceptor::GetInstallError()
{
	return installError;
}

SceneWidgetInterceptor::Scope::Scope(const Context& context) :
	previous(armedContext), previousArmed(armed)
{
	armedContext = context;
	armed = installed && context.feature != nullptr;
}

SceneWidgetInterceptor::Scope::~Scope()
{
	armedContext = previous;
	armed = previousArmed;
}

const SceneWidgetInterceptor::Context* SceneWidgetInterceptor::GetArmedContext()
{
	return armed ? &armedContext : nullptr;
}

bool SceneWidgetInterceptor::IsArmed()
{
	return armed;
}
