// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include <DDSTextureLoader.h>

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/IBL.h"
#include "Features/LightLimitFix.h"
#include "Features/TerrainShadows.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/Skylighting.h"
#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

void SnowDeformation::CopySRVResource(ID3D11ShaderResourceView* a_srcSRV, const char* a_name,
	winrt::com_ptr<ID3D11Texture2D>& a_tex, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv, const D3D11_BOX* a_box)
{
	winrt::com_ptr<ID3D11Resource> srcRes;
	a_srcSRV->GetResource(srcRes.put());
	auto srcTex = srcRes.try_as<ID3D11Texture2D>();
	if (!srcTex)
		return;

	D3D11_TEXTURE2D_DESC srcDesc;
	srcTex->GetDesc(&srcDesc);

	bool recreate = !a_tex || !a_srv;
	if (a_tex) {
		D3D11_TEXTURE2D_DESC haveDesc;
		a_tex->GetDesc(&haveDesc);
		recreate |= haveDesc.Width != srcDesc.Width || haveDesc.Height != srcDesc.Height ||
		            haveDesc.ArraySize != srcDesc.ArraySize || haveDesc.Format != srcDesc.Format;
	}
	if (recreate) {
		a_srv = nullptr;
		a_tex = nullptr;

		D3D11_TEXTURE2D_DESC copyDesc = srcDesc;
		copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		copyDesc.MiscFlags = 0;
		copyDesc.Usage = D3D11_USAGE_DEFAULT;
		copyDesc.CPUAccessFlags = 0;
		if (FAILED(globals::d3d::device->CreateTexture2D(&copyDesc, nullptr, a_tex.put())))
			return;
		Util::SetResourceName(a_tex.get(), a_name);

		// Reuse the source SRV's view description so typeless depth formats
		// resolve to the same shader-readable format.
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		a_srcSRV->GetDesc(&srvDesc);
		if (FAILED(globals::d3d::device->CreateShaderResourceView(a_tex.get(), &srvDesc, a_srv.put()))) {
			a_tex = nullptr;
			return;
		}
	}

	if (a_box)
		globals::d3d::context->CopySubresourceRegion(a_tex.get(), 0, a_box->left, a_box->top, 0, srcTex.get(), 0, a_box);
	else
		globals::d3d::context->CopyResource(a_tex.get(), srcTex.get());
}

/**
 * @brief Lazy-loads the shell snow texture set from the user-configurable
 * path (through the MO2 VFS).
 *
 * The TruePBR variant of the chosen path (Textures\PBR\..., _n / _rmaos
 * companions, linear-color albedo) is probed first. When found, the
 * modlist's own TruePBR config JSON (PBRTextureSets\, matched by texture
 * basename) supplies the authored glint/roughness/specular values.
 */

/**
 * @brief Loads a DDS through the GAME's resource system rather than the filesystem.
 *
 * A plain file open sees only loose files. That is half the game: Bethesda
 * ships almost every texture inside a BSA, so a path that is perfectly valid -
 * and that the game itself renders every day - resolves to nothing unless some
 * mod happens to have unpacked it. The snow shell had that hole from the start
 * and never showed it, because anyone running this feature is also running a
 * snow retexture with loose files. The frost pattern found it immediately: its
 * default is a vanilla landscape texture that lives in Skyrim - Textures5.bsa.
 *
 * BSResourceNiBinaryStream is the game's own lookup, so it reads loose files
 * AND archives, and a modlist's override still wins exactly as it does in
 * game. The filesystem path stays as a fallback for anything outside Data.
 */
bool SnowDeformation::LoadGameDDS(const std::string& a_dataRelativePath, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv)
{
	a_srv = nullptr;
	if (a_dataRelativePath.empty())
		return false;

	// "Textures\Effects\shockbolttile01" is how anyone would write the path,
	// and the resource system wants the extension. Supplying it is kinder than
	// silently loading nothing and leaving the user to guess which half of the
	// path was wrong.
	std::string path = a_dataRelativePath;
	{
		const size_t dot = path.find_last_of('.');
		const size_t sep = path.find_last_of("\\/");
		// No dot at all, or the only dot is in a folder name: npos compares
		// LARGER than any real index, so the two cases cannot share one test.
		if (dot == std::string::npos || (sep != std::string::npos && dot < sep))
			path += ".dds";
	}

	RE::BSResourceNiBinaryStream stream(path);
	if (stream.good() && stream.stream) {
		// The stream reports the size the CONSUMER sees, so a compressed BSA
		// entry gives its uncompressed length here and decompresses as it is
		// read. binary_read is protected; the templated read is the way in.
		const uint32_t total = stream.stream->totalSize;
		if (total > 0) {
			std::vector<uint8_t> bytes(total);
			if (stream.read(reinterpret_cast<char*>(bytes.data()), total) &&
				SUCCEEDED(DirectX::CreateDDSTextureFromMemory(globals::d3d::device, bytes.data(), bytes.size(),
					nullptr, a_srv.put()))) {
				logger::debug("SnowDeformation: loaded {} through the game ({} bytes)", a_dataRelativePath, total);
				return true;
			}
		}
		a_srv = nullptr;
	}

	// Fallback: a real path on disk, for anything the resource system does not
	// own. Loose files already came back above, so this is only ever the
	// unusual case.
	const std::string diskPath = "Data\\" + path;
	const std::wstring wide(diskPath.begin(), diskPath.end());
	if (SUCCEEDED(DirectX::CreateDDSTextureFromFile(globals::d3d::device, wide.c_str(), nullptr, a_srv.put()))) {
		logger::debug("SnowDeformation: loaded {} off disk", path);
		return true;
	}
	a_srv = nullptr;
	logger::debug("SnowDeformation: could not load {} from archives or disk", a_dataRelativePath);
	return false;
}

void SnowDeformation::EnsureFrostPatternTextures()
{
	if (frostPatternAttempted)
		return;
	frostPatternAttempted = true;

	// A TILEABLE surface, not a decal. The first attempt reached for the game's
	// own frost impact art, which was the wrong kind of image: a decal carries
	// its content in the middle and nothing at the edges, because it is printed
	// once. Two offset copies of that blended together give clumps with gaps,
	// which is precisely what it looked like. A landscape texture meets itself
	// on every side, which is the thing the stochastic sampler assumes.
	std::string chosen = settings.FrostTexturePath;
	for (auto& pathChar : chosen)
		if (pathChar == '/')
			pathChar = '\\';
	const std::string base = chosen.size() > 4 ? chosen.substr(0, chosen.size() - 4) : chosen;

	// The normal is what actually matters - it carries the crystal - so the
	// pattern counts as absent without one and the crust falls back to the
	// smooth glaze it had before any of this.
	if (!LoadGameDDS(base + "_n.dds", frostPatternNormalSRV)) {
		logger::debug("SnowDeformation: no frost pattern normal at {}_n.dds; crust keeps its smooth glaze", base);
		return;
	}
	LoadGameDDS(chosen, frostPatternDiffuseSRV);
}

void SnowDeformation::EnsureShellSnowTextures()
{
	if (shellSnowTextureAttempted)
		return;
	LoadTraceScope _loadTrace(this, "Shell: EnsureShellSnowTextures (load)");
	shellSnowTextureAttempted = true;
	shellSnowDiffuseSRV = nullptr;
	shellSnowNormalSRV = nullptr;
	shellSnowRmaosSRV = nullptr;
	shellSnowHeightSRV = nullptr;
	shellSnowTextureIsPBR = false;

	// Through the game's own lookup, so a snow texture that lives only in a BSA
	// loads like any other. This path had the same hole the frost pattern
	// exposed; it simply never showed, because a modlist that wants deep snow
	// invariably ships a loose snow retexture on top of the vanilla one.
	auto tryLoadDDS = [](const std::string& a_path, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv) {
		return LoadGameDDS(a_path, a_srv);
	};

	std::string chosenPath = settings.SnowTexturePath.empty() ? "Textures\\Landscape\\snow01.dds" : settings.SnowTexturePath;
	for (auto& pathChar : chosenPath)
		if (pathChar == '/')
			pathChar = '\\';
	std::string loweredPath = chosenPath;
	std::transform(loweredPath.begin(), loweredPath.end(), loweredPath.begin(),
		[](unsigned char c) { return (char)std::tolower(c); });

	std::string pbrPath;
	if (loweredPath.find("\\pbr\\") != std::string::npos || loweredPath.rfind("pbr\\", 0) == 0)
		pbrPath = chosenPath;
	else if (size_t texPos = loweredPath.find("textures\\"); texPos != std::string::npos)
		pbrPath = chosenPath.substr(0, texPos + 9) + "PBR\\" + chosenPath.substr(texPos + 9);

	if (!pbrPath.empty() && pbrPath.size() > 4 && tryLoadDDS(pbrPath, shellSnowDiffuseSRV)) {
		shellSnowTextureIsPBR = true;
		std::string base = pbrPath.substr(0, pbrPath.size() - 4);
		bool hasNormal = tryLoadDDS(base + "_n.dds", shellSnowNormalSRV);
		bool hasRmaos = tryLoadDDS(base + "_rmaos.dds", shellSnowRmaosSRV);
		bool hasHeight = tryLoadDDS(base + "_p.dds", shellSnowHeightSRV);
		logger::info("[SNOW DEFORMATION] PBR snow set: {} (normal={} rmaos={} height={})", pbrPath, hasNormal, hasRmaos, hasHeight);

		snowGlintLogDensity = 6.0f;
		snowGlintMicroRoughness = 0.3f;
		snowGlintDensityRandomization = 5.0f;
		snowGlintScreenSpaceScale = 1.0f;
		snowRoughnessScale = 0.7f;
		snowSpecularLevel = 0.02f;
		snowDisplacementScale = 1.0f;
		snowPBRSetName.clear();

		// TruePBR already parsed every PBRTextureSets JSON into its table, keyed
		// by filename stem (the texture-set editor ID) - the same string the old
		// directory scan matched against. Match the keys instead: shared struct,
		// shared parse, and ReloadTextureSetData/menu edits reach us through
		// RefreshSnowPBRParams. Exact key first, then the lexicographically
		// smallest substring match so multi-match modlists resolve the same way
		// every run (the old filesystem-order scan did not).
		size_t slashPos = base.find_last_of('\\');
		std::string baseName = (slashPos == std::string::npos) ? base : base.substr(slashPos + 1);
		std::transform(baseName.begin(), baseName.end(), baseName.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		const auto& pbrSets = globals::features::truePBR.pbrTextureSets;
		size_t matchCount = 0;
		// The user's pin wins over any match: a modlist can hold several sets
		// whose key contains the same basename and no rule fits every one.
		if (!settings.SnowPBRConfig.empty()) {
			if (pbrSets.find(settings.SnowPBRConfig) != pbrSets.end()) {
				snowPBRSetName = settings.SnowPBRConfig;
			} else {
				std::string pinLower = settings.SnowPBRConfig;
				std::transform(pinLower.begin(), pinLower.end(), pinLower.begin(),
					[](unsigned char c) { return (char)std::tolower(c); });
				for (const auto& [setName, setData] : pbrSets) {
					std::string keyLower = setName;
					std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(),
						[](unsigned char c) { return (char)std::tolower(c); });
					if (keyLower == pinLower) {
						snowPBRSetName = setName;
						break;
					}
				}
				if (snowPBRSetName.empty())
					logger::info("[SNOW DEFORMATION] pinned TruePBR config '{}' is not loaded; falling back to the automatic match", settings.SnowPBRConfig);
			}
		}
		// RANKED, not first-alphabetically. A modlist holds several keys
		// containing the same basename, and taking the smallest name handed the
		// shell an AutoBlend stub - three fields, no glintParameters block at
		// all - so the snow rendered with glints off and the stub's roughness
		// (2026-09-22: 'Blendsnow01' beat 'LandscapeSnow01' on the letter B and
		// the sparkle vanished from BOTH shells, which share the >1.1 gate).
		// Order: a set that carries the glint block we came for, then the least
		// decorated name, then alphabetically so a tie resolves the same run to
		// run. An exact key still wins outright.
		std::string bestKey;
		bool bestHasGlints = false;
		size_t bestExtra = 0;
		for (const auto& [setName, setData] : pbrSets) {
			std::string keyLower = setName;
			std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(),
				[](unsigned char c) { return (char)std::tolower(c); });
			if (keyLower.find(baseName) == std::string::npos)
				continue;
			++matchCount;
			if (keyLower == baseName && snowPBRSetName.empty()) {
				snowPBRSetName = setName;
				bestKey.clear();
				break;
			}
			const bool hasGlints = setData.glintParameters.enabled;
			const size_t extra = keyLower.size() - baseName.size();
			bool better = bestKey.empty();
			if (!better && hasGlints != bestHasGlints)
				better = hasGlints;
			else if (!better && extra != bestExtra)
				better = extra < bestExtra;
			else if (!better)
				better = setName < bestKey;
			if (better) {
				bestKey = setName;
				bestHasGlints = hasGlints;
				bestExtra = extra;
			}
		}
		if (snowPBRSetName.empty())
			snowPBRSetName = bestKey;
		if (!snowPBRSetName.empty()) {
			RefreshSnowPBRParams();
			logger::info("[SNOW DEFORMATION] TruePBR config matched: {} ({} of {} sets matched '{}'; glintDensity={:.1f} roughScale={:.2f} spec={:.3f})",
				snowPBRSetName, matchCount, pbrSets.size(), baseName, snowGlintLogDensity, snowRoughnessScale, snowSpecularLevel);
		} else {
			logger::info("[SNOW DEFORMATION] no TruePBR config matches '{}' ({} sets loaded); using built-in snow defaults",
				baseName, pbrSets.size());
		}
	} else {
		snowPBRSetName.clear();
		bool ok = tryLoadDDS(chosenPath, shellSnowDiffuseSRV);
		if (!ok && chosenPath != "Textures\\Landscape\\snow01.dds") {
			logger::info("[SNOW DEFORMATION] Snow diffuse not loose-file loadable: {}", chosenPath);
			chosenPath = "Textures\\Landscape\\snow01.dds";
			ok = tryLoadDDS(chosenPath, shellSnowDiffuseSRV);
		}
		logger::info("[SNOW DEFORMATION] Snow diffuse load ({}): {}", ok ? "ok (legacy)" : "missing, using fallback color", chosenPath);
	}
}

void SnowDeformation::RefreshSnowPBRParams()
{
	if (snowPBRSetName.empty())
		return;
	const auto& pbrSets = globals::features::truePBR.pbrTextureSets;
	auto it = pbrSets.find(snowPBRSetName);
	if (it == pbrSets.end())
		return;
	const auto& cfg = it->second;
	snowRoughnessScale = cfg.roughnessScale;
	snowSpecularLevel = cfg.specularLevel;
	snowDisplacementScale = cfg.displacementScale;
	if (cfg.glintParameters.enabled) {
		snowGlintLogDensity = cfg.glintParameters.logMicrofacetDensity;
		// Same clamps Lighting.hlsl applies (PBR::Constants).
		snowGlintMicroRoughness = std::clamp(cfg.glintParameters.microfacetRoughness, 0.005f, 0.3f);
		snowGlintDensityRandomization = std::clamp(cfg.glintParameters.densityRandomization, 0.0f, 5.0f);
		snowGlintScreenSpaceScale = std::max(1.0f, cfg.glintParameters.screenSpaceScale);
	} else {
		snowGlintLogDensity = 0.0f;  // below the shader's >1.1 gate
	}
}

bool SnowDeformation::EnsureShellStatsQueries()
{
	if (shellStatsQuery[kShellStatsRing - 1][1])
		return true;
	auto device = globals::d3d::device;
	for (int ring = 0; ring < kShellStatsRing; ++ring) {
		for (int kind = 0; kind < 2; ++kind) {
			D3D11_QUERY_DESC desc{};
			desc.Query = kind == 0 ? D3D11_QUERY_PIPELINE_STATISTICS : D3D11_QUERY_OCCLUSION;
			if (FAILED(device->CreateQuery(&desc, shellStatsQuery[ring][kind].put())) ||
				FAILED(device->CreateQuery(&desc, staticsStatsQuery[ring][kind].put())) ||
				FAILED(device->CreateQuery(&desc, patchStatsQuery[ring][kind].put()))) {
				logger::error("SnowDeformation: pipeline statistics query creation failed");
				for (auto& r : shellStatsQuery)
					for (auto& q : r)
						q = nullptr;
				for (auto& r : staticsStatsQuery)
					for (auto& q : r)
						q = nullptr;
				for (auto& r : patchStatsQuery)
					for (auto& q : r)
						q = nullptr;
				return false;
			}
		}
	}
	return true;
}

