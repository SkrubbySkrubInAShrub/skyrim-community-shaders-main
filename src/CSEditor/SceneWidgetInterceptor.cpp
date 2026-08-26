#include "SceneWidgetInterceptor.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
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
	Proxy armedProxy;

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
	// Unbound/Unavailable controls are greyed via BeginDisabled precisely so an edit here cannot
	// rewrite the feature's base value; BeginDragDropTarget ignores that flag, so drop acceptance has
	// to check the resolved state itself or it would bypass the same protection.
	bool CanAcceptPaletteDrop(const SceneWidgetBinding::Guard& a_guard)
	{
		const auto state = a_guard.GetState();
		return state != SceneWidgetBinding::State::Unbound &&
		       state != SceneWidgetBinding::State::Unavailable;
	}

	// Mirrors AcceptPaletteColorDrop below: the palette's "VALUE_DND" payload is a raw float, not a
	// type ImGui's own widgets recognise, so every intercepted scalar slider must accept it by hand.
	bool AcceptPaletteValueDrop(double& a_outValue)
	{
		bool accepted = false;
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VALUE_DND")) {
				if (payload->DataSize == sizeof(float)) {
					a_outValue = static_cast<double>(*static_cast<const float*>(payload->Data));
					accepted = true;
				}
			}
			ImGui::EndDragDropTarget();
		}
		return accepted;
	}

	bool DetouredSliderFloat(const char* label, float* v, float vMin, float vMax,
		const char* format, ImGuiSliderFlags flags)
	{
		if (!ShouldIntercept())
			return RealSliderFloat(label, v, vMin, vMax, format, flags);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::Float(v));
		bool changed = RealSliderFloat(label, guard.Float(), vMin, vMax, format, flags);
		if (double dropped; CanAcceptPaletteDrop(guard) && AcceptPaletteValueDrop(dropped)) {
			*guard.Float() = static_cast<float>(std::clamp(dropped, (double)vMin, (double)vMax));
			changed = true;
		}
		return guard.Finish(changed);
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
		bool changed = RealSliderInt(label, guard.Int(), vMin, vMax, format, flags);
		if (double dropped; CanAcceptPaletteDrop(guard) && AcceptPaletteValueDrop(dropped)) {
			*guard.Int() = std::clamp(static_cast<int>(std::lround(dropped)), vMin, vMax);
			changed = true;
		}
		return guard.Finish(changed);
	}

	bool DetouredSliderScalar(const char* label, ImGuiDataType dataType, void* data,
		const void* min, const void* max, const char* format, ImGuiSliderFlags flags)
	{
		if (!ShouldIntercept())
			return RealSliderScalar(label, dataType, data, min, max, format, flags);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::Scalar(data, dataType));
		bool changed = RealSliderScalar(label, dataType, guard.Raw(), min, max, format, flags);
		if (double dropped; CanAcceptPaletteDrop(guard) && AcceptPaletteValueDrop(dropped)) {
			SceneWidgetBinding::WriteScalarValue(guard.Raw(), dataType, dropped);
			changed = true;
		}
		return guard.Finish(changed);
	}

	bool DetouredSliderAngle(const char* label, float* radians, float degreesMin, float degreesMax,
		const char* format, ImGuiSliderFlags flags)
	{
		if (!ShouldIntercept())
			return RealSliderAngle(label, radians, degreesMin, degreesMax, format, flags);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::Float(radians));
		bool changed = RealSliderAngle(label, guard.Float(), degreesMin, degreesMax, format, flags);
		// The palette stores raw slider units, and an angle slider's raw unit is radians, so a
		// dropped value is written as-is rather than reinterpreted through the degree bounds.
		if (double dropped; CanAcceptPaletteDrop(guard) && AcceptPaletteValueDrop(dropped)) {
			*guard.Float() = static_cast<float>(dropped);
			changed = true;
		}
		return guard.Finish(changed);
	}

	bool DetouredCheckbox(const char* label, bool* v)
	{
		if (!ShouldIntercept())
			return RealCheckbox(label, v);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::Bool(v));
		return guard.Finish(RealCheckbox(label, guard.Bool()));
	}

	// The palette drags a custom "COLOR_DND" payload rather than ImGui's native _COL3F/_COL4F, so
	// forwarding straight to the real widget (which only recognises its own payload) drops palette
	// support entirely; every intercepted color control needs this wired in by hand.
	bool AcceptPaletteColorDrop(float* col, int componentCount)
	{
		bool accepted = false;
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("COLOR_DND")) {
				if (payload->DataSize == sizeof(float3)) {
					std::memcpy(col, payload->Data, sizeof(float) * std::min(componentCount, 3));
					accepted = true;
				}
			}
			ImGui::EndDragDropTarget();
		}
		return accepted;
	}

	bool DetouredColorEdit3(const char* label, float col[3], ImGuiColorEditFlags flags)
	{
		if (!ShouldIntercept())
			return RealColorEdit3(label, col, flags);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::FloatVector(col, 3));
		const bool changed = RealColorEdit3(label, guard.Float(), flags);
		const bool dropped = CanAcceptPaletteDrop(guard) && AcceptPaletteColorDrop(guard.Float(), 3);
		return guard.Finish(dropped || changed);
	}

	bool DetouredColorEdit4(const char* label, float col[4], ImGuiColorEditFlags flags)
	{
		if (!ShouldIntercept())
			return RealColorEdit4(label, col, flags);
		InterceptedCall interceptedCall;
		SceneWidgetBinding::Guard guard(label, SceneWidgetBinding::Value::FloatVector(col, 4));
		const bool changed = RealColorEdit4(label, guard.Float(), flags);
		const bool dropped = CanAcceptPaletteDrop(guard) && AcceptPaletteColorDrop(guard.Float(), 4);
		return guard.Finish(dropped || changed);
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
		// A radio group is several calls against one address; only the last owns the gutter.
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

SceneWidgetInterceptor::ProxyScope::ProxyScope(const void* a_member, float a_displayScale) :
	previous(armedProxy)
{
	assert(a_displayScale != 0.0f && "a proxied control cannot collapse its member to one value");
	armedProxy = { a_member, a_displayScale };
}

SceneWidgetInterceptor::ProxyScope::~ProxyScope()
{
	armedProxy = previous;
}

const SceneWidgetInterceptor::Context* SceneWidgetInterceptor::GetArmedContext()
{
	return armed ? &armedContext : nullptr;
}

const SceneWidgetInterceptor::Proxy* SceneWidgetInterceptor::GetArmedProxy()
{
	return armedProxy.member ? &armedProxy : nullptr;
}

bool SceneWidgetInterceptor::IsArmed()
{
	return armed;
}
