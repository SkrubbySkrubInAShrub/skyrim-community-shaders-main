#include "RainRefraction.h"

#include <DirectXTex.h>

#include "Effects11.h"
#include "Effects11/EffectManager.h"
#include "Effects11/SettingManager.h"

#include "Deferred.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	RainRefraction::Settings,
	Enable,
	MotionStretch,
	MotionTransparency,
	ScreenRefraction,
	RefractionFactor,
	MotionBluriness,
	Supersampling)

namespace
{
	constexpr auto kDefaultRaindropsPath = L"Data\\Shaders\\RainRefraction\\Raindrops.png";
	constexpr auto kBackgroundDownsamplePath = L"Data\\Shaders\\RainRefraction\\BackgroundDownsampleCS.hlsl";
	constexpr UINT kBackgroundSlot = 81;

	/** @brief Loads a raindrop normal map (RGB normal, A coverage) as a mipmapped BC7 texture named a_name. */
	RainRefraction::RaindropTexture LoadRaindropTexture(const std::filesystem::path& a_path, const char* a_name)
	{
		RainRefraction::RaindropTexture result;
		const auto fail = [&](std::string a_status) {
			logger::warn("[Rain Refraction] {}: {}", a_path.string(), a_status);
			result.status = std::move(a_status);
			return std::move(result);
		};

		// Most presets ship without enbraindrops.png, so a missing file is not worth a warning
		if (!std::filesystem::exists(a_path)) {
			logger::debug("[Rain Refraction] Raindrop texture not found: {}", a_path.string());
			result.status = std::format("Texture not found: {}", a_path.filename().string());
			return result;
		}

		DirectX::ScratchImage image;
		HRESULT hr = DirectX::LoadFromWICFile(a_path.c_str(), DirectX::WIC_FLAGS_IGNORE_SRGB, nullptr, image);
		if (FAILED(hr))
			return fail(std::format("Failed to load texture (invalid image, HRESULT 0x{:08X})", static_cast<uint32_t>(hr)));

		DirectX::ScratchImage mipImage;
		hr = DirectX::GenerateMipMaps(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DirectX::TEX_FILTER_DEFAULT, 0, mipImage);
		if (FAILED(hr))
			return fail(std::format("Failed to generate mipmaps (HRESULT 0x{:08X})", static_cast<uint32_t>(hr)));

		DirectX::ScratchImage bc7Image;
		hr = DirectX::Compress(mipImage.GetImages(), mipImage.GetImageCount(), mipImage.GetMetadata(),
			DXGI_FORMAT_BC7_UNORM, DirectX::TEX_COMPRESS_BC7_QUICK, 1.0f, bc7Image);
		if (FAILED(hr))
			return fail(std::format("Failed to compress texture (HRESULT 0x{:08X})", static_cast<uint32_t>(hr)));

		const auto& metadata = bc7Image.GetMetadata();
		winrt::com_ptr<ID3D11Texture2D> texture;
		hr = DirectX::CreateTexture(globals::d3d::device, bc7Image.GetImages(), bc7Image.GetImageCount(), metadata,
			reinterpret_cast<ID3D11Resource**>(texture.put()));
		if (FAILED(hr))
			return fail(std::format("Failed to create GPU texture (HRESULT 0x{:08X})", static_cast<uint32_t>(hr)));
		Util::SetResourceName(texture.get(), a_name);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{
			.Format = DXGI_FORMAT_BC7_UNORM,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = static_cast<UINT>(metadata.mipLevels) }
		};
		hr = globals::d3d::device->CreateShaderResourceView(texture.get(), &srvDesc, result.srv.put());
		if (FAILED(hr))
			return fail(std::format("Failed to create shader resource view (HRESULT 0x{:08X})", static_cast<uint32_t>(hr)));
		Util::SetResourceName(result.srv.get(), "%s SRV", a_name);

		logger::info("[Rain Refraction] Loaded raindrop texture: {} ({}x{}, BC7, {} mips)", a_path.string(), metadata.width, metadata.height, metadata.mipLevels);
		return result;
	}
}

#define I18N_KEY_PREFIX "feature.rain_refraction."