void SnowDeformation::ReadShellStatsQueries(ID3D11DeviceContext* a_context)
{
	// Oldest slot, two frames behind the one about to be issued; a slot that
	// is not ready yet is simply tried again next frame.
	const int ring = (shellStatsRing + 1) % kShellStatsRing;
	if (!shellStatsIssued[ring])
		return;
	auto read = [&](winrt::com_ptr<ID3D11Query> (&queries)[2], ShellStatsResult& out) {
		D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
		UINT64 samples = 0;
		if (a_context->GetData(queries[0].get(), &stats, sizeof(stats), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
			a_context->GetData(queries[1].get(), &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			return false;
		out.vsInvocations = stats.VSInvocations;
		out.hsInvocations = stats.HSInvocations;
		out.dsInvocations = stats.DSInvocations;
		out.psInvocations = stats.PSInvocations;
		out.rasterizedPrimitives = stats.CInvocations;
		out.samplesPassed = samples;
		out.valid = true;
		return true;
	};
	if (read(shellStatsQuery[ring], shellStatsLast) && read(staticsStatsQuery[ring], staticsStatsLast) &&
		read(patchStatsQuery[ring], patchStatsLast))
		shellStatsIssued[ring] = false;
}

ID3D11RasterizerState* SnowDeformation::GetSkinRasterState(bool a_decal)
{
	const float bias = settings.SkinDepthBias;
	const float slope = settings.SkinSlopeDepthBias;
	if (skinRasterState && skinDecalRasterState && bias == skinRasterBiasBuilt && slope == skinRasterSlopeBuilt)
		return a_decal ? skinDecalRasterState.get() : skinRasterState.get();

	// Sliders read "toward the camera"; D3D's bias is added to depth and the
	// shells test LESS_EQUAL, so nearer is negative. The clamp bounds the
	// slope term at a third of the old decal cap (-3e-5 NDC), the push that
	// stood a 450 m peak 2000 units in front of its mist.
	D3D11_RASTERIZER_DESC rasterDesc{};
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	rasterDesc.DepthClipEnable = TRUE;
	rasterDesc.DepthBias = -static_cast<INT>(std::lround(bias));
	rasterDesc.SlopeScaledDepthBias = -slope;
	rasterDesc.DepthBiasClamp = -1e-5f;
	winrt::com_ptr<ID3D11RasterizerState> state;
	if (FAILED(globals::d3d::device->CreateRasterizerState(&rasterDesc, state.put()))) {
		logger::error("SnowDeformation: skin raster state creation failed (bias {} slope {})", bias, slope);
		return a_decal ? skinDecalRasterState.get() : skinRasterState.get();
	}
	Util::SetResourceName(state.get(), "SnowDeformation::SkinRasterState");
	// Decal-mode draws: the game's own decal bias (measured: -1, -0.65,
	// clamp -100) on top of the skin's, unclamped like the game's, or the
	// slope term never reaches the mesh's written depth at a grazing view.
	D3D11_RASTERIZER_DESC decalDesc = rasterDesc;
	decalDesc.DepthBias -= 1;
	decalDesc.SlopeScaledDepthBias -= 0.65f;
	decalDesc.DepthBiasClamp = -100.0f;
	winrt::com_ptr<ID3D11RasterizerState> decalState;
	if (FAILED(globals::d3d::device->CreateRasterizerState(&decalDesc, decalState.put()))) {
		logger::error("SnowDeformation: skin decal raster state creation failed (bias {} slope {})", bias, slope);
		return a_decal ? skinDecalRasterState.get() : skinRasterState.get();
	}
	Util::SetResourceName(decalState.get(), "SnowDeformation::SkinDecalRasterState");
	skinRasterState = state;
	skinDecalRasterState = decalState;
	skinRasterBiasBuilt = bias;
	skinRasterSlopeBuilt = slope;
	return a_decal ? skinDecalRasterState.get() : skinRasterState.get();
}

// EHF sun attenuation only compiles when the addon is installed (its hlsli
// is not CORE); SnowShading.hlsli gates on the define.
std::vector<std::pair<const char*, const char*>> SnowDeformation::ShellPSDefines(const char* a_extra)
{
	std::vector<std::pair<const char*, const char*>> defines = { { "PSHADER", "" } };
	if (a_extra)
		defines.emplace_back(a_extra, "");
	if (globals::features::exponentialHeightFog.loaded)
		defines.emplace_back("SNOW_EXP_HEIGHT_FOG", "");
	if (globals::features::ibl.loaded)
		defines.emplace_back("SNOW_IBL", "");
	return defines;
}

ID3D11VertexShader* SnowDeformation::GetShellFillVS()
{
	if (!shellFillVS) {
		logger::debug("Compiling DepthSyncCS FillVS");
		shellFillVS = static_cast<ID3D11VertexShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "vs_5_0", "FillVS"));
	}
	return shellFillVS;
}

ID3D11PixelShader* SnowDeformation::GetShellFillPS()
{
	if (!shellFillPS) {
		logger::debug("Compiling DepthSyncCS FillPS");
		shellFillPS = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "ps_5_0", "FillPS"));
	}
	return shellFillPS;
}

ID3D11PixelShader* SnowDeformation::GetShellFillPSPreSnow()
{
	if (!shellFillPSPreSnow) {
		logger::debug("Compiling DepthSyncCS FillPS (pre-snow)");
		shellFillPSPreSnow = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", { { "FILL_PRE_SNOW", "" } }, "ps_5_0", "FillPS"));
	}
	return shellFillPSPreSnow;
}

ID3D11PixelShader* SnowDeformation::GetShellFillPSClaim()
{
	if (!shellFillPSClaim) {
		logger::debug("Compiling DepthSyncCS FillPS (claim)");
		shellFillPSClaim = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", { { "FILL_CLAIM", "" } }, "ps_5_0", "FillPS"));
	}
	return shellFillPSClaim;
}

ID3D11ComputeShader* SnowDeformation::GetShellTerrainCutCS()
{
	if (!shellTerrainCutCS) {
		logger::debug("Compiling SnowShell TerrainCutCS");
		shellTerrainCutCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "SNOW_TERRAIN_CUT_CS", "" } }, "cs_5_0", "TerrainCutCS"));
	}
	return shellTerrainCutCS;
}

ID3D11ShaderResourceView* SnowDeformation::ShellWaterSRV() const
{
	return waterVisibleTexture && waterVisibleTexture->srv ? waterVisibleTexture->srv.get() : (waterHeightTexture ? waterHeightTexture->srv.get() : nullptr);
}

ID3D11ShaderResourceView* SnowDeformation::TerrainWetSRV(bool a_cut) const
{
	if (a_cut && shellTerrainWetValid)
		return shellTerrainWet->srv.get();
	return shellTerrainTexture ? shellTerrainTexture->srv.get() : nullptr;
}

void SnowDeformation::UpdateTerrainWet()
{
	auto* water = ShellWaterSRV();
	const uint64_t key[3] = { shellTerrainVersion, waterWindowVersion, reinterpret_cast<uint64_t>(water) };
	if (shellTerrainWetValid && std::equal(std::begin(key), std::end(key), std::begin(shellTerrainWetKey)))
		return;
	shellTerrainWetValid = false;
	auto* cs = GetShellTerrainCutCS();
	if (!cs || !shellTerrainTexture || !shellTerrainTexture->srv || !shellTerrainWet || !shellTerrainWet->uav)
		return;
	auto context = globals::d3d::context;
	globals::profiler->BeginPass("SnowDeformation::Ground - Terrain Cut");
	ID3D11Buffer* cb = shellCB->CB();
	context->CSSetConstantBuffers(0, 1, &cb);
	ID3D11ShaderResourceView* terrainSRV = shellTerrainTexture->srv.get();
	context->CSSetShaderResources(0, 1, &terrainSRV);
	context->CSSetShaderResources(27, 1, &water);
	ID3D11UnorderedAccessView* wetUAV = shellTerrainWet->uav.get();
	context->CSSetUnorderedAccessViews(4, 1, &wetUAV, nullptr);
	context->CSSetShader(cs, nullptr, 0);
	context->Dispatch((kShellWindowDim + 7) / 8, (kShellWindowDim + 7) / 8, 1);
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetUnorderedAccessViews(4, 1, &nullUAV, nullptr);
	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetShaderResources(27, 1, &nullSRV);
	context->CSSetShader(nullptr, nullptr, 0);
	globals::profiler->EndPass();
	std::copy(std::begin(key), std::end(key), std::begin(shellTerrainWetKey));
	shellTerrainWetValid = true;
}

bool SnowDeformation::EnsurePrepassResources(ID3D11ShaderResourceView* a_mainDepthSRV)
{
	if (!a_mainDepthSRV)
		return false;
	winrt::com_ptr<ID3D11Resource> depthRes;
	a_mainDepthSRV->GetResource(depthRes.put());
	auto depthTex = depthRes.try_as<ID3D11Texture2D>();
	if (!depthTex)
		return false;
	D3D11_TEXTURE2D_DESC depthDesc{};
	depthTex->GetDesc(&depthDesc);

	auto device = globals::d3d::device;
	if (shellRasterDepth && (shellRasterDepth->desc.Width != depthDesc.Width || shellRasterDepth->desc.Height != depthDesc.Height)) {
		delete shellRasterDepth;
		shellRasterDepth = nullptr;
		shellTestDepthSRV = nullptr;
		shellTestDepthDSV = nullptr;
		shellTestDepth = nullptr;
	}
	if (!shellRasterDepth) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = depthDesc.Width;
		desc.Height = depthDesc.Height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		shellRasterDepth = new Texture2D(desc, "SnowDeformation::ShellRasterDepth");
		shellRasterDepth->CreateSRV(srvDesc);
		shellRasterDepth->CreateRTV(rtvDesc);

		// Typeless so the object-snow prepass can read it back through an
		// R32 view for its write-back into the main depth.
		D3D11_TEXTURE2D_DESC testDesc = desc;
		testDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		testDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(device->CreateTexture2D(&testDesc, nullptr, shellTestDepth.put())))
			return false;
		Util::SetResourceName(shellTestDepth.get(), "SnowDeformation::ShellTestDepth");
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		if (FAILED(device->CreateDepthStencilView(shellTestDepth.get(), &dsvDesc, shellTestDepthDSV.put())))
			return false;
		Util::SetResourceName(shellTestDepthDSV.get(), "SnowDeformation::ShellTestDepth DSV");
		D3D11_SHADER_RESOURCE_VIEW_DESC testSrvDesc = {
			.Format = DXGI_FORMAT_R32_FLOAT,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		if (FAILED(device->CreateShaderResourceView(shellTestDepth.get(), &testSrvDesc, shellTestDepthSRV.put())))
			return false;
		Util::SetResourceName(shellTestDepthSRV.get(), "SnowDeformation::ShellTestDepth SRV");

		// Pre-snow copy of the main depth: R32F, written by PreSnowCopyCS over
		// the rendered rectangle only (DepthSync reads nothing outside it).
		preSnowDepth = nullptr;
		preSnowDepthSRV = nullptr;
		preSnowDepthUAV = nullptr;
		preSnowDepthRTV = nullptr;
		D3D11_TEXTURE2D_DESC copyDesc = desc;
		copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
		D3D11_UNORDERED_ACCESS_VIEW_DESC copyUavDesc{};
		copyUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
		copyUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		D3D11_RENDER_TARGET_VIEW_DESC copyRtvDesc{};
		copyRtvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		copyRtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		if (SUCCEEDED(device->CreateTexture2D(&copyDesc, nullptr, preSnowDepth.put()))) {
			Util::SetResourceName(preSnowDepth.get(), "SnowDeformation::PreSnowDepth");
			if (FAILED(device->CreateShaderResourceView(preSnowDepth.get(), &srvDesc, preSnowDepthSRV.put())) ||
				FAILED(device->CreateUnorderedAccessView(preSnowDepth.get(), &copyUavDesc, preSnowDepthUAV.put())) ||
				FAILED(device->CreateRenderTargetView(preSnowDepth.get(), &copyRtvDesc, preSnowDepthRTV.put()))) {
				preSnowDepth = nullptr;
				preSnowDepthSRV = nullptr;
				preSnowDepthUAV = nullptr;
				preSnowDepthRTV = nullptr;
			} else {
				Util::SetResourceName(preSnowDepthSRV.get(), "SnowDeformation::PreSnowDepth SRV");
				Util::SetResourceName(preSnowDepthUAV.get(), "SnowDeformation::PreSnowDepth UAV");
				Util::SetResourceName(preSnowDepthRTV.get(), "SnowDeformation::PreSnowDepth RTV");
			}
		}
		// Copy of the main depth (CopyResource: same format as the source,
		// read through the game's own SRV format), taken between the
		// landscape shell and the skins.
		D3D11_TEXTURE2D_DESC preDesc = depthDesc;
		preDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		preDesc.MiscFlags = 0;
		preDesc.CPUAccessFlags = 0;
		preDesc.Usage = D3D11_USAGE_DEFAULT;
		D3D11_SHADER_RESOURCE_VIEW_DESC mainSrvDesc{};
		a_mainDepthSRV->GetDesc(&mainSrvDesc);
		bloodPreSkinDepth = nullptr;
		bloodPreSkinDepthSRV = nullptr;
		if (SUCCEEDED(device->CreateTexture2D(&preDesc, nullptr, bloodPreSkinDepth.put()))) {
			Util::SetResourceName(bloodPreSkinDepth.get(), "SnowDeformation::BloodPreSkinDepth");
			if (FAILED(device->CreateShaderResourceView(bloodPreSkinDepth.get(), &mainSrvDesc, bloodPreSkinDepthSRV.put())))
				bloodPreSkinDepth = nullptr;
			else
				Util::SetResourceName(bloodPreSkinDepthSRV.get(), "SnowDeformation::BloodPreSkinDepth SRV");
		}
	}
	if (!shellPrepassMainDepthState) {
		D3D11_DEPTH_STENCIL_DESC dsDesc{};
		dsDesc.DepthEnable = TRUE;
		dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dsDesc.DepthFunc = D3D11_COMPARISON_EQUAL;
		if (FAILED(device->CreateDepthStencilState(&dsDesc, shellPrepassMainDepthState.put())))
			return false;
		Util::SetResourceName(shellPrepassMainDepthState.get(), "SnowDeformation::ShellPrepassMainDepthState");
	}
	if (!shellFillDepthState) {
		D3D11_DEPTH_STENCIL_DESC dsDesc{};
		dsDesc.DepthEnable = TRUE;
		dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		if (FAILED(device->CreateDepthStencilState(&dsDesc, shellFillDepthState.put())))
			return false;
		Util::SetResourceName(shellFillDepthState.get(), "SnowDeformation::ShellFillDepthState");
	}
	return shellRasterDepth->srv && shellRasterDepth->rtv && shellTestDepthDSV && shellTestDepthSRV;
}

bool SnowDeformation::EnsureDeferredResources()
{
	if (!shellRasterDepth)
		return false;
	const auto& base = shellRasterDepth->desc;
	static constexpr DXGI_FORMAT kFormats[3] = { DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT };
	static constexpr const char* kNames[3] = { "SnowDeformation::ShellDeferredGrid", "SnowDeformation::ShellDeferredGridDeriv", "SnowDeformation::ShellDeferredDepth" };
	for (int i = 0; i < 3; i++) {
		auto*& attr = shellDeferredAttr[i];
		if (attr && (attr->desc.Width != base.Width || attr->desc.Height != base.Height)) {
			delete attr;
			attr = nullptr;
		}
		if (!attr) {
			D3D11_TEXTURE2D_DESC desc = base;
			desc.Format = kFormats[i];
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
				.Format = desc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
			};
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
				.Format = desc.Format,
				.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = 0 }
			};
			attr = new Texture2D(desc, kNames[i]);
			attr->CreateSRV(srvDesc);
			attr->CreateRTV(rtvDesc);
		}
	}
	if (!shellDeferredDepthState) {
		D3D11_DEPTH_STENCIL_DESC dsDesc{};
		dsDesc.DepthEnable = TRUE;
		dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dsDesc.DepthFunc = D3D11_COMPARISON_LESS;
		if (FAILED(globals::d3d::device->CreateDepthStencilState(&dsDesc, shellDeferredDepthState.put())))
			return false;
		Util::SetResourceName(shellDeferredDepthState.get(), "SnowDeformation::ShellDeferredDepthState");
	}
	return shellDeferredAttr[0]->rtv && shellDeferredAttr[1]->rtv && shellDeferredAttr[2]->rtv;
}

ID3D11PixelShader* SnowDeformation::GetShellPSDeferred(bool a_parallax, bool a_churnFree)
{
	const uint32_t index = a_parallax ? 1 : (a_churnFree ? 2 : 0);
	auto*& slot = shellPSDeferred[index];
	auto& slotBits = shellPSDeferredLegacyBits[index];
	if (slot && slotBits != shellPSLegacyBits) {
		slot->Release();
		slot = nullptr;
	}
	if (!slot) {
		logger::debug("Compiling SnowShell PS (deferred{})", a_parallax ? ", trench parallax" : (index == 2 ? ", churn-free" : ""));
		auto defines = ShellPSDefines();
		defines.emplace_back("SNOW_SHELL_DEFERRED", "");
		if (shellPSLegacyBits & 1)
			defines.emplace_back("SNOW_TEETH_UNGATED", "");
		if (shellPSLegacyBits & 4)
			defines.emplace_back("SNOW_UNTOUCHED_SKIP_OFF", "");
		if (a_parallax)
			defines.emplace_back("SNOW_TRENCH_PARALLAX", "");
		if (index == 2)
			defines.emplace_back("SNOW_NO_CHURN", "");
		slotBits = shellPSLegacyBits;
		slot = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ps_5_0"));
	}
	return slot;
}

