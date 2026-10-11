// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

void SnowDeformation::CaptureShadowAtlas()
{
	// PS t4 holds the sun cascade atlas only during the shadow-mask pass.
	// A skipped pass keeps the previous atlas: stale cascades are invisible,
	// crisp/VSM flapping is not.
	if (!settings.EnableSnowDeformation || !globals::state->HasDirectionalShadows()) {
		shadowAtlasLiveSRV = nullptr;
		return;
	}

	winrt::com_ptr<ID3D11ShaderResourceView> liveAtlasSRV;
	globals::d3d::context->PSGetShaderResources(4, 1, liveAtlasSRV.put());

	D3D11_TEXTURE2D_DESC atlasDesc{};
	if (liveAtlasSRV) {
		winrt::com_ptr<ID3D11Resource> liveRes;
		liveAtlasSRV->GetResource(liveRes.put());
		if (auto liveTex = liveRes.try_as<ID3D11Texture2D>()) {
			liveTex->GetDesc(&atlasDesc);
			dbgLodAtlasSlices = atlasDesc.ArraySize;
		}
		RecordSunCascadeSlices(atlasDesc.ArraySize);
		// Before the snow reads the atlas, so the snow receives its own casters.
		if (!shellShadowCastDebugOff && settings.SnowCastsShadows)
			InjectShellShadowCasters(liveAtlasSRV.get());

		if (!shadowAtlasLiveReadOff) {
			shadowAtlasLiveSRV = liveAtlasSRV;
			shadowAtlasCopySRV = nullptr;
			shadowAtlasCopyTex = nullptr;
		} else {
			shadowAtlasLiveSRV = nullptr;
			if (shadowCopyWantedFrames > 0) {
				globals::profiler->BeginPass("SnowDeformation::Shadows - Atlas Copy");
				CopySRVResource(liveAtlasSRV.get(), "SnowDeformation::ShadowAtlasCopy", shadowAtlasCopyTex, shadowAtlasCopySRV);
				globals::profiler->EndPass();
			} else
				globals::profiler->MarkPassSkipped("SnowDeformation::Shadows - Atlas Copy");
		}
	}

	// First frames, then every validity flip.
	static uint32_t shadowLogCount = 0;
	static int lastValidState = -1;
	const int validState = ShadowAtlasSRV() ? 1 : 0;
	if ((shadowLogCount < 6 || validState != lastValidState) && shadowLogCount < 40) {
		shadowLogCount++;
		lastValidState = validState;
		D3D11_SHADER_RESOURCE_VIEW_DESC liveViewDesc{};
		if (liveAtlasSRV)
			liveAtlasSRV->GetDesc(&liveViewDesc);
		logger::info("[SNOW DEFORMATION] ShadowCopy: atlasLive={} inPlace={} valid={} atlas={}x{} slices={} fmt={} samples={} liveViewDim={} liveViewFmt={}",
			liveAtlasSRV != nullptr, !shadowAtlasLiveReadOff, validState,
			atlasDesc.Width, atlasDesc.Height, atlasDesc.ArraySize, uint32_t(atlasDesc.Format), atlasDesc.SampleDesc.Count,
			uint32_t(liveViewDesc.ViewDimension), uint32_t(liveViewDesc.Format));
	}
}