void RainRefraction::DrawSettings()
{
	const Util::LockedSection enbLock(IsManagedByENB(),
		T("common.settings_managed_by_enb", "This setting is managed by Effects 11."));

	if (!defaultRaindrops.status.empty())
		Util::Text::WrappedWarning("%s", defaultRaindrops.status.c_str());

	ImGui::Checkbox(T(TKEY("enable"), "Enable"), &settings.Enable);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_tooltip"), "Renders rain as refractive drops. Requires Dynamic Cubemaps."));
	}
	ImGui::SliderFloat(T(TKEY("motion_stretch"), "Motion Stretch"), &settings.MotionStretch, kMinMotionStretch, kMaxMotionStretch, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("motion_stretch_tooltip"), "How far drops stretch along their fall velocity.\nLow values give round drops, 1 = vanilla streak length, higher values stretch further."));
	}
	ImGui::SliderFloat(T(TKEY("motion_transparency"), "Motion Transparency"), &settings.MotionTransparency, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("motion_transparency_tooltip"), "Fades the drops; higher values make rain more transparent."));
	}
	ImGui::Checkbox(T(TKEY("screen_refraction"), "Screen Refraction"), &settings.ScreenRefraction);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("screen_refraction_tooltip"), "Drops refract the scene behind them like a small lens, showing it inverted.\nDynamic Cubemaps fill in where the refracted view leaves the screen."));
	}
	{
		auto _disabled = Util::DisableGuard(!settings.ScreenRefraction);
		ImGui::SliderFloat(T(TKEY("refraction_factor"), "Refraction Strength"), &settings.RefractionFactor, 0.0f, kMaxRefractionFactor, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("refraction_factor_tooltip"), "How strongly drops bend the view behind them. 1 = water, 0 = no bending."));
		}
		ImGui::SliderFloat(T(TKEY("motion_bluriness"), "Refraction Blur"), &settings.MotionBluriness, 0.0f, kMaxMotionBluriness, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("motion_bluriness_tooltip"), "Blurs the refracted scene to suggest motion. Values up to 1 keep it sharp."));
		}
	}
	ImGui::Checkbox(T(TKEY("supersampling"), "Supersampling"), &settings.Supersampling);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("supersampling_tooltip"), "Samples each drop four times per pixel to reduce shimmering on thin streaks, at a small cost."));
	}
}

#undef I18N_KEY_PREFIX

void RainRefraction::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.MotionStretch = std::clamp(settings.MotionStretch, kMinMotionStretch, kMaxMotionStretch);
	settings.MotionTransparency = std::clamp(settings.MotionTransparency, 0.0f, 1.0f);
	settings.RefractionFactor = std::clamp(settings.RefractionFactor, 0.0f, kMaxRefractionFactor);
	settings.MotionBluriness = std::clamp(settings.MotionBluriness, 0.0f, kMaxMotionBluriness);
}

void RainRefraction::SaveSettings(json& o_json)
{
	o_json = settings;
}

void RainRefraction::RestoreDefaultSettings()
{
	settings = {};
}

void RainRefraction::SetupResources()
{
	defaultRaindrops = LoadRaindropTexture(kDefaultRaindropsPath, "RainRefraction::DefaultRaindrops");

	D3D11_BLEND_DESC blendDesc{};
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (FAILED(globals::d3d::device->CreateBlendState(&blendDesc, alphaBlendState.put()))) {
		logger::error("[Rain Refraction] Failed to create alpha blend state, refractive rain disabled");
		return;
	}
	Util::SetResourceName(alphaBlendState.get(), "RainRefraction::AlphaBlendState");
}

void RainRefraction::ClearShaderCache()
{
	downsampleCS = nullptr;
	downsampleCompileFailed = false;
}

void RainRefraction::LoadPresetRaindrops(const std::filesystem::path& a_presetPath)
{
	presetRaindrops = LoadRaindropTexture(a_presetPath / "enbraindrops.png", "RainRefraction::PresetRaindrops");
}

bool RainRefraction::IsManagedByENB() const
{
	const auto& effects11 = globals::features::effects11;
	return effects11.loaded && effects11.enableEffect;
}

ID3D11ShaderResourceView* RainRefraction::GetRaindropSRV() const
{
	if (IsManagedByENB() && presetRaindrops.srv)
		return presetRaindrops.srv.get();
	return defaultRaindrops.srv.get();
}

