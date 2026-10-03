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
				FAILED(device->CreateQuery(&desc, staticsStatsQuery[ring][kind].put()))) {
				logger::error("SnowDeformation: pipeline statistics query creation failed");
				for (auto& r : shellStatsQuery)
					for (auto& q : r)
						q = nullptr;
				for (auto& r : staticsStatsQuery)
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
	if (read(shellStatsQuery[ring], shellStatsLast) && read(staticsStatsQuery[ring], staticsStatsLast))
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

bool SnowDeformation::EnsureShellGridIndexBuffers()
{
	if (shellGridIB[0] && shellGridIB[1])
		return true;

	constexpr uint N = kShellGridDim;
	constexpr uint stride = N + 1;
	// Same corner order the vertex shader expands per quad on the A/B path.
	static constexpr uint8_t kCorners[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	static constexpr uint8_t kCornersFlipped[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };

	// Absolute 32-bit lattice indices, one draw: BaseVertexLocation is NOT
	// folded into SV_VertexID when no vertex buffer is bound, so 16-bit row
	// bands with a base vertex draw the first band seven times over.
	// Warp mirror for the quad's world placement (SnowGrid.hlsli WarpAxis):
	// every band step is a multiple of 8 and the origin snaps by 256, so the
	// world 32-quad a grid quad falls in is fixed per grid index, and its
	// parity - the land's diagonal - can be baked here.
	auto warpAxis = [](int u) {
		float a = float(std::abs(u));
		float off = 0.0f;
		for (int band = 0; band < kShellWarpBands; ++band) {
			const float take = std::min(a, kShellWarpBandVerts[band]);
			off += take * kShellWarpBandMul[band];
			a -= take;
		}
		off += a * kShellWarpBandMul[kShellWarpBands - 1];
		return (u < 0 ? -1.0f : (u > 0 ? 1.0f : 0.0f)) * off * kShellGridSpacing;
	};
	const float halfSpan = ShellWarpedHalfSpan();
	auto landQuad = [&](uint i) { return static_cast<int>(std::floor((warpAxis(int(i) - int(N / 2)) + halfSpan + 0.5f) / 32.0f)); };

	// [0]: each quad split the way the land splits the 32-unit quad it lies
	// in - '/' (kCornersFlipped, h00-h11) where the world 32-quad index sum is
	// even. [1]: the old camera-phased union-jack, kept for the A/B.
	std::vector<uint32_t> indices(static_cast<size_t>(N) * N * 6);
	for (uint parity = 0; parity < 2; ++parity) {
		size_t w = 0;
		for (uint y = 0; y < N; ++y) {
			for (uint x = 0; x < N; ++x) {
				const bool flipped = parity == 0 ? (((landQuad(x) + landQuad(y)) & 1) == 0) : (((x ^ y) ^ 0u) & 1u) != 0;
				const auto& corners = flipped ? kCornersFlipped : kCorners;
				for (uint c = 0; c < 6; ++c)
					indices[w++] = (y + corners[c][1]) * stride + x + corners[c][0];
			}
		}
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		D3D11_SUBRESOURCE_DATA init{};
		init.pSysMem = indices.data();
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, &init, shellGridIB[parity].put()))) {
			logger::error("SnowDeformation: shell grid index buffer {} creation failed", parity);
			shellGridIB[0] = nullptr;
			shellGridIB[1] = nullptr;
			return false;
		}
		Util::SetResourceName(shellGridIB[parity].get(), parity ? "SnowDeformation::ShellGridIB1" : "SnowDeformation::ShellGridIB0");

		// Caster subset: castVis's distance factor alone kills every vertex past
		// 4062.9 u of the grid centre; the nearest lattice distances either side
		// are 4062.685 and 4063.15.
		constexpr float kCasterRadius = 4063.0f;
		std::vector<uint32_t> casterIndices;
		casterIndices.reserve(indices.size() / 2);
		for (uint y = 0; y < N; ++y) {
			for (uint x = 0; x < N; ++x) {
				bool inside = false;
				for (uint cy = y; cy <= y + 1 && !inside; ++cy) {
					for (uint cx = x; cx <= x + 1 && !inside; ++cx) {
						const float ox = warpAxis(int(cx) - int(N / 2));
						const float oy = warpAxis(int(cy) - int(N / 2));
						inside = ox * ox + oy * oy <= kCasterRadius * kCasterRadius;
					}
				}
				if (inside) {
					const size_t q = (size_t(y) * N + x) * 6;
					casterIndices.insert(casterIndices.end(), indices.begin() + q, indices.begin() + q + 6);
				}
			}
		}
		desc.ByteWidth = static_cast<UINT>(casterIndices.size() * sizeof(uint32_t));
		init.pSysMem = casterIndices.data();
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, &init, shellGridCasterIB[parity].put()))) {
			logger::error("SnowDeformation: shell caster index buffer {} creation failed", parity);
			shellGridCasterIB[0] = nullptr;
			shellGridCasterIB[1] = nullptr;
		} else {
			Util::SetResourceName(shellGridCasterIB[parity].get(), parity ? "SnowDeformation::ShellCasterIB1" : "SnowDeformation::ShellCasterIB0");
			shellGridCasterIndexCount = static_cast<uint32_t>(casterIndices.size());
		}
	}
	return true;
}

void SnowDeformation::DrawShellCasterGrid(ID3D11DeviceContext* a_context, const ShellCB& a_cb)
{
	if (shellCasterFullGrid || shellGridNonIndexed || !EnsureShellGridIndexBuffers() || !shellGridCasterIB[0] || !shellGridCasterIB[1]) {
		DrawShellGrid(a_context, a_cb);
		return;
	}
	a_context->IASetIndexBuffer(shellGridCasterIB[shellOldUnionJack ? 1 : 0].get(), DXGI_FORMAT_R32_UINT, 0);
	a_context->DrawIndexed(shellGridCasterIndexCount, 0, 0);
}

void SnowDeformation::DrawShellGridIndexed(ID3D11DeviceContext* a_context, [[maybe_unused]] const ShellCB& a_cb)
{
	// [0] is land-matched; [1] the old union-jack for the A/B.
	a_context->IASetIndexBuffer(shellGridIB[shellOldUnionJack ? 1 : 0].get(), DXGI_FORMAT_R32_UINT, 0);
	a_context->DrawIndexed(kShellGridDim * kShellGridDim * 6, 0, 0);
}

void SnowDeformation::DrawShellGrid(ID3D11DeviceContext* a_context, const ShellCB& a_cb)
{
	if (shellGridNonIndexed)
		a_context->Draw(kShellGridDim * kShellGridDim * 6, 0);
	else if (EnsureShellGridIndexBuffers())
		DrawShellGridIndexed(a_context, a_cb);
}

void SnowDeformation::ShellSeamRows(const ShellCB& a_cb, uint32_t& a_first, uint32_t& a_end)
{
	a_first = 0;
	a_end = kShellGridDim;
	if (a_cb.SeamRampInv <= 0.0f)
		return;
	// Vertex row j's world Y (GeomorphVertexXY is a no-op on the aligned bands);
	// patch row r spans vertex rows r and r + 1.
	const auto rowY = [&](int a_row) { return a_cb.GridOrigin.y + a_cb.WarpedHalfSpan + ShellWarpAxis(a_row - int(kShellGridDim / 2), a_cb.GridSpacing); };
	while (a_first < kShellGridDim && rowY(int(a_first) + 2) < a_cb.SeamBounds.y)
		a_first++;
	while (a_end > a_first && rowY(int(a_end) - 2) > a_cb.SeamBounds.w)
		a_end--;
}

SnowDeformation::ShellSlopeKey SnowDeformation::MakeShellSlopeKey(const ShellCB& a_cb) const
{
	// The leaves of BakeCS's slope output: SampleTerrain's .x (no water cut, no
	// depth scale) at four grid offsets, through the fine layer.
	const auto f = [](float a_v) { return std::bit_cast<uint32_t>(a_v); };
	return { f(a_cb.GridOrigin.x), f(a_cb.GridOrigin.y), f(a_cb.GridSpacing), f(a_cb.WarpedHalfSpan), a_cb.GridDim,
		f(a_cb.GridToTerrainOffset.x), f(a_cb.GridToTerrainOffset.y), f(a_cb.TerrainTexelSize), a_cb.TerrainDim,
		f(a_cb.ShellTriHeight), f(a_cb.FineWindow.x), f(a_cb.FineWindow.y), f(a_cb.FineWindow.z), f(a_cb.FineWindow.w),
		uint32_t(a_cb.ShellFlags.x), shellTerrainVersion, shellFineVersion };
}

ID3D11VertexShader* SnowDeformation::GetShellVS()
{
	if (shellGridNonIndexed) {
		if (!shellVSNonIndexed) {
			logger::debug("Compiling SnowShell VS (non-indexed A/B)");
			shellVSNonIndexed = static_cast<ID3D11VertexShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "VSHADER", "" }, { "SNOW_GRID_NONINDEXED", "" } }, "vs_5_0"));
		}
		return shellVSNonIndexed;
	}
	if (!shellVS) {
		logger::debug("Compiling SnowShell VS");
		shellVS = static_cast<ID3D11VertexShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "VSHADER", "" } }, "vs_5_0"));
	}
	return shellVS;
}

ID3D11VertexShader* SnowDeformation::GetShellShadowVS()
{
	if (shellGridNonIndexed) {
		if (!shellShadowVSNonIndexed) {
			logger::debug("Compiling SnowShell shadow-cast VS (non-indexed A/B)");
			shellShadowVSNonIndexed = static_cast<ID3D11VertexShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "VSHADER", "" }, { "SNOW_SHADOW_CAST", "" }, { "SNOW_GRID_NONINDEXED", "" } }, "vs_5_0"));
		}
		return shellShadowVSNonIndexed;
	}
	if (!shellShadowVS) {
		logger::debug("Compiling SnowShell shadow-cast VS");
		shellShadowVS = static_cast<ID3D11VertexShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "VSHADER", "" }, { "SNOW_SHADOW_CAST", "" } }, "vs_5_0"));
	}
	return shellShadowVS;
}

ID3D11VertexShader* SnowDeformation::GetShellShadowVSBaked(bool a_check)
{
	auto*& slot = a_check ? shellShadowVSBakedCheck : shellShadowVSBaked;
	if (!slot) {
		logger::debug("Compiling SnowShell shadow-cast VS ({})", a_check ? "baked check" : "baked");
		std::vector<std::pair<const char*, const char*>> defines = { { "VSHADER", "" }, { "SNOW_SHADOW_CAST", "" }, { "SNOW_CAST_BAKED", "" } };
		if (a_check)
			defines.push_back({ "SNOW_CAST_BAKED_CHECK", "" });
		slot = static_cast<ID3D11VertexShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "vs_5_0"));
	}
	return slot;
}

ID3D11PixelShader* SnowDeformation::GetShellPS()
{
	// Setting changes reach here from the menu, a settings load and a preset
	// alike, so the permutation is keyed off the compiled-against value rather
	// than off any one of those paths noticing.
	if (shellPS && shellDepthClampCompiled != !shellDepthClampDisabled) {
		shellPS->Release();
		shellPS = nullptr;
	}
	if (!shellPS) {
		logger::debug("Compiling SnowShell PS");
		auto defines = ShellPSDefines();
		// Applied here, not in ShellPSDefines: that is static, and the LOD
		// histogram variant has no reason to carry this.
		if (shellDepthClampDisabled)
			defines.emplace_back("SNOW_SHELL_NO_DEPTH_EXPORT", "");
		if (shellMarchBicubicRestored)
			defines.emplace_back("SNOW_MARCH_BICUBIC", "");
		shellDepthClampCompiled = !shellDepthClampDisabled;
		shellMarchBicubicCompiledPS = shellMarchBicubicRestored;
		shellPS = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ps_5_0"));
	}
	return shellPS;
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

ID3D11PixelShader* SnowDeformation::GetShellLODPS()
{
	// Single-SV_Target permutation: SM5.0 shares PS UAV slots with the
	// render-target outputs, so the histogram UAV (u1) requires dropping the
	// G-buffer down to kMAIN.
	if (!shellLODPS) {
		logger::debug("Compiling SnowShell LOD heatmap PS");
		auto defines = ShellPSDefines("SNOW_LOD_HISTOGRAM");
		shellLODPS = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ps_5_0"));
	}
	return shellLODPS;
}

ID3D11VertexShader* SnowDeformation::GetShellTessVS()
{
	if (!shellTessVS) {
		logger::debug("Compiling SnowShell tess control-point VS");
		shellTessVS = static_cast<ID3D11VertexShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "VSHADER", "" }, { "SNOW_TESS", "" } }, "vs_5_0"));
	}
	return shellTessVS;
}

