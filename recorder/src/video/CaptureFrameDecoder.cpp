#include "CaptureFrameDecoder.h"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <cstring>
#include <limits>

namespace gocue::recorder
{
namespace
{
void avCheck(int result, const char* operation)
{
    if (result >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}
struct LockedBuffer
{
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMF2DBuffer2> twoD;
    BYTE* start = nullptr;
    BYTE* row0 = nullptr;
    DWORD length = 0;
    LONG pitch = 0;
    bool locked = false;
    ~LockedBuffer()
    {
        if (locked) { if (twoD) twoD->Unlock2D(); else buffer->Unlock(); }
    }
};
void checkedCopyRows(std::uint8_t* dest, size_t rowBytes, UINT32 rows, const BYTE* source,
                     LONG pitch, const BYTE* base, DWORD length)
{
    const auto begin = reinterpret_cast<std::uintptr_t>(base);
    const auto first = reinterpret_cast<std::uintptr_t>(source);
    if (first < begin || first - begin > length) throw std::runtime_error("MF first scanline outside locked storage");
    const auto firstOffset = static_cast<std::int64_t>(first - begin);
    for (UINT32 row = 0; row < rows; ++row)
    {
        const auto offset = firstOffset + static_cast<std::int64_t>(row) * pitch;
        if (offset < 0 || static_cast<std::uint64_t>(offset) > length || rowBytes > length - static_cast<std::uint64_t>(offset))
            throw std::runtime_error("MF buffer stride/plane exceeds locked storage");
        std::memcpy(dest + rowBytes * row, base + offset, rowBytes);
    }
}
bool jpegFullFormat(AVPixelFormat format)
{
    return format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUVJ444P || format == AV_PIX_FMT_YUVJ440P;
}
}
struct CaptureFrameDecoder::State
{
    CameraMode mode;
    int threads;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* direct = nullptr;
    ColourDevice colourDevice;
    std::unique_ptr<GpuColourConverter> gpuColour;
    std::vector<std::uint8_t> copied;
    std::string colourDecision;
    State(CameraMode m, int t, ColourDevice d) : mode(m), threads(t), colourDevice(d) {}
    ~State()
    {
        avcodec_free_context(&codec); av_packet_free(&packet); av_frame_free(&frame);
        sws_freeContext(direct);
    }
    void normalise(AVFrame& input, VideoSurface& output)
    {
        if (input.width != static_cast<int>(mode.width) || input.height != static_cast<int>(mode.height))
            throw std::runtime_error("Decoder changed dimensions; dynamic format change requires re-prepare");
        if (output.width != mode.width || output.height != mode.height) throw std::runtime_error("Unprepared output surface");
        const auto format = static_cast<AVPixelFormat>(input.format);
        bool assumed = false;
        int matrix = SWS_CS_ITU709;
        if (input.colorspace == AVCOL_SPC_BT709) matrix = SWS_CS_ITU709;
        else if (input.colorspace == AVCOL_SPC_BT470BG || input.colorspace == AVCOL_SPC_SMPTE170M) matrix = SWS_CS_ITU601;
        else if (input.colorspace != AVCOL_SPC_UNSPECIFIED) throw std::runtime_error("Unsupported decoded colour matrix");
        else if (mode.colour.matrix == MFVideoTransferMatrix_BT709) matrix = SWS_CS_ITU709;
        else if (mode.colour.matrix == MFVideoTransferMatrix_BT601) matrix = SWS_CS_ITU601;
        else if (mode.colour.matrix != 0) throw std::runtime_error("Unsupported MF colour matrix");
        else { matrix = mode.subtype == CaptureSubtype::mjpeg || mode.height < 720 ? SWS_CS_ITU601 : SWS_CS_ITU709; assumed = true; }
        int full = 0;
        if (input.color_range == AVCOL_RANGE_JPEG || jpegFullFormat(format)) full = 1;
        else if (input.color_range == AVCOL_RANGE_MPEG) full = 0;
        else if (mode.colour.range == MFNominalRange_0_255) full = 1;
        else if (mode.colour.range == MFNominalRange_16_235) full = 0;
        else if (mode.colour.range != 0) throw std::runtime_error("Unsupported MF nominal range");
        else { full = mode.subtype == CaptureSubtype::mjpeg ? 1 : 0; assumed = true; }
        // This spike converts YCbCr matrix/range only. SDR camera gamuts (BT.709, SMPTE 170M / BT.470 BG
        // as reported by JFIF MJPEG webcams and the GC311G2, sRGB / BT.601 transfer) are treated as the
        // BT.709 SDR family as a documented approximation pending physical colour-chart validation.
        // Rejecting them (2026-09-09) stopped the StreamCam-style MJPEG path outright. Only HDR / wide
        // gamut signalling is rejected rather than relabelled.
        const auto hdrPrimaries = [] (std::uint32_t p) {
            return p == MFVideoPrimaries_BT2020 || p == MFVideoPrimaries_XYZ || p == MFVideoPrimaries_DCI_P3 || p == MFVideoPrimaries_ACES;
        };
        const auto hdrTransfer = [] (std::uint32_t t) {
            return t == MFVideoTransFunc_2084 || t == MFVideoTransFunc_HLG || t == MFVideoTransFunc_2020 || t == MFVideoTransFunc_2020_const;
        };
        if (hdrPrimaries(mode.colour.primaries))
            throw std::runtime_error("HDR / wide-gamut primaries require a colour-managed path");
        if (hdrTransfer(mode.colour.transfer))
            throw std::runtime_error("HDR transfer function is not supported in the SDR spike");
        if (input.color_primaries == AVCOL_PRI_BT2020 || input.color_primaries == AVCOL_PRI_SMPTE428 || input.color_primaries == AVCOL_PRI_SMPTE431 || input.color_primaries == AVCOL_PRI_SMPTE432)
            throw std::runtime_error("Decoded HDR / wide-gamut primaries require a colour-managed path");
        if (input.color_trc == AVCOL_TRC_SMPTE2084 || input.color_trc == AVCOL_TRC_ARIB_STD_B67 || input.color_trc == AVCOL_TRC_BT2020_10 || input.color_trc == AVCOL_TRC_BT2020_12)
            throw std::runtime_error("Decoded HDR transfer function is unsupported");
        // Anything that is not explicitly BT.709 (missing or another SDR gamut) is a documented assumption.
        if (mode.colour.primaries != MFVideoPrimaries_BT709 && input.color_primaries != AVCOL_PRI_BT709) assumed = true;
        if (mode.colour.transfer != MFVideoTransFunc_709 && input.color_trc != AVCOL_TRC_BT709) assumed = true;
        output.colourAssumed = assumed;
        colourDecision = std::string("input ") + (matrix == SWS_CS_ITU709 ? "BT.709" : "BT.601") + (full ? " full" : " limited")
            + "; output NV12 BT.709 limited; " + (assumed ? "missing metadata uses documented SDR assumptions (not colour-certified)" : "explicit metadata");
        uint8_t* destination[4] = {output.y(), output.uv(), nullptr, nullptr};
        int strides[4] = {static_cast<int>(mode.width), static_cast<int>(mode.width), 0, 0};
        const int width = input.width, height = input.height;
        if (format == AV_PIX_FMT_NV12)
        {
            for (int plane = 0; plane < 2; ++plane)
                for (int row = 0; row < (plane == 0 ? height : height / 2); ++row)
                {
                    auto* dest = destination[plane] + row * strides[plane];
                    const auto* src = input.data[plane] + row * input.linesize[plane];
                    std::memcpy(dest, src, width);
                }
        }
        else
        {
            // Layout/chroma subsampling only, with identical matrix/range at both
            // ends. Never route live preview/recording through CPU RGB swscale.
            const int flags = SWS_BILINEAR | SWS_ACCURATE_RND;
            direct = sws_getCachedContext(direct, width, height, format, width, height, AV_PIX_FMT_NV12, flags, nullptr, nullptr, nullptr);
            if (!direct) throw std::bad_alloc();
            avCheck(sws_setColorspaceDetails(direct, sws_getCoefficients(matrix), full, sws_getCoefficients(matrix), full, 0, 1 << 16, 1 << 16), "Preserve source YUV matrix/range");
            if (sws_scale(direct, input.data, input.linesize, 0, height, destination, strides) != height) throw std::runtime_error("Incomplete NV12 conversion");
        }
        if (matrix != SWS_CS_ITU709 || full)
        {
            if (!gpuColour) gpuColour = std::make_unique<GpuColourConverter>(mode.width, mode.height, colourDevice);
            gpuColour->convert(output, matrix == SWS_CS_ITU601, full != 0);
            colourDecision += "; D3D11 compute + CPU NV12 readback (decode-worker context)";
        }
    }
};
CaptureFrameDecoder::CaptureFrameDecoder(const CameraMode& mode, int threads, ColourDevice device) : state(std::make_unique<State>(mode, threads, device))
{
    if (threads < 1 || threads > 16) throw std::invalid_argument("MJPEG threads must be 1..16");
    if (!mode.width || !mode.height || mode.width > 8192 || mode.height > 8192 || mode.width % 2 || mode.height % 2)
        throw std::invalid_argument("Invalid decoder dimensions");
    if (mode.interlace != 0 && mode.interlace != MFVideoInterlace_Progressive)
        throw std::invalid_argument("Interlaced source is unsupported; select a progressive native mode");
    auto& s = *state;
    s.frame = av_frame_alloc(); s.packet = av_packet_alloc();
    if (!s.frame || !s.packet) throw std::bad_alloc();
    s.copied.reserve(static_cast<size_t>(mode.width) * mode.height * 4);
    if (mode.subtype == CaptureSubtype::mjpeg)
    {
        const auto* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        if (!codec) throw std::runtime_error("Pinned FFmpeg has no CPU MJPEG decoder");
        s.codec = avcodec_alloc_context3(codec);
        if (!s.codec) throw std::bad_alloc();
        s.codec->thread_count = threads;
        s.codec->thread_type = FF_THREAD_SLICE; // avoid frame-thread delay in live preview
        // FFmpeg checks aligned allocation dimensions too (e.g. 16-wide JPEG -> 64).
        // Visible decoded dimensions are separately required to match the native mode.
        s.codec->max_pixels = static_cast<std::int64_t>((mode.width + 63) & ~63u) * ((mode.height + 63) & ~63u);
        avCheck(avcodec_open2(s.codec, codec, nullptr), "Open MJPEG decoder");
    }
}
CaptureFrameDecoder::~CaptureFrameDecoder() = default;
int CaptureFrameDecoder::effectiveThreads() const noexcept { return state->codec ? state->codec->thread_count : 0; }
const std::string& CaptureFrameDecoder::colourDecision() const noexcept { return state->colourDecision; }
void CaptureFrameDecoder::copySample(IMFSample* sample)
{
    auto& s = *state;
    LockedBuffer lock;
    checkHr(sample->ConvertToContiguousBuffer(&lock.buffer), "ConvertToContiguousBuffer(worker)");
    if (s.mode.subtype != CaptureSubtype::mjpeg && SUCCEEDED(lock.buffer.As(&lock.twoD)))
    {
        const auto hr = lock.twoD->Lock2DSize(MF2DBuffer_LockFlags_Read, &lock.row0, &lock.pitch, &lock.start, &lock.length);
        if (FAILED(hr)) lock.twoD.Reset(); else lock.locked = true;
    }
    if (!lock.locked)
    {
        checkHr(lock.buffer->Lock(&lock.start, nullptr, &lock.length), "Lock sample(worker)");
        lock.locked = true; lock.row0 = lock.start;
        lock.pitch = s.mode.stride != 0 ? s.mode.stride : static_cast<LONG>(s.mode.width * (s.mode.subtype == CaptureSubtype::yuy2 ? 2 : 1));
        if (lock.pitch < 0 && s.mode.subtype == CaptureSubtype::yuy2)
        {
            const auto offset = static_cast<std::uint64_t>(-static_cast<std::int64_t>(lock.pitch)) * (s.mode.height - 1);
            if (offset >= lock.length) throw std::runtime_error("Invalid negative MF stride");
            lock.row0 += static_cast<size_t>(offset);
        }
    }
    if (s.mode.subtype == CaptureSubtype::mjpeg)
    {
        if (lock.length == 0 || lock.length > static_cast<size_t>(s.mode.width) * s.mode.height * 4)
            throw std::runtime_error("MJPEG payload exceeds prepared frame byte bound");
        s.copied.assign(lock.start, lock.start + lock.length);
        return;
    }
    const size_t rowBytes = s.mode.width * (s.mode.subtype == CaptureSubtype::yuy2 ? 2u : 1u);
    if (static_cast<std::uint64_t>(std::abs(static_cast<std::int64_t>(lock.pitch))) < rowBytes)
        throw std::runtime_error("MF pitch smaller than row width");
    s.copied.resize(static_cast<size_t>(s.mode.width) * s.mode.height * (s.mode.subtype == CaptureSubtype::yuy2 ? 4 : 3) / 2);
    checkedCopyRows(s.copied.data(), rowBytes, s.mode.height, lock.row0, lock.pitch, lock.start, lock.length);
    if (s.mode.subtype == CaptureSubtype::nv12)
    {
        if (lock.pitch <= 0) throw std::runtime_error("NV12 requires positive stride");
        const auto offset = static_cast<size_t>(lock.pitch) * s.mode.height;
        const auto first = reinterpret_cast<std::uintptr_t>(lock.row0), begin = reinterpret_cast<std::uintptr_t>(lock.start);
        if (first < begin || first - begin > lock.length || offset > lock.length - (first - begin))
            throw std::runtime_error("Invalid NV12 chroma offset");
        checkedCopyRows(s.copied.data() + static_cast<size_t>(s.mode.width) * s.mode.height, s.mode.width, s.mode.height / 2,
                        lock.row0 + offset, lock.pitch, lock.start, lock.length);
    }
}
void CaptureFrameDecoder::decodeCopied(VideoSurface& output, FrameStamp& stamp)
{
    decodeBytes(state->copied.data(), state->copied.size(), output, stamp);
}
void CaptureFrameDecoder::decodeBytes(const std::uint8_t* bytes, size_t length, VideoSurface& output, FrameStamp& stamp)
{
    auto& s = *state;
    stamp.decodeStart = qpcNow();
    av_frame_unref(s.frame);
    if (s.mode.subtype == CaptureSubtype::mjpeg)
    {
        if (!bytes || !length || length > static_cast<size_t>(s.mode.width) * s.mode.height * 4 || length > INT_MAX)
            throw std::runtime_error("Invalid bounded MJPEG payload");
        av_packet_unref(s.packet);
        avCheck(av_new_packet(s.packet, static_cast<int>(length)), "Allocate padded MJPEG packet");
        std::memcpy(s.packet->data, bytes, length);
        s.packet->pts = stamp.pts100ns;
        avCheck(avcodec_send_packet(s.codec, s.packet), "Send MJPEG packet");
        avCheck(avcodec_receive_frame(s.codec, s.frame), "Receive MJPEG frame");
    }
    else
    {
        const auto needed = static_cast<size_t>(s.mode.width) * s.mode.height * (s.mode.subtype == CaptureSubtype::nv12 ? 3 : 4) / 2;
        if (!bytes || length < needed) throw std::runtime_error("Truncated raw YUV sample");
        s.frame->format = s.mode.subtype == CaptureSubtype::nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUYV422;
        s.frame->width = static_cast<int>(s.mode.width); s.frame->height = static_cast<int>(s.mode.height);
        avCheck(av_image_fill_arrays(s.frame->data, s.frame->linesize, bytes, static_cast<AVPixelFormat>(s.frame->format), s.frame->width, s.frame->height, 1), "Bind raw YUV planes");
    }
    stamp.decodeEnd = qpcNow();
    s.normalise(*s.frame, output);
    stamp.normaliseEnd = qpcNow();
    output.stamp = stamp;
}
}
