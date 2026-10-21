#include "ScreenSpaceShadows.h"

#include <DirectXTex.h>

#include "Features/TerrainBlending.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.screen_space_shadows."

#pragma warning(push)
#pragma warning(disable: 4838 4244)
#include "ScreenSpaceShadows/bend_sss_cpu.h"
#pragma warning(pop)

using RE::RENDER_TARGETS;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceShadows::BendSettings,
	Enable,
	SampleCount,
	SurfaceThickness,
	BilinearThreshold,
	ShadowContrast,
	FilterShadowMask)

void ScreenSpaceShadows::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("general"), "General"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable"), "Enable"), (bool*)&bendSettings.Enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("enable_tooltip"), "Enable screen-space contact shadows from the sun/moon direction."));

		ImGui::SliderInt(T(TKEY("sample_count"), "Sample Count Multiplier"), (int*)&bendSettings.SampleCount, 1, 4, "%d", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("sample_count_tooltip"), "Multiplier for shadow ray sample count. Higher values increase shadow reach at the cost of performance. Adapts to render resolution."));

		ImGui::SliderFloat(T(TKEY("surface_thickness"), "Surface Thickness"), &bendSettings.SurfaceThickness, 0.005f, 0.05f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("surface_thickness_tooltip"), "Assumed thickness of surfaces for shadow detection. Lower values produce thinner, more precise shadows."));

		ImGui::SliderFloat(T(TKEY("bilinear_threshold"), "Bilinear Threshold"), &bendSettings.BilinearThreshold, 0.02f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("bilinear_threshold_tooltip"), "Depth threshold for edge detection during bilinear interpolation. Higher values smooth more aggressively across edges."));

		ImGui::SliderFloat(T(TKEY("shadow_contrast"), "Shadow Contrast"), &bendSettings.ShadowContrast, 0.0f, 4.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shadow_contrast_tooltip"), "Contrast boost for the shadow transition. Higher values produce harder shadow edges."));

		ImGui::Checkbox(T(TKEY("filter_shadow_mask"), "Filter Shadow Mask"), (bool*)&bendSettings.FilterShadowMask);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("filter_shadow_mask_tooltip"), "Applies a depth-aware blur to the sun and light shadow mask, softening shadow map aliasing without bleeding across object edges."));

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

void ScreenSpaceShadows::InvalidateRaymarchShaders()
{
	if (raymarchCS) {
		raymarchCS->Release();
		raymarchCS = nullptr;
	}
}

void ScreenSpaceShadows::ClearShaderCache()
{
	InvalidateRaymarchShaders();
	if (shadowMaskFilterCS) {
		shadowMaskFilterCS->Release();
		shadowMaskFilterCS = nullptr;
	}
	shadowMaskFilterCompileFailed = false;
}

void ScreenSpaceShadows::OnShadowMaskRendered()
{
	shadowMaskFrame = globals::state->frameCount;
}

uint ScreenSpaceShadows::GetScaledSampleCount()
{
	float2 renderSize = Util::ConvertToDynamic(float2{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight });

	// Scale sample count based on both dimensions relative to 1920x1080 reference
	float2 referenceRes = { 1920.0f, 1080.0f };
	float referenceArea = referenceRes.x * referenceRes.y;
	float currentArea = renderSize.x * renderSize.y;
	float areaScale = std::sqrt(currentArea / referenceArea);
	uint scaledSampleCount = static_cast<uint>(std::round(std::clamp(bendSettings.SampleCount, 1u, 4u) * 60 * areaScale));

	// Quantize to steps of 8 to prevent frequent recompilation from small DRS oscillations
	scaledSampleCount = ((scaledSampleCount + 7u) / 8u) * 8u;
	scaledSampleCount = std::max(scaledSampleCount, 8u);

	return scaledSampleCount;
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeRaymarch()
{
	uint scaledSampleCount = GetScaledSampleCount();

	if (scaledSampleCount != lastCompiledSampleCount) {
		lastCompiledSampleCount = scaledSampleCount;
		InvalidateRaymarchShaders();
	}

	if (!raymarchCS) {
		auto sampleCount = std::format("{}", scaledSampleCount);
		std::vector<std::pair<const char*, const char*>> defines{ { "SAMPLE_COUNT", sampleCount.c_str() } };
		// TERRAIN_BLENDING flips DepthTexture's HLSL type from `Texture2D<unorm float>`
		// (R24_UNORM_X8_TYPELESS game depth) to `Texture2D<float>` (R32_FLOAT blendedDepth).
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ "TERRAIN_BLENDING", "" });
		raymarchCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", defines, "cs_5_0");
	}
	return raymarchCS;
}