RainRefraction::PerFrame RainRefraction::GetCommonBufferData()
{
	// Effects 11 resolves its active state and time of day on first use each frame
	auto& effects11 = globals::features::effects11;
	if (effects11.loaded)
		effects11.CheckCommonData();

	if (!perFrameCacheChecker.IsNewFrame())
		return perFrameCache;

	PerFrame data{
		.Enable = settings.Enable,
		.MotionStretch = settings.MotionStretch,
		.MotionTransparency = settings.MotionTransparency,
		.ScreenRefraction = settings.ScreenRefraction,
		.RefractionFactor = settings.RefractionFactor,
		.MotionBluriness = settings.MotionBluriness,
		.Brightness = 1.0f,
		.Supersampling = settings.Supersampling
	};

	if (IsManagedByENB()) {
		auto& settingManager = SettingManager::GetSingleton();
		data.Enable = settingManager.GetValue<bool>(EffectManager::GetSingleton().ids.enableRain);
		data.MotionStretch = settingManager.GetInterpolatedTimeOfDayValue("MotionStretch", "RAIN");
		data.MotionTransparency = settingManager.GetInterpolatedTimeOfDayValue("MotionTransparency", "RAIN");
		data.ScreenRefraction = true;
		data.RefractionFactor = settingManager.GetValue<float>(EffectManager::GetSingleton().ids.rainRefractionFactor);
		data.MotionBluriness = settingManager.GetInterpolatedTimeOfDayValue("MotionBluriness", "RAIN");
		data.Brightness = settingManager.GetInterpolatedTimeOfDayValue("Brightness", "RAIN");
		data.Supersampling = settingManager.GetValue<bool>(EffectManager::GetSingleton().ids.rainSupersampling);
	}
	data.Enable = data.Enable && GetRaindropSRV() && alphaBlendState;

	perFrameCache = data;
	return data;
}

bool RainRefraction::IsRefractiveRainPass()
{
	auto state = globals::state;
	// Runs per draw, so reuse the last feature buffer resolve instead of re-querying settings
	return state->currentPixelDescriptor == static_cast<uint32_t>(SIE::ShaderCache::ParticleShaderTechniques::EnvCubeRain) &&
	       perFrameCache.Enable;
}

void RainRefraction::ModifyParticle()
{
	if (!IsRefractiveRainPass())
		return;

	auto context = globals::d3d::context;
	ID3D11ShaderResourceView* srv = GetRaindropSRV();
	context->PSSetShaderResources(80, 1, &srv);

	ID3D11Buffer* cbs[] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->VSSetConstantBuffers(5, 2, cbs);
}

void RainRefraction::ParticleShaderHacks()
{
	auto state = globals::state;
	if (!state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Particle)
		return;
	if (!IsRefractiveRainPass())
		return;

	auto context = globals::d3d::context;

	if (perFrameCache.ScreenRefraction && backgroundFrame != globals::state->frameCount)
		CaptureBackground();

	ID3D11ShaderResourceView* background = backgroundFrame == globals::state->frameCount && backgroundTexture ? backgroundTexture->srv.get() : nullptr;
	context->PSSetShaderResources(kBackgroundSlot, 1, &background);

	float blendFactor[4] = { 0, 0, 0, 0 };
	context->OMSetBlendState(alphaBlendState.get(), blendFactor, 0xFFFFFFFF);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
}

bool RainRefraction::EnsureBackgroundResources()
{
	if (backgroundUnsupported)
		return false;

	if (!backgroundTexture) {
		auto device = globals::d3d::device;

		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		for (auto candidate : { DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT }) {
			UINT support = 0;
			constexpr UINT required = D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW | D3D11_FORMAT_SUPPORT_MIP_AUTOGEN | D3D11_FORMAT_SUPPORT_RENDER_TARGET;
			if (SUCCEEDED(device->CheckFormatSupport(candidate, &support)) && (support & required) == required) {
				format = candidate;
				break;
			}
		}
		if (format == DXGI_FORMAT_UNKNOWN) {
			logger::warn("[Rain Refraction] No format supports a mipmapped background UAV, screen refraction disabled");
			backgroundUnsupported = true;
			return false;
		}

		const UINT width = std::max((globals::game::graphicsState->screenWidth + 1) / 2, 1u);
		const UINT height = std::max((globals::game::graphicsState->screenHeight + 1) / 2, 1u);
		const UINT mipLevels = std::min(static_cast<UINT>(std::floor(std::log2(static_cast<float>(std::max(width, height))))) + 1, 7u);

		D3D11_TEXTURE2D_DESC texDesc{
			.Width = width,
			.Height = height,
			.MipLevels = mipLevels,
			.ArraySize = 1,
			.Format = format,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{
			.Format = format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = mipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{
			.Format = format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		try {
			backgroundTexture = std::make_unique<Texture2D>(texDesc, "RainRefraction::Background");
			backgroundTexture->CreateSRV(srvDesc);
			backgroundTexture->CreateUAV(uavDesc);
			downsampleCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<DownsampleCB>(), "RainRefraction::DownsampleCB");
		} catch (const std::exception& e) {
			logger::error("[Rain Refraction] Failed to create the background texture ({}), screen refraction disabled", e.what());
			backgroundTexture.reset();
			downsampleCB.reset();
			backgroundUnsupported = true;
			return false;
		}
	}

	if (!downsampleCS && !downsampleCompileFailed) {
		downsampleCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(kBackgroundDownsamplePath, {}, "cs_5_0")));
		if (!downsampleCS) {
			logger::error("[Rain Refraction] Failed to compile the background downsample shader, screen refraction disabled");
			downsampleCompileFailed = true;
		}
	}

	return downsampleCS != nullptr;
}

