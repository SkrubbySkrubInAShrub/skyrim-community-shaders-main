#pragma once

#include "Buffer.h"

struct ScreenSpaceShadows : Feature
{
public:
	virtual inline std::string GetName() override { return "Screen Space Shadows"; }
	virtual std::string GetDisplayName() override { return T("feature.screen_space_shadows.name", "Screen Space Shadows"); }
	virtual inline std::string GetShortName() override { return "ScreenSpaceShadows"; }
	virtual inline std::string_view GetShaderDefineName() override { return "SCREEN_SPACE_SHADOWS"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.screen_space_shadows.description", "Screen Space Shadows enhances shadow quality by adding detailed contact shadows and improving shadow accuracy.\nThis technique adds fine-detail shadows that traditional shadow mapping might miss."),
			{ T("feature.screen_space_shadows.key_feature_1", "Enhanced contact shadows"),
				T("feature.screen_space_shadows.key_feature_2", "Improved shadow detail"),
				T("feature.screen_space_shadows.key_feature_3", "Better shadow accuracy"),
				T("feature.screen_space_shadows.key_feature_4", "Fine-scale shadow effects"),
				T("feature.screen_space_shadows.key_feature_5", "Configurable shadow contrast") } };
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	struct BendSettings
	{
		float SurfaceThickness = 0.02f;
		float BilinearThreshold = 0.02f;
		float ShadowContrast = 1.0f;
		uint Enable = 1;
		uint SampleCount = 1;
		uint pad0[3];
	};

	BendSettings bendSettings;

	struct alignas(16) RaymarchCB
	{
		// Runtime data returned from BuildDispatchList():
		float LightCoordinate[4];  // Values stored in DispatchList::LightCoordinate_Shader by BuildDispatchList()
		int WaveOffset[2];         // Values stored in DispatchData::WaveOffset_Shader by BuildDispatchList()

		// Renderer Specific Values:
		float FarDepthValue;   // Set to the Depth Buffer Value for the far clip plane, as determined by renderer projection matrix setup (typically 0).
		float NearDepthValue;  // Set to the Depth Buffer Value for the near clip plane, as determined by renderer projection matrix setup (typically 1).

		// Sampling data:
		float InvDepthTextureSize[2];  // Inverse of the texture dimensions for 'DepthTexture' (used to convert from pixel coordinates to UVs)
									   // If 'PointBorderSampler' is an Unnormalized sampler, then this value can be hard-coded to 1.
									   // The 'USE_HALF_PIXEL_OFFSET' macro might need to be defined if sampling at exact pixel coordinates isn't precise (e.g., if odd patterns appear in the shadow).

		float2 DynamicRes;

		BendSettings settings;
	};
	STATIC_ASSERT_ALIGNAS_16(RaymarchCB);

	ID3D11SamplerState* pointBorderSampler = nullptr;

	ConstantBuffer* raymarchCB = nullptr;
	ID3D11ComputeShader* raymarchCS = nullptr;

	/** @brief What a march's output depends on besides the depth it reads. */
	struct MarchInputs
	{
		std::array<float, 4> lightProjection{};
		std::array<int, 2> viewport{};
		std::array<float, 2> dynamicRes{};
		std::array<float, 3> settings{};
		uint sampleCount = 0;
		bool operator==(const MarchInputs&) const = default;
	};

	/** @brief Frame, depth view and inputs of the last whole-screen march (DrawShadows without changed tiles). */
	uint32_t marchFrame = UINT32_MAX;
	ID3D11ShaderResourceView* marchDepthSRV = nullptr;
	MarchInputs marchInputs;

	Texture2D* screenSpaceShadowsTexture = nullptr;

	/** @brief Creates the raymarch constant buffer, point border sampler, and shadow output texture. */
	virtual void SetupResources() override;

	/** @brief Draws the ImGui settings UI for screen-space shadow configuration. */
	virtual void DrawSettings() override;

	/** @brief Releases the compiled raymarch compute shader for recompilation. */
	virtual void ClearShaderCache() override;
	/** @brief Releases the raymarch compute shader so it is recompiled on next use. */
	void InvalidateRaymarchShaders();
	/** @brief Calculates the resolution-scaled and quantized sample count for the raymarch shader. */
	uint GetScaledSampleCount();
	uint lastCompiledSampleCount = 0;
	/**
	 * @brief Returns the compiled raymarch compute shader, recompiling if the sample count changed.
	 * @return The compiled ID3D11ComputeShader, or nullptr on failure.
	 */
	ID3D11ComputeShader* GetComputeRaymarch();
	/**
	 * @brief The raymarch shader's defines for a sample count.
	 * @param a_sampleCount The SAMPLE_COUNT value; must outlive the returned list.
	 */
	std::vector<std::pair<const char*, const char*>> GetRaymarchDefines(const std::string& a_sampleCount) const;

	/** @brief Clears the shadow texture and dispatches shadow ray marching if conditions are met. Runs after every feature's Prepass so the march sees the depth they wrote. */
	virtual void PostPrepass() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/**
	 * @brief Dispatches the Bend SSS compute shader to generate screen-space contact shadows.
	 * @param a_changedTiles Optional R8_UINT view, one texel per 8x8 depth texels, non-zero where the depth
	 *        may differ from what this frame's whole-screen march read (each changed tile with its neighbours
	 *        marked). Wavefronts whose depth reads all land on unmarked tiles keep that march's output.
	 *        Ignored unless that march ran this frame on the same depth view and inputs.
	 * @param a_changedCS The raymarch built from GetRaymarchDefines(GetScaledSampleCount()) plus SSS_CHANGED_TILES.
	 * @return True when the march used a_changedTiles, false when it redid every wavefront.
	 */
	bool DrawShadows(ID3D11ShaderResourceView* a_changedTiles = nullptr, ID3D11ComputeShader* a_changedCS = nullptr);

	virtual void RestoreDefaultSettings() override;

};