void SnowDeformation::CapturePointShadowMask()
{
	// A local light's descriptor is live only during its mask pass
	// (renderTarget reads -1 once the engine returns the maps); PS t4 holds
	// the local map array then, a texture of its own beside the sun atlas.
	if (!settings.EnableSnowDeformation || REL::Module::IsVR())
		return;

	auto* shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	if (!shadowSceneNode)
		return;

	auto context = globals::d3d::context;
	winrt::com_ptr<ID3D11ShaderResourceView> liveAtlasSRV;
	context->PSGetShaderResources(4, 1, liveAtlasSRV.put());
	if (!liveAtlasSRV)
		return;
	winrt::com_ptr<ID3D11Resource> liveRes;
	liveAtlasSRV->GetResource(liveRes.put());
	auto liveTex = liveRes.try_as<ID3D11Texture2D>();
	if (!liveTex)
		return;
	D3D11_TEXTURE2D_DESC liveDesc;
	liveTex->GetDesc(&liveDesc);
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
	liveAtlasSRV->GetDesc(&srvDesc);
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MostDetailedMip = 0;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.FirstArraySlice = 0;

	if (!pointShadowLiveReadOff) {
		pointShadowAtlasCopySRV = nullptr;
		pointShadowAtlasCopyTex = nullptr;
		if (pointShadowLiveTex != liveTex) {
			pointShadowLiveSRV = nullptr;
			pointShadowLiveTex = nullptr;
			srvDesc.Texture2DArray.ArraySize = liveDesc.ArraySize;
			if (FAILED(globals::d3d::device->CreateShaderResourceView(liveTex.get(), &srvDesc, pointShadowLiveSRV.put())))
				return;
			pointShadowLiveTex = liveTex;
		}
	} else {
		pointShadowLiveSRV = nullptr;
		pointShadowLiveTex = nullptr;
		// One slice per mask channel, whole subresources: partial boxes are
		// illegal on depth resources.
		bool recreate = !pointShadowAtlasCopyTex || !pointShadowAtlasCopySRV;
		if (pointShadowAtlasCopyTex) {
			D3D11_TEXTURE2D_DESC haveDesc;
			pointShadowAtlasCopyTex->GetDesc(&haveDesc);
			recreate |= haveDesc.Width != liveDesc.Width || haveDesc.Height != liveDesc.Height || haveDesc.Format != liveDesc.Format;
		}
		if (recreate) {
			pointShadowAtlasCopySRV = nullptr;
			pointShadowAtlasCopyTex = nullptr;
			D3D11_TEXTURE2D_DESC copyDesc = liveDesc;
			copyDesc.ArraySize = kPointShadowMaxLights;
			copyDesc.MipLevels = 1;
			copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			copyDesc.MiscFlags = 0;
			copyDesc.Usage = D3D11_USAGE_DEFAULT;
			copyDesc.CPUAccessFlags = 0;
			if (FAILED(globals::d3d::device->CreateTexture2D(&copyDesc, nullptr, pointShadowAtlasCopyTex.put())))
				return;
			Util::SetResourceName(pointShadowAtlasCopyTex.get(), "SnowDeformation::PointShadowAtlasCopy");
			srvDesc.Texture2DArray.ArraySize = kPointShadowMaxLights;
			if (FAILED(globals::d3d::device->CreateShaderResourceView(pointShadowAtlasCopyTex.get(), &srvDesc, pointShadowAtlasCopySRV.put()))) {
				pointShadowAtlasCopyTex = nullptr;
				return;
			}
		}
	}

	// Snapshot every local light whose descriptor is live now; entries
	// persist until their channel is reassigned.
	for (auto& lightPtr : shadowSceneNode->GetRuntimeData().activeShadowLights) {
		auto* bsLight = lightPtr.get();
		if (!bsLight || !bsLight->IsShadowLight())
			continue;
		auto* light = static_cast<RE::BSShadowLight*>(bsLight);
		const bool isFrustum = light->GetIsFrustumLight();
		const bool isParabolic = light->GetIsParabolicLight();
		if (!isFrustum && !isParabolic)
			continue;
		auto& lightRuntime = light->GetRuntimeData();
		// 255 = parked/inactive shadow light.
		const uint32_t maskIndex = lightRuntime.maskIndex;
		if (maskIndex >= kPointShadowMaxLights)
			continue;
		if (lightRuntime.shadowmapDescriptors.empty())
			continue;
		auto& desc = lightRuntime.shadowmapDescriptors[0];
		if (!desc.isEnabled || desc.renderTarget == static_cast<RE::RENDER_TARGET_DEPTHSTENCIL>(-1))
			continue;
		if (desc.shadowmapIndex >= liveDesc.ArraySize)
			continue;

		// Lights on another target snapshot at their own pass.
		ID3D11Texture2D* lightTargetTex = nullptr;
		if (auto* gameRenderer = globals::game::renderer) {
			auto& dsData = gameRenderer->GetDepthStencilData().depthStencils[desc.renderTarget];
			lightTargetTex = reinterpret_cast<ID3D11Texture2D*>(dsData.texture);
		}
		if (lightTargetTex != liveTex.get())
			continue;

		PointShadowLightData& dd = pendingPointShadows[maskIndex];
		auto proj = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&desc.lightTransform));
		DirectX::XMStoreFloat4x4(&dd.LightTransform, proj);
		dd.SliceIndex = pointShadowLiveReadOff ? maskIndex : desc.shadowmapIndex;
		dd.LightType = isFrustum ? 1u : (light->GetIsOmniLight() ? 3u : 2u);
		// The radius the map's radial depth was normalised by; ISL rewrites
		// the light's radius later in the frame.
		dd.ShadowRadius = light->light ? light->light->GetLightRuntimeData().radius.x : 0.0f;

		if (pointShadowLiveReadOff && shadowCopyWantedFrames > 0 && pointShadowSliceFrame[maskIndex] != pointShadowFrameIndex) {
			pointShadowSliceFrame[maskIndex] = pointShadowFrameIndex;
			globals::profiler->BeginPass("SnowDeformation::Shadows - Local Atlas Copy");
			context->CopySubresourceRegion(pointShadowAtlasCopyTex.get(), maskIndex, 0, 0, 0, liveTex.get(), desc.shadowmapIndex, nullptr);
			globals::profiler->EndPass();
		}

		static uint32_t layoutLogCount = 0;
		if (layoutLogCount < 12) {
			layoutLogCount++;
			logger::info("[SNOW DEFORMATION] PointShadow: mask={} type={} srcSlice={} rt={} arr={} inPlace={} biasScale={:.3f}",
				maskIndex, dd.LightType, desc.shadowmapIndex, uint32_t(desc.renderTarget), liveDesc.ArraySize, !pointShadowLiveReadOff, lightRuntime.shadowBiasScale);
		}
	}
}

