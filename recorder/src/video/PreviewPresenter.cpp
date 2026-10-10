#include "PreviewPresenter.h"
#include "support/ThreadPriority.h"
#include "PresentPacing.h"
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
    checkHr(hr, "미리보기 셰이더를 컴파일할 수 없습니다.");
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
    checkHr(device->CreateTexture2D(&desc, nullptr, &staging), "미리보기 업로드용 임시 버퍼를 만들 수 없습니다.");
    desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    checkHr(device->CreateTexture2D(&desc, nullptr, &texture), "미리보기 YUV 텍스처를 만들 수 없습니다.");
    checkHr(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "미리보기 YUV 채널을 준비할 수 없습니다.");
}
bool upload(ID3D11DeviceContext* context, UploadSlot& slot, VideoSurface& surface)
{
    D3D11_MAPPED_SUBRESOURCE y{}, uv{};
    const auto yHr = context->Map(slot.stagingY.Get(), 0, D3D11_MAP_WRITE, D3D11_MAP_FLAG_DO_NOT_WAIT, &y);
    if (yHr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    checkHr(yHr, "미리보기 밝기 채널 버퍼에 접근할 수 없습니다.");
    const auto uvHr = context->Map(slot.stagingUv.Get(), 0, D3D11_MAP_WRITE, D3D11_MAP_FLAG_DO_NOT_WAIT, &uv);
    if (FAILED(uvHr))
    {
        context->Unmap(slot.stagingY.Get(), 0);
        if (uvHr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
        checkHr(uvHr, "미리보기 색상 채널 버퍼에 접근할 수 없습니다.");
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
    void run(std::promise<void> ready, std::shared_future<void> measurementStart)
    {
        ScopedRecorderPriority priority(RecorderThreadRole::capturePreview); telemetry->previewPriorityError.store(priority.error);
        bool signalled = false;
        try
        {
            ComApartment apartment;
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            D3D_FEATURE_LEVEL level{};
            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            checkHr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels, 2, D3D11_SDK_VERSION, &device, &level, &context), "D3D11 하드웨어 장치를 만들 수 없습니다.");
            ComPtr<IDXGIDevice1> dxgiDevice; checkHr(device.As(&dxgiDevice), "DXGI 장치를 가져올 수 없습니다.");
            ComPtr<IDXGIAdapter> gpu; checkHr(dxgiDevice->GetAdapter(&gpu), "GPU 장치를 가져올 수 없습니다.");
            DXGI_ADAPTER_DESC gpuDesc{}; checkHr(gpu->GetDesc(&gpuDesc), "GPU 정보를 읽을 수 없습니다.");
            jsonSet(adapter, "description", juce::String(gpuDesc.Description));
            jsonSet(adapter, "vendorId", jsonInt(gpuDesc.VendorId)); jsonSet(adapter, "deviceId", jsonInt(gpuDesc.DeviceId));
            jsonSet(adapter, "featureLevel", static_cast<int>(level));
            LARGE_INTEGER driver{};
            const auto driverHr = gpu->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
            jsonSet(adapter, "driverVersion", SUCCEEDED(driverHr) ? juce::var(juce::String::toHexString(driver.QuadPart)) : juce::var());
            jsonSet(adapter, "driverQueryHresult", hresultText(driverHr));
            ComPtr<IDXGIFactory2> factory; checkHr(gpu->GetParent(IID_PPV_ARGS(&factory)), "DXGI 화면 표시 기능을 가져올 수 없습니다.");
            RECT client{};
            if (!GetClientRect(window, &client)) throw std::runtime_error("미리보기 창을 사용할 수 없습니다.");
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = static_cast<UINT>(client.right); desc.Height = static_cast<UINT>(client.bottom);
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; desc.Scaling = DXGI_SCALING_STRETCH;
            desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            ComPtr<IDXGISwapChain1> swap;
            checkHr(factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &swap), "미리보기 화면 교환 버퍼를 만들 수 없습니다.");
            factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
            ComPtr<IDXGISwapChain2> swap2; checkHr(swap.As(&swap2), "미리보기 화면 대기 기능을 가져올 수 없습니다.");
            checkHr(swap2->SetMaximumFrameLatency(1), "프레임 지연을 1로 설정할 수 없습니다.");
            NativeHandle frameLatency{swap2->GetFrameLatencyWaitableObject()};
            if (!frameLatency.value) throw std::runtime_error("화면 교환 버퍼에 프레임 지연 대기 기능이 없습니다.");
            ComPtr<IDXGIOutput> output;
            jsonSet(adapter, "displayRefreshStatus", "UNAVAILABLE");
            jsonSet(adapter, "displayRefreshReason", "DXGI output or current display settings could not be queried");
            if (SUCCEEDED(swap->GetContainingOutput(&output)))
            {
                DXGI_OUTPUT_DESC out{};
                if (SUCCEEDED(output->GetDesc(&out)))
                {
                    DEVMODEW display{}; display.dmSize = sizeof(display);
                    if (EnumDisplaySettingsW(out.DeviceName, ENUM_CURRENT_SETTINGS, &display))
                    {
                        jsonSet(adapter, "displayRefreshHz", jsonInt(display.dmDisplayFrequency));
                        jsonSet(adapter, "displayRefreshStatus", displayRefreshVerdict(static_cast<int>(display.dmDisplayFrequency)));
                        jsonSet(adapter, "displayRefreshReason", "EnumDisplaySettings integer Hz; 59 includes 59.94; 0/1 is unavailable");
                    }
                }
            }
            ComPtr<ID3D11Texture2D> backBuffer;
            checkHr(swap->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "미리보기 후면 버퍼를 가져올 수 없습니다.");
            ComPtr<ID3D11RenderTargetView> target;
            checkHr(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &target), "미리보기 출력 대상을 만들 수 없습니다.");
            const auto vsCode = compileShader("vsMain", "vs_5_0"), psCode = compileShader("psMain", "ps_5_0");
            ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
            checkHr(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "미리보기 정점 셰이더를 만들 수 없습니다.");
            checkHr(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), "BT.709 YUV→RGB 픽셀 셰이더를 만들 수 없습니다.");
            D3D11_SAMPLER_DESC samplerDesc{};
            samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
            ComPtr<ID3D11SamplerState> sampler; checkHr(device->CreateSamplerState(&samplerDesc, &sampler), "미리보기 샘플러를 만들 수 없습니다.");
            D3D11_RASTERIZER_DESC rasterDesc{}; rasterDesc.FillMode = D3D11_FILL_SOLID; rasterDesc.CullMode = D3D11_CULL_NONE; rasterDesc.DepthClipEnable = TRUE;
            ComPtr<ID3D11RasterizerState> raster; checkHr(device->CreateRasterizerState(&rasterDesc, &raster), "미리보기 래스터라이저를 만들 수 없습니다.");
            std::array<UploadSlot, 3> uploads;
            for (auto& slot : uploads)
            {
                createPlane(device.Get(), width, height, DXGI_FORMAT_R8_UNORM, slot.stagingY, slot.textureY, slot.yView);
                createPlane(device.Get(), width / 2, height / 2, DXGI_FORMAT_R8G8_UNORM, slot.stagingUv, slot.textureUv, slot.uvView);
                const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
                checkHr(device->CreateQuery(&query, &slot.done), "미리보기 업로드 완료 확인 기능을 만들 수 없습니다.");
            }
            NativeHandle timer{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};
            if (!timer.value) timer.value = CreateWaitableTimerW(nullptr, FALSE, nullptr);
            if (!timer.value) throw std::runtime_error("미리보기 간격 타이머를 만들 수 없습니다.");
            jsonSet(adapter, "presentLoopHz", 60); jsonSet(adapter, "maximumFrameLatency", 1);
            jsonSet(adapter, "contextOwner", "present thread only; three fenced upload slots; DO_NOT_WAIT staging maps; no decoder/encoder context sharing");
            int current = -1;
            std::uint64_t lastPresentedFrame = 0;
            bool stallActive = false;
            PresentPacing pacing;
            ready.set_value(); signalled = true;
            if (measurementStart.valid())
                while (!stopping.load() && measurementStart.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {}
            auto lastNewPresent = qpcNow(), previousPresent = std::int64_t{0};
            const auto start = qpcNow();
            std::uint64_t tick = 0;
            while (!stopping.load())
            {
                const auto due = start + static_cast<std::int64_t>(++tick * static_cast<std::uint64_t>(telemetry->frequency) / 60);
                const auto now = qpcNow();
                if (due > now)
                {
                    LARGE_INTEGER relative{};
                    relative.QuadPart = -std::max<std::int64_t>(1, static_cast<std::int64_t>(static_cast<double>(due - now) * 10000000.0 / telemetry->frequency));
                    if (!SetWaitableTimer(timer.value, &relative, 0, nullptr, nullptr, FALSE)) throw std::runtime_error("미리보기 타이머를 설정할 수 없습니다.");
                    if (WaitForSingleObject(timer.value, 50) != WAIT_OBJECT_0) throw std::runtime_error("미리보기 타이머 대기 시간이 초과되었습니다.");
                }
                else if (now - due > telemetry->frequency / 60)
                    tick = static_cast<std::uint64_t>((now - start) * 60 / telemetry->frequency);
                const auto stallNow = qpcNow();
                const auto stallStart = std::max(lastNewPresent, telemetry->firstCallbackQpc.load() + telemetry->frequency);
                if (!stallActive && telemetry->afterWarmup(stallNow) && telemetry->latestReadyFrame.load() > lastPresentedFrame
                    && telemetry->ms(stallNow - stallStart) > 2 * telemetry->fps.periodMs() + 1000.0 / 60)
                { telemetry->loss(LossReason::previewStall); stallActive = true; }
                if (pacing.needsVisibilityTest())
                {
                    const auto visible = swap->Present(0, DXGI_PRESENT_TEST);
                    checkHr(visible, "미리보기 화면의 표시 상태를 확인할 수 없습니다.");
                    pacing.visibilityTest(visible);
                    if (pacing.needsVisibilityTest()) { telemetry->loss(LossReason::presentBusy); continue; }
                }
                if (pacing.needsWait())
                {
                    const auto wait = WaitForSingleObject(frameLatency.value, 0);
                    if (wait == WAIT_FAILED) checkHr(HRESULT_FROM_WIN32(GetLastError()), "미리보기 프레임 지연 대기에 실패했습니다.");
                    if (wait != WAIT_OBJECT_0) { telemetry->loss(LossReason::presentBusy); continue; }
                }
                pool.uploadLatest([&](VideoSurface& surface)
                {
                    int candidate = -1;
                    for (int i = 0; i < static_cast<int>(uploads.size()); ++i)
                    {
                        if (i == current) continue;
                        auto& slot = uploads[static_cast<size_t>(i)];
                        if (!slot.submitted) { candidate = i; break; }
                        BOOL complete = FALSE;
                        const auto hr = context->GetData(slot.done.Get(), &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                        checkHr(hr, "GPU 업로드 버퍼 상태를 확인할 수 없습니다.");
                        if (hr == S_OK && complete) { candidate = i; break; }
                    }
                    surface.stamp.uploadStart = qpcNow();
                    if (candidate >= 0 && upload(context.Get(), uploads[static_cast<size_t>(candidate)], surface))
                    {
                        surface.stamp.uploadEnd = qpcNow();
                        current = candidate; uploads[static_cast<size_t>(current)].stamp = surface.stamp;
                    }
                    else telemetry->loss(LossReason::uploadBusy);
                }); // staging owns the bytes now; release CPU surface on every exit
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
                pacing.presented(hr);
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { telemetry->loss(LossReason::presentBusy); continue; }
                if (hr == DXGI_STATUS_OCCLUDED) { telemetry->loss(LossReason::presentBusy); continue; }
                checkHr(hr, "미리보기 화면을 표시할 수 없습니다.");
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
        done.store(true);
    }
};
PreviewPresenter::PreviewPresenter(HWND window, VideoSurfacePool& pool, std::shared_ptr<CaptureTelemetry> telemetry, UINT width, UINT height)
    : state(std::make_unique<State>(window, pool, std::move(telemetry), width, height)) {}
PreviewPresenter::~PreviewPresenter() { stop(); }
void PreviewPresenter::start(std::shared_future<void> measurementStart)
{
    if (state->thread.joinable()) throw std::logic_error("이미 사용한 미리보기 표시는 다시 시작할 수 없습니다.");
    state->done.store(false);
    std::promise<void> promise; auto future = promise.get_future();
    state->thread = std::thread(&State::run, state.get(), std::move(promise), std::move(measurementStart));
    future.get();
}
void PreviewPresenter::stop() { state->stopping.store(true); if (state->thread.joinable()) state->thread.join(); }
bool PreviewPresenter::finished() const noexcept { return state->done.load(); }
const std::string& PreviewPresenter::error() const noexcept { return state->error; }
juce::var PreviewPresenter::adapterJson() const { return state->adapter; }
}
