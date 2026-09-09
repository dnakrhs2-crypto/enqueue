#include "GpuColourConverter.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstring>

namespace gocue::recorder
{
namespace
{
constexpr char shader[] = R"hlsl(
ByteAddressBuffer source : register(t0);
RWByteAddressBuffer target : register(u0);
cbuffer Settings : register(b0) { uint width; uint height; uint matrix601; uint fullRange; };
uint byteAt(uint offset) { return (source.Load(offset & ~3u) >> ((offset & 3u) * 8u)) & 255u; }
float3 convertPixel(uint x, uint y)
{
    uint uv = width * height + (y / 2) * width + (x & ~1u);
    float Y = (float(byteAt(y * width + x)) - (fullRange ? 0.0 : 16.0)) / (fullRange ? 255.0 : 219.0);
    float U = (float(byteAt(uv)) - 128.0) / (fullRange ? 255.0 : 224.0);
    float V = (float(byteAt(uv + 1)) - 128.0) / (fullRange ? 255.0 : 224.0);
    float3 rgb;
    if (matrix601) rgb = float3(Y + 1.402 * V, Y - 0.344136 * U - 0.714136 * V, Y + 1.772 * U);
    else rgb = float3(Y + 1.5748 * V, Y - 0.187324 * U - 0.468124 * V, Y + 1.8556 * U);
    rgb = saturate(rgb);
    float yy = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    return float3(16.0 + 219.0 * yy, 128.0 + 224.0 * (rgb.b - yy) / 1.8556, 128.0 + 224.0 * (rgb.r - yy) / 1.5748);
}
uint quant(float x) { return uint(clamp(round(x), 0.0, 255.0)); }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint x = id.x * 4, y = id.y * 2;
    if (x >= width || y >= height) return;
    float3 a = convertPixel(x, y), b = convertPixel(x+1, y), c = convertPixel(x+2, y), d = convertPixel(x+3, y);
    float3 e = convertPixel(x, y+1), f = convertPixel(x+1, y+1), g = convertPixel(x+2, y+1), h = convertPixel(x+3, y+1);
    target.Store(y*width+x, quant(a.x) | (quant(b.x)<<8) | (quant(c.x)<<16) | (quant(d.x)<<24));
    target.Store((y+1)*width+x, quant(e.x) | (quant(f.x)<<8) | (quant(g.x)<<16) | (quant(h.x)<<24));
    float3 left = (a+b+e+f)*0.25, right = (c+d+g+h)*0.25;
    target.Store(width*height+(y/2)*width+x, quant(left.y) | (quant(left.z)<<8) | (quant(right.y)<<16) | (quant(right.z)<<24));
}
)hlsl";
}
struct GpuColourConverter::State
{
    unsigned width, height;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Buffer> input, output, readback, settings;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> target;
    ComPtr<ID3D11ComputeShader> compute;
    State(unsigned w, unsigned h) : width(w), height(h) {}
};
GpuColourConverter::GpuColourConverter(unsigned w, unsigned h, ColourDevice kind) : state(std::make_unique<State>(w, h))
{
    if (!w || !h || w % 4 || h % 2 || w > 8192 || h > 8192) throw std::invalid_argument("Compute NV12 conversion requires width multiple of 4 and even height");
    auto& s = *state; D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    checkHr(D3D11CreateDevice(nullptr, kind == ColourDevice::warpForTests ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, levels, 1, D3D11_SDK_VERSION, &s.device, &level, &s.context), "Create decode-worker colour device");
    ComPtr<ID3DBlob> code, errors;
    const auto compiled = D3DCompile(shader, sizeof(shader)-1, "RecorderColour", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(compiled)) throw std::runtime_error(errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "Colour compute compile failed");
    checkHr(s.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s.compute), "Create colour shader");
    D3D11_BUFFER_DESC desc{}; desc.ByteWidth = w * h * 3 / 2; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    checkHr(s.device->CreateBuffer(&desc, nullptr, &s.input), "Allocate colour input");
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    checkHr(s.device->CreateBuffer(&desc, nullptr, &s.output), "Allocate colour output");
    desc.BindFlags = 0; desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    checkHr(s.device->CreateBuffer(&desc, nullptr, &s.readback), "Allocate colour readback");
    desc = {}; desc.ByteWidth = 16; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    checkHr(s.device->CreateBuffer(&desc, nullptr, &s.settings), "Allocate colour settings");
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = DXGI_FORMAT_R32_TYPELESS; srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    srv.BufferEx.NumElements = w * h * 3 / 8; srv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    checkHr(s.device->CreateShaderResourceView(s.input.Get(), &srv, &s.view), "Create raw NV12 shader view");
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = DXGI_FORMAT_R32_TYPELESS; uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = srv.BufferEx.NumElements; uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    checkHr(s.device->CreateUnorderedAccessView(s.output.Get(), &uav, &s.target), "Create raw NV12 compute target");
}
GpuColourConverter::~GpuColourConverter() = default;
void GpuColourConverter::convert(VideoSurface& surface, bool matrix601, bool fullRange)
{
    auto& s = *state;
    if (surface.width != s.width || surface.height != s.height) throw std::invalid_argument("Colour surface changed size");
    const unsigned settings[] = {s.width, s.height, matrix601 ? 1u : 0u, fullRange ? 1u : 0u};
    s.context->UpdateSubresource(s.input.Get(), 0, nullptr, surface.nv12.data(), 0, 0);
    s.context->UpdateSubresource(s.settings.Get(), 0, nullptr, settings, 0, 0);
    ID3D11ShaderResourceView* view = s.view.Get(); ID3D11UnorderedAccessView* output = s.target.Get(); ID3D11Buffer* constants = s.settings.Get();
    s.context->CSSetShader(s.compute.Get(), nullptr, 0); s.context->CSSetShaderResources(0, 1, &view);
    s.context->CSSetUnorderedAccessViews(0, 1, &output, nullptr); s.context->CSSetConstantBuffers(0, 1, &constants);
    s.context->Dispatch((s.width / 4 + 7) / 8, (s.height / 2 + 7) / 8, 1);
    output = nullptr; view = nullptr;
    s.context->CSSetUnorderedAccessViews(0, 1, &output, nullptr); s.context->CSSetShaderResources(0, 1, &view);
    s.context->CopyResource(s.readback.Get(), s.output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checkHr(s.context->Map(s.readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read colour compute NV12 (decode worker)");
    std::memcpy(surface.nv12.data(), mapped.pData, surface.nv12.size()); s.context->Unmap(s.readback.Get(), 0);
}
}