ID3D11PixelShader* SnowDeformation::ConditionViewPS()
{
	if (debugConditionView <= 0)
		return nullptr;
	int state = conditionViewShaderState.load(std::memory_order_acquire);
	if (state == 0) {
		const uint32_t legacy = shellPSLegacyBits;
		conditionViewShaderState.store(1, std::memory_order_release);
		conditionViewCompile = std::async(std::launch::async, [this, legacy] {
			auto defines = ShellPSDefines();
			defines.emplace_back("SNOW_SHELL_DEFERRED", "");
			if (legacy & 1)
				defines.emplace_back("SNOW_TEETH_UNGATED", "");
			if (legacy & 4)
				defines.emplace_back("SNOW_UNTOUCHED_SKIP_OFF", "");
			defines.emplace_back("SNOW_CONDITION_VIEW", "");
			shellPSDeferredConditionView = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ps_5_0"));
			return shellPSDeferredConditionView != nullptr;
		});
		return nullptr;
	}
	if (state == 1) {
		if (conditionViewCompile.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return nullptr;
		state = conditionViewCompile.get() ? 2 : 3;
		conditionViewShaderState.store(state, std::memory_order_release);
		if (state == 3)
			logger::warn("[SNOW DEFORMATION] Snow condition view: SnowShell.hlsl failed to compile, the view stays off");
	}
	return state == 2 ? shellPSDeferredConditionView : nullptr;
}

ID3D11PixelShader* SnowDeformation::GetShellPSPrepassAttr(bool a_keepHidden, bool a_parallax)
{
	auto*& slot = shellPSPrepassAttr[a_keepHidden ? 1 : 0][a_parallax ? 1 : 0];
	if (!slot) {
		logger::debug("Compiling SnowShell PS (depth prepass, deferred attributes{}{})", a_keepHidden ? ", hidden kept" : "", a_parallax ? ", trench parallax" : "");
		auto defines = ShellPSDefines();
		defines.emplace_back("SNOW_SHELL_DEPTH_PREPASS", "");
		defines.emplace_back("SNOW_PREPASS_ATTR", "");
		if (a_keepHidden)
			defines.emplace_back("SNOW_PREPASS_KEEP_HIDDEN", "");
		if (a_parallax)
			defines.emplace_back("SNOW_TRENCH_PARALLAX", "");
		slot = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ps_5_0"));
	}
	return slot;
}

ID3D11PixelShader* SnowDeformation::GetShellPSPrepassEarly(bool a_keepHidden, bool a_parallax)
{
	auto*& slot = shellPSPrepassEarly[a_keepHidden ? 1 : 0][a_parallax ? 1 : 0];
	if (!slot) {
		logger::debug("Compiling SnowShell PS (early depth prepass{}{})", a_keepHidden ? ", hidden kept" : "", a_parallax ? ", trench parallax" : "");
		auto defines = ShellPSDefines();
		defines.emplace_back("SNOW_SHELL_DEPTH_PREPASS", "");
		defines.emplace_back("SNOW_PREPASS_ATTR", "");
		defines.emplace_back("SNOW_PREPASS_EARLY", "");
		if (a_keepHidden)
			defines.emplace_back("SNOW_PREPASS_KEEP_HIDDEN", "");
		if (a_parallax)
			defines.emplace_back("SNOW_TRENCH_PARALLAX", "");
		slot = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ps_5_0"));
	}
	return slot;
}

bool SnowDeformation::TrenchParallaxActive()
{
	const uint32_t frame = globals::state->frameCount;
	if (frame == trenchParallaxFrame)
		return trenchParallaxLatched;
	trenchParallaxFrame = frame;
	trenchParallaxLatched = false;
	if (!settings.TrenchParallax || SnowShadersPending(1))
		return false;
	int state = trenchParallaxShaderState.load(std::memory_order_acquire);
	if (state == 0) {
		// Off the render thread (seconds cold); it reads none of these slots
		// until the state says ready.
		const uint32_t legacy = shellPSLegacyBits;
		trenchParallaxShaderState.store(1, std::memory_order_release);
		trenchParallaxCompile = std::async(std::launch::async, [this, legacy] {
			bool ok = true;
			for (int hidden = 0; hidden < 2; hidden++) {
				ok = GetShellPSPrepassEarly(hidden != 0, true) && ok;
				ok = GetShellPSPrepassAttr(hidden != 0, true) && ok;
			}
			// Legacy bits as captured; a later change recompiles on the render thread.
			if (legacy == shellPSLegacyBits)
				ok = GetShellPSDeferred(true) && ok;
			return ok;
		});
		return false;
	}
	if (state == 1) {
		if (trenchParallaxCompile.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return false;
		const bool ok = trenchParallaxCompile.get();
		state = ok ? 2 : 3;
		trenchParallaxShaderState.store(state, std::memory_order_release);
		if (ok)
			logger::info("[SNOW DEFORMATION] Screen-Space Parallax Trenches: shaders ready");
		else
			logger::warn("[SNOW DEFORMATION] Screen-Space Parallax Trenches: SnowShell.hlsl failed to compile, the trenches stay tessellated");
	}
	trenchParallaxLatched = state == 2 && LandscapeShadeParallaxReady() && GetShellPSDeferred(true);
	return trenchParallaxLatched;
}

bool SnowDeformation::EnsureParallaxOffset()
{
	if (!shellRasterDepth)
		return false;
	const auto& base = shellRasterDepth->desc;
	if (shellParallaxOffset && (shellParallaxOffset->desc.Width != base.Width || shellParallaxOffset->desc.Height != base.Height)) {
		delete shellParallaxOffset;
		shellParallaxOffset = nullptr;
	}
	if (!shellParallaxOffset) {
		D3D11_TEXTURE2D_DESC desc = base;
		desc.Format = DXGI_FORMAT_R16G16_FLOAT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		shellParallaxOffset = new Texture2D(desc, "SnowDeformation::ShellParallaxOffset");
		shellParallaxOffset->CreateSRV(srvDesc);
		shellParallaxOffset->CreateRTV(rtvDesc);
	}
	return shellParallaxOffset->srv && shellParallaxOffset->rtv;
}

ID3D11PixelShader* SnowDeformation::GetShellPSEarlyResolve()
{
	if (!shellPSEarlyResolve) {
		logger::debug("Compiling SnowShell PS (early prepass resolve)");
		shellPSEarlyResolve = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", ShellPSDefines("SNOW_S1_RESOLVE"), "ps_5_0"));
	}
	return shellPSEarlyResolve;
}

bool SnowDeformation::EnsureEarlyPrepassResources()
{
	if (!shellRasterDepth)
		return false;
	const auto& base = shellRasterDepth->desc;
	auto ensure = [&](Texture2D*& a_tex, DXGI_FORMAT a_format, const char* a_name, bool a_uav) {
		if (a_tex && (a_tex->desc.Width != base.Width || a_tex->desc.Height != base.Height)) {
			delete a_tex;
			a_tex = nullptr;
		}
		if (!a_tex) {
			D3D11_TEXTURE2D_DESC desc = base;
			desc.Format = a_format;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			if (a_uav)
				desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
				.Format = a_format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
			};
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
				.Format = a_format,
				.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = 0 }
			};
			a_tex = new Texture2D(desc, a_name);
			a_tex->CreateSRV(srvDesc);
			a_tex->CreateRTV(rtvDesc);
			if (a_uav) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
					.Format = a_format,
					.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
					.Texture2D = { .MipSlice = 0 }
				};
				a_tex->CreateUAV(uavDesc);
			}
		}
		return a_tex->srv && a_tex->rtv;
	};
	// The marks also hold the blended depth SyncSssDepth replaced, until PostPrepass; S1 itself needs no UAV.
	return ensure(shellEarlyPending, DXGI_FORMAT_R16G16B16A16_FLOAT, "SnowDeformation::ShellEarlyPending", false) &&
	       ensure(shellEarlyMarks, DXGI_FORMAT_R32_FLOAT, "SnowDeformation::ShellEarlyMarks", true);
}

void SnowDeformation::SetShellSeamBounds(ShellCB& a_cb, int a_minX, int a_minY, int a_maxX, int a_maxY)
{
	// The fade band sits ~29 m OUTSIDE the true boundary, over the
	// recolored LOD terrain: full-height shell up to the seam, melting
	// into same-material ground beyond it. Ending the fade AT the seam
	// left a visible unshelled strip of full terrain just inside it.
	constexpr float kSeamOverlap = 2048.0f;
	a_cb.SeamBounds = { a_minX * 4096.0f - kSeamOverlap, a_minY * 4096.0f - kSeamOverlap,
		(a_maxX + 1) * 4096.0f + kSeamOverlap, (a_maxY + 1) * 4096.0f + kSeamOverlap };
	a_cb.SeamRampInv = 1.0f / 2048.0f;
}

ID3D11ComputeShader* SnowDeformation::GetPreSnowCopyCS()
{
	if (!preSnowCopyCS) {
		logger::debug("Compiling DepthSyncCS PreSnowCopyCS");
		preSnowCopyCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "cs_5_0", "PreSnowCopyCS"));
	}
	DepthSyncRectBuffer();
	return preSnowCopyCS;
}

ConstantBuffer* SnowDeformation::DepthSyncRectBuffer()
{
	if (!depthSyncRectCB)
		depthSyncRectCB = new ConstantBuffer(ConstantBufferDesc<DepthSyncRectCB>(), "SnowDeformation::DepthSyncRectCB");
	return depthSyncRectCB;
}

D3D11_BOX SnowDeformation::ScreenRectBox(const D3D11_VIEWPORT* a_viewport, uint32_t a_width, uint32_t a_height, uint32_t a_guard) const
{
	D3D11_BOX box{ 0, 0, 0, a_width, a_height, 1 };
	if (screenRectsOff || !a_viewport || a_viewport->Width <= 0.0f || a_viewport->Height <= 0.0f)
		return box;
	const int64_t left = int64_t(std::floor(a_viewport->TopLeftX)) - a_guard;
	const int64_t top = int64_t(std::floor(a_viewport->TopLeftY)) - a_guard;
	const int64_t right = int64_t(std::ceil(a_viewport->TopLeftX + a_viewport->Width)) + a_guard;
	const int64_t bottom = int64_t(std::ceil(a_viewport->TopLeftY + a_viewport->Height)) + a_guard;
	box.left = UINT(std::clamp<int64_t>(left, 0, a_width));
	box.top = UINT(std::clamp<int64_t>(top, 0, a_height));
	box.right = UINT(std::clamp<int64_t>(right, int64_t(box.left), a_width));
	box.bottom = UINT(std::clamp<int64_t>(bottom, int64_t(box.top), a_height));
	return box;
}

void SnowDeformation::TakePreSnowCopy(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_mainDepthSRV)
{
	ID3D11RenderTargetView* rawRTVs[8] = {};
	ID3D11DepthStencilView* rawDSV = nullptr;
	a_context->OMGetRenderTargets(8, rawRTVs, &rawDSV);
	a_context->OMSetRenderTargets(0, nullptr, nullptr);
	globals::profiler->BeginPass("SnowDeformation::Ground - Depth Copy");
	DepthSyncRectCB rectData{ { preSnowRect.left, preSnowRect.top }, { preSnowRect.right - preSnowRect.left, preSnowRect.bottom - preSnowRect.top } };
	depthSyncRectCB->Update(rectData);
	ID3D11Buffer* rectCB = depthSyncRectCB->CB();
	ID3D11UnorderedAccessView* copyUAV = preSnowDepthUAV.get();
	a_context->CSSetConstantBuffers(2, 1, &rectCB);
	a_context->CSSetShaderResources(0, 1, &a_mainDepthSRV);
	a_context->CSSetUnorderedAccessViews(2, 1, &copyUAV, nullptr);
	a_context->CSSetShader(GetPreSnowCopyCS(), nullptr, 0);
	a_context->Dispatch((rectData.Extent[0] + 7) / 8, (rectData.Extent[1] + 7) / 8, 1);
	ID3D11ShaderResourceView* nullCopySRV = nullptr;
	ID3D11UnorderedAccessView* nullCopyUAV = nullptr;
	ID3D11Buffer* nullCopyCB = nullptr;
	a_context->CSSetShaderResources(0, 1, &nullCopySRV);
	a_context->CSSetUnorderedAccessViews(2, 1, &nullCopyUAV, nullptr);
	a_context->CSSetConstantBuffers(2, 1, &nullCopyCB);
	a_context->CSSetShader(nullptr, nullptr, 0);
	globals::profiler->EndPass();
	a_context->OMSetRenderTargets(8, rawRTVs, rawDSV);
	for (auto* rtv : rawRTVs) {
		if (rtv)
			rtv->Release();
	}
	if (rawDSV)
		rawDSV->Release();
	preSnowDepthThisFrame = true;
}

ID3D11ComputeShader* SnowDeformation::GetDepthSyncCS()
{
	if (!depthSyncCS) {
		logger::debug("Compiling DepthSyncCS");
		depthSyncCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "cs_5_0"));
	}
	return depthSyncCS;
}

ID3D11ComputeShader* SnowDeformation::GetSssSyncCS()
{
	if (!sssSyncCS) {
		logger::debug("Compiling DepthSyncCS SssSyncCS");
		sssSyncCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "cs_5_0", "SssSyncCS"));
	}
	return sssSyncCS;
}

ID3D11ComputeShader* SnowDeformation::GetSssRestoreCS()
{
	if (!sssRestoreCS) {
		logger::debug("Compiling DepthSyncCS SssRestoreCS");
		sssRestoreCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "cs_5_0", "SssRestoreCS"));
	}
	return sssRestoreCS;
}

ID3D11ComputeShader* SnowDeformation::GetSkinSssSyncCS()
{
	if (!skinSssSyncCS) {
		logger::debug("Compiling DepthSyncCS SkinSssSyncCS");
		skinSssSyncCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "cs_5_0", "SkinSssSyncCS"));
	}
	return skinSssSyncCS;
}

ID3D11ComputeShader* SnowDeformation::GetSkinSssRestoreCS()
{
	if (!skinSssRestoreCS) {
		logger::debug("Compiling DepthSyncCS SkinSssRestoreCS");
		skinSssRestoreCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "cs_5_0", "SkinSssRestoreCS"));
	}
	return skinSssRestoreCS;
}

void SnowDeformation::RefreshShellGridPlacement(ShellCB& a_cb, const float4* a_camAdjust)
{
	// Grid placement re-derived from THIS frame's camera; shared by
	// DrawShell and the shadow-caster injection: a one-frame-stale grid slides a
	// crisp full-surface shadow whenever the camera moves - the old blob
	// caster hid it). Spacing is FIXED at 8 units: the shell ends at the
	// loaded-cell seam (ShellEdgeFade), so range no longer scales density;
	// tighter spacing was measured to EXPLODE cost (sub-pixel triangles
	// near the camera: 94 m range = 2-unit triangles = 4.2 ms Shell pass
	// vs 1.1 ms at 8). The window-offset fields pair the fresh GridOrigin
	// with the CURRENT window state, which describes the texture content
	// actually bound - correct even when the window scrolls later in the
	// frame.
	auto& fb = globals::game::frameBufferCached;
	const auto camAdjust = a_camAdjust ? *a_camAdjust : fb.GetCameraPosAdjust();
	const float shellSpacing = kShellGridSpacing;
	a_cb.GridSpacing = shellSpacing;
	a_cb.GridDim = kShellGridDim;
	// The warped grid is camera-centered: snap the center to the COARSEST band
	// step, then offset by the warped span. Snapping to the base 8 units is not
	// enough - a coarse-band vertex would then need per-vertex rounding onto its
	// own lattice, and the rounding is what used to flip quad widths as the
	// camera moved (C3 root cause). At kShellOriginSnap every band start is an
	// exact multiple of its own step, so every vertex sits on its lattice with
	// nothing left to round. The cost is that the fine region re-centres in
	// 256-unit steps, leaving the camera up to ~181 units off centre against a
	// 1792-unit inner radius.
	const float warpedHalfSpan = ShellWarpedHalfSpan(shellSpacing);
	a_cb.WarpedHalfSpan = warpedHalfSpan;
	a_cb.GridOrigin = {
		std::floor(camAdjust.x / kShellOriginSnap) * kShellOriginSnap - warpedHalfSpan,
		std::floor(camAdjust.y / kShellOriginSnap) * kShellOriginSnap - warpedHalfSpan
	};
	a_cb.TerrainTexelSize = kShellVertexSpacing;
	a_cb.TerrainDim = kShellWindowDim;
	// Keep shader-side sampling math in small grid-local coordinates.
	constexpr float cellSize = kShellVertexSpacing * kShellTexelsPerCell;
	a_cb.GridToTerrainOffset = {
		a_cb.GridOrigin.x - shellWindowCellX * cellSize,
		a_cb.GridOrigin.y - shellWindowCellY * cellSize
	};
	a_cb.GridToDeformOffset = {
		a_cb.GridOrigin.x - windowOrigin.x,
		a_cb.GridOrigin.y - windowOrigin.y
	};
	// Toroidal addressing pairs with the same live window state; refreshed
	// here so the shadow-caster snapshot cannot go stale against a map that
	// scrolled after it was taken. One b0 serves both shells and the
	// shadow/probe passes.
	a_cb.DeformMapOrigin = mapOrigin;
	// Bit 0 horizon march, bit 2 land-exact height off, bit 4 churn by
	// parallax. .y is DrawShell's (road tops this frame).
	a_cb.ShellFlags.x = (settings.ShellHorizonMarch ? 1 : 0) | (shellLandHeightDisabled ? 4 : 0) | (shellChurnParallaxOff ? 0 : 16);
	const bool fine = shellFineValid && shellTerrainFine && shellTerrainFine->srv;
	a_cb.FineWindow = { a_cb.GridOrigin.x - shellFineOriginX, a_cb.GridOrigin.y - shellFineOriginY,
		fine ? float(kShellFineDim) : 0.0f, kShellFineTexel };
	a_cb.SlopeDrape = { std::clamp(settings.SlopeDrape, 0.0f, 1.0f), 0.5f, 0.866f, 0.0f };
	if (fineProbeArmed) {
		fineProbeArmed = false;
		ProbeFineLayer(a_cb);
	}
	// Snow uv offset folded to the tile period, so shader-side uv math stays
	// in small numbers. Must match kSnowUVTile in SnowShell.hlsl.
	constexpr float kSnowUVTile = 4096.0f / 24.0f;
	a_cb.SnowUVOffset = {
		std::fmod(a_cb.GridOrigin.x, kSnowUVTile),
		std::fmod(a_cb.GridOrigin.y, kSnowUVTile)
	};
}

void SnowDeformation::StartEffectCostProbe()
{
	auto& p = effectCost;
	if (p.running)
		return;
	p = {};
	p.running = true;
	p.savedMask = shellKillMask;
	shellKillMask = 0;
}

// Each effect is read as a pair, everything on then that effect off, so a
// scene that drifts during the run cannot skew it. Each window settles, then
// takes the median of the rows these switches reach - the ground and object
// shading and their depth prepasses; the rest only add noise. The profiler
// drops rows on some frames, and the median ignores those.
void SnowDeformation::TickEffectCostProbe()
{
	auto& p = effectCost;
	if (!p.running)
		return;
	constexpr uint32_t kSettleFrames = 20;
	constexpr uint32_t kSampleFrames = 120;
	float total = 0.0f;
	for (const auto& r : globals::profiler->GetResults())
		if (r.valid && r.name.starts_with("SnowDeformation::") && (r.name.ends_with(" Shading") || r.name.ends_with(" Depth Prepass")))
			total += r.gpuTimeMs;
	if (p.frame >= kSettleFrames)
		p.samples.push_back(total);
	if (++p.frame < kSettleFrames + kSampleFrames)
		return;
	std::nth_element(p.samples.begin(), p.samples.begin() + p.samples.size() / 2, p.samples.end());
	const float median = p.samples.empty() ? 0.0f : p.samples[p.samples.size() / 2];
	(p.step % 2 ? p.withoutMs : p.withMs)[p.step / 2] = median;
	p.step++;
	p.frame = 0;
	p.samples.clear();
	if (p.step < 2 * EffectCostProbe::kEffects) {
		shellKillMask = p.step % 2 ? 1u << EffectCostProbe::kKillBits[p.step / 2] : 0u;
	} else {
		shellKillMask = p.savedMask;
		p.running = false;
		p.done = true;
	}
}

float SnowDeformation::ShellTessTargetRenderPx() const
{
	// Dynamic resolution ratios: render size over display size.
	const auto& dynRes = globals::game::frameBufferCached.GetDynamicResolutionParams1();
	const float scale = (dynRes.x > 0.0f && dynRes.y > 0.0f) ? std::clamp(std::sqrt(dynRes.x * dynRes.y), 0.25f, 1.0f) : 1.0f;
	return std::max(shellTessTargetPx, 0.0f) * scale;
}

