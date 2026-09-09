#include "PreviewPresenter.h"
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>

namespace gocue::recorder
{
namespace
{
const char* shader = R"hlsl(
Texture2D<float> luma : register(t0);
Texture2D<float2> chroma : register(t1);
SamplerState linearSampler : register(s0);
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vsMain(uint id : SV_VertexID)
{
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv.x * 2 - 1, 1 - v.uv.y * 2, 0, 1);
    return v;
}
float4 psMain(Vertex v) : SV_Target
{
    float y = (luma.Sample(linearSampler, v.uv) * 255.0 - 16.0) / 219.0;
    float2 cbcr = (chroma.Sample(linearSampler, v.uv) * 255.0 - 128.0) / 224.0;
    return float4(saturate(float3(y + 1.5748 * cbcr.y,
        y - 0.187324 * cbcr.x - 0.468124 * cbcr.y, y + 1.8556 * cbcr.x)), 1);
}
)hlsl";
ComPtr<ID3DBlob> compileShader(const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code, errors;
    const auto hr = D3DCompile(shader, std::strlen(shader), "RecorderPreview", nullptr, nullptr, entry, target,
                               D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr)) throw std::runtime_error(errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : hresultText(hr));
    return code;
}
struct NativeHandle
{
    HANDLE value = nullptr;
    ~NativeHandle() { if (value) CloseHandle(value); }
};
struct UploadSlot
{
    ComPtr<ID3D11Texture2D> stagingY, stagingUv, textureY, textureUv;
    ComPtr<ID3D11ShaderResourceView> yView, uvView;
    ComPtr<ID3D11Query> done;
    bool submitted = false;
    FrameStamp stamp;
};
void createPlane(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
                 ComPtr<ID3D11Texture2D>& staging, ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>& view)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    checkHr(device->CreateTexture2D(&desc, nullptr, &staging), "Create upload staging plane");
    desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    checkHr(device->CreateTexture2D(&desc, nullptr, &texture), "Create YUV texture plane");
    checkHr(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "Create YUV plane view");
}
bool upload(ID3D11DeviceContext* context, UploadSlot& slot, VideoSurface& surface)
{
    D3D11_MAPPED_SUBRESOURCE y{}, uv{};
    const auto yHr = context->Map(slot.stagingY.Get(), 0, D3D11_MAP_WRITE, D3D11_MAP_FLAG_DO_NOT_WAIT, &y);
    if (yHr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    checkHr(yHr, "Map luma staging");
    const auto uvHr = context->Map(slot.stagingUv.Get(), 0, D3D11_MAP_WRITE, D3D11_MAP_FLAG_DO_NOT_WAIT, &uv);
    if (FAILED(uvHr))
    {
        context->Unmap(slot.stagingY.Get(), 0);
        if (uvHr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
        checkHr(uvHr, "Map chroma staging");
    }
    for (UINT row = 0; row < surface.height; ++row)
        std::memcpy(static_cast<BYTE*>(y.pData) + static_cast<size_t>(row) * y.RowPitch, surface.y() + static_cast<size_t>(row) * surface.width, surface.width);
    for (UINT row = 0; row < surface.height / 2; ++row)
        std::memcpy(static_cast<BYTE*>(uv.pData) + static_cast<size_t>(row) * uv.RowPitch, surface.uv() + static_cast<size_t>(row) * surface.width, surface.width);
    context->Unmap(slot.stagingY.Get(), 0); context->Unmap(slot.stagingUv.Get(), 0);
    context->CopyResource(slot.textureY.Get(), slot.stagingY.Get());
    context->CopyResource(slot.textureUv.Get(), slot.stagingUv.Get());
    return true;
}
}
struct PreviewPresenter::State
{
    HWND window;
    VideoSurfacePool& pool;
    std::shared_ptr<CaptureTelemetry> telemetry;
    UINT width, height;
    std::atomic<bool> stopping{false}, done{true};
    std::thread thread;
    std::string error;
    juce::var adapter = jsonObject();
    State(HWND w, VideoSurfacePool& p, std::shared_ptr<CaptureTelemetry> t, UINT x, UINT y)
        : window(w), pool(p), telemetry(std::move(t)), width(x), height(y) {}
    void run(std::promise<void> ready)
    {
        bool signalled = false;
        int heldCpu = VideoSurfacePool::none;
        try
        {
            ComApartment apartment;
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            D3D_FEATURE_LEVEL level{};
            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            checkHr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels, 2, D3D11_SDK_VERSION, &device, &level, &context), "Create hardware D3D11 device");
            ComPtr<IDXGIDevice1> dxgiDevice; checkHr(device.As(&dxgiDevice), "Get DXGI device");
            ComPtr<IDXGIAdapter> gpu; checkHr(dxgiDevice->GetAdapter(&gpu), "Get GPU adapter");
            DXGI_ADAPTER_DESC gpuDesc{}; checkHr(gpu->GetDesc(&gpuDesc), "Get GPU description");
            jsonSet(adapter, "description", juce::String(gpuDesc.Description));
            jsonSet(adapter, "vendorId", jsonInt(gpuDesc.VendorId)); jsonSet(adapter, "deviceId", jsonInt(gpuDesc.DeviceId));
            jsonSet(adapter, "featureLevel", static_cast<int>(level));
            LARGE_INTEGER driver{};
            const auto driverHr = gpu->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
            jsonSet(adapter, "driverVersion", SUCCEEDED(driverHr) ? juce::var(juce::String::toHexString(driver.QuadPart)) : juce::var());
            jsonSet(adapter, "driverQueryHresult", hresultText(driverHr));
            ComPtr<IDXGIFactory2> factory; checkHr(gpu->GetParent(IID_PPV_ARGS(&factory)), "Get DXGI factory");
            RECT client{};
            if (!GetClientRect(window, &client)) throw std::runtime_error("Preview HWND unavailable");
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = static_cast<UINT>(client.right); desc.Height = static_cast<UINT>(client.bottom);
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; desc.Scaling = DXGI_SCALING_STRETCH;
            desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            ComPtr<IDXGISwapChain1> swap;
            checkHr(factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &swap), "Create preview swapchain");
            factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
            ComPtr<IDXGISwapChain2> swap2; checkHr(swap.As(&swap2), "Get waitable swapchain");
            checkHr(swap2->SetMaximumFrameLatency(1), "Set frame latency 1");
            NativeHandle frameLatency{swap2->GetFrameLatencyWaitableObject()};
            if (!frameLatency.value) throw std::runtime_error("Swapchain has no frame latency waitable object");
            ComPtr<IDXGIOutput> output;
            if (SUCCEEDED(swap->GetContainingOutput(&output)))
            {
                DXGI_OUTPUT_DESC out{};
                if (SUCCEEDED(output->GetDesc(&out)))
                {
                    DEVMODEW display{}; display.dmSize = sizeof(display);
                    if (EnumDisplaySettingsW(out.DeviceName, ENUM_CURRENT_SETTINGS, &display))
                        jsonSet(adapter, "displayRefreshHz", jsonInt(display.dmDisplayFrequency));
                }
            }
            ComPtr<ID3D11Texture2D> backBuffer;
            checkHr(swap->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "Get backbuffer");
            ComPtr<ID3D11RenderTargetView> target;
            checkHr(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &target), "Create render target");
            const auto vsCode = compileShader("vsMain", "vs_5_0"), psCode = compileShader("psMain", "ps_5_0");
            ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
            checkHr(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "Create preview VS");
            checkHr(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), "Create BT.709 YUV->RGB PS");
            D3D11_SAMPLER_DESC samplerDesc{};
            samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
            ComPtr<ID3D11SamplerState> sampler; checkHr(device->CreateSamplerState(&samplerDesc, &sampler), "Create preview sampler");
            D3D11_RASTERIZER_DESC rasterDesc{}; rasterDesc.FillMode = D3D11_FILL_SOLID; rasterDesc.CullMode = D3D11_CULL_NONE; rasterDesc.DepthClipEnable = TRUE;
            ComPtr<ID3D11RasterizerState> raster; checkHr(device->CreateRasterizerState(&rasterDesc, &raster), "Create preview rasterizer");
            std::array<UploadSlot, 3> uploads;
            for (auto& slot : uploads)
            {
                createPlane(device.Get(), width, height, DXGI_FORMAT_R8_UNORM, slot.stagingY, slot.textureY, slot.yView);
                createPlane(device.Get(), width / 2, height / 2, DXGI_FORMAT_R8G8_UNORM, slot.stagingUv, slot.textureUv, slot.uvView);
                const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
                checkHr(device->CreateQuery(&query, &slot.done), "Create upload completion query");
            }
            NativeHandle timer{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};
            if (!timer.value) timer.value = CreateWaitableTimerW(nullptr, FALSE, nullptr);
            if (!timer.value) throw std::runtime_error("Create preview pacing timer failed");
            jsonSet(adapter, "presentLoopHz", 60); jsonSet(adapter, "maximumFrameLatency", 1);
            jsonSet(adapter, "contextOwner", "present thread only; three fenced upload slots; DO_NOT_WAIT staging maps; no decoder/encoder context sharing");
            int current = -1;
            std::uint64_t lastPresentedFrame = 0;
            auto lastNewPresent = qpcNow(), previousPresent = std::int64_t{0};
            bool stallActive = false;
            const auto start = qpcNow();
            std::uint64_t tick = 0;
            ready.set_value(); signalled = true;
            while (!stopping.load())
            {
                const auto due = start + static_cast<std::int64_t>(++tick * static_cast<std::uint64_t>(telemetry->frequency) / 60);
                const auto now = qpcNow();
                if (due > now)
                {
                    LARGE_INTEGER relative{};
                    relative.QuadPart = -std::max<std::int64_t>(1, static_cast<std::int64_t>(static_cast<double>(due - now) * 10000000.0 / telemetry->frequency));
                    if (!SetWaitableTimer(timer.value, &relative, 0, nullptr, nullptr, FALSE)) throw std::runtime_error("Set preview timer failed");
                    if (WaitForSingleObject(timer.value, 50) != WAIT_OBJECT_0) throw std::runtime_error("Preview pacing timer timed out");
                }
                else if (now - due > telemetry->frequency / 60)
                    tick = static_cast<std::uint64_t>((now - start) * 60 / telemetry->frequency);
                const auto stallNow = qpcNow();
                const auto stallStart = std::max(lastNewPresent, telemetry->firstCallbackQpc.load() + telemetry->frequency);
                if (!stallActive && telemetry->afterWarmup(stallNow) && telemetry->latestReadyFrame.load() > lastPresentedFrame
                    && telemetry->ms(stallNow - stallStart) > 2 * telemetry->fps.periodMs() + 1000.0 / 60)
                { telemetry->loss(LossReason::previewStall); stallActive = true; }
                if (WaitForSingleObject(frameLatency.value, 0) != WAIT_OBJECT_0)
                { telemetry->loss(LossReason::presentBusy); continue; }
                const int index = pool.takeLatest();
                if (index != VideoSurfacePool::none)
                {
                    int candidate = -1;
                    for (int i = 0; i < static_cast<int>(uploads.size()); ++i)
                    {
                        if (i == current) continue;
                        auto& slot = uploads[static_cast<size_t>(i)];
                        if (!slot.submitted) { candidate = i; break; }
                        BOOL complete = FALSE;
                        const auto hr = context->GetData(slot.done.Get(), &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                        checkHr(hr, "Poll GPU upload slot");
                        if (hr == S_OK && complete) { candidate = i; break; }
                    }
                    auto& surface = pool.surface(index);
                    surface.stamp.uploadStart = qpcNow();
                    if (candidate >= 0 && upload(context.Get(), uploads[static_cast<size_t>(candidate)], surface))
                    {
                        surface.stamp.uploadEnd = qpcNow();
                        current = candidate; uploads[static_cast<size_t>(current)].stamp = surface.stamp;
                        if (heldCpu != VideoSurfacePool::none) pool.release(heldCpu);
                        heldCpu = index;
                    }
                    else { telemetry->loss(LossReason::uploadBusy); pool.release(index); }
                }
                const float black[] = {0, 0, 0, 1};
                context->ClearRenderTargetView(target.Get(), black);
                if (current >= 0)
                {
                    auto& slot = uploads[static_cast<size_t>(current)];
                    const float scale = std::min(static_cast<float>(desc.Width) / width, static_cast<float>(desc.Height) / height);
                    D3D11_VIEWPORT viewport{(desc.Width - width * scale) / 2, (desc.Height - height * scale) / 2, width * scale, height * scale, 0, 1};
                    context->RSSetViewports(1, &viewport); context->RSSetState(raster.Get());
                    auto* renderTarget = target.Get(); context->OMSetRenderTargets(1, &renderTarget, nullptr);
                    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(ps.Get(), nullptr, 0);
                    auto* linear = sampler.Get(); context->PSSetSamplers(0, 1, &linear);
                    ID3D11ShaderResourceView* views[] = {slot.yView.Get(), slot.uvView.Get()};
                    context->PSSetShaderResources(0, 2, views); context->Draw(3, 0);
                    ID3D11ShaderResourceView* empty[] = {nullptr, nullptr}; context->PSSetShaderResources(0, 2, empty);
                    context->End(slot.done.Get()); slot.submitted = true;
                }
                const auto submitted = qpcNow();
                const auto hr = swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
                const auto returned = qpcNow();
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { telemetry->loss(LossReason::presentBusy); continue; }
                if (hr == DXGI_STATUS_OCCLUDED) { telemetry->loss(LossReason::presentBusy); continue; }
                checkHr(hr, "Preview Present");
                if (previousPresent) telemetry->duration(Timing::presentInterval, telemetry->ms(submitted - previousPresent));
                previousPresent = submitted;
                if (current < 0) continue;
                auto& stamp = uploads[static_cast<size_t>(current)].stamp;
                if (stamp.frame != lastPresentedFrame)
                {
                    stamp.presentSubmit = submitted;
                    telemetry->recordPresent(stamp, returned); telemetry->presented.fetch_add(1);
                    lastPresentedFrame = stamp.frame; lastNewPresent = submitted; stallActive = false;
                }
                else telemetry->repeatedPresents.fetch_add(1);
            }
            context->ClearState(); context->Flush();
        }
        catch (...)
        {
            if (!signalled) ready.set_exception(std::current_exception());
            try { throw; } catch (const std::exception& e) { error = e.what(); }
            telemetry->loss(LossReason::presentFailure);
        }
        if (heldCpu != VideoSurfacePool::none) pool.release(heldCpu);
        done.store(true);
    }
};
PreviewPresenter::PreviewPresenter(HWND window, VideoSurfacePool& pool, std::shared_ptr<CaptureTelemetry> telemetry, UINT width, UINT height)
    : state(std::make_unique<State>(window, pool, std::move(telemetry), width, height)) {}
PreviewPresenter::~PreviewPresenter() { stop(); }
void PreviewPresenter::start()
{
    if (state->thread.joinable()) throw std::logic_error("Presenter instance is single-use");
    state->done.store(false);
    std::promise<void> promise; auto future = promise.get_future();
    state->thread = std::thread(&State::run, state.get(), std::move(promise));
    future.get();
}
void PreviewPresenter::stop() { state->stopping.store(true); if (state->thread.joinable()) state->thread.join(); }
bool PreviewPresenter::finished() const noexcept { return state->done.load(); }
const std::string& PreviewPresenter::error() const noexcept { return state->error; }
juce::var PreviewPresenter::adapterJson() const { return state->adapter; }
}