ID3D11HullShader* SnowDeformation::GetShellHS(bool a_bake)
{
	auto*& slot = a_bake ? shellHSBake : shellHS;
	if (!slot) {
		logger::debug("Compiling SnowShell HS{}", a_bake ? " (bake)" : "");
		std::vector<std::pair<const char*, const char*>> defines;
		if (a_bake)
			defines.push_back({ "SNOW_HS_BAKE", "" });
		slot = static_cast<ID3D11HullShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "hs_5_0"));
	}
	return slot;
}

ID3D11HullShader* SnowDeformation::GetShellHSNear(bool a_bake)
{
	auto*& slot = a_bake ? shellHSNearBake : shellHSNear;
	if (!slot) {
		logger::debug("Compiling SnowShell HS (split near{})", a_bake ? ", bake" : "");
		std::vector<std::pair<const char*, const char*>> defines = { { "SNOW_SPLIT_NEAR", "" } };
		if (a_bake)
			defines.push_back({ "SNOW_HS_BAKE", "" });
		slot = static_cast<ID3D11HullShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "hs_5_0"));
	}
	return slot;
}

ID3D11HullShader* SnowDeformation::GetShellHSFar(bool a_bake)
{
	auto*& slot = a_bake ? shellHSFarBake : shellHSFar;
	if (!slot) {
		logger::debug("Compiling SnowShell HS (split far{})", a_bake ? ", bake" : "");
		std::vector<std::pair<const char*, const char*>> defines = { { "SNOW_SPLIT_FAR", "" } };
		if (a_bake)
			defines.push_back({ "SNOW_HS_BAKE", "" });
		slot = static_cast<ID3D11HullShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "hs_5_0"));
	}
	return slot;
}

ID3D11PixelShader* SnowDeformation::GetShellPSPrepass(bool a_patch, bool a_keepHidden)
{
	auto*& slot = a_keepHidden ? shellPSPrepassKeepHidden[a_patch ? 1 : 0] : (a_patch ? shellPSPrepassPatch : shellPSPrepass);
	if (!slot) {
		logger::debug("Compiling SnowShell PS (depth prepass{}{})", a_patch ? ", patch id" : "", a_keepHidden ? ", hidden kept" : "");
		auto defines = ShellPSDefines();
		defines.emplace_back("SNOW_SHELL_DEPTH_PREPASS", "");
		if (a_patch) {
			defines.emplace_back("SNOW_SHELL_PREPASS_PATCH", "");
			defines.emplace_back("SNOW_SHELL_PATCH_ID", "");
		}
		if (a_keepHidden)
			defines.emplace_back("SNOW_PREPASS_KEEP_HIDDEN", "");
		slot = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ps_5_0"));
	}
	return slot;
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

ID3D11DomainShader* SnowDeformation::GetShellDSPatch(bool a_bake, bool a_check)
{
	auto*& slot = shellDSPatch[a_bake ? (a_check ? 2 : 1) : 0];
	if (!slot) {
		logger::debug("Compiling SnowShell DS (patch id{})", a_bake ? (a_check ? ", bake check" : ", bake") : "");
		std::vector<std::pair<const char*, const char*>> defines = { { "SNOW_SHELL_PATCH_ID", "" } };
		if (a_bake)
			defines.push_back({ a_check ? "SNOW_DS_BAKE_CHECK" : "SNOW_DS_BAKE", "" });
		slot = static_cast<ID3D11DomainShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ds_5_0"));
	}
	return slot;
}

ID3D11DomainShader* SnowDeformation::GetShellDSLean(bool a_patchId)
{
	auto*& slot = shellDSLean[a_patchId ? 1 : 0];
	if (!slot) {
		logger::debug("Compiling SnowShell DS (lean prepass{})", a_patchId ? ", patch id" : "");
		std::vector<std::pair<const char*, const char*>> defines = { { "SNOW_DS_BAKE", "" }, { "SNOW_DS_LEAN", "" } };
		if (a_patchId)
			defines.push_back({ "SNOW_SHELL_PATCH_ID", "" });
		slot = static_cast<ID3D11DomainShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ds_5_0"));
	}
	return slot;
}

ID3D11HullShader* SnowDeformation::GetShellHSVisible(bool a_bake, bool a_trust)
{
	auto*& slot = a_trust ? (a_bake ? shellHSVisibleTrustBake : shellHSVisibleTrust) : (a_bake ? shellHSVisibleBake : shellHSVisible);
	if (!slot) {
		logger::debug("Compiling SnowShell HS (visible patches{}{})", a_bake ? ", bake" : "", a_trust ? ", trust" : "");
		std::vector<std::pair<const char*, const char*>> defines = { { "SNOW_HS_VISIBLE", "" } };
		if (a_bake)
			defines.push_back({ "SNOW_HS_BAKE", "" });
		if (a_trust)
			defines.push_back({ "SNOW_HS_TRUST", "" });
		slot = static_cast<ID3D11HullShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "hs_5_0"));
	}
	return slot;
}

ID3D11ComputeShader* SnowDeformation::GetShellPatchListCS()
{
	if (!shellPatchListCS) {
		logger::debug("Compiling DepthSyncCS PatchListCS");
		shellPatchListCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", { { "PATCH_LIST_CS", "" } }, "cs_5_0", "PatchListCS"));
	}
	return shellPatchListCS;
}

ID3D11VertexShader* SnowDeformation::GetShellTessVSList()
{
	if (!shellTessVSList) {
		logger::debug("Compiling SnowShell tess control-point VS (patch list)");
		shellTessVSList = static_cast<ID3D11VertexShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "VSHADER", "" }, { "SNOW_TESS", "" }, { "SNOW_TESS_LIST", "" } }, "vs_5_0"));
	}
	return shellTessVSList;
}

ID3D11HullShader* SnowDeformation::GetShellHSListMain()
{
	if (!shellHSListMain) {
		logger::debug("Compiling SnowShell HS (patch list, trust)");
		shellHSListMain = static_cast<ID3D11HullShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "SNOW_HS_BAKE", "" }, { "SNOW_HS_TRUST", "" } }, "hs_5_0"));
	}
	return shellHSListMain;
}

ID3D11VertexShader* SnowDeformation::GetShellFlatVS()
{
	if (!shellFlatVS) {
		logger::debug("Compiling SnowShell flat-patch VS");
		shellFlatVS = static_cast<ID3D11VertexShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "VSHADER", "" }, { "SNOW_VS_FLAT", "" } }, "vs_5_0"));
	}
	return shellFlatVS;
}

bool SnowDeformation::EnsurePatchListResources()
{
	if (shellFlatPatches && shellTessPatches && shellFlatArgs && shellTessArgs)
		return true;
	auto device = globals::d3d::device;
	constexpr UINT patches = kShellGridDim * kShellGridDim;
	auto structured = [&](winrt::com_ptr<ID3D11Buffer>& a_buf, winrt::com_ptr<ID3D11UnorderedAccessView>& a_uav, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv, const char* a_name) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = patches * sizeof(uint32_t);
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = sizeof(uint32_t);
		if (FAILED(device->CreateBuffer(&desc, nullptr, a_buf.put()))) {
			logger::info("[SNOW DEFORMATION] patch list: {} creation failed", a_name);
			return false;
		}
		Util::SetResourceName(a_buf.get(), a_name);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = patches;
		if (FAILED(device->CreateUnorderedAccessView(a_buf.get(), &uavDesc, a_uav.put())))
			return false;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = patches;
		return SUCCEEDED(device->CreateShaderResourceView(a_buf.get(), &srvDesc, a_srv.put()));
	};
	auto args = [&](winrt::com_ptr<ID3D11Buffer>& a_buf, winrt::com_ptr<ID3D11UnorderedAccessView>& a_uav, const char* a_name) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = 16;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(device->CreateBuffer(&desc, nullptr, a_buf.put()))) {
			logger::info("[SNOW DEFORMATION] patch list: {} creation failed", a_name);
			return false;
		}
		Util::SetResourceName(a_buf.get(), a_name);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = 4;
		uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		return SUCCEEDED(device->CreateUnorderedAccessView(a_buf.get(), &uavDesc, a_uav.put()));
	};
	bool ok = structured(shellFlatPatches, shellFlatPatchesUAV, shellFlatPatchesSRV, "SnowDeformation::ShellFlatPatches") &&
	          structured(shellTessPatches, shellTessPatchesUAV, shellTessPatchesSRV, "SnowDeformation::ShellTessPatches") &&
	          args(shellFlatArgs, shellFlatArgsUAV, "SnowDeformation::ShellFlatArgs") &&
	          args(shellTessArgs, shellTessArgsUAV, "SnowDeformation::ShellTessArgs");
	if (ok) {
		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = 16;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (int r = 0; r < kShellPatchArgsRing && ok; r++)
			for (int k = 0; k < 2 && ok; k++)
				ok = SUCCEEDED(device->CreateBuffer(&stagingDesc, nullptr, shellPatchArgsStaging[r][k].put()));
	}
	if (!ok) {
		logger::info("[SNOW DEFORMATION] patch list resources failed; the whole-grid shading draw stays");
		shellFlatPatches = nullptr;
		shellTessPatches = nullptr;
		shellFlatArgs = nullptr;
		shellTessArgs = nullptr;
		shellPatchSplit = false;
	}
	return ok;
}

// The counts of the frame kShellPatchArgsRing frames back, without a stall.
void SnowDeformation::ReadPatchListCounts(ID3D11DeviceContext* a_context)
{
	const int readRing = (shellPatchArgsRing + 1) % kShellPatchArgsRing;
	for (int k = 0; k < 2; k++) {
		auto* staging = shellPatchArgsStaging[readRing][k].get();
		if (!staging)
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(a_context->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
			const uint32_t vertices = static_cast<const uint32_t*>(mapped.pData)[0];
			(k == 0 ? shellPatchFlatCount : shellPatchTessCount) = vertices / (k == 0 ? 6u : 4u);
			a_context->Unmap(staging, 0);
		}
	}
}

void SnowDeformation::DrawShellPatchLists(ID3D11DeviceContext* a_context, ID3D11VertexShader* a_tessVS, ID3D11DomainShader* a_ds)
{
	ID3D11ShaderResourceView* tessListSRV = shellTessPatchesSRV.get();
	a_context->VSSetShaderResources(62, 1, &tessListSRV);
	a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
	a_context->VSSetShader(shellTessVSList, nullptr, 0);
	a_context->HSSetShader(shellHSListMain, nullptr, 0);
	a_context->DSSetShader(a_ds, nullptr, 0);
	a_context->DrawInstancedIndirect(shellTessArgs.get(), 0);

	a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11ShaderResourceView* flatSRVs[3] = { shellFlatPatchesSRV.get(), shellVertexBake->srv.get(), shellVertexBakeSlope->srv.get() };
	a_context->VSSetShaderResources(62, 1, &flatSRVs[0]);
	a_context->VSSetShaderResources(9, 1, &flatSRVs[1]);
	a_context->VSSetShaderResources(23, 1, &flatSRVs[2]);
	a_context->VSSetShader(shellFlatVS, nullptr, 0);
	a_context->HSSetShader(nullptr, nullptr, 0);
	a_context->DSSetShader(nullptr, nullptr, 0);
	a_context->DrawInstancedIndirect(shellFlatArgs.get(), 0);

	ID3D11ShaderResourceView* nullSRV = nullptr;
	a_context->VSSetShaderResources(62, 1, &nullSRV);
	a_context->VSSetShaderResources(9, 1, &nullSRV);
	a_context->VSSetShaderResources(23, 1, &nullSRV);
	a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
	a_context->VSSetShader(a_tessVS, nullptr, 0);
}

