// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

// The sun heightfield marches' occluder heights, baked. The marches of the
// ground, the trench patch and the object snow read one gather per tap from
// these instead of rebuilding terrain, land layer, melt, carve, berm and dunes
// at every tap (two dependent fetch rounds, five taps a pixel):
// [0] the ground's tap height per deformation texel, in the map's torus
//     (SnowShell.hlsl MarchHeightCS);
// [1] the statics march's road tap per deformation texel, the sentinel off the
//     roads (SnowStaticsShell.hlsl RoadMarchHeightCS);
// [2] the ground's tap height per land-layer vertex over the FineWindow, for
//     the marches that leave the trail map (MarchHeightFarRowsCS).
//
// The near pair follows the berm bake's tile list (the map or the berm field
// changed there; the scroll's arriving strips are on it too). Every cache
// also refreshes a band of rows a frame for the slow inputs (snowfall,
// objects, water, clearings), faster after a terrain or land-layer rebuild,
// and rebuilds whole when created, re-set, or after a berm bake it missed.

namespace
{
	constexpr uint32_t kMarchCacheRows = 32;
	constexpr uint32_t kMarchCacheFastRows = 256;
	constexpr uint32_t kMarchFarRows = 36;
	constexpr uint32_t kMarchFarFastRows = 288;
	constexpr UINT kMarchTilesSlot = 63;
	constexpr UINT kMarchSlot = 106;
}

ID3D11ComputeShader* SnowDeformation::GetMarchHeightCS(MarchCS a_which)
{
	static constexpr const char* entries[kMarchCSCount] = { "MarchHeightTilesCS", "MarchHeightRowsCS", "RoadMarchTilesCS", "RoadMarchRowsCS", "MarchHeightFarRowsCS" };
	auto*& slot = marchHeightCS[a_which];
	if (!slot) {
		const bool road = a_which == kRoadTilesCS || a_which == kRoadRowsCS;
		logger::debug("Compiling {} {}", road ? "SnowStaticsShell" : "SnowShell", entries[a_which]);
		slot = static_cast<ID3D11ComputeShader*>(road ?
				CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowStaticsShell.hlsl", { { "SNOW_ROAD_MARCH_CS", "" } }, "cs_5_0", entries[a_which]) :
				CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "SNOW_MARCH_HEIGHT_CS", "" } }, "cs_5_0", entries[a_which]));
	}
	return slot;
}

void SnowDeformation::ReleaseMarchCache()
{
	for (auto*& cs : marchHeightCS) {
		if (cs)
			cs->Release();
		cs = nullptr;
	}
	marchCacheValid = false;
	marchFarValid = false;
}

void SnowDeformation::UnbindMarchCaches()
{
	ID3D11ShaderResourceView* nullSRVs[3] = {};
	globals::d3d::context->PSSetShaderResources(kMarchSlot, 3, nullSRVs);
}

namespace
{
	Texture2D* EnsureMarchTexture(Texture2D*& a_tex, uint32_t a_dim, const char* a_name, bool& a_created)
	{
		if (a_tex && a_tex->desc.Width == a_dim)
			return a_tex;
		delete a_tex;
		a_tex = nullptr;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_dim;
		desc.Height = a_dim;
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
		a_tex = new Texture2D(desc, a_name);
		a_tex->CreateSRV(srvDesc);
		a_tex->CreateUAV(uavDesc);
		a_created = true;
		return a_tex->srv && a_tex->uav ? a_tex : nullptr;
	}

	// A band of rows from a_cursor (all of them on a full build); advances
	// the cursor and spends the fast budget.
	uint32_t MarchRows(bool a_full, uint32_t a_dim, uint32_t a_slow, uint32_t a_fast, uint32_t& a_fastLeft, uint32_t& a_cursor, uint32_t& a_start)
	{
		uint32_t rows = a_full ? a_dim : (a_fastLeft > 0 ? std::min(a_fast, a_fastLeft) : a_slow);
		rows = std::min(rows, a_dim);
		a_fastLeft -= std::min(a_fastLeft, rows);
		a_start = a_full ? 0u : a_cursor;
		if (!a_full)
			a_cursor = (a_cursor + rows) % a_dim;
		return rows;
	}
}

