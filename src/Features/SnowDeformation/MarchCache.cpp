// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Features/ExponentialHeightFog.h"
#include "Features/IBL.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <bit>
#include <chrono>

// Cached Sun March (A/B, Debugging Options). The sun heightfield marches of
// the ground, the trench patch and the object snow rebuild every occluder
// height per tap: terrain, land layer, melt, carve, berm, dunes - two dependent
// fetch rounds per tap, five taps a pixel. Two caches hold those heights per
// deformation texel in the map's own torus instead, so a tap is one gather:
// [0] the ground march's tap height (SnowShell.hlsl MarchHeightCS), [1] the
// statics march's road tap (SnowStaticsShell.hlsl RoadMarchHeightCS).
//
// Kept current by the berm bake's tile list (the map or the berm field changed
// there; the scroll's arriving strips are on it too), a band of rows a frame
// for the slow inputs (snowfall, objects coming and going), a faster band after
// a terrain or land-layer rebuild, and a full build when switched on, resized,
// re-set, or after a berm bake the caches missed.

namespace
{
	constexpr uint32_t kMarchCacheRows = 32;
	constexpr uint32_t kMarchCacheFastRows = 256;
	constexpr UINT kMarchTilesSlot = 63;
	constexpr UINT kMarchGroundSlot = 106;
	constexpr UINT kMarchRoadSlot = 107;
	const char* const kStaticsMCNames[4] = { "SnowDeformation::StaticsShellPS MarchCache", "SnowDeformation::StaticsShellPS NoDepth MarchCache",
		"SnowDeformation::StaticsShellPS Drape NoDepth MarchCache", "SnowDeformation::TrenchPatchPS MarchCache" };
}

ID3D11ComputeShader* SnowDeformation::GetMarchHeightCS(bool a_road, bool a_rows)
{
	auto*& slot = marchHeightCS[a_road ? 1 : 0][a_rows ? 1 : 0];
	if (!slot) {
		if (a_road) {
			logger::debug("Compiling SnowStaticsShell {}", a_rows ? "RoadMarchRowsCS" : "RoadMarchTilesCS");
			slot = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowStaticsShell.hlsl", { { "SNOW_ROAD_MARCH_CS", "" } }, "cs_5_0",
				a_rows ? "RoadMarchRowsCS" : "RoadMarchTilesCS"));
		} else {
			logger::debug("Compiling SnowShell {}", a_rows ? "MarchHeightRowsCS" : "MarchHeightTilesCS");
			slot = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "SNOW_MARCH_HEIGHT_CS", "" } }, "cs_5_0",
				a_rows ? "MarchHeightRowsCS" : "MarchHeightTilesCS"));
		}
	}
	return slot;
}

void SnowDeformation::ReleaseMarchCache()
{
	for (auto& pair : marchHeightCS)
		for (auto*& cs : pair) {
			if (cs)
				cs->Release();
			cs = nullptr;
		}
	// A job in flight keeps its future: its shader replaces the slot when it
	// lands, built from whatever sources it started from.
	for (int p = 0; p < 2; p++) {
		if (shellPSDeferredMC[p])
			shellPSDeferredMC[p]->Release();
		shellPSDeferredMC[p] = nullptr;
		shellPSDeferredMCTried[p] = false;
	}
	for (int i = 0; i < 4; i++) {
		if (staticsPSMC[i])
			staticsPSMC[i]->Release();
		staticsPSMC[i] = nullptr;
		staticsPSMCTried[i] = false;
	}
	marchCacheValid = false;
}

