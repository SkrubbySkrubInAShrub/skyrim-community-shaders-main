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
		// Before the snow reads the atlas, so the snowfield receives its own casters.
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

		winrt::com_ptr<ID3D11Resource> liveRes;
		liveAtlasSRV->GetResource(liveRes.put());
		if (auto liveTex = liveRes.try_as<ID3D11Texture2D>()) {
			liveTex->GetDesc(&atlasDesc);
			dbgLodAtlasSlices = atlasDesc.ArraySize;
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

void SnowDeformation::InjectShellShadowCasters(ID3D11ShaderResourceView* a_atlasSRV)
{
	if (SnowShadersPending(1))
		return;
	// One-shot diagnostics: name the exit taken, so a missing shadow points
	// straight at its gate.
	static std::unordered_set<std::string> injectLogged;
	auto logOnce = [](const char* a_msg) {
		if (injectLogged.size() < 12 && injectLogged.insert(a_msg).second)
			logger::info("[SNOW DEFORMATION] ShadowInject: {}", a_msg);
	};

	if (!settings.EnableSnowDeformation || !lastShellCBData) {
		logOnce("skip: disabled or no ShellCB snapshot yet");
		return;
	}

	// Nothing within the shell's own footprint carries positive depth, so every
	// caster vertex is below ground and casts nothing. Measured over the shell's
	// half-span rather than the deformation window: the shell reaches far past
	// the window, and snow at 200 m still casts into view.
	if (!ShellFootprintHasSnow() || GroundShellsSuspended()) {
		logOnce("skip: no snow depth within the shell footprint");
		return;
	}
	// Off-view snow alone: the vertex bake is skipped with the grid, so the
	// caster would evaluate live for a layer whose shadow stays off screen.
	if (!shellDrawGroundGated && !shellViewGateOff) {
		logOnce("skip: no snow in view");
		return;
	}

	auto* vs = GetShellShadowVS();
	if (!vs) {
		logOnce("skip: shell shadow VS unavailable");
		return;
	}

	auto* shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	auto* sunShadowLight = shadowSceneNode ? shadowSceneNode->GetRuntimeData().sunShadowDirLight : nullptr;
	if (!sunShadowLight) {
		logOnce("skip: no sun shadow light");
		return;
	}
	auto& lightRuntime = sunShadowLight->GetRuntimeData();
	// Every sun shadowmap gets the shell: the descriptor list is
	// larger than the two cascades (the focus map is in the family) and its
	// order is not guaranteed - the old min(2) assumption left whichever
	// real cascade sat past index 1 shell-less, presenting as a shadow
	// truncated at the cascade boundary, the cut sweeping with the view.
	const uint32_t cascadeCount = std::min<uint32_t>((uint32_t)lightRuntime.shadowmapDescriptors.size(), 4u);
	if (cascadeCount == 0) {
		logOnce("skip: zero cascades");
		return;
	}

	auto context = globals::d3d::context;
	auto device = globals::d3d::device;

	// Per-slice DSVs on the LIVE atlas texture, cached by texture pointer.
	winrt::com_ptr<ID3D11Resource> atlasRes;
	a_atlasSRV->GetResource(atlasRes.put());
	auto atlasTex = atlasRes.try_as<ID3D11Texture2D>();
	if (!atlasTex) {
		logOnce("skip: atlas SRV resource is not a Texture2D");
		return;
	}
	D3D11_TEXTURE2D_DESC atlasDesc;
	atlasTex->GetDesc(&atlasDesc);
	if (!(atlasDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
		logOnce("skip: atlas lacks DEPTH_STENCIL bind");
		return;
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
		return;
	}
	// Per-cascade DSVs on the REAL slice each descriptor renders to
	// (desc.shadowmapIndex): the atlas is SHARED with local shadow lights,
	// and the sun's cascades move slices as the active-light set changes
	// with the view. Assuming slices 0/1 made the shell's shadow vanish at
	// some camera angles while stamping into other lights' maps; the
	// point-light path at the top of this file always honoured
	// shadowmapIndex. The slices are also recorded for the PS receiving
	// path, which had the same assumption.
	bool sliceFallback = false;
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
			sliceFallback = true;
			if (slice >= atlasDesc.ArraySize)
				continue;
		}
		// The PS receiving path only follows the first two (the cascades it
		// blends); the extra descriptors are inject-only.
		if (i < 2)
			sunCascadeSlice[i] = slice;
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
		logOnce("skip: DSV creation failed");
		return;
	}
	// Change-driven layout log: a one-shot version stops after startup and
	// misses view-dependent reallocation. One line per change of the slice
	// pair, raw descriptor indices or fallback state - reproduce the
	// disappearing shadow and the
	// log names the moment.
	{
		const uint32_t raw0 = lightRuntime.shadowmapDescriptors[0].shadowmapIndex;
		const uint32_t raw1 = cascadeCount > 1 ? lightRuntime.shadowmapDescriptors[1].shadowmapIndex : 0xFFu;
		const uint32_t packed = (sunCascadeSlice[0] & 0xFF) | ((sunCascadeSlice[1] & 0xFF) << 8) |
		                        ((raw0 & 0xFF) << 16) | ((raw1 & 0xFF) << 24);
		static uint32_t lastPacked = 0xFFFFFFFFu;
		static uint32_t changeLogCount = 0;
		if (packed != lastPacked && changeLogCount < 200) {
			lastPacked = packed;
			changeLogCount++;
			logger::info("[SNOW DEFORMATION] ShadowInject: slices used = {}, {} (raw shadowmapIndex = {}, {}; descriptors = {}; atlas has {}; fallback = {})",
				sunCascadeSlice[0], sunCascadeSlice[1], raw0, raw1, (uint32_t)lightRuntime.shadowmapDescriptors.size(), atlasDesc.ArraySize, sliceFallback);
		}
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
		logOnce("skip: RS/DSS creation failed");
		return;
	}
	logOnce("injecting: shell grid into both cascades");

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
	// The vertex bake (t9, t23, t63) and the water windows (t19, t27, t28).
	static constexpr UINT kCasterExtraSlots[6] = { 9, 19, 23, 27, 28, 63 };
	winrt::com_ptr<ID3D11ShaderResourceView> prevVSSRVsExtra[6];
	for (uint32_t i = 0; i < 6; i++)
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

	// Shell field textures for the VS (ShellSurfaceZ): terrain window,
	// deformation, height field. Snow diffuse / scene depth are PS-only.
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
	// The land-exact height layer (t13): the caster must stand on the same
	// ground as the visible shell or it shadows its own snow.
	ID3D11ShaderResourceView* casterFineSRV = (shellFineValid && shellTerrainFine) ? shellTerrainFine->srv.get() : nullptr;
	context->VSSetShaderResources(13, 1, &casterFineSRV);
	// ShellSurfaceZ's berm term reads the bake at t14; without it the caster
	// geometry would lose its berms and undercut the shadows of the shell it
	// is meant to match.
	ID3D11ShaderResourceView* bermSRV = GetBermFieldSRV();
	context->VSSetShaderResources(14, 1, &bermSRV);
	ID3D11ShaderResourceView* extentSRV = GetExtentFieldSRV();
	context->VSSetShaderResources(kExtentFieldSlot, 1, &extentSRV);
	// Same for the wide exclusion field at t15: caster geometry must sink into
	// the clearings, or a melted camp still casts full-depth snow shadows.
	ID3D11ShaderResourceView* exclusionSRV = GetExclusionFieldSRV();
	context->VSSetShaderResources(15, 1, &exclusionSRV);
	// And the baked undulation at t29, or the caster's dunes read zero and
	// its geometry parts company with the drawn shell's.
	ID3D11ShaderResourceView* undulationSRV = GetUndulationFieldSRV();
	context->VSSetShaderResources(29, 1, &undulationSRV);
	// The shore cap's water windows, as DrawShell binds them.
	ID3D11ShaderResourceView* waterSRV = waterVisibleTexture && waterVisibleTexture->srv ? waterVisibleTexture->srv.get() : (waterHeightTexture ? waterHeightTexture->srv.get() : nullptr);
	ID3D11ShaderResourceView* waterSRVs[2] = { waterSRV, waterHeightTexture ? waterHeightTexture->srv.get() : nullptr };
	ID3D11ShaderResourceView* waterNearSRV = waterNearTexture && waterNearTexture->srv ? waterNearTexture->srv.get() : nullptr;
	context->VSSetShaderResources(27, 2, waterSRVs);
	context->VSSetShaderResources(19, 1, &waterNearSRV);
	ID3D11Buffer* cb0 = shellCB->CB();
	context->VSSetConstantBuffers(0, 1, &cb0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Last frame's bake stands in for the surface evaluation while nothing it
	// read has been written since and this frame's placement is the one it
	// baked. With S1 the bake predates DrawShell's object, water and exclusion
	// maps, as the drawn ground does. ObjectLiftCap > 1 reads t11/t12, which
	// the grid caster leaves as it finds them.
	ID3D11VertexShader* casterVS = vs;
	bool castCheckLive = false;
	shellCastBakedLast = false;
	if (shellCastBakeValid && !shellCastBakeOff && !shellGridNonIndexed && lastShellCBData->ObjectLiftCap <= 1.0f &&
		shellVertexBake && shellVertexBakeSlope && shellCastBake) {
		ShellCB placed = *lastShellCBData;
		RefreshShellGridPlacement(placed);
		if (std::memcmp(&placed, lastShellCBData.get(), sizeof(ShellCB)) == 0) {
			ID3D11VertexShader* bakedVS = nullptr;
			if (shellCastBakeCheck && EnsureCastCheckResources()) {
				ReadCastCheck(context);
				bakedVS = GetShellShadowVSBaked(true);
				castCheckLive = bakedVS != nullptr;
			}
			if (!bakedVS)
				bakedVS = GetShellShadowVSBaked(false);
			if (bakedVS) {
				casterVS = bakedVS;
				shellCastBakedLast = true;
				ID3D11ShaderResourceView* bakeSRVs[3] = { shellVertexBake->srv.get(), shellVertexBakeSlope->srv.get(), shellCastBake->srv.get() };
				context->VSSetShaderResources(9, 1, &bakeSRVs[0]);
				context->VSSetShaderResources(23, 1, &bakeSRVs[1]);
				context->VSSetShaderResources(63, 1, &bakeSRVs[2]);
			}
		}
	}
	if (castCheckLive) {
		const UINT zero[4] = {};
		context->ClearUnorderedAccessViewUint(castCheckUAV.get(), zero);
	}

	// Stage-major: every cascade's ground, then every cascade's trench patch,
	// one row each. Both are depth-only into the cascade, so the order cannot
	// change what lands there. The debug split names each cascade instead:
	// the profiler cannot nest and folds repeated names into per-call samples.
	static const char* const kCasterGridRow[4] = { "SnowDeformation::Shadows - Ground Cascade 0", "SnowDeformation::Shadows - Ground Cascade 1", "SnowDeformation::Shadows - Ground Cascade 2", "SnowDeformation::Shadows - Ground Cascade 3" };
	static const char* const kCasterPatchRow[4] = { "SnowDeformation::Shadows - Trench Patch Cascade 0", "SnowDeformation::Shadows - Trench Patch Cascade 1", "SnowDeformation::Shadows - Trench Patch Cascade 2", "SnowDeformation::Shadows - Trench Patch Cascade 3" };
	const bool splitRow = shellCasterSplitDebug;

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
		// Re-anchor the grid to this frame's camera: the snapshot
		// is last frame's, and a one-frame-stale grid slides a crisp
		// full-surface shadow whenever the camera moves.
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

	// Terrain shell: vertex-buffer-less grid with the excess-height caster
	// VS. b1 is cleared for the grid: it declared a b1 of its own until the
	// bow wave was retired, and a stale StaticsCB there once read the patch
	// fill's world-space centre as wave params - phantom trail geometry in
	// the far cascades (Josef's giant trench wedges). The grid reads no b1
	// now, so this only keeps a stale buffer from being visible to it if
	// one is ever added back.
	{
		ID3D11Buffer* nullCB1 = nullptr;
		context->VSSetConstantBuffers(1, 1, &nullCB1);
		context->IASetInputLayout(nullptr);
		ID3D11Buffer* nullVB = nullptr;
		UINT zero = 0;
		context->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
		context->VSSetShader(casterVS, nullptr, 0);
	}
	// Every cell the grid can sample bare: castVis kills every vertex.
	const bool gridMayCast = CasterDiscMayCast();
	if (!gridMayCast) {
		if (!splitRow)
			globals::profiler->MarkPassSkipped("SnowDeformation::Shadows - Ground");
		castCheckLive = false;
	} else {
		if (!splitRow)
			globals::profiler->BeginPass("SnowDeformation::Shadows - Ground");
		for (uint32_t cascade = 0; cascade < cascadeCount; cascade++) {
			if (!shadowAtlasDSV[cascade])
				continue;
			if (splitRow)
				globals::profiler->BeginPass(kCasterGridRow[cascade]);
			const ShellCB shadowCB = bindCascade(cascade);
			if (castCheckLive) {
				ID3D11UnorderedAccessView* checkUAV = castCheckUAV.get();
				context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, shadowAtlasDSV[cascade].get(), 1, 1, &checkUAV, nullptr);
			}
			DrawShellCasterGrid(context, shadowCB);
			if (splitRow)
				globals::profiler->EndPass();
		}
		if (!splitRow)
			globals::profiler->EndPass();
	}
	if (castCheckLive) {
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, 1, 1, &nullUAV, nullptr);
		context->CopyResource(castCheckStaging[castCheckRing].get(), castCheckBuffer.get());
		castCheckIssued[castCheckRing] = true;
		castCheckRing = (castCheckRing + 1) % kCastCheckRing;
	}

	// The TRENCH PATCH casts too - attempt two of "road snow casts
	// shadows" (Josef: trench walls throw no shadow onto their own
	// floor). Attempt one stretched shadows across the surface because
	// the caster pass zeroes the camera adjust and the distance-based
	// precision pad ballooned to full depth - an uncarved slab. The
	// PATCH+SNOW_SHADOW_CAST VS measures the pad from the snapped patch
	// centre instead and fades out over the landscape caster's 40-70 m
	// band. SV_VertexID grid, no IA state; t0 terrain,
	// t1 deformation and t14 berm bake are already bound above for the
	// shell grid, t11/t13 by the skin block; t12 (skin-depth raster,
	// inside the saved t10-t13 range) is the patch's own addition.
	// Same gate as the visible patch draw, one frame stale like the
	// skins: capturedRoads is last frame's count here (Prepass resets it
	// after the mask passes), the frame whose rasters this VS reads, and
	// with none the VS kills every vertex.
	// Indexed over the shared lattice: one VS invocation per vertex, the
	// same triangles in the same order, minus those past the fade band.
	const bool patchIndexed = !patchCasterNonIndexed && !SnowShadersPending(2) && GetPatchShadowVS(true) && EnsurePatchCasterIB();
	if (auto* patchCastVS = SnowShadersPending(2) ? nullptr : GetPatchShadowVS(patchIndexed);
		patchCastVS && heightSkinDepth && heightSkinDepth->srv &&
		heightTopRaw[heightCurrent] && heightTopRaw[heightCurrent]->srv &&
		settings.RoadMeshesDepth > 0.5f && capturedRoads > 0) {
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
	} else if (!splitRow) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Shadows - Trench Patch");
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
		for (uint32_t i = 0; i < 6; i++) {
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

bool SnowDeformation::EnsureCastCheckResources()
{
	if (castCheckBuffer && castCheckUAV && castCheckStaging[kCastCheckRing - 1])
		return true;
	auto device = globals::d3d::device;
	// The counter is written from the vertex shader.
	if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1)
		return false;
	castCheckBuffer = nullptr;
	castCheckUAV = nullptr;
	for (auto& staging : castCheckStaging)
		staging = nullptr;
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = 16;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	if (FAILED(device->CreateBuffer(&desc, nullptr, castCheckBuffer.put())))
		return false;
	Util::SetResourceName(castCheckBuffer.get(), "SnowDeformation::CastCheck");
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.NumElements = 4;
	uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	if (FAILED(device->CreateUnorderedAccessView(castCheckBuffer.get(), &uavDesc, castCheckUAV.put())))
		return false;
	Util::SetResourceName(castCheckUAV.get(), "SnowDeformation::CastCheck UAV");
	D3D11_BUFFER_DESC stagingDesc{};
	stagingDesc.ByteWidth = 16;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (auto& staging : castCheckStaging) {
		if (FAILED(device->CreateBuffer(&stagingDesc, nullptr, staging.put())))
			return false;
		Util::SetResourceName(staging.get(), "SnowDeformation::CastCheck Staging");
	}
	for (auto& issued : castCheckIssued)
		issued = false;
	return true;
}

void SnowDeformation::ReadCastCheck(ID3D11DeviceContext* a_context)
{
	for (int i = 0; i < kCastCheckRing; i++) {
		if (!castCheckIssued[i] || !castCheckStaging[i])
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (a_context->Map(castCheckStaging[i].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK)
			continue;
		const auto* counts = static_cast<const uint32_t*>(mapped.pData);
		castCheckMismatches += counts[0];
		castCheckVertices += counts[1];
		castCheckFrames++;
		a_context->Unmap(castCheckStaging[i].get(), 0);
		castCheckIssued[i] = false;
	}
}