bool SnowDeformation::UpdateMarchCaches(const ShellCB& a_cb)
{
	auto* deform = deformationTextures[0];
	if (!deform || !deform->srv || !bermFieldTexture || !bermFieldTexture->srv || !bermTileSRV || !bermArgsBuffer || !shellTerrainTexture || !shellTerrainTexture->srv ||
		!heightTopRaw[heightCurrent] || !heightSkinDepth || !objectSnowCone || !heightBottomFiltered) {
		marchCacheValid = false;
		marchFarValid = false;
		return false;
	}
	for (uint32_t i = 0; i < kMarchCSCount; i++)
		if (!GetMarchHeightCS(MarchCS(i)))
			return false;
	const bool farCache = shellFineValid && shellTerrainFine && a_cb.FineWindow.z > 0.5f;

	const uint32_t dim = deform->desc.Width;
	bool created = false;
	if (!EnsureMarchTexture(marchHeight[0], dim, "SnowDeformation::MarchHeightGround", created) ||
		!EnsureMarchTexture(marchHeight[1], dim, "SnowDeformation::MarchHeightRoad", created))
		return false;
	if (created)
		marchCacheValid = false;
	bool farCreated = false;
	if (farCache && !EnsureMarchTexture(marchHeight[2], kShellFineDim, "SnowDeformation::MarchHeightFar", farCreated))
		return false;
	if (farCreated)
		marchFarValid = false;
	if (!marchCacheCB)
		marchCacheCB = new ConstantBuffer(ConstantBufferDesc<MarchCacheCB>(), "SnowDeformation::MarchCacheCB");

	// What moves every texel at once: the settings, the window's size, the
	// readers' gates. Snowfall's depth scale (RimStyle.w) creeps every frame
	// while it snows and is left to the row bands. So do the berm and lip
	// amplitudes and the wall slough: freshness and the climate ease them
	// every tick, so the key holds their sliders and the climate's overrides,
	// not their values. Track age the same: the clock runs, Track Ageing keys.
	uint64_t key = 1469598103934665603ull;
	auto mix = [&](const void* a_p, size_t a_n) {
		const auto* b = static_cast<const uint8_t*>(a_p);
		for (size_t n = 0; n < a_n; n++) {
			key ^= b[n];
			key *= 1099511628211ull;
		}
	};
	const float keyFloats[] = { a_cb.DeformInvWorldSize, a_cb.UndulationAmp, a_cb.UndulationScale, settings.BermHeight, settings.RimLip,
		a_cb.BermBakeActive, a_cb.ObjectLiftCap > 0.0f ? 1.0f : 0.0f, a_cb.ExclusionFieldWindow.w, a_cb.FineWindow.z,
		a_cb.ObjectHeightHalfExtent, a_cb.FieldDims.x, a_cb.FieldDims.y, a_cb.FieldDims.z, a_cb.FieldDims.w,
		settings.FreshSnowSoftness, debugFreshnessOverride, settings.SnowClimateStrength, debugClimateTemperatureOn ? debugClimateTemperature : -1000.0f,
		debugClimateShareOverride, debugClimateWetnessOverride, debugClimateStickinessOverride, debugClimateFirmnessOverride, settings.TrackAgeing };
	mix(keyFloats, sizeof(keyFloats));
	mix(&dim, sizeof(dim));
	const bool keyChanged = key != marchCacheKey;
	marchCacheKey = key;
	const bool terrainMoved = shellTerrainVersion != marchCacheTerrainVersion;
	marchCacheTerrainVersion = shellTerrainVersion;

	// Near: the berm bake's tiles, unless the caches missed one.
	const bool missedBake = bermBakeSeq > marchCacheBermSeq + 1;
	const bool full = !marchCacheValid || keyChanged || missedBake;
	const bool tiles = !full && bermBakeSeq == marchCacheBermSeq + 1;
	marchCacheBermSeq = bermBakeSeq;
	const bool fineMoved = shellFineOriginX != marchCacheFineOrigin[0] || shellFineOriginY != marchCacheFineOrigin[1];
	// Tracks age slowly enough for the row bands, except across a wait.
	const bool tracksAged = std::abs(a_cb.TrackAge.x - marchCacheAgeingClock) > 2.0f;
	marchCacheAgeingClock = a_cb.TrackAge.x;
	if (full)
		marchCacheFastRows = 0;
	else if (terrainMoved || fineMoved || tracksAged)
		marchCacheFastRows = dim;
	uint32_t nearStart = 0;
	const uint32_t nearRows = MarchRows(full, dim, kMarchCacheRows, kMarchCacheFastRows, marchCacheFastRows, marchCacheRowCursor, nearStart);

	// Far: whole when the land-layer window moves.
	const bool farFull = farCache && (!marchFarValid || keyChanged || fineMoved);
	if (farCache && !farFull && terrainMoved)
		marchFarFastRows = kShellFineDim;
	uint32_t farStart = 0;
	const uint32_t farRows = farCache ? MarchRows(farFull, kShellFineDim, kMarchFarRows, kMarchFarFastRows, marchFarFastRows, marchFarRowCursor, farStart) : 0;
	marchCacheFineOrigin[0] = shellFineOriginX;
	marchCacheFineOrigin[1] = shellFineOriginY;
	marchCensus[marchCensusCursor] = uint8_t((keyChanged ? 1u : 0u) | (full ? 2u : 0u) | (farFull ? 4u : 0u));
	marchCensusCursor = (marchCensusCursor + 1) % kMarchCensusFrames;
	marchCensusFilled = std::min(marchCensusFilled + 1, kMarchCensusFrames);

	auto context = globals::d3d::context;
	globals::profiler->BeginPass("SnowDeformation::Ground - March Cache");
	ID3D11Buffer* cbs[3] = { shellCB->CB(), nullptr, marchCacheCB->CB() };
	ID3D11ShaderResourceView* tileSRV = bermTileSRV.get();
	context->CSSetShaderResources(kMarchTilesSlot, 1, &tileSRV);
	ID3D11ShaderResourceView* deformSRV = deform->srv.get();
	ID3D11ShaderResourceView* bermSRV = bermFieldTexture->srv.get();
	ID3D11ShaderResourceView* terrainSRV = shellTerrainTexture->srv.get();
	auto rowsDispatch = [&](MarchCS a_cs, uint32_t a_dim, uint32_t a_start, uint32_t a_rows) {
		MarchCacheCB band{};
		band.RowStart = a_start;
		band.RowCount = a_rows;
		marchCacheCB->Update(band);
		context->CSSetShader(marchHeightCS[a_cs], nullptr, 0);
		context->Dispatch((a_dim + 7) / 8, (a_rows + 7) / 8, 1);
	};
	auto setUAV = [&](Texture2D* a_tex) {
		ID3D11UnorderedAccessView* uav = a_tex->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	};

	// Ground, near and far: SnowShell.hlsl's own bindings for what its taps read.
	context->CSSetConstantBuffers(0, 3, cbs);
	ID3D11ShaderResourceView* wetSRV = TerrainWetSRV();
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
	setUAV(marchHeight[0]);
	if (tiles) {
		context->CSSetShader(marchHeightCS[kGroundTilesCS], nullptr, 0);
		context->DispatchIndirect(bermArgsBuffer.get(), 0);
	}
	rowsDispatch(kGroundRowsCS, dim, nearStart, nearRows);
	if (farCache) {
		setUAV(marchHeight[2]);
		rowsDispatch(kFarRowsCS, kShellFineDim, farStart, farRows);
	}

	// Roads: SnowStaticsShell.hlsl's bindings, with the trench patch's window.
	StaticsCB scb{};
	FillPatchDrawCB(scb);
	staticsCB->Update(scb);
	cbs[1] = staticsCB->CB();
	context->CSSetConstantBuffers(0, 3, cbs);
	ID3D11ShaderResourceView* rasterSRVs[3] = { heightTopRaw[heightCurrent]->srv.get(), heightSkinDepth->srv.get(), objectSnowCone->srv.get() };
	context->CSSetShaderResources(11, 3, rasterSRVs);
	setUAV(marchHeight[1]);
	if (tiles) {
		context->CSSetShader(marchHeightCS[kRoadTilesCS], nullptr, 0);
		context->DispatchIndirect(bermArgsBuffer.get(), 0);
	}
	rowsDispatch(kRoadRowsCS, dim, nearStart, nearRows);

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
	marchFarValid = farCache;
	ID3D11ShaderResourceView* cacheSRVs[3] = { marchHeight[0]->srv.get(), marchHeight[1]->srv.get(), farCache ? marchHeight[2]->srv.get() : nullptr };
	context->PSSetShaderResources(kMarchSlot, 3, cacheSRVs);
	return true;
}
