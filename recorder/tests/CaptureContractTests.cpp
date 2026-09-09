#include "capture/CameraCatalog.h"
#include "diagnostics/CaptureTelemetry.h"
#include "support/BoundedSpscQueue.h"
#include "video/CaptureFrameDecoder.h"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <thread>

using namespace gocue::recorder;
namespace
{
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Function> void rejects(Function action)
{
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    require(threw, "Expected rejection");
}
CameraMode rawMode(CaptureSubtype subtype = CaptureSubtype::nv12)
{
    CameraMode mode; mode.width = 16; mode.height = 16; mode.fps = {60000, 1001}; mode.subtype = subtype;
    mode.colour = {MFVideoTransferMatrix_BT709, MFNominalRange_16_235, MFVideoPrimaries_BT709, MFVideoTransFunc_709};
    return mode;
}
std::vector<uint8_t> makeJpeg()
{
    struct Resources
    {
        AVCodecContext* codec = nullptr; AVFrame* frame = av_frame_alloc(); AVPacket* packet = av_packet_alloc();
        ~Resources() { avcodec_free_context(&codec); av_frame_free(&frame); av_packet_free(&packet); }
    } r;
    const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    require(encoder != nullptr && r.frame && r.packet, "MJPEG fixture resources available");
    r.codec = avcodec_alloc_context3(encoder); require(r.codec != nullptr, "MJPEG encoder allocation");
    r.codec->width = 16; r.codec->height = 16; r.codec->pix_fmt = AV_PIX_FMT_YUVJ420P;
    r.codec->time_base = {1, 30}; r.codec->thread_count = 1; r.codec->color_range = AVCOL_RANGE_JPEG;
    require(avcodec_open2(r.codec, encoder, nullptr) == 0, "Open fixture JPEG encoder");
    r.frame->width = 16; r.frame->height = 16; r.frame->format = AV_PIX_FMT_YUVJ420P; r.frame->color_range = AVCOL_RANGE_JPEG;
    require(av_frame_get_buffer(r.frame, 32) == 0, "Allocate fixture planes");
    for (int row = 0; row < 16; ++row) memset(r.frame->data[0] + row * r.frame->linesize[0], 128, 16);
    for (int row = 0; row < 8; ++row)
    {
        memset(r.frame->data[1] + row * r.frame->linesize[1], 128, 8);
        memset(r.frame->data[2] + row * r.frame->linesize[2], 128, 8);
    }
    require(avcodec_send_frame(r.codec, r.frame) == 0 && avcodec_receive_packet(r.codec, r.packet) == 0, "Generate in-memory JPEG fixture");
    return {r.packet->data, r.packet->data + r.packet->size};
}
}
int runCaptureContractTests()
{
    unsigned passed = 0, failed = 0;
    const auto test = [&](const char* name, const std::function<void()>& run)
    {
        try { run(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("rational preserves numerator/denominator without rounding/reduction", []
    {
        require(Rational::parse("60000/1001").text() == "60000/1001", "59.94 is retained");
        require(Rational::parse("120/2").text() == "120/2", "native fraction is not reduced");
        require(std::abs(Rational::parse("60000/1001").periodMs() - 16.6833333333) < 0.00001, "rational period");
        for (const auto* bad : {"60", "0/1", "60/0", "-1/1", "1/2/3", "4294967296/1", "60/1junk", " 60/1"}) rejects([&] { Rational::parse(bad); });
    });
    test("mode grammar and exact native identity", []
    {
        const auto mode = CameraMode::parse("MJPEG 1920x1080 60000/1001");
        require(mode.text() == "MJPEG 1920x1080 60000/1001", "mode roundtrip");
        require(!mode.sameSignal(CameraMode::parse("MJPEG 1920x1080 60/1")), "60 != 59.94");
        for (const auto* bad : {"RGB32 1920x1080 60/1", "NV12 1919x1080 60/1", "NV12 1920x1080 60/1 extra", "NV12 0x1080 60/1"}) rejects([&] { CameraMode::parse(bad); });
    });
    test("bounded queue takes no ownership on overflow and wraps", []
    {
        BoundedSpscQueue<int, 2> queue;
        require(queue.push(10) && queue.push(20), "two pending slots");
        require(!queue.push(30) && queue.reserve() == nullptr, "overflow rejects before retained sample AddRef");
        int value = 0;
        require(queue.pop(value) && value == 10, "FIFO first");
        require(queue.push(40), "reuse retired slot");
        require(queue.pop(value) && value == 20 && queue.pop(value) && value == 40 && !queue.pop(value), "FIFO after wrap and empty");
    });
    test("SPSC concurrent delivery order and capacity", []
    {
        BoundedSpscQueue<unsigned, 2> queue;
        constexpr unsigned count = 100000;
        std::thread producer([&] { for (unsigned i = 1; i <= count; ++i) while (!queue.push(i)) std::this_thread::yield(); });
        bool ordered = true;
        for (unsigned i = 1; i <= count; ++i)
        {
            unsigned value = 0;
            while (!queue.pop(value)) std::this_thread::yield();
            ordered &= value == i;
        }
        producer.join(); require(ordered && queue.producerSize() == 0, "SPSC no duplication/reordering");
    });
    test("late, regression and cadence loss classifications", []
    {
        const Rational rate{60, 1};
        require(classifyLate(0, 166667, true, 2.0, rate) == LateReason::onTime, "2ms boundary");
        require(classifyLate(0, 166667, true, 2.01, rate) == LateReason::queueWait, "late worker wait");
        require(classifyLate(100, 100, true, 8, rate) == LateReason::timestampRegression, "duplicate PTS");
        require(classifyLate(100, 50, true, 0, rate) == LateReason::timestampRegression, "backward PTS");
        require(classifyLate(0, 500000, true, 0, rate) == LateReason::cadenceGap, "source PTS gap");
        require(classifyLate(0, 500000, false, 0, rate) == LateReason::onTime, "first PTS has no gap");
    });
    test("bounded telemetry percentiles and absent data", []
    {
        Distribution values;
        require(values.toJson()["p95Ms"].isVoid(), "empty percentile null");
        for (int i = 1; i <= 100; ++i) values.add(i);
        values.add(-1); values.add(std::numeric_limits<double>::quiet_NaN());
        require(values.count() == 100 && values.percentile(.5) == 50 && values.percentile(.95) == 95 && values.percentile(.99) == 99 && values.max() == 100, "nearest-rank oracle");
        values.add(2200.25); require(values.max() == 2200.25 && values.percentile(1) == 2200.25, "exact overflow maximum");
        auto telemetry = std::make_unique<CaptureTelemetry>(Rational{60, 1});
        telemetry->loss(LossReason::captureDecodeOverflow, 3); telemetry->loss(LossReason::previewMailboxOverwrite, 7);
        require(telemetry->count(LossReason::captureDecodeOverflow) == 3 && telemetry->count(LossReason::previewMailboxOverwrite) == 7, "loss causes separate");
    });
    test("mailbox latest wins, present slot cannot be overwritten", []
    {
        VideoSurfacePool pool(16, 16);
        const int first = pool.acquireWrite(); pool.surface(first).stamp.frame = 1;
        require(!pool.publish(first), "empty mailbox publication");
        const int held = pool.takeLatest();
        require(held == first, "presenter owns first slot");
        for (unsigned frame = 2; frame <= 1000; ++frame)
        {
            const int next = pool.acquireWrite(); require(next != held && next >= 0, "writer never acquires present slot");
            pool.surface(next).stamp.frame = frame;
            require(pool.publish(next) == (frame > 2), "exact unconsumed overwrite count");
        }
        require(pool.surface(held).stamp.frame == 1, "present data stable");
        pool.release(held); // the presenter releases CPU bytes at the end of upload
        const int latest = pool.takeLatest(); require(pool.surface(latest).stamp.frame == 1000, "newest selected");
        require(pool.takeLatest() == VideoSurfacePool::none, "only one mailbox");
        pool.release(latest);
    });
    test("telemetry stage boundaries and bounded timestamp trace", []
    {
        auto telemetry = std::make_unique<CaptureTelemetry>(Rational{60, 1});
        const auto tick = telemetry->frequency / 1000;
        for (unsigned i = 1; i <= 200; ++i)
        {
            FrameStamp stamp; stamp.frame = i; stamp.callback = tick * 1000 * i; stamp.enqueued = stamp.callback;
            stamp.worker = stamp.callback + 2 * tick; stamp.decodeStart = stamp.worker + tick;
            stamp.decodeEnd = stamp.decodeStart + 5 * tick; stamp.normaliseEnd = stamp.decodeEnd + 3 * tick;
            stamp.uploadStart = stamp.normaliseEnd + 4 * tick; stamp.uploadEnd = stamp.uploadStart + tick;
            stamp.presentSubmit = stamp.uploadEnd + 2 * tick;
            telemetry->recordWorker(stamp); telemetry->recordPresent(stamp, stamp.presentSubmit + tick);
        }
        require(std::abs(telemetry->distribution(Timing::callbackToPresent).max() - 18.0) < .001, "callback entry -> Present submission");
        require(std::abs(telemetry->distribution(Timing::cpuDecode).max() - 5.0) < .001, "decode excludes queue/copy/normalise");
        const auto report = telemetry->toJson();
        const auto trace = report["presentTrace"];
        require(trace.size() == 128 && static_cast<int>(trace[0]["frame"]) == 73 && static_cast<int>(trace[127]["frame"]) == 200, "trace retains only latest 128 frames");
    });
    test("JSON writer creates parents and verifies UTF-8 replacement", []
    {
        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("RecorderContract-" + juce::Uuid().toString());
        const auto file = root.getChildFile(juce::String::fromUTF8("한글/report.json"));
        auto report = jsonObject(); jsonSet(report, "status", "unavailable"); jsonSet(report, "reason", juce::String::fromUTF8("장치 없음"));
        CaptureTelemetry::writeJson(file, report);
        auto loaded = juce::JSON::parse(file.loadFileAsString());
        const bool roundtrip = loaded["reason"] == report["reason"];
        jsonSet(report, "status", "available"); CaptureTelemetry::writeJson(file, report);
        loaded = juce::JSON::parse(file.loadFileAsString());
        const bool replaced = loaded["status"].toString() == "available";
        file.deleteFile(); file.getParentDirectory().deleteFile(); root.deleteFile();
        require(roundtrip && replaced, "UTF-8 JSON saved/read/replaced; parent automatically created");
    });
    test("MF synthetic padded NV12 buffer is copied before sample release", []
    {
        ComApartment apartment; MfRuntime mf;
        auto mode = rawMode();
        ComPtr<IMFMediaBuffer> buffer;
        checkHr(MFCreate2DMediaBuffer(mode.width, mode.height, MFVideoFormat_NV12.Data1, FALSE, &buffer), "Create synthetic 2D buffer");
        ComPtr<IMF2DBuffer2> twoD; checkHr(buffer.As(&twoD), "Synthetic IMF2DBuffer2");
        BYTE* row0 = nullptr; BYTE* base = nullptr; LONG pitch = 0; DWORD length = 0;
        checkHr(twoD->Lock2DSize(MF2DBuffer_LockFlags_Write, &row0, &pitch, &base, &length), "Lock fixture");
        memset(base, 0xee, length);
        for (unsigned row = 0; row < mode.height; ++row) memset(row0 + row * pitch, 32 + row, mode.width);
        for (unsigned row = 0; row < mode.height / 2; ++row) memset(row0 + (mode.height + row) * pitch, 128, mode.width);
        checkHr(twoD->Unlock2D(), "Unlock fixture");
        ComPtr<IMFSample> sample; checkHr(MFCreateSample(&sample), "Create synthetic sample");
        checkHr(sample->AddBuffer(buffer.Get()), "Attach synthetic buffer");
        CaptureFrameDecoder decoder(mode, 1, ColourDevice::warpForTests); decoder.copySample(sample.Get());
        sample.Reset(); buffer.Reset(); twoD.Reset();
        VideoSurface output; output.prepare(16, 16); FrameStamp stamp; decoder.decodeCopied(output, stamp);
        bool exact = true;
        for (unsigned row = 0; row < 16; ++row) for (unsigned x = 0; x < 16; ++x) exact &= output.y()[row * 16 + x] == 32 + row;
        for (unsigned i = 0; i < 128; ++i) exact &= output.uv()[i] == 128;
        require(exact, "padding excluded and bytes survive original COM sample destruction");
    });
    test("mailbox concurrent frame data remains stable", []
    {
        VideoSurfacePool pool(16, 16); std::atomic<bool> done{false};
        std::thread producer([&]
        {
            for (unsigned frame = 1; frame <= 20000; ++frame)
            {
                int slot = pool.acquireWrite();
                while (slot < 0) { std::this_thread::yield(); slot = pool.acquireWrite(); }
                pool.surface(slot).stamp.frame = frame;
                std::fill(pool.surface(slot).nv12.begin(), pool.surface(slot).nv12.end(), static_cast<uint8_t>(frame));
                pool.publish(slot);
            }
            done.store(true);
        });
        bool coherent = true; std::uint64_t previous = 0;
        while (true)
        {
            const int slot = pool.takeLatest();
            if (slot >= 0)
            {
                auto& surface = pool.surface(slot); coherent &= surface.stamp.frame > previous; previous = surface.stamp.frame;
                for (const auto byte : surface.nv12) coherent &= byte == static_cast<uint8_t>(previous);
                pool.release(slot);
            }
            else if (done.load()) break;
        }
        producer.join(); require(coherent, "mailbox frame never tears/reorders");
    });
    test("MF bottom-up padded YUY2 preserves logical scanline order", []
    {
        ComApartment apartment; MfRuntime mf;
        auto mode = rawMode(CaptureSubtype::yuy2); mode.stride = -40;
        ComPtr<IMFMediaBuffer> buffer; checkHr(MFCreateMemoryBuffer(640, &buffer), "Create bottom-up fixture");
        BYTE* bytes = nullptr; checkHr(buffer->Lock(&bytes, nullptr, nullptr), "Lock bottom-up fixture");
        memset(bytes, 0xee, 640);
        for (unsigned row = 0; row < 16; ++row)
            for (unsigned x = 0; x < 16; x += 2)
            {
                auto* pair = bytes + (15 - row) * 40 + x * 2;
                pair[0] = pair[2] = static_cast<uint8_t>(32 + row); pair[1] = pair[3] = 128;
            }
        checkHr(buffer->Unlock(), "Unlock bottom-up fixture"); checkHr(buffer->SetCurrentLength(640), "Set fixture length");
        ComPtr<IMFSample> sample; checkHr(MFCreateSample(&sample), "Create bottom-up sample"); checkHr(sample->AddBuffer(buffer.Get()), "Attach fixture");
        CaptureFrameDecoder decoder(mode, 1, ColourDevice::warpForTests); decoder.copySample(sample.Get());
        VideoSurface output; output.prepare(16, 16); FrameStamp stamp; decoder.decodeCopied(output, stamp);
        bool ordered = true;
        for (unsigned row = 0; row < 16; ++row) ordered &= output.y()[row * 16] == 32 + row;
        require(ordered, "negative padded pitch decoded top-to-bottom");
        checkHr(buffer->SetCurrentLength(32), "Truncate fixture storage");
        rejects([&] { decoder.copySample(sample.Get()); });
    });
    test("NV12 limited passthrough preserves every byte and metadata stamp", []
    {
        auto mode = rawMode(); CaptureFrameDecoder decoder(mode, 1, ColourDevice::warpForTests);
        VideoSurface output; output.prepare(16, 16);
        std::vector<uint8_t> input(384, 128); std::fill_n(input.begin(), 256, 64);
        FrameStamp stamp; stamp.frame = 42; stamp.pts100ns = 1234567; stamp.hasDeviceTimestamp = true; stamp.deviceTimestamp100ns = 7654321;
        decoder.decodeBytes(input.data(), input.size(), output, stamp);
        require(output.nv12 == input && !output.colourAssumed, "byte-exact passthrough");
        require(output.stamp.frame == 42 && output.stamp.pts100ns == 1234567 && output.stamp.deviceTimestamp100ns == 7654321, "timing identity preserved");
        rejects([&] { decoder.decodeBytes(input.data(), 10, output, stamp); });
    });
    test("full-range pixels really normalise to limited black/white", []
    {
        auto mode = rawMode(); mode.colour.range = MFNominalRange_0_255;
        CaptureFrameDecoder decoder(mode, 1, ColourDevice::warpForTests); VideoSurface output; output.prepare(16, 16); FrameStamp stamp;
        std::vector<uint8_t> input(384, 128);
        std::fill_n(input.begin(), 128, 0); std::fill(input.begin() + 128, input.begin() + 256, 255);
        decoder.decodeBytes(input.data(), input.size(), output, stamp);
        require(std::abs(output.nv12[0] - 16) <= 1 && std::abs(output.nv12[240] - 235) <= 1, "full 0/255 -> limited 16/235, not a metadata change");
    });
    test("YUY2 BT.601 red is matrix-converted to BT.709 NV12", []
    {
        auto mode = rawMode(CaptureSubtype::yuy2); mode.colour.matrix = MFVideoTransferMatrix_BT601;
        CaptureFrameDecoder decoder(mode, 1, ColourDevice::warpForTests); VideoSurface output; output.prepare(16, 16); FrameStamp stamp;
        std::vector<uint8_t> input(512);
        for (size_t i = 0; i < input.size(); i += 4) { input[i] = input[i+2] = 81; input[i+1] = 90; input[i+3] = 240; }
        decoder.decodeBytes(input.data(), input.size(), output, stamp);
        // Independent BT.709 limited red reference: Y~63, Cb~102, Cr~240.
        require(std::abs(output.y()[0] - 63) <= 4 && std::abs(output.uv()[0] - 102) <= 4 && std::abs(output.uv()[1] - 240) <= 4, "real matrix coefficients");
        require(output.nv12.size() == 384, "8-bit NV12 4:2:0 plane size");
    });
    test("missing colour metadata is flagged; explicit HDR rejected", []
    {
        auto mode = rawMode(); mode.colour = {};
        CaptureFrameDecoder decoder(mode, 1, ColourDevice::warpForTests); VideoSurface output; output.prepare(16, 16); FrameStamp stamp;
        std::vector<uint8_t> input(384, 128); decoder.decodeBytes(input.data(), input.size(), output, stamp);
        require(output.colourAssumed, "unknown is not colour-certified");
        mode.colour.transfer = MFVideoTransFunc_2084;
        CaptureFrameDecoder hdr(mode, 1, ColourDevice::warpForTests); rejects([&] { hdr.decodeBytes(input.data(), input.size(), output, stamp); });
    });
    test("CPU MJPEG decode produces bounded NV12 and correct grey range", []
    {
        auto mode = rawMode(CaptureSubtype::mjpeg); mode.colour = {};
        CaptureFrameDecoder decoder(mode, 1, ColourDevice::warpForTests); VideoSurface output; output.prepare(16, 16); FrameStamp stamp;
        auto jpeg = makeJpeg(); decoder.decodeBytes(jpeg.data(), jpeg.size(), output, stamp);
        require(output.nv12.size() == 384 && output.width == 16 && output.height == 16, "MJPEG -> NV12");
        require(std::abs(output.y()[0] - 126) <= 3 && std::abs(output.uv()[0] - 128) <= 2, "JPEG full-range grey converted to limited");
        require(decoder.effectiveThreads() == 1, "controlled decoder threads");
        rejects([&] { CaptureFrameDecoder invalid(mode, 0); });
        const uint8_t invalid[] = {1,2,3,4}; rejects([&] { decoder.decodeBytes(invalid, sizeof(invalid), output, stamp); });
    });
    std::cout << "RecorderCaptureContracts: " << passed << " passed, " << failed << " failed; no hardware opened\n";
    return failed ? 1 : 0;
}