ID3D11PixelShader* SnowDeformation::GetShellPSDeferredMC(bool a_parallax)
{
	const int p = a_parallax ? 1 : 0;
	ID3D11PixelShader* fallback = shellPSDeferred[p];
	if (!marchCacheReady)
		return fallback;
	auto& job = shellPSDeferredMCJob[p];
	if (job.valid()) {
		if (job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return fallback;
		if (shellPSDeferredMC[p])
			shellPSDeferredMC[p]->Release();
		shellPSDeferredMC[p] = job.get();
		shellPSDeferredMCBits[p] = shellPSDeferredMCJobBits[p];
		shellPSDeferredMCTried[p] = true;
		if (!shellPSDeferredMC[p])
			logger::warn("[SNOW DEFORMATION] Cached Sun March: the deferred variant failed to compile; the live march stays");
	}
	if (shellPSDeferredMCTried[p] && shellPSDeferredMCBits[p] == shellPSLegacyBits)
		return shellPSDeferredMC[p] ? shellPSDeferredMC[p] : fallback;
	const uint32_t legacy = shellPSLegacyBits;
	shellPSDeferredMCJobBits[p] = legacy;
	job = std::async(std::launch::async, [this, a_parallax, legacy]() -> ID3D11PixelShader* {
		auto defines = ShellPSDefines();
		defines.emplace_back("SNOW_SHELL_DEFERRED", "");
		if (legacy & 1)
			defines.emplace_back("SNOW_TEETH_UNGATED", "");
		if (legacy & 4)
			defines.emplace_back("SNOW_UNTOUCHED_SKIP_OFF", "");
		if (a_parallax)
			defines.emplace_back("SNOW_TRENCH_PARALLAX", "");
		defines.emplace_back("SNOW_MARCH_CACHE", "");
		return static_cast<ID3D11PixelShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ps_5_0"));
	});
	return fallback;
}

ID3D11PixelShader* SnowDeformation::MarchCachePS(ID3D11PixelShader* a_ps)
{
	if (!marchCacheReady || !a_ps)
		return a_ps;
	const int i = a_ps == staticsPS ? 0 : a_ps == staticsPSNoDepth  ? 1 :
	                                  a_ps == staticsPSDrapeNoDepth ? 2 :
	                                  a_ps == patchPS               ? 3 :
	                                                                  -1;
	if (i < 0)
		return a_ps;
	auto& job = staticsPSMCJob[i];
	if (job.valid()) {
		if (job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return a_ps;
		if (staticsPSMC[i])
			staticsPSMC[i]->Release();
		staticsPSMC[i] = job.get();
		staticsPSMCTried[i] = true;
		if (!staticsPSMC[i])
			logger::warn("[SNOW DEFORMATION] Cached Sun March: statics variant {} failed to compile; the live march stays", i);
	}
	if (staticsPSMCTried[i])
		return staticsPSMC[i] ? staticsPSMC[i] : a_ps;
	const char* ehf = globals::features::exponentialHeightFog.loaded ? "SNOW_EXP_HEIGHT_FOG" : nullptr;
	const char* ibl = globals::features::ibl.loaded ? "SNOW_IBL" : nullptr;
	job = std::async(std::launch::async, [i, ehf, ibl]() -> ID3D11PixelShader* {
		constexpr auto path = L"Data\\Shaders\\SnowDeformation\\SnowStaticsShell.hlsl";
		winrt::com_ptr<ID3DBlob> blob;
		switch (i) {
		case 0:
			blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", ehf, ibl, "SNOW_MARCH_CACHE"));
			break;
		case 1:
			blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", ehf, ibl, "SNOW_STATICS_NO_DEPTH_EXPORT", "SNOW_MARCH_CACHE"));
			break;
		case 2:
			blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", ehf, ibl, "SNOW_STATICS_NO_DEPTH_EXPORT", "SNOW_STATICS_DRAPE", "SNOW_MARCH_CACHE"));
			break;
		default:
			blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", "PATCH", ehf, ibl, "SNOW_MARCH_CACHE"));
			break;
		}
		ID3D11PixelShader* ps = nullptr;
		if (blob && SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps)))
			Util::SetResourceName(ps, kStaticsMCNames[i]);
		return ps;
	});
	return a_ps;
}

