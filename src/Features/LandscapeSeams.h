#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

struct LandscapeSeams : Feature
{
	virtual inline std::string GetName() override { return "Landscape Seams"; }
	virtual std::string GetDisplayName() override { return T("feature.landscape_seams.name", "Landscape Seam Blending"); }
	virtual inline std::string GetShortName() override { return "LandscapeSeams"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.landscape_seams.description", "Removes the hard lines where landscape textures stop at quad borders, by blending each quad's textures into its neighbours and lifting the six textures per quad limit."),
			{ T("feature.landscape_seams.key_feature_1", "Blends landscape textures across quad and cell borders"),
				T("feature.landscape_seams.key_feature_2", "Up to four extra texture layers per quad, ten in total"),
				T("feature.landscape_seams.key_feature_3", "Works with True PBR, Terrain Helper and terrain parallax"),
				T("feature.landscape_seams.key_feature_4", "No plugin edits, untouched quads render exactly as before") } };
	}

	static constexpr uint32_t GridSize = 17;
	static constexpr uint32_t GridVertices = GridSize * GridSize;
	static constexpr uint32_t PerimeterVertices = 64;
	static constexpr uint32_t EngineLayers = 6;
	static constexpr uint32_t MaxExtraLayers = 4;
	static constexpr uint32_t MaxLayers = EngineLayers + MaxExtraLayers;
	static constexpr uint32_t TexturesPerExtra = 4;
	static constexpr uint32_t FirstPSTexture = 104;
	static constexpr uint32_t NumPSTextures = 2 + MaxExtraLayers * TexturesPerExtra;

	struct Settings
	{
		bool Enabled = true;
		uint32_t BlendRadius = 3;
		uint32_t ExtraLayers = MaxExtraLayers;
	} settings;

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void PostPostLoad() override;
	/** @brief Rebuilds loaded quads after a settings change. */
	virtual void Prepass() override;
	/** @brief Releases land geometry the engine has dropped. Runs every frame, also outside the world or with the shader cache off. */
	virtual void Reset() override;

	struct QuadKey
	{
		uint32_t worldSpace = 0;
		int32_t x = 0;
		int32_t y = 0;

		bool operator==(const QuadKey&) const = default;
	};

	struct QuadKeyHash
	{
		size_t operator()(const QuadKey& a_key) const noexcept
		{
			uint64_t hash = a_key.worldSpace;
			hash = hash * 0x9E3779B97F4A7C15ull + static_cast<uint32_t>(a_key.x);
			hash = hash * 0x9E3779B97F4A7C15ull + static_cast<uint32_t>(a_key.y);
			return static_cast<size_t>(hash ^ (hash >> 32));
		}
	};

	using WeightGrid = std::array<std::array<uint8_t, GridVertices>, EngineLayers>;
	using Border = std::array<std::array<uint8_t, PerimeterVertices>, EngineLayers>;

	struct alignas(16) QuadData
	{
		float2 Origin;
		uint32_t Flags;
		uint32_t ExtraCount;
		float4 IsSnow;
		float4 SpecPower;
		float4 PBRParams[MaxExtraLayers];
		float4 GlintParams[MaxExtraLayers];
	};
	static_assert(sizeof(QuadData) == 176);

	struct Resources
	{
		winrt::com_ptr<ID3D11ShaderResourceView> weights;
		winrt::com_ptr<ID3D11ShaderResourceView> data;
		std::array<std::array<RE::NiSourceTexturePtr, TexturesPerExtra>, MaxExtraLayers> extras;
	};

	struct Quad
	{
		std::array<RE::TESLandTexture*, EngineLayers> slots{};
		Border border{};
		std::unique_ptr<WeightGrid> grid;
		RE::BSGeometry* geometry = nullptr;
		std::shared_ptr<Resources> resources;
		uint64_t generation = 0;
		bool pbr = false;
	};

	struct Healed
	{
		QuadKey key;
		uint64_t generation = 0;
		bool active = false;
		bool pbr = false;
		uint32_t extraCount = 0;
		std::array<RE::TESLandTexture*, MaxExtraLayers> extras{};
		std::array<std::array<uint8_t, GridVertices>, MaxLayers> weights{};
	};

	/** @brief True while the feature is loaded and enabled, so seam permutations may be drawn. */
	bool IsRenderable() const { return loaded && settings.Enabled; }
	/** @brief True when this land quad geometry has blend resources and should draw with the seams permutation. */
	bool IsBlended(RE::BSGeometry* a_geometry);
	/** @brief Binds the quad's weights, quad data and borrowed textures to t104-t121. Render thread only, before the draw. */
	void Bind(RE::BSGeometry* a_geometry);
	/** @brief Reads a newly set up land record's quads and rebuilds them and their neighbours. */
	void TESObjectLAND_SetupMaterial(RE::TESObjectLAND* a_land);

private:
	struct Hooks;

	void Heal(const QuadKey& a_key, const Quad& a_quad, Healed& a_out) const;
	std::shared_ptr<Resources> CreateResources(const Healed& a_healed) const;
	void Rebuild(const std::vector<QuadKey>& a_keys);
	void RebuildAll();
	void Sweep();

	std::shared_mutex mutex;
	std::unordered_map<QuadKey, Quad, QuadKeyHash> quads;
	std::unordered_map<RE::BSGeometry*, QuadKey> loadedQuads;
	std::vector<RE::BSGeometry*> retiredGeometry;
	std::atomic_bool rebuildRequested = false;
	uint32_t currentWorldSpace = 0;
};