ID3D11ComputeShader* ScreenSpaceShadows::GetShadowMaskFilterCS()
{
	if (!shadowMaskFilterCS && !shadowMaskFilterCompileFailed) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ "TERRAIN_BLENDING", "" });
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		if (filteredShadowMaskTexture && filteredShadowMaskTexture->uav)
			filteredShadowMaskTexture->uav->GetDesc(&uavDesc);
		if (DirectX::FormatDataType(uavDesc.Format) == DirectX::FORMAT_TYPE_UNORM)
			defines.push_back({ "SHADOW_MASK_UNORM", "" });
		shadowMaskFilterCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\ShadowMaskFilterCS.hlsl", defines, "cs_5_0");
		if (!shadowMaskFilterCS) {
			logger::error("[SSS] Failed to compile the shadow mask filter shader, shadow mask filtering disabled");
			shadowMaskFilterCompileFailed = true;
		}
	}
	return shadowMaskFilterCS;
}

void ScreenSpaceShadows::FilterShadowMask()
{
	auto renderer = globals::game::renderer;
	if (!filteredShadowMaskTexture || !renderer)
		return;

	auto& shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
	auto* depthSRV = Util::GetCurrentSceneDepthSRV(false);
	if (!shadowMask.texture || !shadowMask.SRV || !depthSRV)
		return;

	auto shader = GetShadowMaskFilterCS();
	if (!shader)
		return;

	ZoneScopedS(8);
	TracyD3D11Zone(globals::state->tracyCtx, "SSS - Shadow Mask Filter");
	globals::profiler->BeginPass("ScreenSpaceShadows::ShadowMaskFilter");

	auto context = globals::d3d::context;

	D3D11_TEXTURE2D_DESC maskDesc{};
	shadowMask.texture->GetDesc(&maskDesc);

	D3D11_TEXTURE2D_DESC depthDesc{};
	{
		winrt::com_ptr<ID3D11Resource> depthResource;
		depthSRV->GetResource(depthResource.put());
		winrt::com_ptr<ID3D11Texture2D> depthTexture;
		if (depthResource && depthResource.try_as(depthTexture) && depthTexture)
			depthTexture->GetDesc(&depthDesc);
	}
	if (!depthDesc.Width || !depthDesc.Height)
		depthDesc = maskDesc;

	const float2 maskSize = { (float)maskDesc.Width, (float)maskDesc.Height };
	const float2 renderSize = Util::ConvertToDynamic(maskSize, true);
	const uint copyWidth = std::min((uint)std::ceil(renderSize.x), maskDesc.Width);
	const uint copyHeight = std::min((uint)std::ceil(renderSize.y), maskDesc.Height);
	if (!copyWidth || !copyHeight) {
		globals::profiler->EndPass();
		return;
	}

	ShadowMaskFilterCB data{};
	data.CameraData = Util::GetCameraData();
	data.DepthScale = { (float)depthDesc.Width / maskSize.x, (float)depthDesc.Height / maskSize.y };
	data.RenderSize = { (float)copyWidth, (float)copyHeight };
	data.FarDepthValue = 1.0f;
	shadowMaskFilterCB->Update(data);

	ID3D11ShaderResourceView* views[2] = { shadowMask.SRV, depthSRV };
	context->CSSetShaderResources(0, 2, views);

	auto uav = filteredShadowMaskTexture->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	auto buffer = shadowMaskFilterCB->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	context->CSSetShader(shader, nullptr, 0);
	context->Dispatch((copyWidth + 7) / 8, (copyHeight + 7) / 8, 1);

	views[0] = nullptr;
	views[1] = nullptr;
	context->CSSetShaderResources(0, 2, views);

	uav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &buffer);

	context->CSSetShader(nullptr, nullptr, 0);

	D3D11_BOX box{ 0, 0, 0, copyWidth, copyHeight, 1 };
	context->CopySubresourceRegion(shadowMask.texture, 0, 0, 0, 0, filteredShadowMaskTexture->resource.get(), 0, &box);

	globals::profiler->EndPass();
}