ID3D11PixelShader* SnowDeformation::GetShellFillPSVisible()
{
	if (!shellFillPSVisible) {
		logger::debug("Compiling DepthSyncCS FillPS (visible patches)");
		shellFillPSVisible = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", { { "FILL_VISIBLE_PATCHES", "" } }, "ps_5_0", "FillPS"));
	}
	return shellFillPSVisible;
}

bool SnowDeformation::EnsureVisiblePatchResources()
{
	auto device = globals::d3d::device;
	if (!shellRasterDepth)
		return false;
	const auto& depthDesc = shellRasterDepth->desc;
	if (shellRasterPatch && (shellRasterPatch->desc.Width != depthDesc.Width || shellRasterPatch->desc.Height != depthDesc.Height)) {
		delete shellRasterPatch;
		shellRasterPatch = nullptr;
	}
	if (!shellRasterPatch) {
		D3D11_TEXTURE2D_DESC desc = depthDesc;
		desc.Format = DXGI_FORMAT_R32_UINT;
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
		shellRasterPatch = new Texture2D(desc, "SnowDeformation::ShellRasterPatch");
		shellRasterPatch->CreateSRV(srvDesc);
		shellRasterPatch->CreateRTV(rtvDesc);
	}
	if (!shellVisiblePatches) {
		constexpr UINT patches = kShellGridDim * kShellGridDim;
		D3D11_BUFFER_DESC bufDesc{};
		bufDesc.ByteWidth = patches * sizeof(uint32_t);
		bufDesc.Usage = D3D11_USAGE_DEFAULT;
		bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bufDesc.StructureByteStride = sizeof(uint32_t);
		if (FAILED(device->CreateBuffer(&bufDesc, nullptr, shellVisiblePatches.put())))
			return false;
		Util::SetResourceName(shellVisiblePatches.get(), "SnowDeformation::ShellVisiblePatches");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = patches;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = patches;
		if (FAILED(device->CreateUnorderedAccessView(shellVisiblePatches.get(), &uavDesc, shellVisiblePatchesUAV.put())) ||
			FAILED(device->CreateShaderResourceView(shellVisiblePatches.get(), &srvDesc, shellVisiblePatchesSRV.put()))) {
			shellVisiblePatches = nullptr;
			shellVisiblePatchesUAV = nullptr;
			shellVisiblePatchesSRV = nullptr;
			return false;
		}
		Util::SetResourceName(shellVisiblePatchesUAV.get(), "SnowDeformation::ShellVisiblePatches UAV");
		Util::SetResourceName(shellVisiblePatchesSRV.get(), "SnowDeformation::ShellVisiblePatches SRV");
	}
	if (shellPrepassLastFrame && !shellVisiblePrev) {
		constexpr UINT patches = kShellGridDim * kShellGridDim;
		D3D11_BUFFER_DESC bufDesc{};
		bufDesc.ByteWidth = patches * sizeof(uint32_t);
		bufDesc.Usage = D3D11_USAGE_DEFAULT;
		bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bufDesc.StructureByteStride = sizeof(uint32_t);
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = patches;
		if (SUCCEEDED(device->CreateBuffer(&bufDesc, nullptr, shellVisiblePrev.put())) &&
			SUCCEEDED(device->CreateShaderResourceView(shellVisiblePrev.get(), &srvDesc, shellVisiblePrevSRV.put()))) {
			Util::SetResourceName(shellVisiblePrev.get(), "SnowDeformation::ShellVisiblePrev");
			Util::SetResourceName(shellVisiblePrevSRV.get(), "SnowDeformation::ShellVisiblePrev SRV");
		} else {
			shellVisiblePrev = nullptr;
			shellVisiblePrevSRV = nullptr;
		}
		shellVisiblePrevValid = false;
	}
	return shellRasterPatch->rtv && shellRasterPatch->srv;
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
		D3D11_TEXTURE2D_DESC copyDesc = desc;
		copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		D3D11_UNORDERED_ACCESS_VIEW_DESC copyUavDesc{};
		copyUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
		copyUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		if (SUCCEEDED(device->CreateTexture2D(&copyDesc, nullptr, preSnowDepth.put()))) {
			Util::SetResourceName(preSnowDepth.get(), "SnowDeformation::PreSnowDepth");
			if (FAILED(device->CreateShaderResourceView(preSnowDepth.get(), &srvDesc, preSnowDepthSRV.put())) ||
				FAILED(device->CreateUnorderedAccessView(preSnowDepth.get(), &copyUavDesc, preSnowDepthUAV.put()))) {
				preSnowDepth = nullptr;
				preSnowDepthSRV = nullptr;
				preSnowDepthUAV = nullptr;
			} else {
				Util::SetResourceName(preSnowDepthSRV.get(), "SnowDeformation::PreSnowDepth SRV");
				Util::SetResourceName(preSnowDepthUAV.get(), "SnowDeformation::PreSnowDepth UAV");
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

ID3D11PixelShader* SnowDeformation::GetShellPSNoDepth()
{
	if (shellPSNoDepth && (shellMarchBicubicCompiledPSNoDepth != shellMarchBicubicRestored || shellPSLegacyBitsCompiled != shellPSLegacyBits)) {
		shellPSNoDepth->Release();
		shellPSNoDepth = nullptr;
	}
	if (!shellPSNoDepth) {
		logger::debug("Compiling SnowShell PS (no depth export)");
		auto defines = ShellPSDefines();
		defines.emplace_back("SNOW_SHELL_NO_DEPTH_EXPORT", "");
		if (shellMarchBicubicRestored)
			defines.emplace_back("SNOW_MARCH_BICUBIC", "");
		if (shellPSLegacyBits & 1)
			defines.emplace_back("SNOW_TEETH_UNGATED", "");
		if (shellPSLegacyBits & 2)
			defines.emplace_back("SNOW_REMARCH_SERIAL", "");
		if (shellPSLegacyBits & 4)
			defines.emplace_back("SNOW_UNTOUCHED_SKIP_OFF", "");
		shellMarchBicubicCompiledPSNoDepth = shellMarchBicubicRestored;
		shellPSLegacyBitsCompiled = shellPSLegacyBits;
		shellPSNoDepth = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ps_5_0"));
	}
	return shellPSNoDepth;
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

ID3D11PixelShader* SnowDeformation::GetShellPSDeferred()
{
	if (shellPSDeferred && shellPSDeferredLegacyBits != shellPSLegacyBits) {
		shellPSDeferred->Release();
		shellPSDeferred = nullptr;
	}
	if (!shellPSDeferred) {
		logger::debug("Compiling SnowShell PS (deferred)");
		auto defines = ShellPSDefines();
		defines.emplace_back("SNOW_SHELL_NO_DEPTH_EXPORT", "");
		defines.emplace_back("SNOW_SHELL_DEFERRED", "");
		if (shellPSLegacyBits & 1)
			defines.emplace_back("SNOW_TEETH_UNGATED", "");
		if (shellPSLegacyBits & 2)
			defines.emplace_back("SNOW_REMARCH_SERIAL", "");
		if (shellPSLegacyBits & 4)
			defines.emplace_back("SNOW_UNTOUCHED_SKIP_OFF", "");
		shellPSDeferredLegacyBits = shellPSLegacyBits;
		shellPSDeferred = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ps_5_0"));
	}
	return shellPSDeferred;
}

ID3D11PixelShader* SnowDeformation::GetShellPSPrepassAttr(bool a_keepHidden)
{
	auto*& slot = shellPSPrepassAttr[a_keepHidden ? 1 : 0];
	if (!slot) {
		logger::debug("Compiling SnowShell PS (depth prepass, deferred attributes{})", a_keepHidden ? ", hidden kept" : "");
		auto defines = ShellPSDefines();
		defines.emplace_back("SNOW_SHELL_DEPTH_PREPASS", "");
		defines.emplace_back("SNOW_PREPASS_ATTR", "");
		if (a_keepHidden)
			defines.emplace_back("SNOW_PREPASS_KEEP_HIDDEN", "");
		slot = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ps_5_0"));
	}
	return slot;
}

ID3D11PixelShader* SnowDeformation::GetShellPSCensus()
{
	if (!shellPSCensus) {
		logger::debug("Compiling SnowShell PS (lane census)");
		shellPSCensus = static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", ShellPSDefines("SNOW_LANE_CENSUS"), "ps_5_0"));
	}
	return shellPSCensus;
}

bool SnowDeformation::EnsureLaneCensusResources()
{
	if (laneCensusBuffer)
		return true;
	auto device = globals::d3d::device;
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = 32;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	if (FAILED(device->CreateBuffer(&desc, nullptr, laneCensusBuffer.put())))
		return false;
	Util::SetResourceName(laneCensusBuffer.get(), "SnowDeformation::LaneCensus");
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.NumElements = 8;
	uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	if (FAILED(device->CreateUnorderedAccessView(laneCensusBuffer.get(), &uavDesc, laneCensusUAV.put()))) {
		laneCensusBuffer = nullptr;
		return false;
	}
	Util::SetResourceName(laneCensusUAV.get(), "SnowDeformation::LaneCensus UAV");
	D3D11_BUFFER_DESC stagingDesc{};
	stagingDesc.ByteWidth = 32;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (auto& staging : laneCensusStaging) {
		if (FAILED(device->CreateBuffer(&stagingDesc, nullptr, staging.put()))) {
			laneCensusBuffer = nullptr;
			laneCensusUAV = nullptr;
			return false;
		}
	}
	return true;
}

void SnowDeformation::ReadLaneCensus(ID3D11DeviceContext* a_context)
{
	for (int r = 0; r < kLaneCensusRing; r++) {
		if (!laneCensusPending[r] || !laneCensusStaging[r])
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(laneCensusStaging[r].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
			continue;
		const auto* v = static_cast<const uint32_t*>(mapped.pData);
		laneCensus.frames++;
		laneCensus.pixels += v[0];
		laneCensus.quads += v[1];
		laneCensus.untouched += v[2];
		for (int k = 0; k < 4; k++)
			laneCensus.quadLive[k] += v[3 + k];
		a_context->Unmap(laneCensusStaging[r].get(), 0);
		laneCensusPending[r] = false;
	}
}

ID3D11ComputeShader* SnowDeformation::GetShellBakeCS()
{
	if (!shellBakeCS) {
		logger::debug("Compiling SnowShell BakeCS");
		shellBakeCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "SNOW_BAKE_CS", "" } }, "cs_5_0", "BakeCS"));
	}
	return shellBakeCS;
}

ID3D11DomainShader* SnowDeformation::GetShellDSBake(bool a_check)
{
	auto*& slot = a_check ? shellDSBakeCheck : shellDSBake;
	if (!slot) {
		logger::debug("Compiling SnowShell DS ({})", a_check ? "bake check" : "bake");
		slot = static_cast<ID3D11DomainShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { a_check ? "SNOW_DS_BAKE_CHECK" : "SNOW_DS_BAKE", "" } }, "ds_5_0"));
	}
	return slot;
}