bool SnowDeformation::MarchCacheCompiling() const
{
	for (const auto& job : shellPSDeferredMCJob)
		if (job.valid() && job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return true;
	for (const auto& job : staticsPSMCJob)
		if (job.valid() && job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return true;
	return false;
}

void SnowDeformation::UnbindMarchCaches()
{
	marchCacheReady = false;
	ID3D11ShaderResourceView* nullSRVs[2] = {};
	globals::d3d::context->PSSetShaderResources(kMarchGroundSlot, 2, nullSRVs);
}

void SnowDeformation::UpdateMarchCaches(const ShellCB& a_cb)
{
	marchCacheReady = false;
	auto* deform = deformationTextures[0];
	if (!marchCacheEnabled || shellBermBakeDisabled || !deform || !deform->srv || !bermFieldTexture || !bermFieldTexture->srv || !bermTileSRV || !bermArgsBuffer ||
		!shellTerrainTexture || !shellTerrainTexture->srv || !heightTopRaw[heightCurrent] || !heightSkinDepth || !objectSnowCone || !heightBottomFiltered) {
		marchCacheValid = false;
		return;
	}
	ID3D11ComputeShader* cs[2][2] = { { GetMarchHeightCS(false, false), GetMarchHeightCS(false, true) }, { GetMarchHeightCS(true, false), GetMarchHeightCS(true, true) } };
	if (!cs[0][0] || !cs[0][1] || !cs[1][0] || !cs[1][1]) {
		marchCacheValid = false;
		return;
	}

	const uint32_t dim = deform->desc.Width;
	for (int c = 0; c < 2; c++) {
		auto*& tex = marchHeight[c];
		if (tex && tex->desc.Width == dim)
			continue;
		delete tex;
		tex = nullptr;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = dim;
		desc.Height = dim;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		tex = new Texture2D(desc, c == 0 ? "SnowDeformation::MarchHeightGround" : "SnowDeformation::MarchHeightRoad");
		tex->CreateSRV(srvDesc);
		tex->CreateUAV(uavDesc);
		marchCacheValid = false;
	}
	if (!marchHeight[0]->uav || !marchHeight[1]->uav)
		return;
	if (!marchCacheCB)
		marchCacheCB = new ConstantBuffer(ConstantBufferDesc<MarchCacheCB>(), "SnowDeformation::MarchCacheCB");

	// What moves every texel at once: the settings, the window's size, the
	// readers' gates.
	uint64_t key = 1469598103934665603ull;
	auto mix = [&](const void* a_p, size_t a_n) {
		const auto* b = static_cast<const uint8_t*>(a_p);
		for (size_t n = 0; n < a_n; n++) {
			key ^= b[n];
			key *= 1099511628211ull;
		}
	};
	const float keyFloats[] = { a_cb.DeformInvWorldSize, a_cb.UndulationAmp, a_cb.UndulationScale, a_cb.BermHeightAmp, a_cb.ObjBermHeightAmp, a_cb.RimStyle.x,
		a_cb.ShellTriHeight, a_cb.BermBakeActive, a_cb.ObjectLiftCap > 0.0f ? 1.0f : 0.0f, a_cb.ExclusionFieldWindow.w, a_cb.CompactLook.x, a_cb.FineWindow.z,
		a_cb.ObjectHeightHalfExtent, a_cb.FieldDims.x, a_cb.FieldDims.y, a_cb.FieldDims.z, a_cb.FieldDims.w };
	mix(keyFloats, sizeof(keyFloats));
	const int landExactOff = a_cb.ShellFlags.x & 4;
	mix(&landExactOff, sizeof(landExactOff));
	mix(&dim, sizeof(dim));

	const bool missedBake = bermBakeSeq > marchCacheBermSeq + 1;
	const bool full = !marchCacheValid || key != marchCacheKey || missedBake;
	const bool tiles = !full && bermBakeSeq == marchCacheBermSeq + 1;
	marchCacheBermSeq = bermBakeSeq;
	marchCacheKey = key;
	if (full) {
		marchCacheFastRows = 0;
		marchCacheTerrainVersion = shellTerrainVersion;
		marchCacheFineOrigin[0] = shellFineOriginX;
		marchCacheFineOrigin[1] = shellFineOriginY;
	} else if (shellTerrainVersion != marchCacheTerrainVersion || shellFineOriginX != marchCacheFineOrigin[0] || shellFineOriginY != marchCacheFineOrigin[1]) {
		marchCacheFastRows = dim;
		marchCacheTerrainVersion = shellTerrainVersion;
		marchCacheFineOrigin[0] = shellFineOriginX;
		marchCacheFineOrigin[1] = shellFineOriginY;
	}
	uint32_t rows = full ? dim : (marchCacheFastRows > 0 ? std::min(kMarchCacheFastRows, marchCacheFastRows) : kMarchCacheRows);
	rows = std::min(rows, dim);
	marchCacheFastRows -= std::min(marchCacheFastRows, rows);
	MarchCacheCB cbData{};
	cbData.RowStart = full ? 0u : marchCacheRowCursor;
	cbData.RowCount = rows;
	marchCacheCB->Update(cbData);
	if (!full)
		marchCacheRowCursor = (marchCacheRowCursor + rows) % dim;

	auto context = globals::d3d::context;
	globals::profiler->BeginPass("SnowDeformation::Ground - March Cache");
	ID3D11Buffer* cbs[3] = { shellCB->CB(), nullptr, marchCacheCB->CB() };
	ID3D11ShaderResourceView* tileSRV = bermTileSRV.get();
	context->CSSetShaderResources(kMarchTilesSlot, 1, &tileSRV);
	ID3D11ShaderResourceView* deformSRV = deform->srv.get();
	ID3D11ShaderResourceView* bermSRV = bermFieldTexture->srv.get();
	ID3D11ShaderResourceView* terrainSRV = shellTerrainTexture->srv.get();
	auto dispatch = [&](ID3D11ComputeShader* a_tiles, ID3D11ComputeShader* a_rows) {
		if (tiles) {
			context->CSSetShader(a_tiles, nullptr, 0);
			context->DispatchIndirect(bermArgsBuffer.get(), 0);
		}
		context->CSSetShader(a_rows, nullptr, 0);
		context->Dispatch((dim + 7) / 8, (rows + 7) / 8, 1);
	};

	// Ground: SnowShell.hlsl's own bindings for what its march taps read.
	{
		context->CSSetConstantBuffers(0, 3, cbs);
		ID3D11ShaderResourceView* wetSRV = TerrainWetSRV(a_cb.CompactLook.x > 0.5f);
		ID3D11ShaderResourceView* fineSRV = (shellFineValid && shellTerrainFine) ? shellTerrainFine->srv.get() : nullptr;
		ID3D11ShaderResourceView* bottomsSRV = heightBottomFiltered->srv.get();
		ID3D11ShaderResourceView* exclusionSRV = exclusionFieldTexture ? exclusionFieldTexture->srv.get() : nullptr;
		context->CSSetShaderResources(0, 1, &terrainSRV);
		context->CSSetShaderResources(1, 1, &deformSRV);
		context->CSSetShaderResources(5, 1, &bottomsSRV);
		context->CSSetShaderResources(13, 1, &fineSRV);
		context->CSSetShaderResources(14, 1, &bermSRV);
		context->CSSetShaderResources(15, 1, &exclusionSRV);
		context->CSSetShaderResources(kTerrainWetSlot, 1, &wetSRV);
		ID3D11UnorderedAccessView* uav = marchHeight[0]->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		dispatch(cs[0][0], cs[0][1]);
	}

	// Roads: SnowStaticsShell.hlsl's bindings, with the trench patch's window.
	{
		StaticsCB scb{};
		FillPatchDrawCB(scb);
		staticsCB->Update(scb);
		cbs[1] = staticsCB->CB();
		context->CSSetConstantBuffers(0, 3, cbs);
		ID3D11ShaderResourceView* rasterSRVs[3] = { heightTopRaw[heightCurrent]->srv.get(), heightSkinDepth->srv.get(), objectSnowCone->srv.get() };
		context->CSSetShaderResources(11, 3, rasterSRVs);
		ID3D11ShaderResourceView* nullSRV = nullptr;
		context->CSSetShaderResources(kTerrainWetSlot, 1, &nullSRV);
		ID3D11UnorderedAccessView* uav = marchHeight[1]->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		dispatch(cs[1][0], cs[1][1]);
	}

	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	ID3D11ShaderResourceView* nullSRVs[16] = {};
	context->CSSetShaderResources(0, 16, nullSRVs);
	context->CSSetShaderResources(kTerrainWetSlot, 1, nullSRVs);
	context->CSSetShaderResources(kMarchTilesSlot, 1, nullSRVs);
	ID3D11Buffer* nullCBs[3] = {};
	context->CSSetConstantBuffers(0, 3, nullCBs);
	context->CSSetShader(nullptr, nullptr, 0);
	globals::profiler->EndPass();

	marchCacheValid = true;
	marchCacheReady = true;
	ID3D11ShaderResourceView* cacheSRVs[2] = { marchHeight[0]->srv.get(), marchHeight[1]->srv.get() };
	context->PSSetShaderResources(kMarchGroundSlot, 2, cacheSRVs);
}