// S1: the ground prepass at the end of Prepass, before the game's opaque
// draws. Last DrawShell's CB re-placed for this frame's camera and windows:
// the deformation map and its fields are this frame's, the object maps and
// water windows last frame's.
void SnowDeformation::DrawShellEarlyPrepass()
{
	shellEarlyState = EarlyPrepass::NotDrawing;
	const uint32_t frame = globals::state->frameCount;
	if (!settings.EnableSnowDeformation || !globals::state->inWorld || SnowShadersPending(1) || !lastShellCBData ||
		shellDrawViewportFrame + 1 != frame || !shellDrawGroundGated)
		return;
	// The viewport is last frame's: a new render size leaves this frame to
	// DrawShell's own prepass.
	const auto& frameDynRes = globals::game::frameBufferCached.GetDynamicResolutionParams1();
	if (frameDynRes.x != shellDrawDynRes[0] || frameDynRes.y != shellDrawDynRes[1])
		return;
	auto& tb = globals::features::terrainBlending;
	if (!tb.loaded || !tb.settings.Enabled || !tb.blendedDepthTexture || !tb.blendedDepthTexture->srv) {
		shellEarlyState = EarlyPrepass::NeedsTerrainBlending;
		return;
	}
	shellEarlyState = EarlyPrepass::Unavailable;
	auto renderer = globals::game::renderer;
	const auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	const bool parallax = TrenchParallaxActive();
	auto* psEarly = GetShellPSPrepassEarly(shellPrepassExitOff, parallax);
	auto* fillVS = GetShellFillVS();
	auto* fillPS = fillVS ? GetShellFillPS() : nullptr;
	auto* fillClaim = fillVS ? GetShellFillPSClaim() : nullptr;
	// The replay draws the ground; without this frame's capture it is DrawShell's.
	if (!psEarly || !fillPS || !fillClaim || !GetShellPSEarlyResolve() || !GetShellPSDeferred(parallax) || !mainDepth.views[0] || !mainDepth.texture ||
		!shellFillDepthState || !LandscapeShadeReady())
		return;
	// DrawShell sizes the prepass targets from the main depth; checked here only
	// (until Terrain Blending's replay the main depth's SRV is its blended copy).
	D3D11_TEXTURE2D_DESC mainDesc{};
	mainDepth.texture->GetDesc(&mainDesc);
	if (!shellRasterDepth || shellRasterDepth->desc.Width != mainDesc.Width || shellRasterDepth->desc.Height != mainDesc.Height || !shellTestDepthDSV ||
		!EnsureDeferredResources() || !EnsureEarlyPrepassResources() || (parallax && !EnsureParallaxOffset()) || !shellTerrainTexture || !shellTerrainTexture->srv ||
		!heightTopFiltered || !heightBottomFiltered || !heightTopRaw[heightCurrent] || !heightSkinDepth)
		return;

	auto context = globals::d3d::context;
	auto& fb = globals::game::frameBufferCached;
	ShellCB cb = *lastShellCBData;
	cb.CameraViewProj = fb.GetCameraViewProj();
	cb.CameraViewProjUnjittered = fb.GetCameraViewProjUnjittered();
	cb.CameraPreviousViewProjUnjittered = fb.GetCameraPreviousViewProjUnjittered();
	cb.CameraView = fb.GetCameraView();
	cb.CameraPosAdjust = fb.GetCameraPosAdjust();
	cb.CameraPreviousPosAdjust = fb.GetCameraPreviousPosAdjust();
	RefreshShellGridPlacement(cb);
	cb.SeamRampInv = 0.0f;
	int seamMinX, seamMinY, seamMaxX, seamMaxY;
	if (LoadedCellSquare(seamMinX, seamMinY, seamMaxX, seamMaxY))
		SetShellSeamBounds(cb, seamMinX, seamMinY, seamMaxX, seamMaxY);
	cb.DeformInvWorldSize = 1.0f / deformWorldSize;
	cb.ObjectHeightCenter = heightWindowCenter;
	cb.ObjectHeightHalfExtent = ObjectRasterHalfExtent();
	cb.BermBakeActive = (!shellBermBakeDisabled && bermFieldTexture) ? 1.0f : 0.0f;
	cb.UndulationFieldWindow = { undulationFieldCenter.x, undulationFieldCenter.y,
		1.0f / kUndulationFieldHalfExtent,
		(undulationFieldValid && !shellUndulationBakeDisabled) ? 1.0f : 0.0f };
	cb.ExclusionFieldWindow = { exclusionFieldCenter.x, exclusionFieldCenter.y,
		1.0f / kExclusionFieldHalfExtent,
		(exclusionFieldValid && !shellDistantExclusionsDisabled) ? 1.0f : 0.0f };
	const auto mapDim = [](const Texture2D* a_tex) { return (a_tex && a_tex->srv) ? float(a_tex->desc.Width) : 0.0f; };
	cb.FieldDims = { mapDim(deformationTextures[0]), mapDim(bermFieldTexture), mapDim(heightTopFiltered), mapDim(exclusionFieldTexture) };
	cb.TessScreen = { ShellTessTargetRenderPx(), shellDrawViewport.Width * 0.5f, shellDrawViewport.Height * 0.5f, shellDrawViewport.MaxDepth - shellDrawViewport.MinDepth };
	cb.SnowParallax.w = parallax ? std::max(trenchParallaxReach, 0.0f) : 0.0f;
	shellCB->Update(cb);

	// The game re-binds only what its state cache believes changed: every slot
	// this pass touches goes back as found.
	struct StageBinds
	{
		ID3D11ShaderResourceView* srv[34]{};
		ID3D11Buffer* cb[4]{};
		ID3D11SamplerState* sampler = nullptr;
	};
	StageBinds saved[5];
#define SNOW_SAVE_STAGE(P, b)                                    \
	context->P##GetShaderResources(0, 30, b.srv);                \
	context->P##GetShaderResources(56, 4, b.srv + 30);           \
	context->P##GetConstantBuffers(0, 1, b.cb);                  \
	context->P##GetConstantBuffers(4, 3, b.cb + 1);              \
	context->P##GetSamplers(0, 1, &b.sampler)
#define SNOW_RESTORE_STAGE(P, b)                                 \
	context->P##SetShaderResources(0, 30, b.srv);                \
	context->P##SetShaderResources(56, 4, b.srv + 30);           \
	context->P##SetConstantBuffers(0, 1, b.cb);                  \
	context->P##SetConstantBuffers(4, 3, b.cb + 1);              \
	context->P##SetSamplers(0, 1, &b.sampler)
	SNOW_SAVE_STAGE(VS, saved[0]);
	SNOW_SAVE_STAGE(HS, saved[1]);
	SNOW_SAVE_STAGE(DS, saved[2]);
	SNOW_SAVE_STAGE(PS, saved[3]);
	SNOW_SAVE_STAGE(CS, saved[4]);
	winrt::com_ptr<ID3D11VertexShader> prevVS;
	winrt::com_ptr<ID3D11HullShader> prevHS;
	winrt::com_ptr<ID3D11DomainShader> prevDS;
	winrt::com_ptr<ID3D11PixelShader> prevPS;
	winrt::com_ptr<ID3D11ComputeShader> prevCS;
	context->VSGetShader(prevVS.put(), nullptr, nullptr);
	context->HSGetShader(prevHS.put(), nullptr, nullptr);
	context->DSGetShader(prevDS.put(), nullptr, nullptr);
	context->PSGetShader(prevPS.put(), nullptr, nullptr);
	context->CSGetShader(prevCS.put(), nullptr, nullptr);
	ID3D11UnorderedAccessView* prevCSUAVs[5]{};
	context->CSGetUnorderedAccessViews(1, 5, prevCSUAVs);
	winrt::com_ptr<ID3D11InputLayout> prevLayout;
	context->IAGetInputLayout(prevLayout.put());
	D3D11_PRIMITIVE_TOPOLOGY prevTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	context->IAGetPrimitiveTopology(&prevTopology);
	winrt::com_ptr<ID3D11RasterizerState> prevRaster;
	context->RSGetState(prevRaster.put());
	D3D11_VIEWPORT prevViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	UINT prevViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	context->RSGetViewports(&prevViewportCount, prevViewports);
	winrt::com_ptr<ID3D11DepthStencilState> prevDepth;
	UINT prevStencilRef = 0;
	context->OMGetDepthStencilState(prevDepth.put(), &prevStencilRef);
	winrt::com_ptr<ID3D11BlendState> prevBlend;
	FLOAT prevBlendFactor[4]{};
	UINT prevSampleMask = 0xFFFFFFFF;
	context->OMGetBlendState(prevBlend.put(), prevBlendFactor, &prevSampleMask);
	ID3D11RenderTargetView* prevRTVs[8]{};
	ID3D11DepthStencilView* prevDSV = nullptr;
	context->OMGetRenderTargets(8, prevRTVs, &prevDSV);

	// A terrain window this Prepass moved: the water windows follow it here, as
	// DrawShell would, before the wet cut and the prepass read them on its grid.
	// The slots the capture touches outside the saved set go back as found.
	if (shellTerrainVersion != shellDrawTerrainVersion) {
		ID3D11Buffer* prevVB = nullptr;
		UINT prevVBStride = 0, prevVBOffset = 0;
		ID3D11Buffer* prevIB = nullptr;
		DXGI_FORMAT prevIBFormat = DXGI_FORMAT_UNKNOWN;
		UINT prevIBOffset = 0;
		ID3D11Buffer* prevVSCB1 = nullptr;
		ID3D11ShaderResourceView* prevWaterFineSRV = nullptr;
		ID3D11UnorderedAccessView* prevCSUAV0 = nullptr;
		context->IAGetVertexBuffers(0, 1, &prevVB, &prevVBStride, &prevVBOffset);
		context->IAGetIndexBuffer(&prevIB, &prevIBFormat, &prevIBOffset);
		context->VSGetConstantBuffers(1, 1, &prevVSCB1);
		context->PSGetShaderResources(104, 1, &prevWaterFineSRV);
		context->CSGetUnorderedAccessViews(0, 1, &prevCSUAV0);
		RenderWaterCapture();
		CoverWaterWindow();
		context->IASetVertexBuffers(0, 1, &prevVB, &prevVBStride, &prevVBOffset);
		context->IASetIndexBuffer(prevIB, prevIBFormat, prevIBOffset);
		context->VSSetConstantBuffers(1, 1, &prevVSCB1);
		context->PSSetShaderResources(104, 1, &prevWaterFineSRV);
		const UINT keepCount = UINT(-1);
		context->CSSetUnorderedAccessViews(0, 1, &prevCSUAV0, &keepCount);
		const auto release = [](IUnknown* a_held) {
			if (a_held)
				a_held->Release();
		};
		release(prevVB);
		release(prevIB);
		release(prevVSCB1);
		release(prevWaterFineSRV);
		release(prevCSUAV0);
	}

	if (cb.CompactLook.x > 0.5f)
		UpdateTerrainWet();

	ID3D11Buffer* cbs[1] = { shellCB->CB() };
	auto state = globals::state;
	ID3D11Buffer* sharedBuffers[3] = { state->permutationCB->CB(), state->sharedDataCB->CB(), state->featureDataCB->CB() };
	ID3D11ShaderResourceView* shellSRVs[9] = { shellTerrainTexture->srv.get(), GetDeformationSRV(), shellSnowDiffuseSRV.get(), Util::GetCurrentSceneDepthSRV(false), heightTopFiltered->srv.get(), heightBottomFiltered->srv.get(), shellSnowNormalSRV.get(), shellSnowRmaosSRV.get(), shellSnowHeightSRV.get() };
	ID3D11ShaderResourceView* wetSRV = TerrainWetSRV(cb.CompactLook.x > 0.5f);
	ID3D11ShaderResourceView* fineSRV = (shellFineValid && shellTerrainFine) ? shellTerrainFine->srv.get() : nullptr;
	ID3D11ShaderResourceView* waterSRVs[2] = { ShellWaterSRV(), waterHeightTexture ? waterHeightTexture->srv.get() : nullptr };
	ID3D11ShaderResourceView* waterNearSRV = waterNearTexture && waterNearTexture->srv ? waterNearTexture->srv.get() : nullptr;
	ID3D11ShaderResourceView* objectCapSRVs[2] = { heightTopRaw[heightCurrent]->srv.get(), heightSkinDepth->srv.get() };
	ID3D11ShaderResourceView* untouchedSRV = untouchedTilesSRV.get();
	ID3D11ShaderResourceView* exclusionSRV = GetExclusionFieldSRV();
	ID3D11SamplerState* snowSampler = shellSnowSampler.get();

	globals::profiler->BeginPass("SnowDeformation::Ground - Shell Depth Prepass");
	// The private depth starts as the blended scene depth: the terrain's true
	// depth, where the main buffer still holds Terrain Blending's offset copy.
	D3D11_VIEWPORT fillViewport = shellDrawViewport;
	fillViewport.MinDepth = 0.0f;
	fillViewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &fillViewport);
	context->RSSetState(shellRasterState.get());
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->OMSetRenderTargets(0, nullptr, shellTestDepthDSV.get());
	context->OMSetDepthStencilState(shellFillDepthState.get(), 0);
	context->VSSetShader(fillVS, nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(fillPS, nullptr, 0);
	ID3D11ShaderResourceView* sceneDepthSRV = tb.blendedDepthTexture->srv.get();
	context->PSSetShaderResources(9, 1, &sceneDepthSRV);
	context->Draw(3, 0);
	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->PSSetShaderResources(9, 1, &nullSRV);

	// The prepass: raster depth (negative while pending), the deferred
	// attributes and the pending inputs, against the private depth.
	const float rasterClear[4] = {};
	context->ClearRenderTargetView(shellRasterDepth->rtv.get(), rasterClear);
	ID3D11RenderTargetView* prepassRTVs[6] = { shellRasterDepth->rtv.get(), shellDeferredAttr[0]->rtv.get(), shellDeferredAttr[1]->rtv.get(), shellDeferredAttr[2]->rtv.get(), shellEarlyPending->rtv.get(),
		parallax ? shellParallaxOffset->rtv.get() : nullptr };
	context->OMSetRenderTargets(parallax ? 6 : 5, prepassRTVs, shellTestDepthDSV.get());
	context->OMSetDepthStencilState(shellDepthState.get(), 0);
	context->RSSetViewports(1, &shellDrawViewport);
	context->PSSetShader(psEarly, nullptr, 0);
	context->PSSetConstantBuffers(0, 1, cbs);
	context->PSSetConstantBuffers(4, 3, sharedBuffers);
	context->PSSetShaderResources(0, 9, shellSRVs);
	context->PSSetShaderResources(11, 2, objectCapSRVs);
	context->PSSetShaderResources(13, 1, &fineSRV);
	context->PSSetShaderResources(15, 1, &exclusionSRV);
	context->PSSetShaderResources(19, 1, &waterNearSRV);
	context->PSSetShaderResources(27, 2, waterSRVs);
	context->PSSetShaderResources(kUntouchedTilesSlot, 1, &untouchedSRV);
	context->PSSetShaderResources(kTerrainWetSlot, 1, &wetSRV);
	ID3D11ShaderResourceView* bareSRV = shellTerrainBare ? shellTerrainBare->srv.get() : nullptr;
	context->PSSetShaderResources(kTerrainBareSlot, 1, &bareSRV);
	// The parallax march reads the berm and undulation fields (inside the saved slots).
	if (parallax) {
		ID3D11ShaderResourceView* marchSRVs[2] = { GetBermFieldSRV(), GetUndulationFieldSRV() };
		context->PSSetShaderResources(14, 1, &marchSRVs[0]);
		context->PSSetShaderResources(29, 1, &marchSRVs[1]);
	}
	context->PSSetSamplers(0, 1, &snowSampler);
	// Terrain Blending's mask: whether the land or an object is the surface at a pixel.
	ID3D11ShaderResourceView* tbMaskSRV = tb.terrainDepth.depthSRV;
	context->PSSetShaderResources(kTBMaskSlot, 1, &tbMaskSRV);
	DrawLandscapeSnowPrepass(context, cb, cbs[0], parallax);
	tbMaskSRV = nullptr;
	context->PSSetShaderResources(kTBMaskSlot, 1, &tbMaskSRV);

	// The settled pixels into the main depth: the game's opaque draws, the
	// terrain replay among them, now test against the snow.
	context->OMSetRenderTargets(0, nullptr, mainDepth.views[0]);
	context->RSSetViewports(1, &fillViewport);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(fillVS, nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(fillClaim, nullptr, 0);
	ID3D11ShaderResourceView* rasterSRV = shellRasterDepth->srv.get();
	context->PSSetShaderResources(9, 1, &rasterSRV);
	context->Draw(3, 0);
	context->PSSetShaderResources(9, 1, &nullSRV);
	globals::profiler->EndPass();

	context->OMSetRenderTargets(8, prevRTVs, prevDSV);
	SNOW_RESTORE_STAGE(VS, saved[0]);
	SNOW_RESTORE_STAGE(HS, saved[1]);
	SNOW_RESTORE_STAGE(DS, saved[2]);
	SNOW_RESTORE_STAGE(PS, saved[3]);
	SNOW_RESTORE_STAGE(CS, saved[4]);
#undef SNOW_SAVE_STAGE
#undef SNOW_RESTORE_STAGE
	const UINT keepCounts[5] = { UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1) };
	context->CSSetUnorderedAccessViews(1, 5, prevCSUAVs, keepCounts);
	context->VSSetShader(prevVS.get(), nullptr, 0);
	context->HSSetShader(prevHS.get(), nullptr, 0);
	context->DSSetShader(prevDS.get(), nullptr, 0);
	context->PSSetShader(prevPS.get(), nullptr, 0);
	context->CSSetShader(prevCS.get(), nullptr, 0);
	context->IASetInputLayout(prevLayout.get());
	context->IASetPrimitiveTopology(prevTopology);
	context->RSSetState(prevRaster.get());
	if (prevViewportCount)
		context->RSSetViewports(prevViewportCount, prevViewports);
	context->OMSetDepthStencilState(prevDepth.get(), prevStencilRef);
	context->OMSetBlendState(prevBlend.get(), prevBlendFactor, prevSampleMask);
	for (auto& b : saved) {
		for (auto* v : b.srv)
			if (v)
				v->Release();
		for (auto* c : b.cb)
			if (c)
				c->Release();
		if (b.sampler)
			b.sampler->Release();
	}
	for (auto* u : prevCSUAVs)
		if (u)
			u->Release();
	for (auto* r : prevRTVs)
		if (r)
			r->Release();
	if (prevDSV)
		prevDSV->Release();
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET, RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT,
		RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE, RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND, RE::BSGraphics::ShaderFlags::DIRTY_RASTER_CULL_MODE,
		RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);

	if (!shellEarlyCB)
		shellEarlyCB = std::make_unique<ShellCB>();
	*shellEarlyCB = cb;
	shellEarlyViewport = shellDrawViewport;
	shellEarlyFrame = frame;
	shellEarlyState = EarlyPrepass::Drawing;
}