bool SnowDeformation::EnsureShellVertexBake()
{
	if (shellVertexBake && shellVertexBakeSlope && shellCastBake)
		return shellVertexBake->srv && shellVertexBake->uav && shellVertexBakeSlope->srv && shellVertexBakeSlope->uav && shellCastBake->srv && shellCastBake->uav;
	constexpr UINT dim = kShellGridDim + 1;
	auto make = [&](DXGI_FORMAT a_format, const char* a_name) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = dim;
		desc.Height = dim;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = a_format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = a_format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = a_format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		auto* tex = new Texture2D(desc, a_name);
		tex->CreateSRV(srvDesc);
		tex->CreateUAV(uavDesc);
		return tex;
	};
	if (!shellVertexBake)
		shellVertexBake = make(DXGI_FORMAT_R32G32B32A32_FLOAT, "SnowDeformation::ShellVertexBake");
	if (!shellVertexBakeSlope)
		shellVertexBakeSlope = make(DXGI_FORMAT_R32G32_FLOAT, "SnowDeformation::ShellVertexBakeSlope");
	if (!shellCastBake)
		shellCastBake = make(DXGI_FORMAT_R32_FLOAT, "SnowDeformation::ShellCastBake");
	return shellVertexBake->srv && shellVertexBake->uav && shellVertexBakeSlope->srv && shellVertexBakeSlope->uav && shellCastBake->srv && shellCastBake->uav;
}

ID3D11DomainShader* SnowDeformation::GetShellDS()
{
	if (shellFlatDSDebug) {
		if (!shellDSFlat) {
			logger::debug("Compiling SnowShell DS (flat A/B)");
			shellDSFlat = static_cast<ID3D11DomainShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "SNOW_DS_FLAT", "" } }, "ds_5_0"));
		}
		return shellDSFlat;
	}
	if (!shellDS) {
		logger::debug("Compiling SnowShell DS");
		shellDS = static_cast<ID3D11DomainShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", {}, "ds_5_0"));
	}
	return shellDS;
}

ID3D11ComputeShader* SnowDeformation::GetPreSnowCopyCS()
{
	if (!preSnowCopyCS) {
		logger::debug("Compiling DepthSyncCS PreSnowCopyCS");
		preSnowCopyCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "cs_5_0", "PreSnowCopyCS"));
	}
	if (!depthSyncRectCB)
		depthSyncRectCB = new ConstantBuffer(ConstantBufferDesc<DepthSyncRectCB>(), "SnowDeformation::DepthSyncRectCB");
	return preSnowCopyCS;
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

ID3D11ComputeShader* SnowDeformation::GetDepthSyncCS()
{
	if (!depthSyncCS) {
		logger::debug("Compiling DepthSyncCS");
		depthSyncCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "cs_5_0"));
	}
	return depthSyncCS;
}

ID3D11ComputeShader* SnowDeformation::GetSeamShieldCS()
{
	if (!seamShieldCS) {
		logger::debug("Compiling SeamShieldCS");
		seamShieldCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\TerrainWindowFillCS.hlsl", {}, "cs_5_0", "SeamShieldCS"));
	}
	return seamShieldCS;
}

bool SnowDeformation::EnsureSeamMask()
{
	if (seamMaskTexture)
		return seamMaskTexture->srv && seamMaskTexture->uav;
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = kShellWindowDim;
	desc.Height = kShellWindowDim;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = desc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	seamMaskTexture = new Texture2D(desc, "SnowDeformation::SeamShieldMask");
	seamMaskTexture->CreateSRV(srvDesc);
	seamMaskTexture->CreateUAV(uavDesc);
	seamMaskTerrainVersion = ~0u;
	return seamMaskTexture->srv && seamMaskTexture->uav;
}

void SnowDeformation::BindSeamShield()
{
	// Called from Deferred's exterior composite dispatch. Inactive frames
	// still need the CB bound with the flag off once the define exists, but
	// skipping the bind entirely is also safe: the shader gates on
	// SnowSeamParams.w, and an unbound b7 reads zeros = inactive.
	const bool active = settings.EnableSnowDeformation && globals::state->inWorld &&
	                    seamMaskTexture && seamMaskTexture->srv && seamMaskFrame == globals::state->frameCount &&
	                    lastShellCBData != nullptr;
	if (!active)
		return;
	if (!seamShieldCB)
		seamShieldCB = new ConstantBuffer(ConstantBufferDesc<SeamShieldCB>(), "SnowDeformation::SeamShieldCB");
	SeamShieldCB data{};
	data.WindowOffset = { lastShellCBData->GridToTerrainOffset.x - lastShellCBData->GridOrigin.x,
		lastShellCBData->GridToTerrainOffset.y - lastShellCBData->GridOrigin.y };
	data.TexelSize = lastShellCBData->TerrainTexelSize;
	data.Dim = (float)lastShellCBData->TerrainDim;
	data.Params = { 0.75f, 0.0f, 0.0f, 1.0f };
	seamShieldCB->Update(data);

	auto context = globals::d3d::context;
	ID3D11ShaderResourceView* srv = seamMaskTexture->srv.get();
	context->CSSetShaderResources(16, 1, &srv);
	ID3D11Buffer* cb = seamShieldCB->CB();
	context->CSSetConstantBuffers(7, 1, &cb);
}

