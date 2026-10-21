#pragma once

#include <winrt/base.h>

#include "Buffer.h"

/**
 * @brief Renders rain as refractive drops that reflect and refract the Dynamic Cubemaps environment.
 * While Effects 11 is active its preset owns the settings and supplies the raindrop texture.
 */
struct RainRefraction : Feature
{
public:
	virtual bool IsCore() const override { return true; }

	virtual inline std::string GetName() override { return "Rain Refraction"; }
	virtual std::string GetDisplayName() override { return T("feature.rain_refraction.name", "Rain Refraction"); }
	virtual inline std::string GetShortName() override { return "RainRefraction"; }
	virtual inline std::string_view GetShaderDefineName() override { return "RAIN_REFRACTION"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSky; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Particle; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.rain_refraction.description", "Renders rain as refractive drops that reflect and refract the surrounding environment."),
			{ T("feature.rain_refraction.key_feature_1", "Raindrop normal map with Fresnel reflection and refraction from Dynamic Cubemaps"),
				T("feature.rain_refraction.key_feature_4", "Drops refract the scene behind them like a lens, falling back to Dynamic Cubemaps off screen"),
				T("feature.rain_refraction.key_feature_2", "Adjustable motion stretch and transparency"),
				T("feature.rain_refraction.key_feature_3", "Driven by the Effects 11 preset and its enbraindrops.png while Effects 11 is active") } };
	}

	static constexpr float kMinMotionStretch = 0.01f;
	static constexpr float kMaxMotionStretch = 3.0f;
	static constexpr float kMaxRefractionFactor = 2.0f;
	static constexpr float kMaxMotionBluriness = 6.0f;

	/** @brief User settings, used while Effects 11 is not active; mirrors the preset's [RAIN] Enable, MotionStretch, MotionTransparency, RefractionFactor, MotionBluriness and EnableSupersampling. */
	struct Settings
	{
		bool Enable = true;
		float MotionStretch = 2.0f;
		float MotionTransparency = 0.91f;
		bool ScreenRefraction = true;
		float RefractionFactor = 1.0f;
		float MotionBluriness = 1.0f;
		bool Supersampling = false;
	} settings;

	/** @brief Feature buffer data, matching RainRefractionSettings in SharedData.hlsli; Enable is cleared when the raindrop texture or blend state is missing. */
	struct PerFrame
	{
		uint Enable;
		float MotionStretch;
		float MotionTransparency;
		uint ScreenRefraction;
		float RefractionFactor;
		float MotionBluriness;
		float Brightness;
		uint Supersampling;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);

	/** @brief A loaded raindrop texture, or the reason it failed to load. */
	struct RaindropTexture
	{
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		std::string status;  ///< Failure reason; empty when loaded
	};

	RaindropTexture defaultRaindrops;  ///< Bundled with the feature
	RaindropTexture presetRaindrops;   ///< enbraindrops.png from the Effects 11 preset

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;
	/** @brief Loads the bundled default raindrop texture. */
	virtual void SetupResources() override;
	/** @brief Releases the background downsample compute shader so it is recompiled on next use. */
	virtual void ClearShaderCache() override;

	/** @brief Shader data for the current frame, taken from the Effects 11 preset while IsManagedByENB(). */
	PerFrame GetCommonBufferData();
	/** @brief Returns true while Effects 11 is active, which then owns the rain settings. */
	bool IsManagedByENB() const;
	/** @brief The preset's raindrop texture while managed by ENB and it loaded, otherwise the bundled default; null if neither loaded. */
	ID3D11ShaderResourceView* GetRaindropSRV() const;
	/** @brief Loads enbraindrops.png from the Effects 11 preset folder; called whenever a preset is applied. */
	void LoadPresetRaindrops(const std::filesystem::path& a_presetPath);

	/** @brief Binds the raindrop texture and the vertex shader feature buffers for refractive rain passes. */
	void ModifyParticle();
	/** @brief Switches refractive rain passes to alpha blending and binds the scene behind the rain. */
	void ParticleShaderHacks();

private:
	/** @brief Returns true when the current particle pass is rain and refractive rain is active this frame. */
	bool IsRefractiveRainPass();

	/** @brief Copies the bound main render target into the mipmapped half resolution background, once per frame. */
	void CaptureBackground();
	/** @brief Creates the background texture and its downsample shader on first use; false if unsupported. */
	bool EnsureBackgroundResources();

	winrt::com_ptr<ID3D11BlendState> alphaBlendState;

	struct alignas(16) DownsampleCB
	{
		float2 InvSourceSize;
		uint OutputSize[2];
		float2 SourceUVMax;
		float2 pad0;
	};
	STATIC_ASSERT_ALIGNAS_16(DownsampleCB);

	std::unique_ptr<Texture2D> backgroundTexture;
	std::unique_ptr<ConstantBuffer> downsampleCB;
	winrt::com_ptr<ID3D11ComputeShader> downsampleCS;
	bool backgroundUnsupported = false;
	bool downsampleCompileFailed = false;
	uint32_t backgroundFrame = UINT32_MAX;

	// The feature buffer is rebuilt several times per frame and the particle hooks run per pass
	PerFrame perFrameCache{};
	Util::FrameChecker perFrameCacheChecker;
};
