#include "TestSupport.h"
#include "capture/CaptureDecodeRecovery.h"
#include "video/PresentPacing.h"
#include "fixtures/MfJpegRangeFull601.h"
extern "C"
{
#include <libavcodec/avcodec.h>
}
#include <cmath>
#include <cstring>
#include <future>
#include <thread>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
CameraMode fixtureMode()
{
    auto mode = CameraMode::parse("MJPEG 352x32 60/1");
    mode.colour = {MFVideoTransferMatrix_BT601, MFNominalRange_0_255, MFVideoPrimaries_BT709, MFVideoTransFunc_709};
    return mode;
}
ComPtr<IMFSample> packetSample(const std::vector<std::uint8_t>& bytes)
{
    ComPtr<IMFMediaBuffer> buffer;
    checkHr(MFCreateMemoryBuffer(static_cast<DWORD>(std::max<size_t>(1, bytes.size())), &buffer), "Create packet fixture");
    BYTE* data = nullptr; checkHr(buffer->Lock(&data, nullptr, nullptr), "Lock fixture");
    if (!bytes.empty()) std::memcpy(data, bytes.data(), bytes.size());
    checkHr(buffer->Unlock(), "Unlock fixture");
    checkHr(buffer->SetCurrentLength(static_cast<DWORD>(bytes.size())), "Set packet fixture length");
    ComPtr<IMFSample> sample; checkHr(MFCreateSample(&sample), "Create fixture sample");
    checkHr(sample->AddBuffer(buffer.Get()), "Attach packet fixture"); return sample;
}
}
int runCaptureReviewTests()
{
    Suite suite;
    suite.test("presenter holds at most one CPU surface, releasing success/busy/exception", []
    {
        VideoSurfacePool pool(16,16);
        const int first = pool.acquireWrite(); pool.publish(first);
        for (int attempt = 0; attempt < 30; ++attempt)
        {
            bool called = false, threw = false;
            try
            {
                pool.uploadLatest([&](VideoSurface&)
                {
                    called = true;
                    const int mailbox = pool.acquireWrite(); require(mailbox >= 0, "second slot for latest mailbox");
                    pool.publish(mailbox);
                    const int writing = pool.acquireWrite(); require(writing >= 0, "third slot always free for worker while uploading ONE surface");
                    pool.release(writing);
                    if (attempt % 3 == 2) throw std::runtime_error("Injected upload failure");
                    // attempt%3==1 models a DO_NOT_WAIT map refusing the upload.
                });
            }
            catch (const std::runtime_error&) { threw = true; }
            require(called && threw == (attempt % 3 == 2), "upload result/exception contract");
            const int a = pool.acquireWrite(), b = pool.acquireWrite();
            require(a >= 0 && b >= 0, "after upload only the new mailbox can own a CPU slot");
            pool.release(a); pool.release(b);
        }
        pool.uploadLatest([](VideoSurface&) {});
    });
    suite.test("busy and occluded Present retain wait credit until a real Present succeeds", []
    {
        PresentPacing p; require(p.needsWait(), "initial latency wait");
        p.presented(DXGI_ERROR_WAS_STILL_DRAWING);
        for (int i = 0; i < 60; ++i)
        { require(!p.needsWait(), "busy retry does not need a new signal"); p.presented(DXGI_ERROR_WAS_STILL_DRAWING); }
        p.presented(DXGI_STATUS_OCCLUDED);
        require(!p.needsWait() && p.needsVisibilityTest(), "occluded needs TEST without wait");
        p.visibilityTest(DXGI_STATUS_OCCLUDED);
        require(p.needsVisibilityTest() && !p.needsWait(), "still occluded");
        p.visibilityTest(S_OK);
        require(!p.needsVisibilityTest() && !p.needsWait(), "restored TEST did not replenish wait credit");
        p.presented(S_OK); require(p.needsWait(), "successful Present restores normal pacing");
    });
    suite.test("integer 59.94 and unavailable display refresh are distinguished", []
    {
        require(std::string(displayRefreshVerdict(59)) == "PASS" && std::string(displayRefreshVerdict(60)) == "PASS", "59/60 accepted");
        require(std::string(displayRefreshVerdict(0)) == "UNAVAILABLE" && std::string(displayRefreshVerdict(1)) == "UNAVAILABLE", "missing/default frequency unavailable");
        require(std::string(displayRefreshVerdict(30)) == "FAIL", "known 30 Hz fails");
    });
    suite.test("bad MJPEG packet fixtures increment decoderError and next frame continues", []
    {
        ComApartment com; MfRuntime mf;
        const auto good = makeJpegRangeFixture(352,32);
        const std::vector<std::vector<std::uint8_t>> bad{{}, {0xff,0xd8}, {0xff,0xd8,0xff,0xd9}, {1,2,3,4,5,6}};
        CaptureFrameDecoder decoder(fixtureMode(), 16, ColourDevice::warpForTests);
        CaptureDecodeRecovery recovery;
        auto telemetry = std::make_unique<CaptureTelemetry>(Rational{60,1});
        VideoSurface output; output.prepare(352,32); FrameStamp stamp;
        for (const auto& corrupt : bad)
        {
            auto sample = packetSample(corrupt);
            require(!recovery.frame(decoder, *telemetry, [&] { decoder.copySample(sample.Get()); sample.Reset(); decoder.decodeCopied(output, stamp); }), "single damaged sample discarded");
            require(recovery.consecutiveFailures() == 1, "isolated failure streak");
            sample = packetSample(good); ++stamp.frame;
            require(recovery.frame(decoder, *telemetry, [&] { decoder.copySample(sample.Get()); sample.Reset(); decoder.decodeCopied(output, stamp); }), "same decoder/worker processes next valid sample");
            require(recovery.consecutiveFailures() == 0 && output.stamp.frame == stamp.frame, "valid frame resets streak with current identity");
        }
        require(telemetry->count(LossReason::decoderError) == bad.size(), "one loss per bad frame");
        const auto* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        require(codec && !(codec->capabilities & (AV_CODEC_CAP_FRAME_THREADS | AV_CODEC_CAP_SLICE_THREADS)), "pinned MJPEG decoder has no threading capability");
        require(decoder.effectiveThreads() == 1, "--decoder-threads 16 is ineffective for pinned MJPEG");
    });
    suite.test("decode stops only on the 30th consecutive failure", []
    {
        CaptureFrameDecoder decoder(fixtureMode(), 1, ColourDevice::warpForTests);
        CaptureDecodeRecovery recovery;
        auto telemetry = std::make_unique<CaptureTelemetry>(Rational{60,1});
        VideoSurface output; output.prepare(352,32); FrameStamp stamp;
        const auto fail = [&] { return recovery.frame(decoder, *telemetry, [&] { decoder.decodeBytes(nullptr,0,output,stamp); }); };
        for (unsigned i = 1; i < CaptureDecodeRecovery::failureLimit; ++i) require(!fail(), "recover before limit");
        rejects(fail);
        require(telemetry->count(LossReason::decoderError) == 30 && recovery.consecutiveFailures() == 30, "terminal frame counted once");
    });
    suite.test("identical current media type can change metadata without changing signal", []
    {
        const auto original = fixtureMode(); auto changed = original;
        changed.stride = 384; changed.colour = {};
        requireSameCaptureSignal(original,changed);
        changed.fps = {60000,1001}; rejects([&] { requireSameCaptureSignal(original,changed); });
        changed = original; changed.width += 16; rejects([&] { requireSameCaptureSignal(original,changed); });
        changed = original; changed.subtype = CaptureSubtype::nv12; rejects([&] { requireSameCaptureSignal(original,changed); });
    });
    suite.test("measurement reset clears counts, timings and traces while owners are held", []
    {
        auto t = std::make_unique<CaptureTelemetry>(Rational{60,1});
        t->samples.store(3); t->loss(LossReason::presentBusy,60); t->firstCallbackQpc.store(42);
        FrameStamp stamp; stamp.frame=1; t->recordWorker(stamp); t->recordPresent(stamp,0);
        t->reset();
        require(!t->samples.load() && !t->firstCallbackQpc.load() && !t->count(LossReason::presentBusy), "counts reset");
        require(!t->distribution(Timing::workerTotal).count() && t->toJson()["presentTrace"].size() == 0, "histograms and trace reset");
    });
    suite.test("measured Microsoft MFT fixture is full 601, not HD raw limited 709", []
    {
        const auto decision = classifyJpegOutput(mfJpegFull601Fixture);
        require(decision.model == JpegOutputModel::full601 && decision.bestMaxError == 0, "measured full601 exact");
        require(decision.rmse[3] > 8 && decision.runnerUpMargin > 6, "limited709 decisively rejected");
        // Independent quantised limited-601 fixture (luma 16+219Y/255; chroma 128+224C/255).
        const std::vector<YuvValue> limited{{16,128,128},{30,128,128},{126,128,128},{218,128,128},{235,128,128},
            {99,105,196},{137,83,72},{74,196,117},{152,151,60},{112,173,184},{177,60,139}};
        require(classifyJpegOutput(limited).model == JpegOutputModel::limited601, "limited conversion separately detected");
        const std::vector<YuvValue> full709{{0,128,128},{16,128,128},{128,128,128},{235,128,128},{255,128,128},
            {84,110,205},{161,69,59},{61,205,121},{171,146,51},{92,187,197},{194,51,135}};
        const std::vector<YuvValue> limited709{{16,128,128},{30,128,128},{126,128,128},{218,128,128},{235,128,128},
            {88,112,196},{154,76,67},{69,196,122},{163,144,60},{95,180,189},{182,60,134}};
        require(classifyJpegOutput(full709).model == JpegOutputModel::full709, "709 matrix with full range detected");
        require(classifyJpegOutput(limited709).model == JpegOutputModel::limited709, "709 matrix with limited range detected");
        auto clipped = mfJpegFull601Fixture; clipped[0][0]=16; clipped[4][0]=235;
        require(classifyJpegOutput(clipped).model == JpegOutputModel::unknown, "endpoint clipping is not mistaken for range conversion");
    });
    suite.test("MF JPEG origin converts full 601 Y/UV before preview and encoder", []
    {
        auto mode = CameraMode::parse("NV12 32x1080 60/1"); mode.colour = {};
        CaptureFrameDecoder decoder(mode,1,ColourDevice::warpForTests,CaptureDecodeOrigin::mfMjpeg);
        VideoSurface output; output.prepare(32,1080); FrameStamp stamp;
        std::vector<std::uint8_t> input(output.nv12.size());
        // CPU numeric 601->709 oracle vs the production D3D11 compute shader.
        for (const auto& patch : mfJpegFull601Fixture)
        {
            std::fill_n(input.data(),32*1080,static_cast<std::uint8_t>(patch[0]));
            for (size_t i=32*1080;i<input.size();i+=2) { input[i]=static_cast<std::uint8_t>(patch[1]); input[i+1]=static_cast<std::uint8_t>(patch[2]); }
            decoder.decodeBytes(input.data(),input.size(),output,stamp);
            const auto expected = expectedJpegOutput(patch,JpegOutputModel::limited709);
            require(std::abs(output.y()[0]-expected[0])<=1 && std::abs(output.uv()[0]-expected[1])<=1 && std::abs(output.uv()[1]-expected[2])<=1, "WARP pixels match range/matrix oracle");
            require(output.colourAssumed && decoder.colourDecision().find("input BT.601 full") != std::string::npos, "JPEG provenance retained despite NV12 subtype");
        }
        CaptureFrameDecoder rawHd(mode,1,ColourDevice::warpForTests);
        rawHd.decodeBytes(input.data(),input.size(),output,stamp);
        require(output.nv12 == input && rawHd.colourDecision().find("input BT.709 limited") != std::string::npos, "native HD raw keeps its separate documented default");
        // Explicit output signalling takes precedence over the offline fallback.
        mode.colour = {MFVideoTransferMatrix_BT709,MFNominalRange_16_235,MFVideoPrimaries_BT709,MFVideoTransFunc_709};
        decoder.updateOutputMode(mode); decoder.decodeBytes(input.data(),input.size(),output,stamp);
        require(output.nv12 == input, "explicit 709 limited never converted twice");
    });
    return suite.result("capture-review");
}