void SnowDeformation::RefreshShellGridPlacement(ShellCB& a_cb)
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
	const auto camAdjust = fb.GetCameraPosAdjust();
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
	// Bit 0 horizon march, bit 1 hull frustum cull, bit 2 land-exact height
	// off, bit 3 the tessellated patches' old, land-crossing diagonal, bit 4
	// churn by parallax, bit 5 far relief off. .y is DrawShell's (road tops this frame).
	a_cb.ShellFlags.x = (settings.ShellHorizonMarch ? 1 : 0) | (shellFrustumCullDisabled ? 0 : 2) |
	                    (shellLandHeightDisabled ? 4 : 0) | (shellTessDiagonalUnflip ? 8 : 0) | (shellChurnParallaxOff ? 0 : 16) | (shellFarTessDisabled ? 32 : 0);
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
		shellKillMask = p.step % 2 ? 1u << (p.step / 2) : 0u;
	} else {
		shellKillMask = p.savedMask;
		p.running = false;
		p.done = true;
	}
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

	auto vs = GetShellVS();
	auto ps = GetShellPS();
	if (!vs || !ps)
		return;

	ScopedTicks _drawShellTicks(cpuCensus.spanTicks[CpuCensus::kSpanDrawShell], cpuCensus.spanCalls[CpuCensus::kSpanDrawShell]);
	SpanClock spanClock;
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto& fb = globals::game::frameBufferCached;

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
	cbData.ShellDebugData = shellDataDebug ? 1u : (shellExclusionDebug ? 2u : (shellBorderDebug ? 3u : (shellSSSDebug ? 4u : (shellWallDebug ? 5u : (shellEdgeHeightDebug ? 6u : (shellTrampleDebug ? 7u : 0u))))));
	cbData.ShellLODDebug = (uint32_t)std::clamp(lodDebugView, 0, 4);
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
	if (LoadedCellSquare(seamMinX, seamMinY, seamMaxX, seamMaxY)) {
		// The fade band sits ~29 m OUTSIDE the true boundary, over the
		// recolored LOD terrain: full-height shell up to the seam, melting
		// into same-material ground beyond it. Ending the fade AT the seam
		// left a visible unshelled strip of full terrain just inside it.
		constexpr float kSeamOverlap = 2048.0f;
		cbData.SeamBounds = { seamMinX * 4096.0f - kSeamOverlap, seamMinY * 4096.0f - kSeamOverlap,
			(seamMaxX + 1) * 4096.0f + kSeamOverlap, (seamMaxY + 1) * 4096.0f + kSeamOverlap };
		cbData.SeamRampInv = 1.0f / 2048.0f;
		// C3 mechanism 3: the square snaps to the player's cell, so every
		// crossing rewrites the whole far field's edge fade in one frame.
		const int cellX = (seamMinX + seamMaxX) / 2;
		const int cellY = (seamMinY + seamMaxY) / 2;
		static int lastSeamCellX = INT_MIN, lastSeamCellY = INT_MIN;
		if (cellX != lastSeamCellX || cellY != lastSeamCellY) {
			if (lastSeamCellX != INT_MIN)
				lodSeamChanges++;
			lastSeamCellX = cellX;
			lastSeamCellY = cellY;
		}
	}
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
		0.0f
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
	const float snowSoftness = GetSnowSoftness();
	cbData.BermHeightAmp = std::clamp(settings.BermHeight, 0.0f, 1.0f) * std::lerp(1.0f, 0.35f, snowSoftness);
	cbData.BermReach = BermReachScale();
	// .w claims RimStyle's documented spare rather than growing ShellCB: the
	// buffer is hand-mirrored across two shaders and the statics pass binds
	// this same b0, so one component serves both shells with nothing moving.
	cbData.RimStyle = { std::clamp(settings.RimLip, 0.0f, 0.3f) * (1.0f - snowSoftness),
		std::clamp(settings.RimTeeth, 0.0f, 1.0f) * (1.0f - snowSoftness),
		std::clamp(settings.BermClods, 0.0f, 6.0f),
		GetAccumulationDepthScale() };
	// A stale extent field turns the class switch off with it.
	const float2 trampleBracket = TrampleBracket();
	cbData.TrampleStyle = { 0.0f,
		ExtentLive() ? std::clamp(settings.TrampledFlattening, 0.0f, 1.0f) : 1.0f,
		trampleBracket.x,
		trampleBracket.y };
	// Loose snow holds no clods. ObjChurnHeightAmp copies this below.
	cbData.ChurnHeightAmp = std::clamp(settings.ChurnHeight, 0.0f, 8.0f) * std::lerp(1.0f, std::clamp(settings.FreshSnowChurn, 0.0f, 1.0f), snowSoftness);
	cbData.ChurnSizeScale = std::clamp(settings.ChurnSize, 0.25f, 4.0f);
	// The retired crisp-grain pair: the landscape half now carries the A/B
	// flags, the Obj half is a layout keeper.
	cbData.DebugNoFarPad = lodDebugNoFarPad ? 1.0f : 0.0f;
	{
		// Pixels per world unit at distance d = focalPx / d, with
		// focalPx = half the screen height times the projection's y scale.
		const auto& eye = globals::game::shadowState->GetRuntimeData().cameraData.getEye();
		const float focalPx = 0.5f * static_cast<float>(globals::game::graphicsState->screenHeight) * eye.projMat(1, 1);
		const float capPx = std::clamp(settings.SkinTessCapPx, 0.0f, 64.0f);
		cbData.SkinTessCapSlope = (focalPx > 1.0f && capPx > 0.0f) ? capPx / focalPx : 0.0f;
	}
	cbData.DebugNoDataMorph = lodDebugNoDataMorph ? 1.0f : 0.0f;
	// Object trench detail follows the landscape set. The CB rows stay because
	// both shells read them.
	cbData.ObjBermHeightAmp = cbData.BermHeightAmp;
	cbData.ObjChurnHeightAmp = cbData.ChurnHeightAmp;
	cbData.ObjChurnSizeScale = cbData.ChurnSizeScale;
	cbData.HasSnowNormal = shellSnowNormalSRV ? 1.0f : 0.0f;
	cbData.HasSnowRmaos = shellSnowRmaosSRV ? 1.0f : 0.0f;
	cbData.SnowRoughnessScale = snowRoughnessScale;
	cbData.SnowGlintParams = { snowGlintLogDensity, snowGlintMicroRoughness, snowGlintDensityRandomization, snowGlintScreenSpaceScale };
	cbData.SnowSpecularLevel = snowSpecularLevel;
	// Sparkle: the shell's specular runs TruePBR's glint NDF when its shared
	// noise texture exists (bound to t20 below for the whole pass).
	cbData.EnableGlints = (settings.SnowGlints && globals::features::truePBR.glintsNoiseTexture) ? 1.0f : 0.0f;
	cbData.BorderNoise = settings.SnowBorderNoise;
	cbData.BorderSmooth = settings.SnowBorderSmoothness;
	cbData.BorderStyle = { settings.SnowBorderDithering ? 1.0f : 0.0f,
		std::clamp(settings.TrenchFloorFraction, 0.0f, 1.0f),
		(float)sunCascadeSlice[0], (float)sunCascadeSlice[1] };
	// x compaction matte; y the shell-surface SSS re-march toggle; zw its
	// dynamic-resolution scale - the shell pass does not bind FrameBuffer
	// b12, so the screen-space taps cannot call
	// GetDynamicResolutionAdjustedScreenPosition and take the scale here.
	const auto& dynRes = globals::game::frameBufferCached.GetDynamicResolutionParams1();
	// y packs mode + caster cap: integer part 0/1/2, fraction = cap/1000
	// (cap clamped below 1000 so the integer part stays the mode).
	const float remarchMode = settings.ShellSSSRemarch ? (settings.ShellSSSRemarchThickness ? 2.0f : 1.0f) : 0.0f;
	const float remarchCap = std::clamp(settings.ShellSSSRemarchCasterCap, 10.0f, 999.0f);
	// x = the water cut (runtime A/B in Debugging Options).
	cbData.CompactLook = { debugWaterCutDisabled ? 0.0f : 1.0f, remarchMode + remarchCap / 1000.0f, dynRes.x, dynRes.y };
	// Border Fade is a percent in the UI; the shader band stays 2..64.
	cbData.BorderUntrampledFade = std::lerp(2.0f, 64.0f, std::clamp(settings.SnowBorderFade, 0.0f, 100.0f) / 100.0f);
	// Layout keeper; the shader hard-codes its old default-0 resolution.
	cbData.ShellTriHeight = shellBilinearHeight ? 0.0f : 1.0f;
	// Layout keeper, retired with the object/landscape seam cross-fade.
	// Gated on the terrain window existing: without it the cull's SampleTerrain
	// reads zeros, which look like fully bare ground everywhere.
	cbData.ShellCullBare = (settings.ShellBareGroundCull && shellTerrainTexture) ? 1.0f : 0.0f;
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
	// WHITE every Prepass even when disabled, so multiplying is always safe
	// once the texture exists.
	auto& screenSpaceShadowsFeature = globals::features::screenSpaceShadows;
	cbData.ScreenSpaceShadowsActive = (settings.SnowScreenSpaceShadows && screenSpaceShadowsFeature.loaded && screenSpaceShadowsFeature.screenSpaceShadowsTexture) ? 1.0f : 0.0f;

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
		if (kill & (1u << 5))
			cbData.CompactLook.y = 0.0f;
		if (kill & (1u << 6))
			cbData.EnableGlints = 0.0f;
		if (kill & (1u << 7))
			cbData.PointLightsActive = 0.0f;
		if (kill & (1u << 8))
			cbData.SkylightingActive = 0.0f;
		if (kill & (1u << 9))
			cbData.HasSnowTexture = 0;
	}
	// Bit 1: the slope's inputs match the last bake's store, so BakeCS keeps it.
	// In .y: the caster's placement memcmp rewrites .x.
	shellSlopeKeyPending = MakeShellSlopeKey(cbData);
	if (shellSlopeKeyValid && shellSlopeKeyPending == shellSlopeKey)
		cbData.ShellFlags.y |= 2;
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
	// past it never loads, so a test over the whole span never settles. A
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
		                       CellRangeSnowVerdict(sqMinX, sqMinY, sqMaxX, sqMaxY) :
		                       kSnowGateUnknown;
	}
	shellGateBareFrames = shellSnowVerdict == kSnowGateBare ? std::min(shellGateBareFrames + 1, kShellGateBareFrames) : 0;
	const bool shellGround = shellGateBareFrames < kShellGateBareFrames && !GroundShellsSuspended();

	// Rasterize this frame's captured statics top-down into the object
	// height windows, then restore the viewport for the screen-space passes.
	// Waits for the prime's object group: EnsureStaticsShaders would
	// otherwise compile its ten variants synchronously right here.
	// Every reader of these fields (shell, skins, trench patch, caster,
	// volume) is off in a bare square with nothing captured.
	spanClock.Lap(CpuCensus::kSpanShellSetup);
	const bool nothingDraws = !shellGround && capturedStatics.empty() && !settings.VolumeSnow;
	if (nothingDraws)
		SkipObjectHeightMap();
	else if (!SnowShadersPending(2) && EnsureStaticsShaders())
		RenderObjectHeightMap();
	spanClock.Lap(CpuCensus::kSpanObjectHeightMap);
	RenderWaterCapture();
	spanClock.Lap(CpuCensus::kSpanWaterCapture);
	CoverWaterWindow();
	spanClock.Lap(CpuCensus::kSpanWaterCover);
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
	if (prevViewportCount && mainViewportFrame == globals::state->frameCount && !shellMainViewportRangeDisabled) {
		D3D11_VIEWPORT shellViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		for (UINT i = 0; i < prevViewportCount; i++) {
			shellViewports[i] = prevViewports[i];
			shellViewports[i].MinDepth = mainViewportMinDepth;
			shellViewports[i].MaxDepth = mainViewportMaxDepth;
		}
		context->RSSetViewports(prevViewportCount, shellViewports);
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
		cbData.TessScreen = { std::max(shellTessTargetPx, 0.0f), prevViewports[0].Width * 0.5f, prevViewports[0].Height * 0.5f, 0.0f };
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

	// Main depth before any snow draw: DepthSync syncs the pixels the snow
	// passes (shell, skins, patch) move nearer than this copy. Targets are
	// unbound around the copy like the masks copy above. Not for LOD-batch
	// coats alone: they lie on their mesh, past anything Terrain Blending
	// softened.
	shellPrepassThisFrame = false;
	preSnowDepthThisFrame = false;
	if (shellGround || capturedLoaded > 0 || (settings.VolumeSnow && !capturedStatics.empty())) {
		auto preDepthSRV = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
		auto* copyCS = GetPreSnowCopyCS();
		if (preDepthSRV && copyCS && depthSyncRectCB && EnsurePrepassResources(preDepthSRV) && preSnowDepth && preSnowDepthSRV && preSnowDepthUAV) {
			context->OMSetRenderTargets(0, nullptr, nullptr);
			globals::profiler->BeginPass("SnowDeformation::Ground - Depth Copy");
			D3D11_TEXTURE2D_DESC preDesc{};
			preSnowDepth->GetDesc(&preDesc);
			const D3D11_BOX rect = ScreenRectBox(prevViewportCount ? &prevViewports[0] : nullptr, preDesc.Width, preDesc.Height, 0);
			DepthSyncRectCB rectData{ { rect.left, rect.top }, { rect.right - rect.left, rect.bottom - rect.top } };
			depthSyncRectCB->Update(rectData);
			ID3D11Buffer* rectCB = depthSyncRectCB->CB();
			ID3D11UnorderedAccessView* copyUAV = preSnowDepthUAV.get();
			context->CSSetConstantBuffers(2, 1, &rectCB);
			context->CSSetShaderResources(0, 1, &preDepthSRV);
			context->CSSetUnorderedAccessViews(2, 1, &copyUAV, nullptr);
			context->CSSetShader(copyCS, nullptr, 0);
			context->Dispatch((rectData.Extent[0] + 7) / 8, (rectData.Extent[1] + 7) / 8, 1);
			ID3D11ShaderResourceView* nullCopySRV = nullptr;
			ID3D11UnorderedAccessView* nullCopyUAV = nullptr;
			ID3D11Buffer* nullCopyCB = nullptr;
			context->CSSetShaderResources(0, 1, &nullCopySRV);
			context->CSSetUnorderedAccessViews(2, 1, &nullCopyUAV, nullptr);
			context->CSSetConstantBuffers(2, 1, &nullCopyCB);
			context->CSSetShader(nullptr, nullptr, 0);
			globals::profiler->EndPass();
			preSnowDepthThisFrame = true;
		}
	}

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
	auto dsv = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0];
	context->OMSetRenderTargets(8, rtvs, dsv);

	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->RSSetState(shellRasterState.get());
	context->OMSetDepthStencilState(shellDepthState.get(), 0);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	// Heatmap mode: depth test ALWAYS + no write (the PS re-creates
	// occlusion) so poke-under pixels survive to be measured, and the
	// G-buffer shrinks to kMAIN so the histogram UAV fits under the FL11.0
	// 8-slot RTV+UAV limit.
	const bool lodHeatmap = lodDebugView == 1 && EnsureLODDebugResources() && GetShellLODPS();
	if (lodHeatmap) {
		const UINT histClear[4] = {};
		context->ClearUnorderedAccessViewUint(lodHistogramUAV.get(), histClear);
		ID3D11UnorderedAccessView* histUAV = lodHistogramUAV.get();
		context->OMSetRenderTargetsAndUnorderedAccessViews(1, rtvs, dsv, 1, 1, &histUAV, nullptr);
		context->OMSetDepthStencilState(shellLODDepthState.get(), 0);
		ps = shellLODPS;
	}


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
	// Trample extent (t56): CarveProfile reads it, so it goes wherever the
	// berm field does.
	ID3D11ShaderResourceView* extentSRV = GetExtentFieldSRV();
	context->VSSetShaderResources(kExtentFieldSlot, 1, &extentSRV);
	context->PSSetShaderResources(kExtentFieldSlot, 1, &extentSRV);
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

	context->PSSetShader(ps, nullptr, 0);

	// Tessellated path: 4-control-point patches through the hull/domain
	// stages, so near-camera snow renders the deformation map and the PBR
	// displacement relief as real geometry. The domain shader runs the full
	// surface evaluation, so it needs the same field textures and CB the
	// legacy VS reads, plus the height map and its sampler.
	// Gated on Tessellation, NOT on ReliefDepth: the tessellated path's real
	// client is trench smoothness (the hull factors key off the deformation
	// map), and that must survive relief being turned off. With relief at 0
	// the factors collapse to 1 on undeformed ground, so the path stays cheap.
	auto* tessVS = settings.Tessellation ? GetShellTessVS() : nullptr;
	// Vertex bake: the base-grid corners' surface is evaluated once per
	// frame by a compute pass and read back by index; the bake DS is chosen
	// only once the pass and its texture are known good, so a failure falls
	// to the live DS rather than to an unbound read.
	ID3D11ComputeShader* bakeCS = (settings.Tessellation && !shellVertexBakeDisabled && !shellFlatDSDebug) ? GetShellBakeCS() : nullptr;
	bool bake = bakeCS && EnsureShellVertexBake();
	ID3D11DomainShader* tessDS = nullptr;
	if (settings.Tessellation) {
		if (bake)
			tessDS = GetShellDSBake(shellVertexBakeCheck);
		if (!tessDS) {
			bake = false;
			tessDS = GetShellDS();
		}
	}
	// Hull twins read the edge corners' deformation taps from the bake; a
	// missing twin falls to the live hull, which is always valid.
	ID3D11HullShader* tessHS = nullptr;
	if (settings.Tessellation) {
		if (bake)
			tessHS = GetShellHS(true);
		if (!tessHS)
			tessHS = GetShellHS(false);
	}
	const bool tessellate = tessVS && tessHS && tessDS;
	if (!shellGround)
		globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Vertex Bake");
	if (shellGround && tessellate && bake) {
		globals::profiler->BeginPass("SnowDeformation::Ground - Vertex Bake");
		context->CSSetConstantBuffers(0, 1, cbs);
		context->CSSetConstantBuffers(4, 3, sharedBuffers);
		context->CSSetShaderResources(0, 6, shellSRVs);
		context->CSSetShaderResources(13, 1, &fineSRV);
		context->CSSetShaderResources(27, 2, waterSRVs);
		context->CSSetShaderResources(19, 1, &waterNearSRV);
		ID3D11ShaderResourceView* csHeightSRV = shellSnowHeightSRV.get();
		context->CSSetShaderResources(8, 1, &csHeightSRV);
		context->CSSetShaderResources(11, 2, objectCapSRVs);
		context->CSSetShaderResources(14, 1, &bermSRV);
		context->CSSetShaderResources(kExtentFieldSlot, 1, &extentSRV);
		context->CSSetShaderResources(15, 1, &exclusionSRV);
		context->CSSetShaderResources(29, 1, &undulationSRV);
		context->CSSetShaderResources(kUntouchedTilesSlot, 1, &untouchedSRV);
		ID3D11SamplerState* csSampler = shellSnowSampler.get();
		context->CSSetSamplers(0, 1, &csSampler);
		ID3D11UnorderedAccessView* bakeUAVs[3] = { shellVertexBake->uav.get(), shellVertexBakeSlope->uav.get(), shellCastBake->uav.get() };
		context->CSSetUnorderedAccessViews(1, 3, bakeUAVs, nullptr);
		context->CSSetShader(bakeCS, nullptr, 0);
		constexpr UINT bakeGroups = (kShellGridDim + 1 + 7) / 8;
		context->Dispatch(bakeGroups, bakeGroups, 1);
		ID3D11UnorderedAccessView* nullBakeUAVs[3] = {};
		context->CSSetUnorderedAccessViews(1, 3, nullBakeUAVs, nullptr);
		// Every field ShellSurfaceZ reads is final for this frame here and is next
		// written in Prepass, which runs after the next frame's shadow mask.
		shellCastBakeValid = true;
		shellSlopeKey = shellSlopeKeyPending;
		shellSlopeKeyValid = true;
		context->CSSetShader(nullptr, nullptr, 0);
		// The field textures are written by compute passes next Prepass;
		// leave none of them bound as CS inputs.
		ID3D11ShaderResourceView* nullCSSRVs[6] = {};
		context->CSSetShaderResources(0, 6, nullCSSRVs);
		context->CSSetShaderResources(8, 1, nullCSSRVs);
		context->CSSetShaderResources(11, 2, nullCSSRVs);
		context->CSSetShaderResources(13, 3, nullCSSRVs);
		context->CSSetShaderResources(kExtentFieldSlot, 1, nullCSSRVs);
		context->CSSetShaderResources(kUntouchedTilesSlot, 1, nullCSSRVs);
		context->CSSetShaderResources(24, 1, &nullCSSRVs[0]);
		context->CSSetShaderResources(26, 1, &nullCSSRVs[1]);
		context->CSSetShaderResources(29, 1, nullCSSRVs);
		globals::profiler->EndPass();
	}
	// The stats ring pairs the shell and statics queries per slot, so a
	// gated frame issues neither.
	const bool shellStats = shellGround && shellPipelineStatsEnabled && EnsureShellStatsQueries();
	if (shellStats) {
		ReadShellStatsQueries(context);
		context->Begin(shellStatsQuery[shellStatsRing][0].get());
		context->Begin(shellStatsQuery[shellStatsRing][1].get());
	}
	if (!shellGround) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Shell");
	} else {
		globals::profiler->BeginPass(shellSplitTiming ? "SnowDeformation::Ground - Shell Setup" : "SnowDeformation::Ground - Shell");
		if (tessellate) {
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
			context->VSSetShader(tessVS, nullptr, 0);
			context->HSSetShader(tessHS, nullptr, 0);
			context->DSSetShader(tessDS, nullptr, 0);
			context->HSSetConstantBuffers(0, 1, cbs);
			context->DSSetConstantBuffers(0, 1, cbs);
			// SharedData (b5): the skirt descent reads the EM height-blending
			// gate; bind the b4-b6 triple exactly as the PS gets it above.
			context->DSSetConstantBuffers(4, 3, sharedBuffers);
			// t0-t5, not just the deformation map at t1: the bare-ground cull reads
			// the terrain window (t0) and the object height field (t4), and an
			// unbound t0 samples as zeros - which reads as fully bare and would
			// cull the entire shell.
			context->HSSetShaderResources(0, 6, shellSRVs);
			context->DSSetShaderResources(0, 6, shellSRVs);
			context->HSSetShaderResources(13, 1, &fineSRV);
			context->DSSetShaderResources(13, 1, &fineSRV);
			context->HSSetShaderResources(27, 2, waterSRVs);
			context->DSSetShaderResources(27, 2, waterSRVs);
			context->HSSetShaderResources(19, 1, &waterNearSRV);
			context->DSSetShaderResources(19, 1, &waterNearSRV);
			ID3D11ShaderResourceView* hsBakeSRV = bake ? shellVertexBake->srv.get() : nullptr;
			context->HSSetShaderResources(9, 1, &hsBakeSRV);
			ID3D11ShaderResourceView* dsHeightSRV = shellSnowHeightSRV.get();
			context->DSSetShaderResources(8, 1, &dsHeightSRV);
			context->DSSetShaderResources(11, 2, objectCapSRVs);
			context->DSSetShaderResources(14, 1, &bermSRV);
			context->DSSetShaderResources(kExtentFieldSlot, 1, &extentSRV);
			context->DSSetShaderResources(15, 1, &exclusionSRV);
			context->DSSetShaderResources(29, 1, &undulationSRV);
			ID3D11ShaderResourceView* dsBakeSRV = bake ? shellVertexBake->srv.get() : nullptr;
			context->DSSetShaderResources(9, 1, &dsBakeSRV);
			ID3D11ShaderResourceView* dsBakeSlopeSRV = bake ? shellVertexBakeSlope->srv.get() : nullptr;
			context->DSSetShaderResources(23, 1, &dsBakeSlopeSRV);
			ID3D11SamplerState* dsSampler = shellSnowSampler.get();
			context->DSSetSamplers(0, 1, &dsSampler);

			// Split draw: only the far field needs the depth clamp, so draw the
			// near half with the export compiled out and get early-Z rejection
			// back for the pixels that actually have occluders in front of them.
			// Skipped when the clamp is already off (one pass, nothing to keep) or
			// when the LOD heatmap owns the PS, and it falls back to the single
			// draw if either variant failed to compile.
			// Depth prepass, three draws: (1) the whole grid with a cheap PS that
			// runs the alpha cut and writes the clamped depth into the main
			// buffer as before, plus every winning fragment's RASTER depth into
			// shellRasterDepth (cleared to 0); (2) a fullscreen fill that turns
			// that into shellTestDepth, a private depth buffer; (3) the shading
			// draw against shellTestDepth under EQUAL with writes off. Raster
			// depth is bit-identical between draws of the same geometry, and 0
			// matches no fragment, so the EQUAL test is a hardware early-Z that
			// admits exactly the pixels the prepass wrote - occluded, self-hidden
			// and alpha-cut fragments never launch the shader. A test against the
			// main buffer cannot do this: the clamps write a depth that is not
			// the rasterised one, and a GREATER_EQUAL variant that let a software
			// test decide launched the full shader on every occluded fragment and
			// measured slower than no prepass at all.
			auto mainDepthSRV = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
			const bool prepassWanted = !shellDepthPrepassDisabled && !shellDepthClampDisabled && !lodHeatmap;
			// The hidden-fragment exit reads Terrain Blending's blended depth as the main depth.
			const bool keepHidden = shellPrepassExitOff || !globals::features::terrainBlending.loaded || !globals::features::terrainBlending.settings.Enabled;
			auto* psPrepass = prepassWanted ? GetShellPSPrepass(false, keepHidden) : nullptr;
			auto* psPrepassMain = psPrepass ? GetShellPSNoDepth() : nullptr;
			auto* fillVS = psPrepassMain ? GetShellFillVS() : nullptr;
			auto* fillPS = fillVS ? GetShellFillPS() : nullptr;
			const bool prepass = fillPS && EnsurePrepassResources(mainDepthSRV);
			shellPrepassThisFrame = prepass;
			const bool splitWanted = !prepass && !shellDepthClampDisabled && !lodHeatmap && !shellSplitDisabled;
			auto* hsNear = splitWanted ? GetShellHSNear(bake) : nullptr;
			auto* hsFar = hsNear ? GetShellHSFar(bake) : nullptr;
			if (splitWanted && bake && !hsFar) {
				hsNear = GetShellHSNear(false);
				hsFar = hsNear ? GetShellHSFar(false) : nullptr;
			}
			auto* psNear = hsFar ? GetShellPSNoDepth() : nullptr;
			auto splitRow = [&](const char* a_name) {
				if (shellSplitTiming) {
					globals::profiler->EndPass();
					globals::profiler->BeginPass(a_name);
				}
			};
			if (prepass) {
				// Visible-patch cull: the prepass records each pixel's winning
				// patch, the fill folds that into one flag per patch, and the
				// shading pass's hull skips every patch that won nothing.
				// Deferred shading: no shading geometry, so no visible-patch flags
				// or lists either. Debug views keep the raster path.
				const bool deferred = shellDeferredShading && cbData.ShellDebugData == 0 && cbData.ShellLODDebug == 0 && !lodHeatmap &&
				                      EnsureDeferredResources() && GetShellPSPrepassAttr(keepHidden) && GetShellPSDeferred();
				shellDeferredThisFrame = deferred;
				auto* hsVisible = !deferred && shellVisibleCull && EnsureVisiblePatchResources() ? GetShellHSVisible(bake) : nullptr;
				auto* hsTrust = hsVisible && !shellHullTrustOff ? GetShellHSVisible(bake, true) : nullptr;
				auto* hsMain = hsTrust ? hsTrust : hsVisible;
				auto* fillPSVisible = hsVisible ? GetShellFillPSVisible() : nullptr;
				auto* psPrepassPatch = fillPSVisible ? GetShellPSPrepass(true, keepHidden) : nullptr;
				auto* dsPrepassPatch = psPrepassPatch ? GetShellDSPatch(bake, shellVertexBakeCheck) : nullptr;
				const bool visibleCull = dsPrepassPatch != nullptr;
				if (visibleCull)
					psPrepass = psPrepassPatch;
				if (deferred)
					psPrepass = GetShellPSPrepassAttr(keepHidden);

				splitRow("SnowDeformation::Ground - Shell Depth Prepass");
				const float rasterClear[4] = {};
				context->ClearRenderTargetView(shellRasterDepth->rtv.get(), rasterClear);
				ID3D11RenderTargetView* prepassRTVs[4] = { shellRasterDepth->rtv.get(), visibleCull ? shellRasterPatch->rtv.get() : nullptr, nullptr, nullptr };
				if (visibleCull)
					context->ClearRenderTargetView(prepassRTVs[1], rasterClear);
				// The attributes are read only where the prepass won, so no clear.
				if (deferred) {
					for (int i = 0; i < 3; i++)
						prepassRTVs[1 + i] = shellDeferredAttr[i]->rtv.get();
				}
				context->OMSetRenderTargets(deferred ? 4 : (visibleCull ? 2 : 1), prepassRTVs, dsv);
				// The prepass PS reads few of the DS outputs; the lean bake DS writes
				// only those. The shading draw binds tessDS again.
				auto* dsLean = bake && !shellVertexBakeCheck ? GetShellDSLean(visibleCull) : nullptr;
				if (dsLean)
					context->DSSetShader(dsLean, nullptr, 0);
				else if (visibleCull)
					context->DSSetShader(dsPrepassPatch, nullptr, 0);
				const bool prepassLastFrame = visibleCull && shellPrepassLastFrame && shellVisiblePrevValid && shellVisiblePrevSRV;
				if (prepassLastFrame) {
					ID3D11ShaderResourceView* prevSRV = shellVisiblePrevSRV.get();
					context->HSSetShader(hsVisible, nullptr, 0);
					context->HSSetShaderResources(60, 1, &prevSRV);
				}
				context->PSSetShader(psPrepass, nullptr, 0);
				// The last-frame hull indexes its flags by SV_PrimitiveID, which
				// restarts at the first patch drawn.
				uint32_t firstRow = 0, endRow = kShellGridDim;
				if (!prepassLastFrame)
					ShellSeamRows(cbData, firstRow, endRow);
				context->Draw((endRow - firstRow) * kShellGridDim * 4, firstRow * kShellGridDim * 4);
				if (prepassLastFrame) {
					ID3D11ShaderResourceView* nullPrevSRV = nullptr;
					context->HSSetShaderResources(60, 1, &nullPrevSRV);
					context->HSSetShader(tessHS, nullptr, 0);
				}

				// Deferred: the fill is ~0.013 ms of its own plus the prepass's
				// drain, so it stays in the prepass row.
				if (!deferred)
					splitRow("SnowDeformation::Ground - Shell Visible Patches");
				else if (shellSplitTiming)
					globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Shell Visible Patches");
				if (visibleCull) {
					const UINT zeros[4] = {};
					context->ClearUnorderedAccessViewUint(shellVisiblePatchesUAV.get(), zeros);
					ID3D11UnorderedAccessView* visibleUAV = shellVisiblePatchesUAV.get();
					context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, shellTestDepthDSV.get(), 1, 1, &visibleUAV, nullptr);
					ID3D11ShaderResourceView* patchSRV = shellRasterPatch->srv.get();
					context->PSSetShaderResources(61, 1, &patchSRV);
				} else {
					context->OMSetRenderTargets(0, nullptr, shellTestDepthDSV.get());
				}
				context->OMSetDepthStencilState(shellFillDepthState.get(), 0);
				context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				context->VSSetShader(fillVS, nullptr, 0);
				context->HSSetShader(nullptr, nullptr, 0);
				context->DSSetShader(nullptr, nullptr, 0);
				ID3D11ShaderResourceView* rasterSRV = shellRasterDepth->srv.get();
				context->PSSetShaderResources(9, 1, &rasterSRV);
				context->PSSetShader(visibleCull ? fillPSVisible : fillPS, nullptr, 0);
				context->Draw(3, 0);
				ID3D11ShaderResourceView* nullRasterSRV = nullptr;
				context->PSSetShaderResources(9, 1, &nullRasterSRV);
				if (visibleCull) {
					context->PSSetShaderResources(61, 1, &nullRasterSRV);
					ID3D11UnorderedAccessView* nullUAV = nullptr;
					context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1, &nullUAV, nullptr);
					if (shellMainCullAll) {
						const UINT zeros[4] = {};
						context->ClearUnorderedAccessViewUint(shellVisiblePatchesUAV.get(), zeros);
					}
				}
				// Patch split: the visible patches into a flat and a tessellated
				// list, by the prepass hull's own verdict.
				const bool split = visibleCull && shellPatchSplit && bake && !shellVertexBakeCheck && EnsurePatchListResources() &&
				                   GetShellPatchListCS() && GetShellTessVSList() && GetShellHSListMain() && GetShellFlatVS();
				shellPatchSplitThisFrame = split;
				if (split) {
					ReadPatchListCounts(context);
					const UINT zeros[4] = {};
					context->ClearUnorderedAccessViewUint(shellFlatArgsUAV.get(), zeros);
					context->ClearUnorderedAccessViewUint(shellTessArgsUAV.get(), zeros);
					ID3D11ShaderResourceView* visibleInSRV = shellVisiblePatchesSRV.get();
					context->CSSetShaderResources(60, 1, &visibleInSRV);
					ID3D11UnorderedAccessView* listUAVs[4] = { shellFlatPatchesUAV.get(), shellTessPatchesUAV.get(), shellFlatArgsUAV.get(), shellTessArgsUAV.get() };
					context->CSSetUnorderedAccessViews(0, 4, listUAVs, nullptr);
					context->CSSetShader(shellPatchListCS, nullptr, 0);
					context->Dispatch((kShellGridDim * kShellGridDim + 63) / 64, 1, 1);
					ID3D11UnorderedAccessView* nullListUAVs[4] = {};
					context->CSSetUnorderedAccessViews(0, 4, nullListUAVs, nullptr);
					ID3D11ShaderResourceView* nullVisibleInSRV = nullptr;
					context->CSSetShaderResources(60, 1, &nullVisibleInSRV);
					context->CSSetShader(nullptr, nullptr, 0);
					// The counts, read back kShellPatchArgsRing frames later.
					context->CopyResource(shellPatchArgsStaging[shellPatchArgsRing][0].get(), shellFlatArgs.get());
					context->CopyResource(shellPatchArgsStaging[shellPatchArgsRing][1].get(), shellTessArgs.get());
					shellPatchArgsRing = (shellPatchArgsRing + 1) % kShellPatchArgsRing;
				}

				splitRow("SnowDeformation::Ground - Shell Shading");
				context->OMSetRenderTargets(8, rtvs, shellTestDepthDSV.get());
				if (deferred) {
					// One full-screen triangle at z 0: LESS passes wherever the
					// fill left a prepass winner's depth, rejects the cleared 0.
					context->OMSetDepthStencilState(shellDeferredDepthState.get(), 0);
					context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					context->VSSetShader(fillVS, nullptr, 0);
					context->HSSetShader(nullptr, nullptr, 0);
					context->DSSetShader(nullptr, nullptr, 0);
					ID3D11ShaderResourceView* attrSRVs[3] = { shellDeferredAttr[0]->srv.get(), shellDeferredAttr[1]->srv.get(), shellDeferredAttr[2]->srv.get() };
					context->PSSetShaderResources(91, 3, attrSRVs);
					context->PSSetShader(shellPSDeferred, nullptr, 0);
					context->Draw(3, 0);
					ID3D11ShaderResourceView* nullAttrSRVs[3] = {};
					context->PSSetShaderResources(91, 3, nullAttrSRVs);
				}
				context->OMSetDepthStencilState(shellPrepassMainDepthState.get(), 0);
				context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
				context->VSSetShader(tessVS, nullptr, 0);
				context->HSSetShader(visibleCull ? hsMain : tessHS, nullptr, 0);
				if (visibleCull) {
					ID3D11ShaderResourceView* visibleSRV = shellVisiblePatchesSRV.get();
					context->HSSetShaderResources(60, 1, &visibleSRV);
				}
				context->DSSetShader(tessDS, nullptr, 0);
				context->PSSetShader(psPrepassMain, nullptr, 0);
				if (split)
					DrawShellPatchLists(context, tessVS, tessDS);
				else if (!deferred)
					context->Draw(kShellGridDim * kShellGridDim * 4, 0);
				// Lane census: the same lists again, no depth test, the PS
				// deciding EQUAL itself and counting live lanes and quads.
				const bool censusPending = laneCensusPending[0] || laneCensusPending[1] || laneCensusPending[2];
				if (split && (laneCensusFramesLeft > 0 || censusPending) && EnsureLaneCensusResources() && GetShellPSCensus()) {
					ReadLaneCensus(context);
					if (laneCensusFramesLeft > 0 && !laneCensusPending[laneCensusRing]) {
						const UINT zeros[4] = {};
						context->ClearUnorderedAccessViewUint(laneCensusUAV.get(), zeros);
						ID3D11UnorderedAccessView* censusUAV = laneCensusUAV.get();
						context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &censusUAV, nullptr);
						ID3D11ShaderResourceView* testDepthSRV = shellTestDepthSRV.get();
						context->PSSetShaderResources(61, 1, &testDepthSRV);
						context->PSSetShader(shellPSCensus, nullptr, 0);
						DrawShellPatchLists(context, tessVS, tessDS);
						ID3D11ShaderResourceView* nullTestSRV = nullptr;
						context->PSSetShaderResources(61, 1, &nullTestSRV);
						ID3D11UnorderedAccessView* nullCensusUAV = nullptr;
						context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &nullCensusUAV, nullptr);
						context->CopyResource(laneCensusStaging[laneCensusRing].get(), laneCensusBuffer.get());
						laneCensusPending[laneCensusRing] = true;
						laneCensusRing = (laneCensusRing + 1) % kLaneCensusRing;
						laneCensusFramesLeft--;
					}
					context->OMSetRenderTargets(8, rtvs, shellTestDepthDSV.get());
				}
				if (visibleCull) {
					ID3D11ShaderResourceView* nullVisibleSRV = nullptr;
					context->HSSetShaderResources(60, 1, &nullVisibleSRV);
				}
				shellVisiblePrevValid = visibleCull && shellPrepassLastFrame && !shellMainCullAll && shellVisiblePrev;
				if (shellVisiblePrevValid)
					context->CopyResource(shellVisiblePrev.get(), shellVisiblePatches.get());

				context->OMSetRenderTargets(8, rtvs, dsv);
				context->OMSetDepthStencilState(shellDepthState.get(), 0);
			} else if (hsNear && hsFar && psNear) {
				context->HSSetShader(hsNear, nullptr, 0);
				context->PSSetShader(psNear, nullptr, 0);
				context->Draw(kShellGridDim * kShellGridDim * 4, 0);

				context->HSSetShader(hsFar, nullptr, 0);
				context->PSSetShader(ps, nullptr, 0);
				context->Draw(kShellGridDim * kShellGridDim * 4, 0);
			} else {
				context->Draw(kShellGridDim * kShellGridDim * 4, 0);
			}
			// The statics pass and everything after run the normal pipeline.
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		} else {
			context->VSSetShader(vs, nullptr, 0);
			DrawShellGrid(context, cbData);
		}
		globals::profiler->EndPass();
	}
	if (shellStats) {
		context->End(shellStatsQuery[shellStatsRing][1].get());
		context->End(shellStatsQuery[shellStatsRing][0].get());
	}

	if (lodHeatmap) {
		// Statics + the depth copy below expect the full G-buffer and the
		// standard depth state; rebinding also unbinds the histogram UAV
		// before its CopyResource.
		context->OMSetRenderTargets(8, rtvs, dsv);
		context->OMSetDepthStencilState(shellDepthState.get(), 0);
		context->CopyResource(lodHistogramStaging[lodReadbackRing].get(), lodHistogram.get());
		lodHistStagingValid[lodReadbackRing] = true;
	}

	// The post-shell depth copy that used to run here went with the seam
	// cross-fade it served. Restoring the pair-3 contest needs it back first;
	// see HEIGHT-BLEND-PLAN.md.

	// Captured projected-snow statics, inflated with the same material.
	// Inherits this pass's bindings (b0, t0-t8, s0, b4-b6, RTs, depth).
	// Their own raster state carries the skin depth bias: a skin and its
	// mesh rasterise the same vertices, so without a bias the far ones
	// z-fight.
	spanClock.Lap(CpuCensus::kSpanShellGrid);
	if (auto* skinRaster = GetSkinRasterState())
		context->RSSetState(skinRaster);
	if (shellStats) {
		context->Begin(staticsStatsQuery[shellStatsRing][0].get());
		context->Begin(staticsStatsQuery[shellStatsRing][1].get());
	}
	DrawCapturedStatics();
	spanClock.Lap(CpuCensus::kSpanStatics);
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
	// The extent field and the untouched mask are compute targets next Prepass.
	context->PSSetShaderResources(kExtentFieldSlot, 1, &nullFinePS[0]);
	context->PSSetShaderResources(kUntouchedTilesSlot, 1, &nullFinePS[0]);
	context->VSSetShaderResources(kExtentFieldSlot, 1, &nullFinePS[0]);
	context->DSSetShaderResources(kExtentFieldSlot, 1, &nullFinePS[0]);
	context->PSSetShaderResources(26, 1, &nullFinePS[1]);
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

	RunLODProbePass();
	spanClock.Lap(CpuCensus::kSpanLODProbe);

	// SSGI seam shield mask for the composite: the window's fringe band,
	// gated to real snow nearby (see SeamShieldCS). Its only input is the
	// terrain window, so it is rebuilt when the window is.
	if (shellGround && shellTerrainTexture && shellTerrainTexture->srv && EnsureSeamMask()) {
		if (auto cs = GetSeamShieldCS()) {
			if (seamMaskTerrainVersion != shellTerrainVersion) {
				ID3D11ShaderResourceView* maskSRV = shellTerrainTexture->srv.get();
				ID3D11UnorderedAccessView* maskUAV = seamMaskTexture->uav.get();
				context->CSSetShaderResources(5, 1, &maskSRV);
				context->CSSetUnorderedAccessViews(1, 1, &maskUAV, nullptr);
				context->CSSetShader(cs, nullptr, 0);
				globals::profiler->BeginPass("SnowDeformation::Ground - Seam Shield");
				context->Dispatch((kShellWindowDim + 7) / 8, (kShellWindowDim + 7) / 8, 1);
				globals::profiler->EndPass();
				ID3D11ShaderResourceView* nullMaskSRV = nullptr;
				ID3D11UnorderedAccessView* nullMaskUAV = nullptr;
				context->CSSetShaderResources(5, 1, &nullMaskSRV);
				context->CSSetUnorderedAccessViews(1, 1, &nullMaskUAV, nullptr);
				context->CSSetShader(nullptr, nullptr, 0);
				seamMaskTerrainVersion = shellTerrainVersion;
			} else {
				globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Seam Shield");
			}
			seamMaskFrame = globals::state->frameCount;
		}
	}

	// Screen-space passes running after us (SSGI, SSS, the shadow mask) read
	// Terrain Blending's blended depth, finalized during opaque rendering;
	// without a sync they see buried geometry poking through the snow and
	// paint occlusion, shadow and GI from under the sheet. min() the fresh
	// depth into both blended copies (DSV is unbound again at this point),
	// on the module's own pixels only: the shell prepass's mark, or a depth
	// the snow passes moved nearer than the pre-snow copy (skins, patch).
	auto& tb = globals::features::terrainBlending;
	const bool rasterMarks = shellGround && shellPrepassThisFrame && shellRasterDepth && shellRasterDepth->srv;
	const bool syncable = rasterMarks || preSnowDepthThisFrame;
	if (!syncable)
		globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Depth Sync");
	if (syncable && tb.loaded && tb.settings.Enabled && tb.blendedDepthTexture && tb.blendedDepthTexture16) {
		if (auto cs = GetDepthSyncCS(); cs && GetPreSnowCopyCS() && depthSyncRectCB) {
			auto mainDepthSRV = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
			ID3D11UnorderedAccessView* syncUAVs[2] = { tb.blendedDepthTexture->uav.get(), tb.blendedDepthTexture16->uav.get() };
			ID3D11ShaderResourceView* syncSRVs[3] = { mainDepthSRV,
				rasterMarks ? shellRasterDepth->srv.get() : nullptr,
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

// ---- Distant-snow / LOD diagnostics ----

bool SnowDeformation::EnsureLODDebugResources()
{
	if (shellLODDepthState && lodHistogram && lodProbeBuffer)
		return true;

	auto device = globals::d3d::device;

	D3D11_DEPTH_STENCIL_DESC depthDesc{};
	depthDesc.DepthEnable = TRUE;
	depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
	if (FAILED(device->CreateDepthStencilState(&depthDesc, shellLODDepthState.put())))
		return false;
	Util::SetResourceName(shellLODDepthState.get(), "SnowDeformation::ShellLODDepthState");

	auto makeStructured = [&](uint32_t a_count, uint32_t a_stride, const char* a_name,
							  winrt::com_ptr<ID3D11Buffer>& a_buf, winrt::com_ptr<ID3D11UnorderedAccessView>& a_uav,
							  winrt::com_ptr<ID3D11Buffer>(&a_staging)[2]) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = a_count * a_stride;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = a_stride;
		if (FAILED(device->CreateBuffer(&desc, nullptr, a_buf.put())))
			return false;
		Util::SetResourceName(a_buf.get(), a_name);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = a_count;
		if (FAILED(device->CreateUnorderedAccessView(a_buf.get(), &uavDesc, a_uav.put())))
			return false;

		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = a_count * a_stride;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (int i = 0; i < 2; ++i)
			if (FAILED(device->CreateBuffer(&stagingDesc, nullptr, a_staging[i].put())))
				return false;
		return true;
	};

	if (!makeStructured(kLODHistBands * kLODHistBuckets, 4, "SnowDeformation::LODHistogram", lodHistogram, lodHistogramUAV, lodHistogramStaging))
		return false;
	if (!makeStructured(kLODProbeCount, 4, "SnowDeformation::LODProbeHeights", lodProbeBuffer, lodProbeUAV, lodProbeStaging))
		return false;
	return true;
}

ID3D11ComputeShader* SnowDeformation::GetLODProbeCS()
{
	if (!lodProbeCS) {
		logger::debug("Compiling SnowShell LOD probe CS");
		lodProbeCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", {}, "cs_5_0"));
	}
	return lodProbeCS;
}

/** @brief Distance band of a probe/pixel, matching the shader's histogram banding. */
static uint32_t LODBandOfRadius(float a_radius)
{
	return a_radius < 4000.0f ? 0u : (a_radius < 8000.0f ? 1u : (a_radius < 16000.0f ? 2u : 3u));
}

void SnowDeformation::RunLODProbePass()
{
	auto context = globals::d3d::context;

	if (lodShimmerMeter && EnsureLODDebugResources()) {
		if (auto cs = GetLODProbeCS()) {
			ID3D11Buffer* csCB = shellCB->CB();
			context->CSSetConstantBuffers(0, 1, &csCB);
			// ShellSurfaceZ reads the terrain window, deformation map and the
			// object height/shelter/cap fields; t2/t3 (snow diffuse, scene
			// depth) are unused by the surface math.
			ID3D11ShaderResourceView* csSRVs[6] = { shellTerrainTexture->srv.get(), GetDeformationSRV(), nullptr, nullptr, heightTopFiltered->srv.get(), heightBottomFiltered->srv.get() };
			context->CSSetShaderResources(0, 6, csSRVs);
			ID3D11ShaderResourceView* probeFineSRV = (shellFineValid && shellTerrainFine) ? shellTerrainFine->srv.get() : nullptr;
			context->CSSetShaderResources(13, 1, &probeFineSRV);
			ID3D11ShaderResourceView* probeWaterSRV = waterHeightTexture ? waterHeightTexture->srv.get() : nullptr;
			context->CSSetShaderResources(27, 1, &probeWaterSRV);
			ID3D11ShaderResourceView* csCapSRVs[2] = { heightTopRaw[heightCurrent]->srv.get(), heightSkinDepth->srv.get() };
			context->CSSetShaderResources(11, 2, csCapSRVs);
			// ShellSurfaceZ's berm term reads the bake at t14.
			ID3D11ShaderResourceView* csBermSRV = GetBermFieldSRV();
			context->CSSetShaderResources(14, 1, &csBermSRV);
			ID3D11ShaderResourceView* csExtentSRV = GetExtentFieldSRV();
			context->CSSetShaderResources(kExtentFieldSlot, 1, &csExtentSRV);
			ID3D11ShaderResourceView* csExclusionSRV = GetExclusionFieldSRV();
			context->CSSetShaderResources(15, 1, &csExclusionSRV);
			ID3D11UnorderedAccessView* probeUAV = lodProbeUAV.get();
			context->CSSetUnorderedAccessViews(0, 1, &probeUAV, nullptr);
			context->CSSetShader(cs, nullptr, 0);
			globals::profiler->BeginPass("SnowDeformation::Ground - LOD Probes");
			context->Dispatch((kLODProbeCount + 63) / 64, 1, 1);
			globals::profiler->EndPass();

			ID3D11UnorderedAccessView* nullUAV = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			ID3D11ShaderResourceView* nullSRVs[6] = {};
			context->CSSetShaderResources(0, 6, nullSRVs);
			context->CSSetShaderResources(11, 2, nullSRVs);
			context->CSSetShaderResources(14, 1, nullSRVs);
			context->CSSetShaderResources(kExtentFieldSlot, 1, nullSRVs);
			context->CSSetShaderResources(15, 1, nullSRVs);
			context->CSSetShaderResources(13, 1, nullSRVs);
			context->CSSetShaderResources(27, 1, nullSRVs);
			context->CSSetShaderResources(24, 1, &nullSRVs[0]);
			context->CSSetShaderResources(26, 1, &nullSRVs[1]);
			ID3D11Buffer* nullCB = nullptr;
			context->CSSetConstantBuffers(0, 1, &nullCB);
			context->CSSetShader(nullptr, nullptr, 0);

			context->CopyResource(lodProbeStaging[lodReadbackRing].get(), lodProbeBuffer.get());
			lodProbeStagingValid[lodReadbackRing] = true;
			auto eyeFB = globals::game::frameBufferCached.GetCameraPosAdjust();
			lodProbeAnchorAtCopy[lodReadbackRing] = { std::floor(eyeFB.x / 512.0f) * 512.0f, std::floor(eyeFB.y / 512.0f) * 512.0f };
		}
	}

	ReadbackLODDiagnostics();
	lodReadbackRing ^= 1;
}

void SnowDeformation::ReadbackLODDiagnostics()
{
	auto context = globals::d3d::context;
	const int mapRing = lodReadbackRing ^ 1;

	// Both maps target last frame's copies; DO_NOT_WAIT keeps a slow frame
	// from stalling the render thread (the table just lags one more frame).
	if (lodHistStagingValid[mapRing]) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(lodHistogramStaging[mapRing].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
			std::memcpy(lodHistData, mapped.pData, sizeof(lodHistData));
			context->Unmap(lodHistogramStaging[mapRing].get(), 0);
			lodHistStagingValid[mapRing] = false;
		}
	}

	if (!lodProbeStagingValid[mapRing])
		return;
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(lodProbeStaging[mapRing].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
		return;
	lodProbeStagingValid[mapRing] = false;

	const float* heights = static_cast<const float*>(mapped.pData);
	const float2 anchor = lodProbeAnchorAtCopy[mapRing];
	const bool sameAnchor = lodProbePrevValid && anchor.x == lodProbeAnchor.x && anchor.y == lodProbeAnchor.y;

	float sum[kLODHistBands] = {};
	float mx[kLODHistBands] = {};
	uint32_t hops[kLODHistBands] = {};
	uint32_t cnt[kLODHistBands] = {};
	uint32_t validCnt[kLODHistBands] = {};

	for (uint32_t i = 0; i < kLODProbeCount; ++i) {
		const float h = heights[i];
		const uint32_t band = LODBandOfRadius(kLODProbeRadius[i / kLODProbeAzimuths]);
		const bool nowValid = h < 1.0e37f;
		if (nowValid)
			validCnt[band]++;
		if (sameAnchor && nowValid && lodProbePrev[i] < 1.0e37f) {
			const float d = std::abs(h - lodProbePrev[i]);
			sum[band] += d;
			mx[band] = std::max(mx[band], d);
			if (d > 1.0f)
				hops[band]++;
			cnt[band]++;
		}
		lodProbePrev[i] = h;
	}
	context->Unmap(lodProbeStaging[mapRing].get(), 0);

	// Pause frames (anchor requantized, cnt 0) publish NOTHING. Writing their
	// zeros used to land a full row of 0.00 in a readout that is per-frame, so
	// a screenshot taken on one read as a perfectly stable far field while the
	// valid counts stayed populated - the exact false negative that voided the
	// first C3 A/B round.
	for (uint32_t band = 0; band < kLODHistBands; ++band)
		lodShimmerValid[band] = validCnt[band];
	uint32_t totalCnt = 0;
	for (uint32_t band = 0; band < kLODHistBands; ++band)
		totalCnt += cnt[band];

	if (totalCnt != 0) {
		for (uint32_t band = 0; band < kLODHistBands; ++band) {
			lodShimmerMax[band] = mx[band];
			lodShimmerAvg[band] = cnt[band] ? sum[band] / cnt[band] : 0.0f;
			lodShimmerHops[band] = hops[band];
			lodShimmerHistoryBuf[band][lodShimmerHistoryIdx] = mx[band];
			// Windowed accumulators: a single frame is not a measurement, and
			// the readout is what gets screenshotted.
			lodShimmerRunMax[band] = std::max(lodShimmerRunMax[band], mx[band]);
			lodShimmerRunSum[band] += sum[band];
			lodShimmerRunCnt[band] += cnt[band];
			lodShimmerRunHops[band] += hops[band];
		}
		lodShimmerRunFrames++;
		lodShimmerHistoryIdx = (lodShimmerHistoryIdx + 1) % kLODShimmerHistory;
	}
	lodProbePrevValid = true;
	lodProbeAnchor = anchor;
}