void SnowDeformation::UpdatePointShadowLights()
{
	auto context = globals::d3d::context;

	if (!pointShadowLights) {
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(PointShadowLightData);
		sbDesc.ByteWidth = sizeof(PointShadowLightData) * kPointShadowMaxLights;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = kPointShadowMaxLights;

		pointShadowLights = new Buffer(sbDesc, nullptr, "SnowDeformation::PointShadowLights");
		pointShadowLights->CreateSRV(srvDesc);
	}

	// No maps = an empty table: shadow-casting lights render unshadowed.
	PointShadowLightData table[kPointShadowMaxLights]{};
	if (PointShadowAtlasSRV())
		memcpy(table, pendingPointShadows, sizeof(table));

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (SUCCEEDED(context->Map(pointShadowLights->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		memcpy(mapped.pData, table, sizeof(table));
		context->Unmap(pointShadowLights->resource.get(), 0);
	}
}

// The sun cascades' atlas slices for the snow's shadow reads (ShellCB.BorderStyle.zw): the atlas
// is shared with local shadow lights, and the sun's cascades move slices as the active-light set
// changes with the view. Recorded every frame, whether or not anything casts.
void SnowDeformation::RecordSunCascadeSlices(uint32_t a_atlasSlices)
{
	auto* shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	auto* sunShadowLight = shadowSceneNode ? shadowSceneNode->GetRuntimeData().sunShadowDirLight : nullptr;
	if (!sunShadowLight || a_atlasSlices == 0)
		return;
	const auto& descriptors = sunShadowLight->GetRuntimeData().shadowmapDescriptors;
	const uint32_t count = std::min<uint32_t>((uint32_t)descriptors.size(), 2u);
	bool fallback = false;
	for (uint32_t i = 0; i < count; i++) {
		// An out-of-range shadowmapIndex means the descriptor is not live this
		// instant: cascade == slice, a possibly offset shadow over a missing one.
		uint32_t slice = descriptors[i].shadowmapIndex;
		if (slice >= a_atlasSlices) {
			slice = i;
			fallback = true;
		}
		if (slice < a_atlasSlices)
			sunCascadeSlice[i] = slice;
	}
	// One line per change of the slice pair, the raw indices or the fallback.
	const uint32_t raw0 = count > 0 ? descriptors[0].shadowmapIndex : 0xFFu;
	const uint32_t raw1 = count > 1 ? descriptors[1].shadowmapIndex : 0xFFu;
	const uint32_t packed = (sunCascadeSlice[0] & 0xFF) | ((sunCascadeSlice[1] & 0xFF) << 8) | ((raw0 & 0xFF) << 16) | ((raw1 & 0xFF) << 24);
	static uint32_t lastPacked = 0xFFFFFFFFu;
	static uint32_t changeLogCount = 0;
	if (packed != lastPacked && changeLogCount < 200) {
		lastPacked = packed;
		changeLogCount++;
		logger::info("[SNOW DEFORMATION] Sun cascades: slices used = {}, {} (raw shadowmapIndex = {}, {}; descriptors = {}; atlas has {}; fallback = {})",
			sunCascadeSlice[0], sunCascadeSlice[1], raw0, raw1, (uint32_t)descriptors.size(), a_atlasSlices, fallback);
	}
}

// The road trench patch into the sun cascades; the ground snow casts through
// the raised landscape (LandscapeSnow.cpp).
void SnowDeformation::InjectShellShadowCasters(ID3D11ShaderResourceView* a_atlasSRV)
{
	if (SnowShadersPending(2))
		return;
	// One-shot diagnostics: name the exit taken, so a missing shadow points
	// straight at its gate.
	static std::unordered_set<std::string> injectLogged;
	auto logOnce = [](const char* a_msg) {
		if (injectLogged.size() < 12 && injectLogged.insert(a_msg).second)
			logger::info("[SNOW DEFORMATION] ShadowInject: {}", a_msg);
	};
	const bool splitRow = shellCasterSplitDebug;
	auto skipPatch = [&](const char* a_msg) {
		logOnce(a_msg);
		if (!splitRow)
			globals::profiler->MarkPassSkipped("SnowDeformation::Shadows - Trench Patch");
	};

	if (!settings.EnableSnowDeformation || !lastShellCBData)
		return skipPatch("skip: disabled or no ShellCB snapshot yet");
	// Same gate as the visible patch draw, one frame stale like the skins:
	// capturedRoads is last frame's count here (Prepass resets it after the
	// mask passes), the frame whose rasters this VS reads.
	if (settings.RoadMeshesDepth <= 0.5f || capturedRoads == 0 || !heightSkinDepth || !heightSkinDepth->srv ||
		!heightTopRaw[heightCurrent] || !heightTopRaw[heightCurrent]->srv)
		return skipPatch("skip: no road meshes captured");
	if (!ShellFootprintHasSnow() || GroundShellsSuspended())
		return skipPatch("skip: no snow depth within the footprint");
	if (!shellDrawGroundGated && !shellViewGateOff)
		return skipPatch("skip: no snow in view");
	// Indexed over the shared lattice: one VS invocation per vertex, the same
	// triangles in the same order, minus those past the fade band.
	const bool patchIndexed = !patchCasterNonIndexed && GetPatchShadowVS(true) && EnsurePatchCasterIB();
	auto* patchCastVS = GetPatchShadowVS(patchIndexed);
	if (!patchCastVS)
		return skipPatch("skip: patch shadow VS unavailable");

	auto* shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	auto* sunShadowLight = shadowSceneNode ? shadowSceneNode->GetRuntimeData().sunShadowDirLight : nullptr;
	if (!sunShadowLight)
		return skipPatch("skip: no sun shadow light");
	auto& lightRuntime = sunShadowLight->GetRuntimeData();
	// Every sun shadowmap: the descriptor list is larger than the two cascades
	// (the focus map is in the family) and its order is not guaranteed.
	const uint32_t cascadeCount = std::min<uint32_t>((uint32_t)lightRuntime.shadowmapDescriptors.size(), 4u);
	if (cascadeCount == 0)
		return skipPatch("skip: zero cascades");

	auto context = globals::d3d::context;
	auto device = globals::d3d::device;

	// Per-slice DSVs on the LIVE atlas texture, cached by texture pointer.
	winrt::com_ptr<ID3D11Resource> atlasRes;
	a_atlasSRV->GetResource(atlasRes.put());
	auto atlasTex = atlasRes.try_as<ID3D11Texture2D>();
	if (!atlasTex) {
		return skipPatch("skip: atlas SRV resource is not a Texture2D");
	}
	D3D11_TEXTURE2D_DESC atlasDesc;
	atlasTex->GetDesc(&atlasDesc);
	if (!(atlasDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
		return skipPatch("skip: atlas lacks DEPTH_STENCIL bind");
	}
	DXGI_FORMAT dsvFormat;
	switch (atlasDesc.Format) {
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_D16_UNORM:
		dsvFormat = DXGI_FORMAT_D16_UNORM;
		break;
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
		dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
		break;
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT:
		dsvFormat = DXGI_FORMAT_D32_FLOAT;
		break;
	default:
		return skipPatch("skip: atlas format");
	}
	// Per-cascade DSVs on the REAL slice each descriptor renders to
	// (desc.shadowmapIndex): the atlas is SHARED with local shadow lights,
	// and the sun's cascades move slices as the active-light set changes
	// with the view.
	for (uint32_t i = 0; i < cascadeCount; i++) {
		uint32_t slice = lightRuntime.shadowmapDescriptors[i].shadowmapIndex;
		// Fail-safe: an out-of-range shadowmapIndex means the
		// descriptor is not live at this instant for this view state; the
		// old silent `continue` here was an appear/disappear shadow keyed
		// to view direction. Fall back to the legacy cascade==slice
		// assumption - a possibly-offset shadow beats a missing one, and
		// the change-driven log below records every occurrence.
		if (slice >= atlasDesc.ArraySize) {
			slice = i;
			if (slice >= atlasDesc.ArraySize)
				continue;
		}
		if (shadowAtlasDSVTexture == atlasTex.get() && shadowAtlasDSVSlice[i] == slice && shadowAtlasDSV[i])
			continue;
		shadowAtlasDSV[i] = nullptr;
		shadowAtlasDSVSlice[i] = slice;
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.Format = dsvFormat;
		dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
		dsvDesc.Texture2DArray.MipSlice = 0;
		dsvDesc.Texture2DArray.FirstArraySlice = slice;
		dsvDesc.Texture2DArray.ArraySize = 1;
		device->CreateDepthStencilView(atlasTex.get(), &dsvDesc, shadowAtlasDSV[i].put());
		if (shadowAtlasDSV[i])
			Util::SetResourceName(shadowAtlasDSV[i].get(), "SnowDeformation::ShadowAtlas DSV");
	}
	shadowAtlasDSVTexture = atlasTex.get();
	if (!shadowAtlasDSV[0]) {
		return skipPatch("skip: DSV creation failed");
	}
	if (!shadowCastRS) {
		D3D11_RASTERIZER_DESC rsDesc{};
		rsDesc.FillMode = D3D11_FILL_SOLID;
		rsDesc.CullMode = D3D11_CULL_NONE;
		rsDesc.DepthClipEnable = TRUE;
		device->CreateRasterizerState(&rsDesc, shadowCastRS.put());
		Util::SetResourceName(shadowCastRS.get(), "SnowDeformation::ShadowCastRS");
	}
	if (!shadowCastDSS) {
		D3D11_DEPTH_STENCIL_DESC dsDesc{};
		dsDesc.DepthEnable = TRUE;
		dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dsDesc.DepthFunc = D3D11_COMPARISON_LESS;
		device->CreateDepthStencilState(&dsDesc, shadowCastDSS.put());
		Util::SetResourceName(shadowCastDSS.get(), "SnowDeformation::ShadowCastDSS");
	}
	if (!shadowCastRS || !shadowCastDSS) {
		return skipPatch("skip: RS/DSS creation failed");
	}
	logOnce("injecting: trench patch into the sun cascades");

	// ---- Save every piece of state we touch: we are inside the game's setup
	// for the shadow-mask draw and must hand it back byte-identical.
	winrt::com_ptr<ID3D11RenderTargetView> prevRTVs[8];
	winrt::com_ptr<ID3D11DepthStencilView> prevDSV;
	{
		ID3D11RenderTargetView* rtvs[8] = {};
		ID3D11DepthStencilView* dsv = nullptr;
		context->OMGetRenderTargets(8, rtvs, &dsv);
		for (uint32_t i = 0; i < 8; i++)
			prevRTVs[i].attach(rtvs[i]);
		prevDSV.attach(dsv);
	}
	UINT prevViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT prevViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	context->RSGetViewports(&prevViewportCount, prevViewports);
	winrt::com_ptr<ID3D11RasterizerState> prevRS;
	context->RSGetState(prevRS.put());
	winrt::com_ptr<ID3D11DepthStencilState> prevDSS;
	UINT prevStencilRef = 0;
	context->OMGetDepthStencilState(prevDSS.put(), &prevStencilRef);
	winrt::com_ptr<ID3D11VertexShader> prevVS;
	context->VSGetShader(prevVS.put(), nullptr, nullptr);
	winrt::com_ptr<ID3D11PixelShader> prevPS;
	context->PSGetShader(prevPS.put(), nullptr, nullptr);
	D3D11_PRIMITIVE_TOPOLOGY prevTopology;
	context->IAGetPrimitiveTopology(&prevTopology);
	winrt::com_ptr<ID3D11InputLayout> prevLayout;
	context->IAGetInputLayout(prevLayout.put());
	winrt::com_ptr<ID3D11Buffer> prevVSCBs[2];
	{
		ID3D11Buffer* cbs[2] = {};
		context->VSGetConstantBuffers(0, 2, cbs);
		prevVSCBs[0].attach(cbs[0]);
		prevVSCBs[1].attach(cbs[1]);
	}
	winrt::com_ptr<ID3D11ShaderResourceView> prevVSSRVs[6];
	{
		ID3D11ShaderResourceView* srvs[6] = {};
		context->VSGetShaderResources(0, 6, srvs);
		for (uint32_t i = 0; i < 6; i++)
			prevVSSRVs[i].attach(srvs[i]);
	}
	// The patch caster binds t11-t13 (top / skin depth / cone).
	winrt::com_ptr<ID3D11ShaderResourceView> prevVSSRVsSkin[4];
	{
		ID3D11ShaderResourceView* srvs[4] = {};
		context->VSGetShaderResources(10, 4, srvs);
		for (uint32_t i = 0; i < 4; i++)
			prevVSSRVsSkin[i].attach(srvs[i]);
	}
	winrt::com_ptr<ID3D11Buffer> prevVB;
	UINT prevVBStride = 0, prevVBOffset = 0;
	{
		ID3D11Buffer* vb = nullptr;
		context->IAGetVertexBuffers(0, 1, &vb, &prevVBStride, &prevVBOffset);
		prevVB.attach(vb);
	}
	winrt::com_ptr<ID3D11Buffer> prevIB;
	DXGI_FORMAT prevIBFormat = DXGI_FORMAT_UNKNOWN;
	UINT prevIBOffset = 0;
	{
		ID3D11Buffer* ib = nullptr;
		context->IAGetIndexBuffer(&ib, &prevIBFormat, &prevIBOffset);
		prevIB.attach(ib);
	}
	winrt::com_ptr<ID3D11ShaderResourceView> prevPSSRV4;
	{
		ID3D11ShaderResourceView* srv = nullptr;
		context->PSGetShaderResources(4, 1, &srv);
		prevPSSRV4.attach(srv);
	}
	// The water windows (t19, t27, t28).
	static constexpr UINT kCasterExtraSlots[3] = { 19, 27, 28 };
	winrt::com_ptr<ID3D11ShaderResourceView> prevVSSRVsExtra[3];
	for (uint32_t i = 0; i < 3; i++)
		context->VSGetShaderResources(kCasterExtraSlots[i], 1, prevVSSRVsExtra[i].put());

	// ---- Common caster state.
	context->PSSetShader(nullptr, nullptr, 0);  // depth-only
	context->RSSetState(shadowCastRS.get());
	context->OMSetDepthStencilState(shadowCastDSS.get(), 0);
	D3D11_VIEWPORT atlasViewport{ 0.0f, 0.0f, float(atlasDesc.Width), float(atlasDesc.Height), 0.0f, 1.0f };
	context->RSSetViewports(1, &atlasViewport);
	// Binding the atlas as DSV force-nulls its SRV binding at PS t4; we
	// restore it at the end.
	ID3D11ShaderResourceView* nullPSSRV = nullptr;
	context->PSSetShaderResources(4, 1, &nullPSSRV);

	// Snow field textures for the patch VS: terrain window, deformation,
	// height field.
	ID3D11ShaderResourceView* vsSRVs[6] = { shellTerrainTexture ? shellTerrainTexture->srv.get() : nullptr,
		GetDeformationSRV(), nullptr, nullptr,
		heightTopFiltered ? heightTopFiltered->srv.get() : nullptr,
		heightBottomFiltered ? heightBottomFiltered->srv.get() : nullptr };
	context->VSSetShaderResources(0, 6, vsSRVs);
	// SampleTerrain's taps, cut as DrawShell cut them for this CB.
	ID3D11ShaderResourceView* casterWetSRV = TerrainWetSRV(lastShellCBData->CompactLook.x > 0.5f);
	context->VSSetShaderResources(kTerrainWetSlot, 1, &casterWetSRV);
	ID3D11ShaderResourceView* casterPlainSRV = TerrainPlainSRV(*lastShellCBData);
	context->VSSetShaderResources(kTerrainPlainSlot, 1, &casterPlainSRV);
	// The land-exact height layer (t13).
	ID3D11ShaderResourceView* casterFineSRV = (shellFineValid && shellTerrainFine) ? shellTerrainFine->srv.get() : nullptr;
	context->VSSetShaderResources(13, 1, &casterFineSRV);
	// The berm field (t14) and trample extent (t56).
	ID3D11ShaderResourceView* bermSRV = GetBermFieldSRV();
	context->VSSetShaderResources(14, 1, &bermSRV);
	ID3D11ShaderResourceView* extentSRV = GetExtentFieldSRV();
	context->VSSetShaderResources(kExtentFieldSlot, 1, &extentSRV);
	// The wide exclusion field (t15).
	ID3D11ShaderResourceView* exclusionSRV = GetExclusionFieldSRV();
	context->VSSetShaderResources(15, 1, &exclusionSRV);
	// The baked undulation (t29).
	ID3D11ShaderResourceView* undulationSRV = GetUndulationFieldSRV();
	context->VSSetShaderResources(29, 1, &undulationSRV);
	// The shore cap's water windows.
	ID3D11ShaderResourceView* waterSRV = waterVisibleTexture && waterVisibleTexture->srv ? waterVisibleTexture->srv.get() : (waterHeightTexture ? waterHeightTexture->srv.get() : nullptr);
	ID3D11ShaderResourceView* waterSRVs[2] = { waterSRV, waterHeightTexture ? waterHeightTexture->srv.get() : nullptr };
	ID3D11ShaderResourceView* waterNearSRV = waterNearTexture && waterNearTexture->srv ? waterNearTexture->srv.get() : nullptr;
	context->VSSetShaderResources(27, 2, waterSRVs);
	context->VSSetShaderResources(19, 1, &waterNearSRV);
	ID3D11Buffer* cb0 = shellCB->CB();
	context->VSSetConstantBuffers(0, 1, &cb0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// The debug split names each cascade: the profiler cannot nest and folds
	// repeated names into per-call samples.
	static const char* const kCasterPatchRow[4] = { "SnowDeformation::Shadows - Trench Patch Cascade 0", "SnowDeformation::Shadows - Trench Patch Cascade 1", "SnowDeformation::Shadows - Trench Patch Cascade 2", "SnowDeformation::Shadows - Trench Patch Cascade 3" };

	// Points shellCB and the depth target at one cascade.
	auto bindCascade = [&](uint32_t a_cascade) {
		// lightTransform maps absolute world -> per-slice [0,1] UV + depth
		// (row-vector XM convention, proven by GetVSMShadow2D). Append the
		// UV->clip mapping and a small caster push AWAY from the light so
		// receivers exactly at the shell surface (fence tops under their own
		// snow) do not acne.
		DirectX::XMMATRIX uvProj = DirectX::XMLoadFloat4x4(
			reinterpret_cast<const DirectX::XMFLOAT4X4*>(&lightRuntime.shadowmapDescriptors[a_cascade].lightTransform));
		constexpr float kCasterDepthPush = 0.0008f;
		DirectX::XMMATRIX uvToClip = DirectX::XMMatrixSet(
			2.0f, 0.0f, 0.0f, 0.0f,
			0.0f, -2.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			-1.0f, 1.0f, kCasterDepthPush, 1.0f);
		DirectX::XMMATRIX clip = DirectX::XMMatrixMultiply(uvProj, uvToClip);

		// ShellCB matrices are row_major with mul(M, v): store the transpose.
		ShellCB shadowCB = *lastShellCBData;
		// Re-anchor the placement to this frame's camera: the snapshot is
		// last frame's, and a one-frame-stale field slides the shadow
		// whenever the camera moves.
		RefreshShellGridPlacement(shadowCB);
		shadowCB.CameraViewProj = DirectX::XMMatrixTranspose(clip);
		// Absolute-world rendering: zero the camera-relative adjust.
		shadowCB.CameraPosAdjust = { 0.0f, 0.0f, 0.0f, 0.0f };
		shadowCB.CameraPreviousPosAdjust = { 0.0f, 0.0f, 0.0f, 0.0f };
		shadowCB.ShellDebugData = 0;
		shellCB->Update(shadowCB);

		context->OMSetRenderTargets(0, nullptr, shadowAtlasDSV[a_cascade].get());
		return shadowCB;
	};

	// The trench patch's caster VS (PATCH + SNOW_SHADOW_CAST) measures its
	// precision pad from the snapped patch centre and fades out over the
	// landscape caster's 40-70 m band. SV_VertexID grid, no IA state; t12
	// (skin-depth raster) is the patch's own addition.
	{
		ID3D11Buffer* nullCB1 = nullptr;
		context->VSSetConstantBuffers(1, 1, &nullCB1);
		context->IASetInputLayout(nullptr);
		ID3D11Buffer* nullVB = nullptr;
		UINT zero = 0;
		context->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
	}
	{
		if (!splitRow)
			globals::profiler->BeginPass("SnowDeformation::Shadows - Trench Patch");
		context->VSSetShader(patchCastVS, nullptr, 0);
		if (patchIndexed)
			context->IASetIndexBuffer(patchCasterIB.get(), DXGI_FORMAT_R32_UINT, 0);
		ID3D11Buffer* patchCB1 = staticsCB->CB();
		context->VSSetConstantBuffers(1, 1, &patchCB1);
		ID3D11ShaderResourceView* patchCastSRVs[3] = {
			heightTopRaw[heightCurrent]->srv.get(),
			heightSkinDepth->srv.get(),
			objectSnowCone ? objectSnowCone->srv.get() : nullptr
		};
		context->VSSetShaderResources(11, 3, patchCastSRVs);
		// Far road level (t40): the same fallback the visible patch takes
		// past the coarse window, so caster and surface stay one recipe.
		ID3D11ShaderResourceView* patchCastFarSRV = (farHalfExtentLive > 0.0f && heightTopRawFar && heightTopRawFar->srv) ? heightTopRawFar->srv.get() : nullptr;
		context->VSSetShaderResources(40, 1, &patchCastFarSRV);
		StaticsCB pscb{};
		FillPatchDrawCB(pscb);
		staticsCB->Update(pscb);
		for (uint32_t cascade = 0; cascade < cascadeCount; cascade++) {
			if (!shadowAtlasDSV[cascade])
				continue;
			if (splitRow)
				globals::profiler->BeginPass(kCasterPatchRow[cascade]);
			bindCascade(cascade);
			if (patchIndexed)
				context->DrawIndexed(patchCasterIndexCount, 0, 0);
			else
				context->Draw(kPatchGridDim * kPatchGridDim * 6, 0);
			if (splitRow)
				globals::profiler->EndPass();
		}
		if (!splitRow)
			globals::profiler->EndPass();
	}

	// ---- Restore everything.
	// Including the CONSTANT BUFFER, not just pipeline state. The caster pass
	// above overwrites the SHARED shellCB with a light-space view and a zeroed
	// CameraPosAdjust (correct for its own absolute-world rendering). Every
	// later consumer this frame reads the same buffer, and the statics skins
	// derive camDist from CameraPosAdjust: left zeroed it puts the camera at
	// the world origin, so every object measures ~80k units away and the
	// distance collapse flattens every skin's lift to nothing. Presented as
	// "distant objects have no snow" because near objects were re-drawn on the
	// frames where the shell pass had refreshed the buffer.
	shellCB->Update(*lastShellCBData);
	{
		ID3D11RenderTargetView* rtvs[8];
		for (uint32_t i = 0; i < 8; i++)
			rtvs[i] = prevRTVs[i].get();
		context->OMSetRenderTargets(8, rtvs, prevDSV.get());
	}
	if (prevViewportCount)
		context->RSSetViewports(prevViewportCount, prevViewports);
	context->RSSetState(prevRS.get());
	context->OMSetDepthStencilState(prevDSS.get(), prevStencilRef);
	context->VSSetShader(prevVS.get(), nullptr, 0);
	context->PSSetShader(prevPS.get(), nullptr, 0);
	context->IASetPrimitiveTopology(prevTopology);
	context->IASetInputLayout(prevLayout.get());
	{
		ID3D11Buffer* cbs[2] = { prevVSCBs[0].get(), prevVSCBs[1].get() };
		context->VSSetConstantBuffers(0, 2, cbs);
	}
	{
		ID3D11ShaderResourceView* srvs[6];
		for (uint32_t i = 0; i < 6; i++)
			srvs[i] = prevVSSRVs[i].get();
		context->VSSetShaderResources(0, 6, srvs);
		// t14 was not saved (nothing else in the frame uses it); clear it.
		ID3D11ShaderResourceView* nullBermSRV = nullptr;
		context->VSSetShaderResources(14, 1, &nullBermSRV);
		context->VSSetShaderResources(kExtentFieldSlot, 1, &nullBermSRV);
		context->VSSetShaderResources(15, 1, &nullBermSRV);
		context->VSSetShaderResources(13, 1, &nullBermSRV);
		context->VSSetShaderResources(29, 1, &nullBermSRV);
		context->VSSetShaderResources(kTerrainWetSlot, 1, &nullBermSRV);
		context->VSSetShaderResources(kTerrainPlainSlot, 1, &nullBermSRV);
		// The patch caster's map set.
		ID3D11ShaderResourceView* skinSrvs[4];
		for (uint32_t i = 0; i < 4; i++)
			skinSrvs[i] = prevVSSRVsSkin[i].get();
		context->VSSetShaderResources(10, 4, skinSrvs);
		for (uint32_t i = 0; i < 3; i++) {
			ID3D11ShaderResourceView* srv = prevVSSRVsExtra[i].get();
			context->VSSetShaderResources(kCasterExtraSlots[i], 1, &srv);
		}
	}
	{
		ID3D11Buffer* vb = prevVB.get();
		context->IASetVertexBuffers(0, 1, &vb, &prevVBStride, &prevVBOffset);
	}
	context->IASetIndexBuffer(prevIB.get(), prevIBFormat, prevIBOffset);
	{
		ID3D11ShaderResourceView* srv = prevPSSRV4.get();
		context->PSSetShaderResources(4, 1, &srv);
	}
}