void ScreenSpaceShadows::DrawShadows()
{
	ZoneScopedS(8);
	TracyD3D11Zone(globals::state->tracyCtx, "Screen Space Shadows");

	auto context = globals::d3d::context;

	auto accumulator = *globals::game::currentAccumulator.get();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	auto& directionNi = dirLight->GetWorldDirection();
	float3 light = { directionNi.x, directionNi.y, directionNi.z };
	light.Normalize();
	float4 lightProjection = float4(-light.x, -light.y, -light.z, 0.0f);

	// Helper lambda to calculate light projection
	auto CalculateLightProjection = [&]() -> std::array<float, 4> {
		auto viewProjMat = globals::game::frameBufferCached.GetCameraViewProj().Transpose();
		auto projectedLight = DirectX::SimpleMath::Vector4::Transform(lightProjection, viewProjMat);
		return { projectedLight.x, projectedLight.y, projectedLight.z, projectedLight.w };
	};

	auto lightProjectionF = CalculateLightProjection();

	float2 renderSize = Util::ConvertToDynamic(float2{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight });
	int viewportSize[2] = { (int)renderSize.x, (int)renderSize.y };

	int minRenderBounds[2] = { 0, 0 };
	int maxRenderBounds[2] = { viewportSize[0], viewportSize[1] };

	// Setup common render state.
	// SSS always uses 24/32-bit depth, never the R16_UNORM half-precision path.
	// With TerrainBlending loaded the SRV is R32_FLOAT (blendedDepthTexture);
	// without it, the game's kPOST_ZPREPASS_COPY (R24_UNORM_X8_TYPELESS).
	// The shader's DepthTexture declaration is conditional on TERRAIN_BLENDING:
	// `<float>` for the R32_FLOAT path, `<unorm float>` for the R24_UNORM path.
	auto* depthSRV = Util::GetCurrentSceneDepthSRV(false);
	context->CSSetShaderResources(0, 1, &depthSRV);

	auto uav = screenSpaceShadowsTexture->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	context->CSSetSamplers(0, 1, &pointBorderSampler);

	auto buffer = raymarchCB->CB();
	context->CSSetConstantBuffers(1, 1, &buffer);

	auto viewport = globals::game::graphicsState;

	float2 dynamicRes = { viewport->GetRuntimeData().dynamicResolutionWidthRatio, viewport->GetRuntimeData().dynamicResolutionHeightRatio };

	// Shared dispatch logic
	auto Dispatch = [&](ID3D11ComputeShader* shader, const float* lightProj,
						float invTexSizeX, float invTexSizeY) {
		globals::profiler->BeginPass("ScreenSpaceShadows::RayMarch");

		if (globals::state->frameAnnotations) {
			globals::state->BeginPerfEvent("SSS - Ray March");
		}

		context->CSSetShader(shader, nullptr, 0);

		auto dispatchList = Bend::BuildDispatchList(const_cast<float*>(lightProj), viewportSize, minRenderBounds, maxRenderBounds);

		for (int i = 0; i < dispatchList.DispatchCount; i++) {
			auto dispatchData = dispatchList.Dispatch[i];

			{
				TracyD3D11Zone(globals::state->tracyCtx, "SSS - Dispatch CB");

				RaymarchCB data{};
				data.LightCoordinate[0] = dispatchList.LightCoordinate_Shader[0];
				data.LightCoordinate[1] = dispatchList.LightCoordinate_Shader[1];
				data.LightCoordinate[2] = dispatchList.LightCoordinate_Shader[2];
				data.LightCoordinate[3] = dispatchList.LightCoordinate_Shader[3];

				data.WaveOffset[0] = dispatchData.WaveOffset_Shader[0];
				data.WaveOffset[1] = dispatchData.WaveOffset_Shader[1];

				data.FarDepthValue = 1.0f;
				data.NearDepthValue = 0.0f;

				data.DynamicRes = dynamicRes;

				data.InvDepthTextureSize[0] = invTexSizeX;
				data.InvDepthTextureSize[1] = invTexSizeY;

				data.settings = bendSettings;

				raymarchCB->Update(data);
			}

			{
				TracyD3D11Zone(globals::state->tracyCtx, "SSS - Dispatch Sweep");
				context->Dispatch(dispatchData.WaveCount[0], dispatchData.WaveCount[1], dispatchData.WaveCount[2]);
			}
		}

		if (globals::state->frameAnnotations) {
			globals::state->EndPerfEvent();
		}

		globals::profiler->EndPass();
	};

	float InvTexSizeX = 1.0f / (float)viewportSize[0];
	float InvTexSizeY = 1.0f / (float)viewportSize[1];

	Dispatch(GetComputeRaymarch(), lightProjectionF.data(), InvTexSizeX, InvTexSizeY);

	ID3D11ShaderResourceView* views[1]{ nullptr };
	context->CSSetShaderResources(0, 1, views);

	ID3D11UnorderedAccessView* uavs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11SamplerState* sampler = nullptr;
	context->CSSetSamplers(0, 1, &sampler);

	buffer = nullptr;
	context->CSSetConstantBuffers(1, 1, &buffer);
}