void SnowDeformation::SyncSssDepth()
{
	if (!settings.EnableSnowDeformation || !globals::state->inWorld)
		return;
	const uint32_t frame = globals::state->frameCount;
	auto& ss = globals::features::screenSpaceShadows;
	auto& tb = globals::features::terrainBlending;
	auto* blended = tb.blendedDepthTexture;
	auto sky = globals::game::sky;
	// ScreenSpaceShadows::PostPrepass marches GetCurrentSceneDepthSRV(false) on these conditions.
	const bool marched = settings.SnowScreenSpaceShadows && ss.loaded && ss.screenSpaceShadowsTexture && ss.bendSettings.Enable &&
	                     sky && sky->mode.get() == RE::Sky::Mode::kFull && tb.loaded && tb.settings.Enabled;
	// The restore has to follow the march in the PostPrepass loop.
	static const bool marchFirst = [] {
		const auto& list = Feature::GetFeatureList();
		const auto at = [&](const Feature* a_feature) { return std::find(list.begin(), list.end(), a_feature) - list.begin(); };
		const bool ok = at(&globals::features::screenSpaceShadows) < at(&globals::features::snowDeformation);
		if (!ok)
			logger::warn("[SNOW DEFORMATION] Screen-Space Shadows is not ahead of Snow Deformation in the feature list; the shadow depth sync stays off");
		return ok;
	}();
	const auto rasterSized = [&](const Texture2D* a_tex) {
		return a_tex && a_tex->uav && a_tex->desc.Width == shellRasterDepth->desc.Width && a_tex->desc.Height == shellRasterDepth->desc.Height;
	};
	// An early pass this frame means the prime has published these shaders.
	const bool early = shellEarlyFrame == frame && shellEarlyState == EarlyPrepass::Drawing;
	auto* syncCS = (early && marchFirst && marched) ? GetSssSyncCS() : nullptr;
	if (!syncCS || !shellRasterDepth || !shellRasterDepth->srv || !rasterSized(blended) || !rasterSized(shellEarlyMarks) || !GetSssRestoreCS() ||
		!DepthSyncRectBuffer()) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Shadow Depth Sync");
		globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Shadow Depth Restore");
		return;
	}
	shellSssRect = ScreenRectBox(&shellEarlyViewport, blended->desc.Width, blended->desc.Height, 0);
	DispatchSssDepth(syncCS, "SnowDeformation::Ground - Shadow Depth Sync");
	shellSssSyncFrame = frame;
}

void SnowDeformation::RestoreSssDepth()
{
	if (shellSssSyncFrame == globals::state->frameCount)
		DispatchSssDepth(GetSssRestoreCS(), "SnowDeformation::Ground - Shadow Depth Restore");
}

void SnowDeformation::DispatchSssDepth(ID3D11ComputeShader* a_cs, const char* a_pass)
{
	auto& tb = globals::features::terrainBlending;
	if (!a_cs || !tb.blendedDepthTexture || !tb.blendedDepthTexture->uav || !shellRasterDepth || !shellRasterDepth->srv || !shellEarlyMarks ||
		!shellEarlyMarks->uav || !depthSyncRectCB)
		return;
	auto context = globals::d3d::context;
	ID3D11ShaderResourceView* prevSRV = nullptr;
	ID3D11UnorderedAccessView* prevUAVs[2]{};
	ID3D11Buffer* prevCB = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> prevCS;
	context->CSGetShaderResources(1, 1, &prevSRV);
	context->CSGetUnorderedAccessViews(0, 2, prevUAVs);
	context->CSGetConstantBuffers(2, 1, &prevCB);
	context->CSGetShader(prevCS.put(), nullptr, nullptr);

	globals::profiler->BeginPass(a_pass);
	DepthSyncRectCB rectData{ { shellSssRect.left, shellSssRect.top }, { shellSssRect.right - shellSssRect.left, shellSssRect.bottom - shellSssRect.top } };
	depthSyncRectCB->Update(rectData);
	ID3D11Buffer* rectCB = depthSyncRectCB->CB();
	ID3D11ShaderResourceView* rasterSRV = shellRasterDepth->srv.get();
	ID3D11UnorderedAccessView* uavs[2] = { tb.blendedDepthTexture->uav.get(), shellEarlyMarks->uav.get() };
	context->CSSetConstantBuffers(2, 1, &rectCB);
	context->CSSetShaderResources(1, 1, &rasterSRV);
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
	context->CSSetShader(a_cs, nullptr, 0);
	context->Dispatch((rectData.Extent[0] + 7) / 8, (rectData.Extent[1] + 7) / 8, 1);
	globals::profiler->EndPass();

	const UINT keepCounts[2] = { UINT(-1), UINT(-1) };
	context->CSSetUnorderedAccessViews(0, 2, prevUAVs, keepCounts);
	context->CSSetShaderResources(1, 1, &prevSRV);
	context->CSSetConstantBuffers(2, 1, &prevCB);
	context->CSSetShader(prevCS.get(), nullptr, 0);
	if (prevSRV)
		prevSRV->Release();
	for (auto* u : prevUAVs)
		if (u)
			u->Release();
	if (prevCB)
		prevCB->Release();
}

bool SnowDeformation::SkinSssMarchReady()
{
	auto& ss = globals::features::screenSpaceShadows;
	auto& tb = globals::features::terrainBlending;
	auto* blended = tb.blendedDepthTexture;
	auto sky = globals::game::sky;
	if (!settings.SnowScreenSpaceShadows || !ss.loaded || !ss.screenSpaceShadowsTexture || !ss.screenSpaceShadowsTexture->srv || !ss.bendSettings.Enable ||
		!sky || sky->mode.get() != RE::Sky::Mode::kFull || !tb.loaded || !tb.settings.Enabled || !blended || !blended->uav || !preSnowDepthSRV)
		return false;
	if (skinSssSaved && (skinSssSaved->desc.Width != blended->desc.Width || skinSssSaved->desc.Height != blended->desc.Height)) {
		delete skinSssSaved;
		skinSssSaved = nullptr;
	}
	if (!skinSssSaved) {
		D3D11_TEXTURE2D_DESC desc = blended->desc;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		skinSssSaved = new Texture2D(desc, "SnowDeformation::SkinSssSaved");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = DXGI_FORMAT_R32_FLOAT,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		skinSssSaved->CreateUAV(uavDesc);
	}
	return skinSssSaved->uav && GetSkinSssSyncCS() && GetSkinSssRestoreCS() && DepthSyncRectBuffer();
}

void SnowDeformation::MarchSkinSss(ID3D11ShaderResourceView* a_skinDepthSRV, ID3D11ShaderResourceView* a_sceneDepthSRV, const D3D11_VIEWPORT* a_viewport)
{
	auto context = globals::d3d::context;
	auto& ss = globals::features::screenSpaceShadows;
	auto& tb = globals::features::terrainBlending;
	const auto& blendedDesc = tb.blendedDepthTexture->desc;
	const D3D11_BOX rect = ScreenRectBox(a_viewport, blendedDesc.Width, blendedDesc.Height, 0);
	DepthSyncRectCB rectData{ { rect.left, rect.top }, { rect.right - rect.left, rect.bottom - rect.top } };
	depthSyncRectCB->Update(rectData);
	ID3D11Buffer* rectCB = depthSyncRectCB->CB();
	ID3D11ShaderResourceView* srvs[3] = { a_skinDepthSRV, shellGroundMarksSRV, preSnowDepthSRV.get() };
	ID3D11UnorderedAccessView* uavs[2] = { tb.blendedDepthTexture->uav.get(), skinSssSaved->uav.get() };
	const auto dispatch = [&](ID3D11ComputeShader* a_cs) {
		context->CSSetConstantBuffers(2, 1, &rectCB);
		context->CSSetShaderResources(0, 3, srvs);
		context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
		context->CSSetShader(a_cs, nullptr, 0);
		context->Dispatch((rectData.Extent[0] + 7) / 8, (rectData.Extent[1] + 7) / 8, 1);
		ID3D11ShaderResourceView* nullSRVs[3] = {};
		ID3D11UnorderedAccessView* nullUAVs[2] = {};
		ID3D11Buffer* nullCB = nullptr;
		context->CSSetShaderResources(0, 3, nullSRVs);
		context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
		context->CSSetConstantBuffers(2, 1, &nullCB);
		context->CSSetShader(nullptr, nullptr, 0);
	};
	// The skins' private depth is the bound DSV; its SRV is read here.
	context->OMSetRenderTargets(0, nullptr, nullptr);
	dispatch(GetSkinSssSyncCS());
	// The march times its own row; profiler passes do not nest.
	globals::profiler->EndPass();
	ss.DrawShadows();
	globals::profiler->BeginPass("SnowDeformation::Objects - Shadow Depth Restore");
	dispatch(GetSkinSssRestoreCS());
	// The UAV writes unbound the blended depth (PS t3) and the mask (PS t45).
	ID3D11ShaderResourceView* maskSRV = ss.screenSpaceShadowsTexture->srv.get();
	context->PSSetShaderResources(3, 1, &a_sceneDepthSRV);
	context->PSSetShaderResources(45, 1, &maskSRV);
	skinSssMarchFrame = globals::state->frameCount;
}