void RainRefraction::CaptureBackground()
{
	auto renderer = globals::game::renderer;
	if (!renderer)
		return;

	if (!EnsureBackgroundResources())
		return;

	auto context = globals::d3d::context;

	ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
	ID3D11DepthStencilView* depthStencil = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, &depthStencil);

	const auto releaseTargets = [&]() {
		for (auto*& renderTarget : renderTargets) {
			if (renderTarget)
				renderTarget->Release();
			renderTarget = nullptr;
		}
		if (depthStencil)
			depthStencil->Release();
		depthStencil = nullptr;
	};

	const auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN];
	if (!renderTargets[0] || renderTargets[0] != main.RTV || !main.SRV || !main.texture) {
		releaseTargets();
		return;
	}

	ZoneScopedN("RainRefraction::CaptureBackground");
	TracyD3D11Zone(globals::state->tracyCtx, "Rain Refraction - Capture Background");

	D3D11_TEXTURE2D_DESC mainDesc{};
	main.texture->GetDesc(&mainDesc);

	const float2 renderSize = Util::ConvertToDynamic(float2{ static_cast<float>(mainDesc.Width), static_cast<float>(mainDesc.Height) }, true);
	const uint outputWidth = backgroundTexture->desc.Width;
	const uint outputHeight = backgroundTexture->desc.Height;

	DownsampleCB data{
		.InvSourceSize = { 1.0f / static_cast<float>(mainDesc.Width), 1.0f / static_cast<float>(mainDesc.Height) },
		.OutputSize = { outputWidth, outputHeight },
		.SourceUVMax = { std::max(renderSize.x - 1.0f, 0.5f) / static_cast<float>(mainDesc.Width), std::max(renderSize.y - 1.0f, 0.5f) / static_cast<float>(mainDesc.Height) }
	};
	downsampleCB->Update(data);

	ID3D11ComputeShader* savedShader = nullptr;
	ID3D11ShaderResourceView* savedSRV = nullptr;
	ID3D11UnorderedAccessView* savedUAV = nullptr;
	ID3D11SamplerState* savedSampler = nullptr;
	ID3D11Buffer* savedCB = nullptr;
	context->CSGetShader(&savedShader, nullptr, nullptr);
	context->CSGetShaderResources(0, 1, &savedSRV);
	context->CSGetUnorderedAccessViews(0, 1, &savedUAV);
	context->CSGetSamplers(0, 1, &savedSampler);
	context->CSGetConstantBuffers(0, 1, &savedCB);

	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->PSSetShaderResources(kBackgroundSlot, 1, &nullSRV);
	context->OMSetRenderTargets(0, nullptr, nullptr);

	ID3D11ShaderResourceView* source = main.SRV;
	ID3D11UnorderedAccessView* output = backgroundTexture->uav.get();
	ID3D11SamplerState* sampler = globals::deferred->linearSampler;
	ID3D11Buffer* buffer = downsampleCB->CB();
	context->CSSetShader(downsampleCS.get(), nullptr, 0);
	context->CSSetShaderResources(0, 1, &source);
	context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
	context->CSSetSamplers(0, 1, &sampler);
	context->CSSetConstantBuffers(0, 1, &buffer);
	context->Dispatch((outputWidth + 7) / 8, (outputHeight + 7) / 8, 1);

	context->CSSetShaderResources(0, 1, &nullSRV);
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);

	context->GenerateMips(backgroundTexture->srv.get());

	context->CSSetShader(savedShader, nullptr, 0);
	context->CSSetShaderResources(0, 1, &savedSRV);
	context->CSSetUnorderedAccessViews(0, 1, &savedUAV, nullptr);
	context->CSSetSamplers(0, 1, &savedSampler);
	context->CSSetConstantBuffers(0, 1, &savedCB);
	for (IUnknown* saved : std::initializer_list<IUnknown*>{ savedShader, savedSRV, savedUAV, savedSampler, savedCB }) {
		if (saved)
			saved->Release();
	}

	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, depthStencil);
	releaseTargets();

	backgroundFrame = globals::state->frameCount;
}