void ScreenSpaceShadows::Prepass()
{
	auto context = globals::d3d::context;

	if (bendSettings.FilterShadowMask && shadowMaskFrame == globals::state->frameCount)
		FilterShadowMask();

	float white[4] = { 1, 1, 1, 1 };
	context->ClearUnorderedAccessViewFloat(screenSpaceShadowsTexture->uav.get(), white);

	if (auto sky = globals::game::sky)
		if (bendSettings.Enable && sky->mode.get() == RE::Sky::Mode::kFull) {
			DrawShadows();
		}

	auto view = screenSpaceShadowsTexture->srv.get();
	context->PSSetShaderResources(45, 1, &view);
}

void ScreenSpaceShadows::LoadSettings(json& o_json)
{
	bendSettings = o_json;
}

void ScreenSpaceShadows::SaveSettings(json& o_json)
{
	o_json = bendSettings;
}

void ScreenSpaceShadows::RestoreDefaultSettings()
{
	bendSettings = {};
}

bool ScreenSpaceShadows::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}

void ScreenSpaceShadows::SetupResources()
{
	raymarchCB = new ConstantBuffer(ConstantBufferDesc<RaymarchCB>(), "SSS::RaymarchCB");

	{
		auto device = globals::d3d::device;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		samplerDesc.BorderColor[0] = 1.0f;
		samplerDesc.BorderColor[1] = 1.0f;
		samplerDesc.BorderColor[2] = 1.0f;
		samplerDesc.BorderColor[3] = 1.0f;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointBorderSampler));
		Util::SetResourceName(pointBorderSampler, "SSS::PointBorderSampler");
	}

	{
		auto renderer = globals::game::renderer;
		auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

		shadowMask.texture->GetDesc(&texDesc);
		shadowMask.SRV->GetDesc(&srvDesc);

		// Store normal front-facing visibility and the reversed depth-offset
		// visibility used by back-facing SSS/transmission receivers.
		texDesc.Format = DXGI_FORMAT_R8G8_UNORM;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		srvDesc.Format = texDesc.Format;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		screenSpaceShadowsTexture = new Texture2D(texDesc, "SSS::ShadowTexture");
		screenSpaceShadowsTexture->CreateSRV(srvDesc);
		screenSpaceShadowsTexture->CreateUAV(uavDesc);
	}

	shadowMaskFilterCB = new ConstantBuffer(ConstantBufferDesc<ShadowMaskFilterCB>(), "SSS::ShadowMaskFilterCB");

	{
		auto device = globals::d3d::device;
		auto shadowMask = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

		shadowMask.texture->GetDesc(&texDesc);
		shadowMask.SRV->GetDesc(&srvDesc);

		UINT formatSupport = 0;
		if (texDesc.SampleDesc.Count == 1 &&
			SUCCEEDED(device->CheckFormatSupport(srvDesc.Format, &formatSupport)) &&
			(formatSupport & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
			texDesc.MipLevels = 1;
			texDesc.ArraySize = 1;
			texDesc.Usage = D3D11_USAGE_DEFAULT;
			texDesc.CPUAccessFlags = 0;
			texDesc.MiscFlags = 0;
			texDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
				.Format = srvDesc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = 0 }
			};
			filteredShadowMaskTexture = new Texture2D(texDesc, "SSS::FilteredShadowMask");
			filteredShadowMaskTexture->CreateUAV(uavDesc);
		} else {
			logger::warn("[SSS] Shadow mask format {} has no typed UAV support, shadow mask filter disabled", magic_enum::enum_name(srvDesc.Format));
		}
	}
}
#undef I18N_KEY_PREFIX