void SnowDeformation::DrawShell()
{
	TickEffectCostProbe();
	// First profiled code after the opaque batches; the profiler cannot nest.
	CloseLandscapeRun();
	if (!settings.EnableSnowDeformation)
		return;
	if (SnowShadersPending(1))
		return;
	LoadTraceScope _loadTrace(this, "Shell: DrawShell");

	if (!globals::state->inWorld)
		return;

	ScopedTicks _drawShellTicks(cpuCensus.spanTicks[CpuCensus::kSpanDrawShell], cpuCensus.spanCalls[CpuCensus::kSpanDrawShell]);
	SpanClock spanClock;
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto& fb = globals::game::frameBufferCached;

	// Latched per frame: the early prepass's choice when it drew.
	const bool parallax = TrenchParallaxActive();

	ShellCB cbData{};
	cbData.CameraViewProj = fb.GetCameraViewProj();
	cbData.CameraViewProjUnjittered = fb.GetCameraViewProjUnjittered();
	cbData.CameraPreviousViewProjUnjittered = fb.GetCameraPreviousViewProjUnjittered();
	cbData.CameraView = fb.GetCameraView();
	cbData.CameraPosAdjust = fb.GetCameraPosAdjust();
	cbData.CameraPreviousPosAdjust = fb.GetCameraPreviousPosAdjust();

	RefreshShellGridPlacement(cbData);
	// Bit 0: a road can write road tops into the skin-depth raster this frame
	// (the skin march's road taps read nothing else).
	cbData.ShellFlags.y = (objectRoadGateOff || (settings.RoadHeightfield && capturedRoads > 0)) ? 1 : 0;
	cbData.StaticsDebugView = float(staticsDebugView);
	cbData.DebugSkinDepth = { settings.SkinDepthBias, settings.SkinSlopeDepthBias, 0.0f, 0.0f };
	cbData.BloodLook = { std::clamp(settings.BloodIntensity, 0.0f, 2.0f), bloodBurialClock,
		gameClockHours.load(std::memory_order_relaxed), std::max(settings.BloodBurial, 0.01f) };
	cbData.BloodLook2 = { std::max(settings.BloodAgeHours, 0.1f), std::clamp(settings.BloodSheen, 0.0f, 1.0f),
		(settings.BloodOnSnow && bloodMapTexture && bloodClockTexture) ? 1.0f : 0.0f, BloodTilesLive() ? 1.0f : 0.0f };
	cbData.BloodLook3 = { 0.0f, 0.0f, BloodTileCell(), BloodTileTexel() };

	// Loaded-cell boundary square around the PLAYER's cell (cell attachment
	// follows the player, not the camera): full terrain inside, LOD outside.
	// The shell's edge fade anchors here so it hands off to the horizon
	// recolor exactly where the game swaps terrain for LOD meshes.
	cbData.SeamRampInv = 0.0f;
	int seamMinX, seamMinY, seamMaxX, seamMaxY;
	if (LoadedCellSquare(seamMinX, seamMinY, seamMaxX, seamMaxY))
		SetShellSeamBounds(cbData, seamMinX, seamMinY, seamMaxX, seamMaxY);
	cbData.DeformInvWorldSize = 1.0f / deformWorldSize;

	EnsureShellSnowTextures();
	// Per frame so TruePBR's hot-reload and live menu edits of the matched
	// texture set reach the shell the same frame they reach the ground.
	RefreshSnowPBRParams();
	cbData.HasSnowTexture = shellSnowDiffuseSRV != nullptr;
	cbData.SnowTextureIsLinear = shellSnowTextureIsPBR ? 1.0f : 0.0f;
	cbData.HasSnowHeight = shellSnowHeightSRV ? 1.0f : 0.0f;
	// Tessellated relief amplitude, straight from the slider (world units;
	// the PBR config's displacementScale is deliberately not multiplied in,
	// the slider is authoritative).
	// Relief retired (setting removed 2026-08-27, slider long gone): 0 keeps
	// the tess reach collapsing to 1 on undeformed ground and the DS flat.
	cbData.SnowReliefDepth = 0.0f;
	// Parallax. HeightScale is the PBR config's displacementScale verbatim:
	// kSnowUVTile now equals the game's landscape tiling, so the UV-space slab
	// depth Extended Materials derives from it lands on the same world depth
	// the ground beside us gets. No correction factor.
	// w is a documented spare: the march runs Extended Materials' own step
	// budget since M4, so the old step count read nowhere and was retired.
	cbData.SnowParallax = {
		snowDisplacementScale,
		std::clamp(settings.ParallaxShadowStrength, 0.0f, 2.0f),
		std::clamp(settings.ParallaxDepth, 0.0f, 2.0f),
		parallax ? std::max(trenchParallaxReach, 0.0f) : 0.0f
	};
	cbData.SpellShading = { std::max(settings.ScorchStrength, 0.0f),
		std::clamp(settings.CrustGloss, 0.0f, 1.0f),
		std::clamp(settings.CrustRoughness, 0.02f, 0.6f),
		std::clamp(settings.CrustNormalFlatten, 0.0f, 1.0f) };
	cbData.CrustLook = { std::clamp(settings.CrustSpecular, 0.0f, 0.5f),
		settings.CrustTint[0], settings.CrustTint[1],
		std::max(settings.CrustSheen, 0.0f) };
	// yzw ride spare room already in this row rather than growing the shared
	// constant buffer, which is hand-mirrored across two shaders and is the
	// silent collision CLAUDE.md warns about.
	cbData.CrustLook2 = { settings.CrustTint[2],
		std::max(settings.FrostPatternStrength, 0.0f),
		std::max(settings.FrostPatternScale, 4.0f),
		frostPatternNormalSRV ? 1.0f : 0.0f };
	// Fresh snow compacts instead of being thrown: little berm, no cut rim.
	// Snow Climate moves each lever from that fixed look to the climate's.
	const float snowSoftness = GetSnowSoftness();
	const SnowClimateLook look = ComputeSnowClimateLook();
	const float bermSlider = std::clamp(settings.BermHeight, 0.0f, 1.0f);
	const float lipSlider = std::clamp(settings.RimLip, 0.0f, 0.3f);
	const float teethSlider = std::clamp(settings.RimTeeth, 0.0f, 1.0f);
	const float clodSlider = std::clamp(settings.BermClods, 0.0f, 6.0f);
	cbData.BermHeightAmp = std::lerp(bermSlider * std::lerp(1.0f, 0.35f, snowSoftness), bermSlider * look.berm, look.strength);
	cbData.BermReach = BermReachScale();
	// .w claims RimStyle's documented spare rather than growing ShellCB: the
	// buffer is hand-mirrored across two shaders and the statics pass binds
	// this same b0, so one component serves both shells with nothing moving.
	cbData.RimStyle = { std::lerp(lipSlider * (1.0f - snowSoftness), lipSlider * look.lip, look.strength),
		std::lerp(teethSlider * (1.0f - snowSoftness), teethSlider * look.teeth, look.strength),
		std::lerp(clodSlider, clodSlider * look.clods, look.strength),
		GetAccumulationDepthScale() };
	// Trampled ground: floors walked deeper than one pass flatten their churn.
	cbData.TrampleStyle = { 0.0f, std::clamp(settings.TrampledFlattening, 0.0f, 1.0f), BodyPrintScale(), 0.0f };
	// Loose snow holds no clods. Under Snow Climate the per-pixel ramp
	// (SnowClimateLUT) scales the slider instead; roads take it below.
	const float churnSlider = std::clamp(settings.ChurnHeight, 0.0f, 8.0f);
	const float churnBase = std::lerp(churnSlider * std::lerp(1.0f, std::clamp(settings.FreshSnowChurn, 0.0f, 1.0f), snowSoftness), churnSlider, look.strength);
	cbData.ChurnHeightAmp = churnBase;
	cbData.ChurnSizeScale = std::clamp(settings.ChurnSize, 0.25f, 4.0f);
	{
		// Pixels per world unit at distance d = focalPx / d, with
		// focalPx = half the screen height times the projection's y scale.
		const auto& eye = globals::game::shadowState->GetRuntimeData().cameraData.getEye();
		const float focalPx = 0.5f * static_cast<float>(globals::game::graphicsState->screenHeight) * eye.projMat(1, 1);
		const float capPx = std::clamp(settings.SkinTessCapPx, 0.0f, 64.0f);
		cbData.SkinTessCapSlope = (focalPx > 1.0f && capPx > 0.0f) ? capPx / focalPx : 0.0f;
	}
	// No texel of the window can churn: the churn-free deferred PS draws, and
	// the geometry's churn path skips with it. Fire and shock marks churn in
	// any climate.
	shellChurnFree = !debugChurnFreeOff && cbData.RimStyle.y <= 0.001f && cbData.RimStyle.z <= 0.01f &&
	                 (cbData.ChurnHeightAmp <= 0.01f || (look.churnNowhere && spellMarkTexels == 0));
	if (shellChurnFree)
		cbData.ChurnHeightAmp = 0.0f;
	// Object trench detail follows the landscape set. The CB rows stay because
	// both shells read them. The road patch's churn reads no climate rows: its
	// cover is the road layer's, so the ramp at that depth is folded in here.
	cbData.ObjBermHeightAmp = cbData.BermHeightAmp;
	cbData.ObjChurnHeightAmp = churnBase * look.roadChurn;
	cbData.ObjChurnSizeScale = cbData.ChurnSizeScale;
	cbData.HasSnowNormal = shellSnowNormalSRV ? 1.0f : 0.0f;
	cbData.HasSnowRmaos = shellSnowRmaosSRV ? 1.0f : 0.0f;
	cbData.SnowRoughnessScale = snowRoughnessScale;
	cbData.SnowGlintParams = { snowGlintLogDensity, snowGlintMicroRoughness, snowGlintDensityRandomization, snowGlintScreenSpaceScale };
	cbData.SnowSpecularLevel = snowSpecularLevel;
	// Sparkle: the shell's specular runs TruePBR's glint NDF when its shared
	// noise texture exists (bound to t20 below for the whole pass).
	cbData.EnableGlints = (settings.SnowGlints && globals::features::truePBR.glintsNoiseTexture) ? 1.0f : 0.0f;
	// xy are layout keepers: the retired dust switch and trench floor.
	cbData.BorderStyle = { 0.0f, 0.0f, (float)sunCascadeSlice[0], (float)sunCascadeSlice[1] };
	const auto& dynRes = globals::game::frameBufferCached.GetDynamicResolutionParams1();
	// x = the water cut (runtime A/B in Debugging Options); y, z = the land-blend
	// contest's depth bonus and carve penalty (log2 odds); w = the wall slough.
	const float2 landContest = LandBlendContest();
	cbData.CompactLook = { debugWaterCutDisabled ? 0.0f : 1.0f, landContest.x, landContest.y, look.strength * look.slough };
	{
		const SnowCondition condition = GetSnowCondition();
		auto* player = RE::PlayerCharacter::GetSingleton();
		cbData.SnowClimate = { condition.temperature, kClimateLapse, player ? player->GetPosition().z : 0.0f, condition.freshness };
		cbData.SnowClimate2 = { condition.windPacking, condition.wetness, (float)std::clamp(debugConditionView, 0, 5), 1.0f - look.strength };
		cbData.SnowClimateOverride = { debugClimateWetnessOverride, debugClimateStickinessOverride, debugClimateFirmnessOverride, 0.0f };
		std::copy(look.lut.begin(), look.lut.end(), cbData.SnowClimateLUT);
	}
	cbData.DebugNoFarPad = 0.0f;
	cbData.DebugParallaxView = (parallax && trenchParallaxView) ? 1.0f : 0.0f;
	cbData.ShellTriHeight = shellBilinearHeight ? 0.0f : 1.0f;
	// Gated on the terrain window existing: without it the cull's SampleTerrain
	// reads zeros, which look like fully bare ground everywhere.
	cbData.ShellCullBare = shellTerrainTexture ? 1.0f : 0.0f;
	// Statics-skin distance dissolve: starts at the blend slider, fully gone
	// at the Object Snow capture range (floored one meter past the start so
	// the smoothstep never degenerates when the sliders cross).
	// Distant Snow Blend (RangeSkinsFadeM) is retired: its dissolve END was
	// anchored at the capture range, so with both at 750 m and captures bounded
	// by the loaded cells (~207 m at uGrids 5) the slider could thin a real
	// object by at most ~7% - inert in practice, its stated job long since
	// taken over by Distant Bare Rock and the geometry-range flatten. The skin
	// now dissolves over a fixed ~29 m band inside the capture range, so
	// lowering Object Snow keeps a soft edge without a second slider.
	cbData.SkinFadeEnd = settings.RangeSkinsM * kUnitsPerMeter;
	cbData.SkinFadeStart = std::max(cbData.SkinFadeEnd - 2048.0f, 0.0f);
	// Field enable gate + window addressing for the t4/t5 samplers; the
	// center is re-uploaded below once the height pass has recentered.
	cbData.ObjectLiftCap = groundLiftDebug ? kObjectLiftCap : 0.5f;
	cbData.ObjectHeightCenter = heightWindowCenter;
	cbData.ObjectHeightHalfExtent = ObjectRasterHalfExtent();

	// Crisp shadows: full-resolution comparison PCF against the cascade-atlas
	// copies taken at the shadow-mask pass. When the copies are missing this
	// frame, the shader falls back to the blurred VSM path.
	cbData.CrispShadows = ShadowAtlasSRV() ? 1.0f : 0.0f;
	// Screen-Space Shadows availability: the feature clears its texture to
	// white every frame even when disabled, so multiplying is always safe
	// once the texture exists. 2 = this frame's mask was marched on the snow
	// (SyncSssDepth).
	auto& screenSpaceShadowsFeature = globals::features::screenSpaceShadows;
	cbData.ScreenSpaceShadowsActive = (settings.SnowScreenSpaceShadows && screenSpaceShadowsFeature.loaded && screenSpaceShadowsFeature.screenSpaceShadowsTexture) ?
	                                      (shellSssSyncFrame == globals::state->frameCount ? 2.0f : 1.0f) :
	                                      0.0f;

	cbData.UndulationAmp = std::max(settings.UndulationStrength, 0.0f);
	cbData.UndulationScale = std::max(settings.UndulationSpacing, 0.05f);
	// The bake is only usable once its texture exists AND Prepass has filled it
	// this frame; the A/B toggle suppresses both together.
	cbData.BermBakeActive = (!shellBermBakeDisabled && bermFieldTexture) ? 1.0f : 0.0f;
	cbData.UndulationFieldWindow = { undulationFieldCenter.x, undulationFieldCenter.y,
		1.0f / kUndulationFieldHalfExtent,
		(undulationFieldValid && !shellUndulationBakeDisabled) ? 1.0f : 0.0f };
	// Wide exclusion field window: filled provisionally here and RE-UPLOADED
	// below, after the height pass has rebaked the field and moved its centre.
	// Sampling this frame's texture through last frame's centre offsets the
	// whole field by a texel whenever the camera advances one, which reads as
	// clearing depth (and the berm riding on it) twitching while the camera
	// moves and settling when it stops. w gates the sampler off until the bake
	// has actually run.
	cbData.ExclusionFieldWindow = { exclusionFieldCenter.x, exclusionFieldCenter.y,
		1.0f / kExclusionFieldHalfExtent,
		(exclusionFieldValid && !shellDistantExclusionsDisabled) ? 1.0f : 0.0f };

	// Point lights: LLF's clustered visible-light list. The cluster buffers
	// are only coherent when LLF ran this frame (CORE, but boot-disableable).
	auto& lightLimitFix = globals::features::lightLimitFix;
	cbData.PointLightsActive = (lightLimitFix.loaded && lightLimitFix.lights && lightLimitFix.lightIndexList && lightLimitFix.lightGrid) ? 1.0f : 0.0f;
	// Skylighting probe volume for ambient parity with terrain shading.
	auto& skylighting = globals::features::skylighting;
	cbData.SkylightingActive = (skylighting.loaded && skylighting.texProbeArray) ? 1.0f : 0.0f;

	// Shadow-source diagnostics for the settings UI.
	dbgLodDescriptorCount = 0;
	if (auto* shadowSceneNode = globals::game::smState->shadowSceneNode[0]) {
		if (auto* sunShadowLight = shadowSceneNode->GetRuntimeData().sunShadowDirLight) {
			auto& dirLightData = sunShadowLight->GetShadowDirectionalLightRuntimeData();
			dbgLodDescriptorCount = (uint32_t)sunShadowLight->GetRuntimeData().shadowmapDescriptors.size();
			dbgLodEndSplits[0] = dirLightData.endSplitDistances[0];
			dbgLodEndSplits[1] = dirLightData.endSplitDistances[1];
			dbgLodEndSplits[2] = dirLightData.endSplitDistances[2];
		}
	}

	if (const uint32_t kill = shellKillMask) {
		if (kill & (1u << 0)) {
			cbData.SnowParallax.y = 0.0f;
			cbData.SnowParallax.z = 0.0f;
		}
		if (kill & (1u << 1))
			cbData.HasSnowNormal = 0.0f;
		if (kill & (1u << 2))
			cbData.HasSnowRmaos = 0.0f;
		if (kill & (1u << 3))
			cbData.CrispShadows = 0.0f;
		if (kill & (1u << 4))
			cbData.ScreenSpaceShadowsActive = 0.0f;
		if (kill & (1u << 6))
			cbData.EnableGlints = 0.0f;
		if (kill & (1u << 7))
			cbData.PointLightsActive = 0.0f;
		if (kill & (1u << 8))
			cbData.SkylightingActive = 0.0f;
		if (kill & (1u << 9))
			cbData.HasSnowTexture = 0;
	}
	shellCB->Update(cbData);

	// Back up the pipeline state we touch so the composite and later game
	// passes see exactly what they expect.
	winrt::com_ptr<ID3D11RasterizerState> prevRaster;
	winrt::com_ptr<ID3D11DepthStencilState> prevDepth;
	winrt::com_ptr<ID3D11BlendState> prevBlend;
	UINT prevStencilRef = 0;
	FLOAT prevBlendFactor[4]{};
	UINT prevSampleMask = 0xFFFFFFFF;
	D3D11_PRIMITIVE_TOPOLOGY prevTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	context->RSGetState(prevRaster.put());
	context->OMGetDepthStencilState(prevDepth.put(), &prevStencilRef);
	context->OMGetBlendState(prevBlend.put(), prevBlendFactor, &prevSampleMask);
	context->IAGetPrimitiveTopology(&prevTopology);
	winrt::com_ptr<ID3D11Buffer> prevIB;
	DXGI_FORMAT prevIBFormat = DXGI_FORMAT_UNKNOWN;
	UINT prevIBOffset = 0;
	{
		ID3D11Buffer* ib = nullptr;
		context->IAGetIndexBuffer(&ib, &prevIBFormat, &prevIBOffset);
		prevIB.attach(ib);
	}
	D3D11_VIEWPORT prevViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	UINT prevViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	context->RSGetViewports(&prevViewportCount, prevViewports);

	// Only the loaded square is asked: the shell ends at its seam, and land
	// past it never loads, so a test over the whole span never settles.
	// Snowy cells outside the frustum count as bare. A
	// loaded cell still baking opens the gate at once; bare must hold for
	// kShellGateBareFrames before the grid draw and its dependents (bake,
	// stats, depth sync) are skipped. The CB upload above stays - the
	// statics and the caster read it.
	// An interior has no landscape, so the square under the player is
	// unbaked for ever and read as unknown (Alternate Start's dungeon ran the
	// shell, bake and sync at full cost).
	if (InInteriorCell()) {
		shellSnowVerdict = kSnowGateBare;
	} else {
		int sqMinX, sqMinY, sqMaxX, sqMaxY;
		shellSnowVerdict = LoadedCellSquare(sqMinX, sqMinY, sqMaxX, sqMaxY) ?
		                       ViewSnowVerdict(sqMinX, sqMinY, sqMaxX, sqMaxY, cbData) :
		                       kSnowGateUnknown;
	}
	shellGateBareFrames = shellSnowVerdict == kSnowGateBare ? std::min(shellGateBareFrames + 1, kShellGateBareFrames) : 0;
	// S1: the early prepass already put this frame's ground snow into the main
	// depth, so the shell must shade it even on the frame the gate closes.
	const bool early = shellEarlyFrame == globals::state->frameCount;
	shellDrawGroundGated = shellGateBareFrames < kShellGateBareFrames && !GroundShellsSuspended();
	const bool shellGround = shellDrawGroundGated || early;
	// The raised sun cascades read the same object fields as the ground snow.
	const bool landscapeReads = settings.SnowCastsShadows && !GroundShellsSuspended();
	// Off the ground, the captures feed only their own skins (and the volume).
	if (!shellGround && !landscapeReads && !settings.VolumeSnow)
		CullUnseenCaptures(cbData);
	else {
		captureViewCulled = 0;
		captureCullBail = 1;
	}

	// Rasterize this frame's captured statics top-down into the object
	// height windows, then restore the viewport for the screen-space passes.
	// Waits for the prime's object group: EnsureStaticsShaders would
	// otherwise compile its ten variants synchronously right here.
	// Every reader of these fields (shell, skins, trench patch, caster,
	// volume) is off in a bare square with nothing captured.
	spanClock.Lap(CpuCensus::kSpanShellSetup);
	const bool nothingDraws = !shellGround && !landscapeReads && capturedStatics.empty() && !settings.VolumeSnow;
	if (nothingDraws)
		SkipObjectHeightMap();
	else if (!SnowShadersPending(2) && EnsureStaticsShaders())
		RenderObjectHeightMap();
	spanClock.Lap(CpuCensus::kSpanObjectHeightMap);
	RenderWaterCapture();
	spanClock.Lap(CpuCensus::kSpanWaterCapture);
	CoverWaterWindow();
	spanClock.Lap(CpuCensus::kSpanWaterCover);
	if (cbData.CompactLook.x > 0.5f)
		UpdateTerrainWet();
	shellDrawTerrainVersion = shellTerrainVersion;
	RenderBloodCapture();
	spanClock.Lap(CpuCensus::kSpanBloodCapture);
	RenderRuneCapture();
	spanClock.Lap(CpuCensus::kSpanRuneCapture);
	if (prevViewportCount)
		context->RSSetViewports(prevViewportCount, prevViewports);
	// The main pass's depth range, not the decal viewport the deferred span
	// ended on: through the decal cap every shell sat a constant ~3e-5 NDC
	// nearer than its object - a third of a unit on a rock, two thousand on
	// a mountain at 450 m, in front of the mist meant to wrap it (RenderDoc
	// pixel history against re-rasterised triangles, 2026-09-04).
	const bool mainRange = prevViewportCount && mainViewportFrame == globals::state->frameCount && !shellMainViewportRangeDisabled;
	if (mainRange) {
		D3D11_VIEWPORT shellViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		for (UINT i = 0; i < prevViewportCount; i++) {
			shellViewports[i] = prevViewports[i];
			shellViewports[i].MinDepth = mainViewportMinDepth;
			shellViewports[i].MaxDepth = mainViewportMaxDepth;
		}
		context->RSSetViewports(prevViewportCount, shellViewports);
	}
	// Next frame's early prepass draws with this viewport.
	shellDrawViewportFrame = UINT32_MAX;
	if (prevViewportCount) {
		D3D11_VIEWPORT used = prevViewports[0];
		if (mainRange) {
			used.MinDepth = mainViewportMinDepth;
			used.MaxDepth = mainViewportMaxDepth;
		}
		shellDrawViewport = used;
		shellDrawViewportFrame = globals::state->frameCount;
		shellDrawDynRes[0] = dynRes.x;
		shellDrawDynRes[1] = dynRes.y;
	}

	// The height pass recentered its window after the shell CB was filled;
	// sampling the freshly scrolled maps with last frame's center makes the
	// whole field trail the camera by one frame of movement. Re-upload with
	// the current center. (Any CPU value consumed by both a constant buffer
	// and a same-frame-scrolled texture must be uploaded after the scroll.)
	cbData.ObjectHeightCenter = heightWindowCenter;
	cbData.ExclusionFieldWindow = { exclusionFieldCenter.x, exclusionFieldCenter.y,
		1.0f / kExclusionFieldHalfExtent,
		(exclusionFieldValid && !shellDistantExclusionsDisabled) ? 1.0f : 0.0f };
	cbData.ObjectMeetBand = std::clamp(settings.ObjectMeetBand, 0.0f, 64.0f);
	if (prevViewportCount)
		cbData.TessScreen = { ShellTessTargetRenderPx(), prevViewports[0].Width * 0.5f, prevViewports[0].Height * 0.5f, shellDrawViewport.MaxDepth - shellDrawViewport.MinDepth };
	// GetDimensions of the field maps every landscape-shell pass binds from here
	// on; they change size only in Prepass, so they hold for the next frame's
	// caster too.
	const auto mapDim = [](const Texture2D* a_tex) { return (a_tex && a_tex->srv) ? float(a_tex->desc.Width) : 0.0f; };
	cbData.FieldDims = { mapDim(deformationTextures[0]), mapDim(bermFieldTexture), mapDim(heightTopFiltered), mapDim(exclusionFieldTexture) };
	shellCB->Update(cbData);

	// Snapshot for next frame's shadow-caster injection (it runs at the
	// shadow-mask pass, before DrawShell recomputes these values).
	if (!lastShellCBData)
		lastShellCBData = std::make_unique<ShellCB>();
	*lastShellCBData = cbData;

	// The sun marches read only the baked heights: without them this frame,
	// no march.
	UnbindMarchCaches();
	if ((cbData.ShellFlags.x & 1) != 0 && !UpdateMarchCaches(cbData)) {
		cbData.ShellFlags.x &= ~1;
		shellCB->Update(cbData);
		*lastShellCBData = cbData;
	}

	// Pre-shell copy of MASKS: Masks.y carries the land's EM grain height
	// (Lighting.hlsl LANDSCAPE; 0 = no data), and the land under the shell
	// is only readable before the shell overwrites the G-buffer. Unbind
	// around the copy; our own targets are set immediately after.
	{
		auto& masksRT = renderer->GetRuntimeData().renderTargets[MASKS];
		// Read by the shell PS (shellGround) and the skins (any capture) only.
		if (masksRT.SRV && masksRT.texture && (screenRectsOff || !nothingDraws)) {
			context->OMSetRenderTargets(0, nullptr, nullptr);
			globals::profiler->BeginPass("SnowDeformation::Ground - Masks Copy");
			D3D11_TEXTURE2D_DESC masksDesc{};
			masksRT.texture->GetDesc(&masksDesc);
			const D3D11_BOX masksBox = ScreenRectBox(prevViewportCount ? &prevViewports[0] : nullptr, masksDesc.Width, masksDesc.Height, 1);
			CopySRVResource(masksRT.SRV, "SnowDeformation::LandMasksCopy", landMasksCopyTex, landMasksCopySRV, &masksBox);
			// The game's blood decals wrote Masks.y = 0 under themselves, which
			// the coat reads as no paint: over their rectangle the copy takes
			// the masks from before the first decal drew (Blood.cpp).
			if (landMasksCopyTex && bloodPreDecalValid && bloodPreDecalMasksTex && DecalOverlayWanted()) {
				const RECT& rc = bloodCopyRect;
				const D3D11_BOX box{ UINT(rc.left), UINT(rc.top), 0, UINT(rc.right), UINT(rc.bottom), 1 };
				context->CopySubresourceRegion(landMasksCopyTex.get(), 0, box.left, box.top, 0, bloodPreDecalMasksTex.get(), 0, &box);
			}
			globals::profiler->EndPass();
		} else {
			globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Masks Copy");
		}
		// The G-buffer before any snow, over the blood decals' rectangle: the
		// overlay puts the decals' own contribution back over thin snow
		// (Blood.cpp). Only while blood decals are in view.
		bloodPreSnowValid = false;
		if (!bloodOverlays.empty() && DecalOverlayWanted() && bloodPreDecalValid)
			CopyBloodTargets(false);
		// Pre-shell normals for the S4 shell's per-pixel footprint cut
		// (per-pixel nz, normal maps included). Gated on the 3D toggle so
		// the off-cost is zero; the null SRV keeps HasSkinNormalCopy off.
		// The pre-shell normals copy is retired: it only existed above depth
		// 0.05, so the S4 footprint changed with the depth slider, and it
		// held the projected snow's own normal on painted pixels. The skin
		// reads the recolor's real weight from LandMasksCopy instead.
		preSkinNormalsCopySRV = nullptr;
	}

	// The ground snow: the landscape replay's prepass (S1 before the opaque
	// draws, or here when S1 did not run), the resolve and the deferred pass.
	const auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	// The hidden-fragment exit reads Terrain Blending's blended depth as the main depth.
	const bool keepHidden = shellPrepassExitOff || !globals::features::terrainBlending.loaded || !globals::features::terrainBlending.settings.Enabled;
	auto* fillVS = shellGround ? GetShellFillVS() : nullptr;
	auto* fillPS = fillVS ? GetShellFillPS() : nullptr;
	auto* psPrepass = (fillPS && !early) ? GetShellPSPrepassAttr(keepHidden, parallax) : nullptr;
	const bool ground = fillPS && (early || (psPrepass && LandscapeShadeReady())) && GetShellPSDeferred(parallax) &&
	                    EnsurePrepassResources(mainDepth.depthSRV) && EnsureDeferredResources() && (!parallax || EnsureParallaxOffset());

	// Main depth before any snow draw: DepthSync syncs the pixels the snow
	// passes (shell, skins, patch) move nearer than this copy. Targets are
	// unbound around the copy like the masks copy above. Not for LOD-batch
	// coats alone: they lie on their mesh, past anything Terrain Blending
	// softened. With the prepass's raster marks DepthSync reads it only off
	// the shell's pixels, where the main depth keeps the pre-snow value until
	// a statics pass writes: then no copy is taken without one, and with one
	// DrawCapturedStatics takes it (in the skins' first fill when it can).
	shellPrepassThisFrame = false;
	preSnowDepthThisFrame = false;
	preSnowDepthDeferred = false;
	if (shellGround || capturedLoaded > 0 || (settings.VolumeSnow && !capturedStatics.empty())) {
		auto preDepthSRV = mainDepth.depthSRV;
		if (preDepthSRV && GetPreSnowCopyCS() && depthSyncRectCB && EnsurePrepassResources(preDepthSRV) && preSnowDepth && preSnowDepthSRV && preSnowDepthUAV) {
			D3D11_TEXTURE2D_DESC preDesc{};
			preSnowDepth->GetDesc(&preDesc);
			preSnowRect = ScreenRectBox(prevViewportCount ? &prevViewports[0] : nullptr, preDesc.Width, preDesc.Height, 0);
			// Skins, patch and volume all draw past DrawCapturedStatics's
			// empty-capture return.
			if (ground && shellRasterDepth && shellRasterDepth->srv)
				preSnowDepthDeferred = !capturedStatics.empty();
			else
				TakePreSnowCopy(context, preDepthSRV);
		}
	}
	if (!preSnowDepthThisFrame && !preSnowDepthDeferred)
		globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Depth Copy");

	// Bind the deferred G-buffer exactly as StartDeferred configures it,
	// plus the main depth buffer for correct intersection with the world.
	auto& rtData = renderer->GetRuntimeData();
	ID3D11RenderTargetView* rtvs[8] = {
		rtData.renderTargets[RE::RENDER_TARGETS::kMAIN].RTV,
		rtData.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].RTV,
		rtData.renderTargets[NORMALROUGHNESS].RTV,
		rtData.renderTargets[ALBEDO].RTV,
		rtData.renderTargets[SPECULAR].RTV,
		rtData.renderTargets[REFLECTANCE].RTV,
		rtData.renderTargets[MASKS].RTV,
		rtData.renderTargets[MASKS2].RTV
	};
	auto dsv = mainDepth.views[0];
	context->OMSetRenderTargets(8, rtvs, dsv);

	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->RSSetState(shellRasterState.get());
	context->OMSetDepthStencilState(shellDepthState.get(), 0);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	ID3D11Buffer* cbs[1] = { shellCB->CB() };
	context->VSSetConstantBuffers(0, 1, cbs);
	context->PSSetConstantBuffers(0, 1, cbs);
	// SharedData (b5) supplies SH ambient + sun for the PS lighting; rebind
	// the b4-b6 triple exactly as Deferred does for its own passes.
	auto state = globals::state;
	ID3D11Buffer* sharedBuffers[3] = { state->permutationCB->CB(), state->sharedDataCB->CB(), state->featureDataCB->CB() };
	context->PSSetConstantBuffers(4, 3, sharedBuffers);
	// The PS evaluates the terrain/deformation fields for per-pixel coverage
	// and normals, so the field textures must be bound to both stages; the
	// snow maps and scene depth are PS-only. The depth SRV is a copy (Terrain
	// Blending's blended depth when available), never the bound DSV, so
	// sampling it here is legal; the PS fades the shell where it hovers close
	// in front of any geometry so it dissolves into statics (walkways, mesh
	// roads, rocks).
	ID3D11ShaderResourceView* shellSRVs[9] = { shellTerrainTexture->srv.get(), GetDeformationSRV(), shellSnowDiffuseSRV.get(), Util::GetCurrentSceneDepthSRV(false), heightTopFiltered->srv.get(), heightBottomFiltered->srv.get(), shellSnowNormalSRV.get(), shellSnowRmaosSRV.get(), shellSnowHeightSRV.get() };
	context->VSSetShaderResources(0, 6, shellSRVs);
	ID3D11ShaderResourceView* wetSRV = TerrainWetSRV(cbData.CompactLook.x > 0.5f);
	context->VSSetShaderResources(kTerrainWetSlot, 1, &wetSRV);
	ID3D11ShaderResourceView* fineSRV = (shellFineValid && shellTerrainFine) ? shellTerrainFine->srv.get() : nullptr;
	context->VSSetShaderResources(13, 1, &fineSRV);
	ID3D11ShaderResourceView* waterSRV = waterVisibleTexture && waterVisibleTexture->srv ? waterVisibleTexture->srv.get() : (waterHeightTexture ? waterHeightTexture->srv.get() : nullptr);
	ID3D11ShaderResourceView* waterSRVs[2] = { waterSRV, waterHeightTexture ? waterHeightTexture->srv.get() : nullptr };
	ID3D11ShaderResourceView* waterNearSRV = waterNearTexture && waterNearTexture->srv ? waterNearTexture->srv.get() : nullptr;
	context->VSSetShaderResources(27, 2, waterSRVs);
	context->VSSetShaderResources(19, 1, &waterNearSRV);
	// The game's texture tracker rebinds a slot only when it believes the
	// binding changed. t3 is the effect shaders' soft-particle depth, set
	// once per frame, so the null the skin pass left there faded every mist
	// and cloud to nothing wherever a soft effect drew (Josef's peaks,
	// 2026-09-04). Save the slots this pass overwrites and put them back.
	winrt::com_ptr<ID3D11ShaderResourceView> prevShellSRVs[9];
	for (uint i = 0; i < 9; i++)
		context->PSGetShaderResources(i, 1, prevShellSRVs[i].put());
	// The frost pair takes t16/t17; t17 is CS's shared scene depth, bound once
	// a frame and read by the forward passes. Restored at the end of DrawShell.
	winrt::com_ptr<ID3D11ShaderResourceView> prevFrostSlotSRVs[2];
	for (uint i = 0; i < 2; i++)
		context->PSGetShaderResources(16 + i, 1, prevFrostSlotSRVs[i].put());
	context->PSSetShaderResources(0, 9, shellSRVs);
	context->PSSetShaderResources(kTerrainWetSlot, 1, &wetSRV);
	ID3D11ShaderResourceView* bareSRV = shellTerrainBare ? shellTerrainBare->srv.get() : nullptr;
	context->PSSetShaderResources(kTerrainBareSlot, 1, &bareSRV);
	context->PSSetShaderResources(13, 1, &fineSRV);
	context->PSSetShaderResources(27, 2, waterSRVs);
	context->PSSetShaderResources(19, 1, &waterNearSRV);
	// Raw object tops + skin-depth raster (t11/t12, shared with the trench
	// patch): the object-depth cap on the shell's layer.
	ID3D11ShaderResourceView* objectCapSRVs[2] = { heightTopRaw[heightCurrent]->srv.get(), heightSkinDepth->srv.get() };
	context->VSSetShaderResources(11, 2, objectCapSRVs);
	context->PSSetShaderResources(11, 2, objectCapSRVs);
	// Land grain height copy (t10) for the two-sided edge contest.
	if (landMasksCopySRV) {
		ID3D11ShaderResourceView* landMasksSRV = landMasksCopySRV.get();
		context->PSSetShaderResources(10, 1, &landMasksSRV);
	}
	// Baked berm field (t14). The berm is read by the surface evaluation
	// (VS/DS) and by the normal block (PS), so every stage that can run
	// ShellSurfaceZ or shade needs it.
	ID3D11ShaderResourceView* bermSRV = GetBermFieldSRV();
	context->VSSetShaderResources(14, 1, &bermSRV);
	context->PSSetShaderResources(14, 1, &bermSRV);
	ID3D11ShaderResourceView* untouchedSRV = untouchedTilesSRV.get();
	context->PSSetShaderResources(kUntouchedTilesSlot, 1, &untouchedSRV);
	ID3D11ShaderResourceView* bloodSRVs[2] = { GetBloodMapSRV(), GetBloodClockSRV() };
	context->PSSetShaderResources(30, 2, bloodSRVs);
	// Blood detail tiles (t66-t69): cell index, pigment atlas, (t68 retired with the soak), clock blocks.
	ID3D11ShaderResourceView* bloodTileSRVs[4] = { bloodTileIndexSRV.get(), bloodTileAtlasSRV.get(), nullptr, bloodTileClockSRV.get() };
	context->PSSetShaderResources(66, 4, bloodTileSRVs);
	// Rune glyph atlas (t90) and its tile table (b7). Unbound reads as no tiles.
	ID3D11ShaderResourceView* runeSRV = GetRuneAtlasSRV();
	context->PSSetShaderResources(90, 1, &runeSRV);
	ID3D11Buffer* runeBuffer = runeCB ? runeCB->CB() : nullptr;
	context->PSSetConstantBuffers(7, 1, &runeBuffer);
	// Each tile's own diffuse (t80-t83) and normal map (t84-t87).
	ID3D11ShaderResourceView* runeTextures[2 * kRuneMaxTiles];
	for (uint32_t i = 0; i < kRuneMaxTiles; ++i) {
		runeTextures[i] = runeTileDiffuse[i].get();
		runeTextures[kRuneMaxTiles + i] = runeTileNormal[i].get();
	}
	context->PSSetShaderResources(80, 2 * kRuneMaxTiles, runeTextures);
	ID3D11ShaderResourceView* undulationSRV = GetUndulationFieldSRV();
	context->VSSetShaderResources(29, 1, &undulationSRV);
	context->PSSetShaderResources(29, 1, &undulationSRV);
	// The teeth noise table and the churn tile, for this pass and the trench patch's.
	ID3D11ShaderResourceView* noiseSRV = NoiseHashSRV();
	context->PSSetShaderResources(kNoiseHashSlot, 1, &noiseSRV);
	ID3D11ShaderResourceView* churnSRV = ChurnTileSRV();
	context->PSSetShaderResources(kChurnTileSlot, 1, &churnSRV);

	EnsureFrostPatternTextures();
	ID3D11ShaderResourceView* frostSRVs[] = { frostPatternNormalSRV.get(), frostPatternDiffuseSRV.get() };
	context->PSSetShaderResources(16, ARRAYSIZE(frostSRVs), frostSRVs);
	// Wide exclusion field (t15): read by the surface evaluation and by the
	// pixel-side melt overrides, so the same stages as the berm.
	ID3D11ShaderResourceView* exclusionSRV = GetExclusionFieldSRV();
	context->VSSetShaderResources(15, 1, &exclusionSRV);
	context->PSSetShaderResources(15, 1, &exclusionSRV);
	// Glint noise (t20): TruePBR binds this each prepass, but slot 20's state
	// at deferred time is not guaranteed; bind explicitly for this pass.
	if (globals::features::truePBR.glintsNoiseTexture) {
		ID3D11ShaderResourceView* glintSRV = globals::features::truePBR.glintsNoiseTexture->srv.get();
		context->PSSetShaderResources(20, 1, &glintSRV);
	}
	// IBL SH textures (t76/t77, the include's forward slots): same not-
	// guaranteed-at-deferred-time reasoning as the glint noise above.
	if (globals::features::ibl.loaded && globals::features::ibl.envIBLTexture && globals::features::ibl.skyIBLTexture) {
		ID3D11ShaderResourceView* iblSRVs[2] = { globals::features::ibl.envIBLTexture->srv.get(), globals::features::ibl.skyIBLTexture->srv.get() };
		context->PSSetShaderResources(76, 2, iblSRVs);
	}
	// Terrain Shadows height map (t60) + Cloud Shadows cube copy (t25):
	// GetWorldShadow reads both, and the Prepass-time binds do not survive
	// to this pass (the t102 lesson). Unbound t60 reads zeros, which the
	// helper's ramp maps to shadow 1.0 — the far shell stood bright while
	// the LOD terrain past the seam wore the mountain shadow. The statics
	// skin inherits these like t22.
	if (globals::features::terrainShadows.loaded && globals::features::terrainShadows.texShadowHeight) {
		ID3D11ShaderResourceView* terrainShadowSRV = globals::features::terrainShadows.texShadowHeight->srv.get();
		context->PSSetShaderResources(60, 1, &terrainShadowSRV);
	}
	if (globals::features::cloudShadows.loaded && globals::features::cloudShadows.texCubemapCloudOccCopy) {
		ID3D11ShaderResourceView* cloudShadowSRV = globals::features::cloudShadows.texCubemapCloudOccCopy->srv.get();
		context->PSSetShaderResources(25, 1, &cloudShadowSRV);
	}
	// One-shot audit: settles whether the t60 bind fires at all (feature off,
	// texture absent) before reaching for a capture.
	{
		static std::atomic<bool> loggedShadowBind{ false };
		if (!loggedShadowBind.exchange(true))
			logger::info("[SNOW DEFORMATION] shell far-shadow bind: terrainShadows loaded={} tex={} enabled={} | cloudShadows loaded={} tex={}",
				globals::features::terrainShadows.loaded,
				globals::features::terrainShadows.texShadowHeight != nullptr,
				globals::features::terrainShadows.settings.EnableTerrainShadow,
				globals::features::cloudShadows.loaded,
				globals::features::cloudShadows.texCubemapCloudOccCopy != nullptr);
	}
	// Comparison sampler (s2), shared by the crisp cascade path and the
	// point-light shadow path.
	if (cbData.CrispShadows > 0.5f || PointShadowAtlasSRV()) {
		if (!shadowCmpSampler) {
			D3D11_SAMPLER_DESC cmpDesc{};
			cmpDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
			cmpDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			cmpDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			cmpDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			cmpDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
			cmpDesc.MaxLOD = D3D11_FLOAT32_MAX;
			globals::d3d::device->CreateSamplerState(&cmpDesc, shadowCmpSampler.put());
			Util::SetResourceName(shadowCmpSampler.get(), "SnowDeformation::ShadowCmpSampler");
		}
		ID3D11SamplerState* cmpSampler = shadowCmpSampler.get();
		context->PSSetSamplers(2, 1, &cmpSampler);
	}
	// Raw shadow-atlas copy (t22) for crisp cascade shadows; the statics
	// skin inherits it too.
	if (cbData.CrispShadows > 0.5f) {
		ID3D11ShaderResourceView* shadowAtlasSRV = ShadowAtlasSRV();
		context->PSSetShaderResources(22, 1, &shadowAtlasSRV);
	}
	// Screen-Space Shadows output (t45) for the shell + statics passes.
	if (cbData.ScreenSpaceShadowsActive > 0.5f) {
		ID3D11ShaderResourceView* sssSRV = screenSpaceShadowsFeature.screenSpaceShadowsTexture->srv.get();
		context->PSSetShaderResources(45, 1, &sssSRV);
	}
	// LLF cluster buffers (t35-t37) + the point-shadow light table (t38) and
	// the local shadow maps (t39). LLF binds t35-37 each Prepass, but slot
	// state at deferred time is not guaranteed; rebind explicitly like t20.
	// Do NOT null t35-37 in teardown: later forward passes (water, effects)
	// read LLF's own binding, which this rebind matches exactly.
	if (cbData.PointLightsActive > 0.5f) {
		ID3D11ShaderResourceView* lightSRVs[3] = { lightLimitFix.lights->srv.get(), lightLimitFix.lightIndexList->srv.get(), lightLimitFix.lightGrid->srv.get() };
		context->PSSetShaderResources(35, 3, lightSRVs);
		UpdatePointShadowLights();
		if (pointShadowLights) {
			ID3D11ShaderResourceView* pointShadowSRVs[2] = { pointShadowLights->srv.get(), PointShadowAtlasSRV() };
			context->PSSetShaderResources(38, 2, pointShadowSRVs);
		}
	}
	// Skylighting probe volume (t50). Skylighting's own SRV; no teardown
	// needed for the same reason as t35-37.
	if (cbData.SkylightingActive > 0.5f) {
		ID3D11ShaderResourceView* probeSRV = skylighting.texProbeArray->srv.get();
		context->PSSetShaderResources(50, 1, &probeSRV);
	}

	winrt::com_ptr<ID3D11SamplerState> prevSamplers[2];
	context->PSGetSamplers(0, 1, prevSamplers[0].put());
	context->PSGetSamplers(1, 1, prevSamplers[1].put());
	ID3D11SamplerState* shellSamplers[2] = { shellSnowSampler.get(), shellLinearSampler.get() };
	context->PSSetSamplers(0, 2, shellSamplers);

	if (early && (std::memcmp(&shellDrawViewport, &shellEarlyViewport, sizeof(D3D11_VIEWPORT)) != 0 ||
					 std::memcmp(&cbData.CameraViewProj, &shellEarlyCB->CameraViewProj, sizeof(cbData.CameraViewProj)) != 0))
		shellEarlyViewMismatches++;
	// The stats ring pairs the shell and statics queries per slot, so a
	// gated frame issues neither.
	const bool shellStats = ground && shellPipelineStatsEnabled && EnsureShellStatsQueries();
	if (shellStats) {
		ReadShellStatsQueries(context);
		context->Begin(shellStatsQuery[shellStatsRing][0].get());
		context->Begin(shellStatsQuery[shellStatsRing][1].get());
	}
	const bool earlyMarks = ground && early && shellPSEarlyResolve && shellEarlyPending && shellEarlyMarks;
	shellPrepassThisFrame = ground;
	shellDeferredThisFrame = ground;
	if (!ground) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Shell");
	} else {
		globals::profiler->BeginPass(shellSplitTiming ? "SnowDeformation::Ground - Shell Setup" : "SnowDeformation::Ground - Shell");
		auto splitRow = [&](const char* a_name) {
			if (shellSplitTiming) {
				globals::profiler->EndPass();
				globals::profiler->BeginPass(a_name);
			}
		};
		// The fields the captured statics' hull and domain stages inherit.
		context->HSSetConstantBuffers(0, 1, cbs);
		context->DSSetConstantBuffers(0, 1, cbs);
		context->DSSetConstantBuffers(4, 3, sharedBuffers);
		context->HSSetShaderResources(0, 6, shellSRVs);
		context->DSSetShaderResources(0, 6, shellSRVs);
		context->HSSetShaderResources(kTerrainWetSlot, 1, &wetSRV);
		context->DSSetShaderResources(kTerrainWetSlot, 1, &wetSRV);
		context->HSSetShaderResources(13, 1, &fineSRV);
		context->DSSetShaderResources(13, 1, &fineSRV);
		context->HSSetShaderResources(27, 2, waterSRVs);
		context->DSSetShaderResources(27, 2, waterSRVs);
		context->HSSetShaderResources(19, 1, &waterNearSRV);
		context->DSSetShaderResources(19, 1, &waterNearSRV);
		ID3D11ShaderResourceView* dsHeightSRV = shellSnowHeightSRV.get();
		context->DSSetShaderResources(8, 1, &dsHeightSRV);
		context->DSSetShaderResources(11, 2, objectCapSRVs);
		context->DSSetShaderResources(14, 1, &bermSRV);
		context->DSSetShaderResources(15, 1, &exclusionSRV);
		context->DSSetShaderResources(29, 1, &undulationSRV);
		ID3D11SamplerState* dsSampler = shellSnowSampler.get();
		context->DSSetSamplers(0, 1, &dsSampler);

		if (!earlyMarks) {
			// The replay's prepass: raster depth, the deferred attributes and the
			// clamped depth into the main buffer. The fill turns the raster depth
			// into the private depth the deferred pass tests.
			splitRow("SnowDeformation::Ground - Shell Depth Prepass");
			const float rasterClear[4] = {};
			context->ClearRenderTargetView(shellRasterDepth->rtv.get(), rasterClear);
			ID3D11RenderTargetView* prepassRTVs[5] = { shellRasterDepth->rtv.get(), shellDeferredAttr[0]->rtv.get(), shellDeferredAttr[1]->rtv.get(), shellDeferredAttr[2]->rtv.get(),
				parallax ? shellParallaxOffset->rtv.get() : nullptr };
			context->OMSetRenderTargets(parallax ? 5 : 4, prepassRTVs, dsv);
			context->PSSetShader(psPrepass, nullptr, 0);
			const auto& terrainBlend = globals::features::terrainBlending;
			ID3D11ShaderResourceView* tbMaskSRV = (terrainBlend.loaded && terrainBlend.settings.Enabled) ? terrainBlend.terrainDepth.depthSRV : nullptr;
			context->PSSetShaderResources(kTBMaskSlot, 1, &tbMaskSRV);
			DrawLandscapeSnowPrepass(context, cbData, cbs[0], parallax);
			tbMaskSRV = nullptr;
			context->PSSetShaderResources(kTBMaskSlot, 1, &tbMaskSRV);
			context->OMSetRenderTargets(0, nullptr, shellTestDepthDSV.get());
			context->OMSetDepthStencilState(shellFillDepthState.get(), 0);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->VSSetShader(fillVS, nullptr, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			ID3D11ShaderResourceView* rasterSRV = shellRasterDepth->srv.get();
			context->PSSetShaderResources(9, 1, &rasterSRV);
			context->PSSetShader(fillPS, nullptr, 0);
			context->Draw(3, 0);
			ID3D11ShaderResourceView* nullRasterSRV = nullptr;
			context->PSSetShaderResources(9, 1, &nullRasterSRV);
		}

		splitRow("SnowDeformation::Ground - Shell Shading");
		if (earlyMarks) {
			// The pending pixels against this frame's land masks; every winner's
			// depth into the main buffer under LESS_EQUAL, so one something drew
			// in front of since the claim loses. The marks fill the private depth.
			const float marksClear[4] = {};
			context->ClearRenderTargetView(shellEarlyMarks->rtv.get(), marksClear);
			ID3D11RenderTargetView* marksRTV = shellEarlyMarks->rtv.get();
			context->OMSetRenderTargets(1, &marksRTV, dsv);
			context->OMSetDepthStencilState(shellDepthState.get(), 0);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->VSSetShader(fillVS, nullptr, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			ID3D11ShaderResourceView* landSRV = landMasksCopySRV.get();
			context->PSSetShaderResources(10, 1, &landSRV);
			ID3D11ShaderResourceView* resolveSRVs[2] = { shellRasterDepth->srv.get(), shellEarlyPending->srv.get() };
			context->PSSetShaderResources(61, 2, resolveSRVs);
			ID3D11ShaderResourceView* attrDepthSRV = shellDeferredAttr[2]->srv.get();
			context->PSSetShaderResources(93, 1, &attrDepthSRV);
			context->PSSetShader(shellPSEarlyResolve, nullptr, 0);
			context->Draw(3, 0);
			ID3D11ShaderResourceView* nullResolveSRVs[2] = {};
			context->PSSetShaderResources(61, 2, nullResolveSRVs);
			context->PSSetShaderResources(93, 1, nullResolveSRVs);
			context->OMSetRenderTargets(0, nullptr, shellTestDepthDSV.get());
			context->OMSetDepthStencilState(shellFillDepthState.get(), 0);
			ID3D11ShaderResourceView* marksSRV = shellEarlyMarks->srv.get();
			context->PSSetShaderResources(9, 1, &marksSRV);
			context->PSSetShader(fillPS, nullptr, 0);
			context->Draw(3, 0);
			context->PSSetShaderResources(9, 1, nullResolveSRVs);
		}
		// One full-screen triangle at z 0: LESS passes wherever the fill left a
		// prepass winner's depth, rejects the cleared 0.
		context->OMSetRenderTargets(8, rtvs, shellTestDepthDSV.get());
		context->OMSetDepthStencilState(shellDeferredDepthState.get(), 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(fillVS, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		ID3D11ShaderResourceView* attrSRVs[3] = { shellDeferredAttr[0]->srv.get(), shellDeferredAttr[1]->srv.get(), shellDeferredAttr[2]->srv.get() };
		context->PSSetShaderResources(91, 3, attrSRVs);
		ID3D11ShaderResourceView* debugParallaxSRV = cbData.DebugParallaxView > 0.5f ? shellParallaxOffset->srv.get() : nullptr;
		context->PSSetShaderResources(99, 1, &debugParallaxSRV);
		ID3D11PixelShader* conditionView = parallax ? nullptr : ConditionViewPS();
		ID3D11PixelShader* deferredPS = shellPSDeferred[parallax ? 1 : 0];
		if (!parallax && shellChurnFree) {
			if (auto* churnFreePS = GetShellPSDeferred(false, true))
				deferredPS = churnFreePS;
		}
		context->PSSetShader(conditionView ? conditionView : deferredPS, nullptr, 0);
		context->Draw(3, 0);
		ID3D11ShaderResourceView* nullAttrSRVs[3] = {};
		context->PSSetShaderResources(91, 3, nullAttrSRVs);
		context->PSSetShaderResources(99, 1, nullAttrSRVs);
		context->OMSetRenderTargets(8, rtvs, dsv);
		context->OMSetDepthStencilState(shellDepthState.get(), 0);
		globals::profiler->EndPass();
	}
	if (shellStats) {
		context->End(shellStatsQuery[shellStatsRing][1].get());
		context->End(shellStatsQuery[shellStatsRing][0].get());
	}

	// The post-shell depth copy that used to run here went with the seam
	// cross-fade it served. Restoring the pair-3 contest needs it back first;
	// see HEIGHT-BLEND-PLAN.md.

	// Captured projected-snow statics, inflated with the same material.
	// Inherits this pass's bindings (b0, t0-t8, s0, b4-b6, RTs, depth).
	// Their own raster state carries the skin depth bias: a skin and its
	// mesh rasterise the same vertices, so without a bias the far ones
	// z-fight.
	spanClock.Lap(CpuCensus::kSpanGround);
	if (auto* skinRaster = GetSkinRasterState())
		context->RSSetState(skinRaster);
	if (shellStats) {
		context->Begin(staticsStatsQuery[shellStatsRing][0].get());
		context->Begin(staticsStatsQuery[shellStatsRing][1].get());
	}
	const bool rasterMarks = ground && shellRasterDepth && shellRasterDepth->srv;
	shellGroundMarksSRV = rasterMarks ? (earlyMarks ? shellEarlyMarks->srv.get() : shellRasterDepth->srv.get()) : nullptr;
	patchStatsArmed = shellStats;
	DrawCapturedStatics();
	spanClock.Lap(CpuCensus::kSpanStatics);
	if (patchStatsArmed) {
		// No patch drew: an empty pair keeps this ring slot's read in step.
		context->Begin(patchStatsQuery[shellStatsRing][0].get());
		context->Begin(patchStatsQuery[shellStatsRing][1].get());
		context->End(patchStatsQuery[shellStatsRing][1].get());
		context->End(patchStatsQuery[shellStatsRing][0].get());
		patchStatsArmed = false;
	}
	if (shellStats) {
		context->End(staticsStatsQuery[shellStatsRing][1].get());
		context->End(staticsStatsQuery[shellStatsRing][0].get());
		shellStatsIssued[shellStatsRing] = true;
		shellStatsRing = (shellStatsRing + 1) % kShellStatsRing;
	}
	context->RSSetState(shellRasterState.get());

	// Restore everything we changed. DS/HS state is cleared unconditionally:
	// the statics pass binds its own DS resources even when the landscape
	// tessellation is unavailable, and the deformation map is UAV-written
	// next Prepass so it must not linger on a DS slot.
	{
		ID3D11ShaderResourceView* nullDSSRVs[24] = {};
		context->DSSetShaderResources(0, 24, nullDSSRVs);
		context->HSSetShaderResources(0, 24, nullDSSRVs);
		context->DSSetShaderResources(kTerrainWetSlot, 1, nullDSSRVs);
		context->HSSetShaderResources(kTerrainWetSlot, 1, nullDSSRVs);
		ID3D11Buffer* nullStageCB = nullptr;
		context->DSSetConstantBuffers(0, 1, &nullStageCB);
		context->HSSetConstantBuffers(0, 1, &nullStageCB);
		ID3D11Buffer* nullSharedCBs[3] = {};
		context->DSSetConstantBuffers(4, 3, nullSharedCBs);
		ID3D11SamplerState* nullDSSampler = nullptr;
		context->DSSetSamplers(0, 1, &nullDSSampler);
	}
	context->VSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(nullptr, nullptr, 0);
	ID3D11Buffer* nullCB = nullptr;
	context->VSSetConstantBuffers(0, 1, &nullCB);
	context->PSSetConstantBuffers(0, 1, &nullCB);
	ID3D11ShaderResourceView* nullSRVs[16] = {};
	context->VSSetShaderResources(0, 16, nullSRVs);
	ID3D11ShaderResourceView* nullFarSRVs[2] = {};
	context->VSSetShaderResources(24, 1, &nullFarSRVs[0]);
	context->VSSetShaderResources(26, 1, &nullFarSRVs[1]);
	context->PSSetShaderResources(0, 16, nullSRVs);
	context->VSSetShaderResources(kTerrainWetSlot, 1, nullSRVs);
	context->PSSetShaderResources(kTerrainWetSlot, 1, nullSRVs);
	// t22/t23 hold SRVs of the game's shadow depth targets; they must be
	// unbound before the next shadow render binds those targets as DSVs, or
	// D3D silently drops the binding with warning spam. t20 (glint noise) and
	// t21 are cleared alongside.
	ID3D11ShaderResourceView* nullShadowSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
	context->PSSetShaderResources(20, 4, nullShadowSRVs);
	ID3D11ShaderResourceView* restoreShellSRVs[9];
	for (uint i = 0; i < 9; i++)
		restoreShellSRVs[i] = prevShellSRVs[i].get();
	context->PSSetShaderResources(0, 9, restoreShellSRVs);
	ID3D11ShaderResourceView* nullFinePS[2] = {};
	context->PSSetShaderResources(90, 1, nullFinePS);
	ID3D11ShaderResourceView* nullRuneTextures[2 * kRuneMaxTiles] = {};
	context->PSSetShaderResources(80, 2 * kRuneMaxTiles, nullRuneTextures);
	context->PSSetShaderResources(13, 1, nullFinePS);
	context->PSSetShaderResources(27, 2, nullFinePS);
	context->PSSetShaderResources(19, 1, nullFinePS);
	context->PSSetShaderResources(24, 1, &nullFinePS[0]);
	// The untouched mask is a compute target next Prepass.
	context->PSSetShaderResources(kUntouchedTilesSlot, 1, &nullFinePS[0]);
	context->PSSetShaderResources(26, 1, &nullFinePS[1]);
	UnbindMarchCaches();
	ID3D11SamplerState* restoreSamplers[2] = { prevSamplers[0].get(), prevSamplers[1].get() };
	context->PSSetSamplers(0, 2, restoreSamplers);
	ID3D11SamplerState* nullCmpSampler = nullptr;
	context->PSSetSamplers(2, 1, &nullCmpSampler);
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->RSSetState(prevRaster.get());
	context->OMSetDepthStencilState(prevDepth.get(), prevStencilRef);
	context->OMSetBlendState(prevBlend.get(), prevBlendFactor, prevSampleMask);
	context->IASetPrimitiveTopology(prevTopology);
	context->IASetIndexBuffer(prevIB.get(), prevIBFormat, prevIBOffset);
	if (prevViewportCount)
		context->RSSetViewports(prevViewportCount, prevViewports);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
	spanClock.Lap(CpuCensus::kSpanShellRestore);

	// Screen-space passes running after us (SSGI, SSS, the shadow mask) read
	// Terrain Blending's blended depth, finalized during opaque rendering;
	// without a sync they see buried geometry poking through the snow and
	// paint occlusion, shadow and GI from under the sheet. min() the fresh
	// depth into both blended copies (DSV is unbound again at this point),
	// on the module's own pixels only: the shell prepass's mark, or a depth
	// the snow passes moved nearer than the pre-snow copy (skins, patch).
	auto& tb = globals::features::terrainBlending;
	const bool syncable = rasterMarks || preSnowDepthThisFrame;
	if (!syncable)
		globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Depth Sync");
	if (syncable && tb.loaded && tb.settings.Enabled && tb.blendedDepthTexture && tb.blendedDepthTexture16) {
		if (auto cs = GetDepthSyncCS(); cs && GetPreSnowCopyCS() && depthSyncRectCB) {
			auto mainDepthSRV = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
			ID3D11UnorderedAccessView* syncUAVs[2] = { tb.blendedDepthTexture->uav.get(), tb.blendedDepthTexture16->uav.get() };
			ID3D11ShaderResourceView* syncSRVs[3] = { mainDepthSRV,
				shellGroundMarksSRV,
				preSnowDepthThisFrame ? preSnowDepthSRV.get() : nullptr };
			// Outside the rendered rectangle nothing moved the main depth and the raster
			// marks are clear: the full-target pass returned there. The pre-snow copy
			// covers this same rectangle.
			const auto& depthDesc = tb.blendedDepthTexture->desc;
			const D3D11_BOX rect = ScreenRectBox(prevViewportCount ? &prevViewports[0] : nullptr, depthDesc.Width, depthDesc.Height, 0);
			DepthSyncRectCB rectData{ { rect.left, rect.top }, { rect.right - rect.left, rect.bottom - rect.top } };
			depthSyncRectCB->Update(rectData);
			ID3D11Buffer* rectCB = depthSyncRectCB->CB();
			context->CSSetConstantBuffers(2, 1, &rectCB);
			context->CSSetShaderResources(0, 3, syncSRVs);
			context->CSSetUnorderedAccessViews(0, 2, syncUAVs, nullptr);
			context->CSSetShader(cs, nullptr, 0);
			globals::profiler->BeginPass("SnowDeformation::Ground - Depth Sync");
			context->Dispatch((rectData.Extent[0] + 7) / 8, (rectData.Extent[1] + 7) / 8, 1);
			globals::profiler->EndPass();

			ID3D11ShaderResourceView* nullSyncSRVs[3] = { nullptr, nullptr, nullptr };
			ID3D11UnorderedAccessView* nullSyncUAVs[2] = { nullptr, nullptr };
			ID3D11Buffer* nullSyncCB = nullptr;
			context->CSSetShaderResources(0, 3, nullSyncSRVs);
			context->CSSetUnorderedAccessViews(0, 2, nullSyncUAVs, nullptr);
			context->CSSetConstantBuffers(2, 1, &nullSyncCB);
			context->CSSetShader(nullptr, nullptr, 0);
		}
	}
	// After Depth Sync: its UAV write unbinds the blended depth from PS t17.
	ID3D11ShaderResourceView* restoreFrostSlotSRVs[2] = { prevFrostSlotSRVs[0].get(), prevFrostSlotSRVs[1].get() };
	context->PSSetShaderResources(16, 2, restoreFrostSlotSRVs);
	spanClock.Lap(CpuCensus::kSpanShellTail);
}
