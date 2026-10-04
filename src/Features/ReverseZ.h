#pragma once

#include "Feature.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ID3D11DepthStencilState;
struct ID3D11DepthStencilView;
struct ID3D11RasterizerState;
struct ID3D11RenderTargetView;

namespace RE
{
	class NiCamera;
}

namespace globals
{
	struct FrameBuffer;
}

struct ReverseZ : Feature
{
	virtual std::string GetName() override { return "Reverse Z-Buffer"; }
	virtual std::string GetDisplayName() override { return T("feature.reverse_z.name", "Reverse Z-Buffer"); }
	virtual std::string GetShortName() override { return "ReverseZ"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kDisplay; }
	virtual std::string_view GetShaderDefineName() override { return "REVERSE_Z"; }

	/** @brief Returns true for every shader type while active: a shader that includes the shared
	 *  reverse-Z header without the define silently compiles to identity depth math. */
	virtual bool HasShaderDefine(RE::BSShader::Type) override;

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;

	struct Settings
	{
		bool EnableReverseZ = false;
	};

	Settings settings;

	inline static constexpr std::array<Util::Settings::RestartFieldInfo, 1> kRestartFields{ {
		{ "EnableReverseZ", "Reverse Z-Buffer", offsetof(Settings, EnableReverseZ), sizeof(bool) },
	} };

	std::span<const Util::Settings::RestartFieldInfo> GetRestartRequiredFields() const override { return kRestartFields; }
	const void* GetBootValue(std::string_view jsonKey) const override;
	const void* GetSettingsBlob() const override { return &settings; }
	size_t GetSettingsBlobSize() const override { return sizeof(settings); }

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	/** @brief Reallocates the engine depth targets in a float format before other features cache their views. */
	virtual void OnRenderTargetsCreated() override;
	/** @brief Installs the D3D state hooks. */
	virtual void SetupResources() override;
	virtual void DataLoaded() override;
	virtual void PostPostLoad() override;

	/** @brief Reverses the camera matrices b12 carries before they reach the GPU. */
	virtual bool WantsFrameBufferFixup() const override { return activeThisBoot; }
	virtual void FixupMappedFrameBuffer(globals::FrameBuffer& a_frameBuffer) override;

	/** @brief True while the reversed convention is actually in effect for this boot. */
	[[nodiscard]] bool IsActive() const { return activeThisBoot; }

	/** @brief The raw depth value the far plane carries under the active convention. */
	[[nodiscard]] float GetFarDepth() const { return IsActive() ? 0.0f : 1.0f; }
	/** @brief The raw depth value the near plane carries under the active convention. */
	[[nodiscard]] float GetNearDepth() const { return IsActive() ? 1.0f : 0.0f; }

	/** @brief Latches the boot-time enable state; the depth format change cannot be undone live. */
	void LatchBootState();
	/** @brief Converts every depth target the engine rasterizes the scene into. */
	void SetupDepthTargets();
	/** @brief Installs the depth-target reallocation's device-context vtable detours. */
	void InstallRuntimeHooks();

	/** @brief Reverses the projection of the cache entry the engine just built. */
	void ApplyReverseProjection(void* a_cameraStateEntry, const RE::NiCamera* a_camera);
	/** @brief Reverses the camera the engine just published for the current depth target. */
	void ReversePublishedProjection();
	/** @brief Logs each distinct camera/target/convention triple once, for in-game diagnosis. */
	void TracePublishedCamera(const RE::NiCamera* a_camera);

	/** @brief Substitutes the reflection cubemap's own depth view when a cubemap face is drawn with a converted depth view. */
	[[nodiscard]] ID3D11DepthStencilView* ResolveCubemapFaceDepthView(ID3D11RenderTargetView* a_renderTarget, ID3D11DepthStencilView* a_depthView) const;
	/** @brief Returns a cached copy of a_state with its depth comparison flipped. */
	[[nodiscard]] ID3D11DepthStencilState* GetReversedState(ID3D11DepthStencilState* a_state);
	/** @brief Returns a cached copy of a_state with its depth bias negated. */
	[[nodiscard]] ID3D11RasterizerState* GetReversedRasterizerState(ID3D11RasterizerState* a_state);
	/** @brief True when a camera publishes a projection a converted depth target must see reversed. */
	[[nodiscard]] bool ExpectPublishedReversal(const RE::NiCamera* a_camera, bool a_renderingCubemap) const;
	/** @brief True when a_target was reallocated in the float format. */
	[[nodiscard]] bool IsConvertedDepthTarget(uint32_t a_target) const;
	/** @brief True when a_view belongs to a converted depth target. */
	[[nodiscard]] bool IsReverseDepthView(ID3D11DepthStencilView* a_view) const;

private:
	Settings bootSettings;
	bool bootSettingsLatched = false;
	bool bootLatched = false;
	bool activeThisBoot = false;
	bool depthTargetsConverted = false;
	bool runtimeHooksInstalled = false;
	uint32_t convertedTargetMask = 0;
};
