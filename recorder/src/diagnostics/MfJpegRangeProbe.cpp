#include "MfJpegRangeProbe.h"
#include "capture/CameraCatalog.h"
#include <mftransform.h>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}
#include <algorithm>
#include <cmath>
#include <cstring>

namespace gocue::recorder
{
namespace
{
constexpr std::array<JpegPatch, 11> patches{{
    {"Y0", {0,128,128}}, {"Y16", {16,128,128}}, {"Y128", {128,128,128}},
    {"Y235", {235,128,128}}, {"Y255", {255,128,128}},
    {"red", {97,102,205}}, {"green", {141,77,64}}, {"blue", {68,205,116}},
    {"cyan", {158,154,51}}, {"magenta", {112,179,192}}, {"yellow", {187,51,140}}
}};
constexpr std::array<JpegOutputModel, 4> models{JpegOutputModel::full601, JpegOutputModel::limited601,
    JpegOutputModel::full709, JpegOutputModel::limited709};
unsigned tileWidth(unsigned width) { return (width / static_cast<unsigned>(patches.size())) & ~15u; }
void avCheck(int code, const char* operation)
{
    if (code >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(code, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}
juce::var yuvJson(const YuvValue& v)
{
    juce::Array<juce::var> a; for (double x : v) a.add(x); return a;
}
ComPtr<IMFSample> sampleBuffer(DWORD bytes, DWORD alignment = 0)
{
    ComPtr<IMFSample> sample; checkHr(MFCreateSample(&sample), "Create MFT probe sample");
    ComPtr<IMFMediaBuffer> buffer;
    checkHr(MFCreateAlignedMemoryBuffer(bytes, alignment ? alignment - 1 : 0, &buffer), "Allocate MFT probe buffer");
    checkHr(sample->AddBuffer(buffer.Get()), "Attach MFT probe buffer");
    return sample;
}
ComPtr<IMFMediaType> videoType(GUID subtype, unsigned width, unsigned height)
{
    ComPtr<IMFMediaType> type; checkHr(MFCreateMediaType(&type), "Create JPEG probe type");
    checkHr(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set probe video type");
    checkHr(type->SetGUID(MF_MT_SUBTYPE, subtype), "Set probe subtype");
    checkHr(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height), "Set probe dimensions");
    checkHr(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 60, 1), "Set probe FPS");
    checkHr(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Set probe aspect");
    checkHr(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set probe interlace");
    return type; // deliberately no requested range/matrix, matching SourceReader negotiation
}
void setNv12Output(IMFTransform* transform, DWORD stream, unsigned width, unsigned height)
{
    for (DWORD index = 0; ; ++index)
    {
        ComPtr<IMFMediaType> type;
        const auto hr = transform->GetOutputAvailableType(stream, index, &type);
        if (hr == MF_E_NO_MORE_TYPES) break;
        checkHr(hr, "Enumerate MFT output types");
        GUID subtype{};
        if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12)
        {
            checkHr(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height), "Set MFT NV12 dimensions");
            if (SUCCEEDED(transform->SetOutputType(stream, type.Get(), 0))) return;
        }
    }
    throw std::runtime_error("Enumerated MJPEG decoder did not accept NV12 output");
}
std::vector<std::uint8_t> readNv12(IMFSample* sample, IMFMediaType* type, unsigned width, unsigned height)
{
    const DWORD bytes = width * height * 3 / 2;
    std::vector<std::uint8_t> packed(bytes);
    ComPtr<IMFMediaBuffer> buffer;
    checkHr(sample->ConvertToContiguousBuffer(&buffer), "Read MFT output sample");
    ComPtr<IMF2DBuffer> twoD;
    if (SUCCEEDED(buffer.As(&twoD)))
    {
        DWORD size = 0; checkHr(twoD->GetContiguousLength(&size), "Read packed MFT length");
        if (size != bytes) throw std::runtime_error("MFT NV12 packed dimensions changed");
        checkHr(twoD->ContiguousCopyTo(packed.data(), bytes), "Copy raw MFT NV12 planes");
        return packed;
    }
    UINT32 attribute = width; type->GetUINT32(MF_MT_DEFAULT_STRIDE, &attribute);
    const auto stride = static_cast<LONG>(attribute);
    if (stride < static_cast<LONG>(width)) throw std::runtime_error("Invalid MFT NV12 stride");
    BYTE* data = nullptr; DWORD length = 0;
    checkHr(buffer->Lock(&data, nullptr, &length), "Lock raw MFT output");
    struct Unlock { IMFMediaBuffer* buffer; ~Unlock() { buffer->Unlock(); } } unlock{buffer.Get()};
    const auto required = static_cast<std::uint64_t>(stride) * (height + height / 2 - 1) + width;
    if (required > length) throw std::runtime_error("Truncated MFT NV12 planes");
    for (unsigned row = 0; row < height + height / 2; ++row)
        std::memcpy(packed.data() + static_cast<size_t>(row) * width, data + static_cast<size_t>(row) * stride, width);
    return packed;
}
std::vector<YuvValue> samplePatches(const std::vector<std::uint8_t>& nv12, unsigned width, unsigned height)
{
    std::vector<YuvValue> values;
    const unsigned tile = tileWidth(width), cy = (height / 2) & ~1u;
    for (unsigned i = 0; i < patches.size(); ++i)
    {
        const unsigned cx = (i * tile + tile / 2) & ~1u;
        YuvValue mean{};
        for (unsigned y = cy - 8; y < cy + 8; ++y)
            for (unsigned x = cx - 8; x < cx + 8; ++x)
            {
                mean[0] += nv12[static_cast<size_t>(y) * width + x] / 256.0;
                const auto uv = static_cast<size_t>(width) * height + (y / 2) * width + (x & ~1u);
                mean[1] += nv12[uv] / 256.0; mean[2] += nv12[uv + 1] / 256.0;
            }
        values.push_back(mean);
    }
    return values;
}
juce::var probeTransform(IMFActivate* activation, unsigned width, unsigned height, const std::vector<std::uint8_t>& jpeg)
{
    ComPtr<IMFTransform> transform;
    checkHr(activation->ActivateObject(IID_PPV_ARGS(&transform)), "Activate MJPEG decoder MFT");
    struct Shutdown { IMFActivate* value; ~Shutdown() { value->ShutdownObject(); } } shutdown{activation};
    DWORD inputs = 0, outputs = 0, inputId = 0, outputId = 0;
    checkHr(transform->GetStreamCount(&inputs, &outputs), "Read MFT streams");
    if (inputs != 1 || outputs != 1) throw std::runtime_error("Probe requires one MFT input/output");
    const auto idHr = transform->GetStreamIDs(1, &inputId, 1, &outputId);
    if (idHr != E_NOTIMPL) checkHr(idHr, "Read MFT stream IDs");
    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(transform->GetAttributes(&attributes))) attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    auto input = videoType(MFVideoFormat_MJPG, width, height);
    checkHr(transform->SetInputType(inputId, input.Get(), 0), "Set direct MJPEG input");
    setNv12Output(transform.Get(), outputId, width, height);
    checkHr(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Begin MFT streaming");
    checkHr(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Start MFT stream");
    auto sample = sampleBuffer(static_cast<DWORD>(jpeg.size()));
    ComPtr<IMFMediaBuffer> inputBuffer; checkHr(sample->GetBufferByIndex(0, &inputBuffer), "Get JPEG buffer");
    BYTE* data = nullptr; checkHr(inputBuffer->Lock(&data, nullptr, nullptr), "Lock JPEG buffer");
    std::memcpy(data, jpeg.data(), jpeg.size()); inputBuffer->Unlock();
    checkHr(inputBuffer->SetCurrentLength(static_cast<DWORD>(jpeg.size())), "Set JPEG size");
    checkHr(sample->SetSampleTime(0), "Set JPEG time");
    checkHr(sample->SetSampleDuration(166667), "Set JPEG duration");
    checkHr(transform->ProcessInput(inputId, sample.Get(), 0), "Feed synthetic JPEG to MFT");
    for (unsigned attempt = 0; attempt < 4; ++attempt)
    {
        MFT_OUTPUT_STREAM_INFO info{}; checkHr(transform->GetOutputStreamInfo(outputId, &info), "Read MFT output buffer contract");
        ComPtr<IMFSample> storage;
        if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES))
            storage = sampleBuffer(std::max<DWORD>(info.cbSize, width * height * 3 / 2), info.cbAlignment);
        MFT_OUTPUT_DATA_BUFFER output{}; output.dwStreamID = outputId; output.pSample = storage.Get();
        DWORD flags = 0;
        const auto hr = transform->ProcessOutput(0, 1, &output, &flags);
        ComPtr<IMFCollection> events; events.Attach(output.pEvents);
        ComPtr<IMFSample> supplied;
        if (output.pSample != storage.Get()) supplied.Attach(output.pSample);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { setNv12Output(transform.Get(), outputId, width, height); continue; }
        checkHr(hr, "Decode synthetic JPEG to direct MFT NV12");
        if (!output.pSample) throw std::runtime_error("MFT returned no NV12 sample");
        ComPtr<IMFMediaType> actual;
        checkHr(transform->GetOutputCurrentType(outputId, &actual), "Read actual MFT output type");
        UINT32 w = 0, h = 0; checkHr(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h), "Read MFT dimensions");
        if (w != width || h != height) throw std::runtime_error("MFT changed probe dimensions");
        const auto observations = samplePatches(readNv12(output.pSample, actual.Get(), width, height), width, height);
        auto report = classifyJpegOutput(observations).toJson();
        juce::Array<juce::var> measured;
        for (size_t i = 0; i < patches.size(); ++i)
        {
            auto row = jsonObject(); jsonSet(row, "patch", patches[i].name);
            jsonSet(row, "inputFull601Yuv", yuvJson(patches[i].input));
            jsonSet(row, "outputYuvMean", yuvJson(observations[i])); measured.add(row);
        }
        jsonSet(report, "patches", measured);
        const auto mode = CameraCatalog::readMode(actual.Get());
        if (mode) jsonSet(report, "outputMediaType", CameraCatalog::modeJson(*mode));
        jsonSet(report, "jpegBytes", jsonInt(jpeg.size()));
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, inputId);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        return report;
    }
    throw std::runtime_error("MFT output type never stabilised");
}
}
const std::array<JpegPatch, 11>& jpegRangePatches() { return patches; }
const char* jpegOutputModelName(JpegOutputModel model) noexcept
{
    switch (model)
    {
        case JpegOutputModel::full601: return "full-601";
        case JpegOutputModel::limited601: return "limited-601";
        case JpegOutputModel::full709: return "full-709";
        case JpegOutputModel::limited709: return "limited-709";
        default: return "unknown";
    }
}
YuvValue expectedJpegOutput(YuvValue input, JpegOutputModel model)
{
    const bool limited = model == JpegOutputModel::limited601 || model == JpegOutputModel::limited709;
    if (model == JpegOutputModel::full709 || model == JpegOutputModel::limited709)
    {
        const double y = input[0] / 255, u = (input[1] - 128) / 255, v = (input[2] - 128) / 255;
        const double r = std::clamp(y + 1.402 * v, 0.0, 1.0), g = std::clamp(y - .344136 * u - .714136 * v, 0.0, 1.0), b = std::clamp(y + 1.772 * u, 0.0, 1.0);
        const double yy = .2126*r + .7152*g + .0722*b;
        input = {255*yy, 128+255*(b-yy)/1.8556, 128+255*(r-yy)/1.5748};
    }
    if (limited) input = {16+219*input[0]/255, 128+224*(input[1]-128)/255, 128+224*(input[2]-128)/255};
    return input;
}
JpegRangeDecision classifyJpegOutput(const std::vector<YuvValue>& observed)
{
    if (observed.size() != patches.size()) throw std::invalid_argument("JPEG judgement requires all 11 Y/UV patches");
    JpegRangeDecision result; std::array<double,4> maximum{};
    for (size_t m = 0; m < models.size(); ++m)
    {
        double sum = 0;
        for (size_t p = 0; p < patches.size(); ++p)
        {
            const auto expected = expectedJpegOutput(patches[p].input, models[m]);
            for (size_t c = 0; c < 3; ++c)
            {
                if (!std::isfinite(observed[p][c]) || observed[p][c] < 0 || observed[p][c] > 255)
                    throw std::invalid_argument("Invalid measured 8-bit JPEG patch");
                const double error = observed[p][c] - expected[c];
                sum += error * error; maximum[m] = std::max(maximum[m], std::abs(error));
            }
        }
        result.rmse[m] = std::sqrt(sum / (patches.size() * 3));
    }
    std::array<size_t,4> order{0,1,2,3};
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return result.rmse[a] < result.rmse[b]; });
    result.bestMaxError = maximum[order[0]]; result.runnerUpMargin = result.rmse[order[1]] - result.rmse[order[0]];
    if (result.bestMaxError <= 3 && result.runnerUpMargin >= 2) result.model = models[order[0]];
    return result;
}
juce::var JpegRangeDecision::toJson() const
{
    auto report = jsonObject(), scores = jsonObject();
    for (size_t i = 0; i < models.size(); ++i) jsonSet(scores, jpegOutputModelName(models[i]), rmse[i]);
    jsonSet(report, "model", jpegOutputModelName(model)); jsonSet(report, "rmse8bit", scores);
    jsonSet(report, "bestMaxAbsError8bit", bestMaxError); jsonSet(report, "runnerUpRmseMargin8bit", runnerUpMargin);
    jsonSet(report, "classificationRule", "All 11 flat patches (Y,U,V): best max error <=3 codes and next-model RMSE margin >=2 codes; otherwise unknown");
    return report;
}
std::vector<std::uint8_t> makeJpegRangeFixture(unsigned width, unsigned height)
{
    if (width < 352 || width > 8192 || height < 32 || height > 8192 || width % 2 || height % 2)
        throw std::invalid_argument("JPEG range fixture requires even dimensions, width >=352, height >=32, <=8192");
    struct Resources
    {
        AVCodecContext* codec = nullptr; AVFrame* frame = av_frame_alloc(); AVPacket* packet = av_packet_alloc();
        ~Resources() { avcodec_free_context(&codec); av_frame_free(&frame); av_packet_free(&packet); }
    } r;
    const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!encoder || !r.frame || !r.packet) throw std::runtime_error("FFmpeg MJPEG fixture encoder unavailable");
    r.codec = avcodec_alloc_context3(encoder); if (!r.codec) throw std::bad_alloc();
    r.codec->width = static_cast<int>(width); r.codec->height = static_cast<int>(height);
    r.codec->pix_fmt = AV_PIX_FMT_YUVJ420P; r.codec->time_base = {1,60}; r.codec->thread_count = 1;
    r.codec->color_range = AVCOL_RANGE_JPEG; r.codec->colorspace = AVCOL_SPC_SMPTE170M;
    r.codec->flags |= AV_CODEC_FLAG_QSCALE; r.codec->global_quality = FF_QP2LAMBDA;
    avCheck(avcodec_open2(r.codec, encoder, nullptr), "Open synthetic JPEG encoder");
    r.frame->width = static_cast<int>(width); r.frame->height = static_cast<int>(height);
    r.frame->format = r.codec->pix_fmt; r.frame->color_range = r.codec->color_range;
    r.frame->colorspace = r.codec->colorspace; r.frame->quality = r.codec->global_quality;
    avCheck(av_frame_get_buffer(r.frame, 32), "Allocate JPEG fixture planes");
    for (unsigned plane = 0; plane < 3; ++plane)
    {
        const unsigned divisor = plane ? 2 : 1;
        for (unsigned y = 0; y < height / divisor; ++y)
            for (unsigned x = 0; x < width / divisor; ++x)
            {
                const auto p = std::min<size_t>(x * divisor / tileWidth(width), patches.size()-1);
                r.frame->data[plane][y*r.frame->linesize[plane]+x] = static_cast<std::uint8_t>(patches[p].input[plane]);
            }
    }
    avCheck(avcodec_send_frame(r.codec, r.frame), "Encode known YUV patches");
    avCheck(avcodec_receive_packet(r.codec, r.packet), "Receive synthetic JPEG");
    return {r.packet->data, r.packet->data+r.packet->size};
}
juce::var probeMfJpegRange()
{
    struct Activations
    {
        IMFActivate** items = nullptr; UINT32 count = 0;
        ~Activations() { for (UINT32 i = 0; i < count; ++i) items[i]->Release(); CoTaskMemFree(items); }
    } found;
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_MJPG}, output{MFMediaType_Video, MFVideoFormat_NV12};
    checkHr(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &input, &output, &found.items, &found.count), "Enumerate synchronous MJPEG -> NV12 decoder MFTs");
    auto report = jsonObject(); juce::Array<juce::var> runs;
    bool classified = found.count > 0;
    for (const auto dimensions : {std::pair<unsigned,unsigned>{1920,1080}, {640,480}})
    {
        const auto jpeg = makeJpegRangeFixture(dimensions.first, dimensions.second);
        for (UINT32 i = 0; i < found.count; ++i)
        {
            auto run = jsonObject();
            try
            {
                run = probeTransform(found.items[i], dimensions.first, dimensions.second, jpeg);
                classified &= run["model"].toString() != "unknown";
            }
            catch (const std::exception& e) { jsonSet(run, "error", e.what()); classified = false; }
            jsonSet(run, "width", dimensions.first); jsonSet(run, "height", dimensions.second);
            WCHAR name[512]{}; UINT32 length = 0;
            if (SUCCEEDED(found.items[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 512, &length))) jsonSet(run, "decoder", juce::String(name));
            GUID clsid{}; WCHAR guid[64]{};
            if (SUCCEEDED(found.items[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid)))
            { StringFromGUID2(clsid, guid, 64); jsonSet(run, "clsid", juce::String(guid)); }
            runs.add(run);
        }
    }
    jsonSet(report, "runs", runs); jsonSet(report, "allClassified", classified); jsonSet(report, "decoderCount", found.count);
    jsonSet(report, "sourceKind", "synthetic-offline");
    jsonSet(report, "method", "FFmpeg mjpeg YUVJ420P full BT.601 -> direct synchronous MFTEnumEx MJPG/NV12 decoder; no colour processor, camera, GPU or RGB swscale; 16x16 interior patch means");
    jsonSet(report, "scope", "Inference about the listed OS MFTs and resolutions only. Camera JPEG metadata, SourceReader-selected MFT identity and physical chart certification require separate validation.");
    return report;
}
}
