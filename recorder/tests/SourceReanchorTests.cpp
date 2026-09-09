#include "TestSupport.h"
#include "sync/CameraClockMapper.h"
#include "record/NvencEncoder.h"
#include "record/TakeController.h"
#include "record/ReferenceMixWriter.h"
#include "export/FinalVideoExporter.h"
#include <algorithm>

using namespace gocue::recorder;
using recorder_test::require;
namespace
{
constexpr Sample hz = 10000000, base = 1000000000, origin = 48000, latency = 20000;
FrameStamp stamp(unsigned index, bool direct, Sample ptsShift = 0)
{
    FrameStamp f; f.frame = index + 1; f.generation = 7;
    f.pts100ns = Sample(index) * hz / 60 + ptsShift;
    f.callback = base + Sample(index) * hz / 60 + latency;
    f.hasDeviceTimestamp = f.deviceTimestampValid = direct;
    if (direct) f.deviceTimestamp100ns = std::uint64_t(f.callback);
    return f;
}
void audioBlock(ClockMapper& master, unsigned n)
{
    BlockStamp s{}; s.sequence = n; s.samplePosition = Sample(n) * 480;
    s.sampleRate = 48000; s.numSamples = 480; s.flags = samplePositionValid;
    s.callbackQpc = base + Sample(n) * 100000; master.observe(s);
}
ClockSnapshot warm(ClockMapper& master, CameraClockMapper& camera, bool direct)
{
    for (unsigned n = 0; n <= 100; ++n) audioBlock(master, n);
    for (unsigned n = 0; n < 60; ++n) require(camera.observe(stamp(n, direct)), "Warm camera clock");
    require(master.snapshot()->valid && camera.snapshot()->valid, "Prepared clocks available");
    return *master.snapshot();
}
void encodeRecovery(bool dubbing, bool direct)
{
    ClockMapper master(hz); CameraClockMapper camera(master, hz, {60, 1}, latency);
    const auto audio = warm(master, camera, direct);
    std::unique_ptr<CameraTimeMapper> mapper;
    if (dubbing) mapper = std::make_unique<AnchoredCameraTimeMapper>(audio, *camera.snapshot(), origin, 48000);
    else mapper = std::make_unique<CameraSampleTimeMapper>(camera, origin, 48000, audio.epoch, *camera.snapshot());

    const auto* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4); require(codec != nullptr, "CPU regression encoder exists");
    CodecPtr encoder(avcodec_alloc_context3(codec)); require(encoder != nullptr, "Allocate CPU codec");
    encoder->width = encoder->height = 16; encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->time_base = {1, 60}; encoder->framerate = {60, 1}; encoder->gop_size = 60;
    encoder->max_b_frames = 0; encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ffCheck(avcodec_open2(encoder.get(), codec, nullptr), "Open CPU recovery codec");
    auto picture = ffFrame(); picture->format = encoder->pix_fmt; picture->width = picture->height = 16;
    ffCheck(av_frame_get_buffer(picture.get(), 32), "Allocate CPU picture");
    for (int plane = 0; plane < 3; ++plane) for (int row = 0; row < (plane ? 8 : 16); ++row)
        std::fill_n(picture->data[plane] + row * picture->linesize[plane], plane ? 8 : 16, std::uint8_t(plane ? 128 : 80));
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SourceReanchor-" + newId());
    require(directory.createDirectory().wasOk(), "Create recovery artifact directory");
    const auto output = directory.getChildFile("recovered.mp4.partial");
    ReferenceMixWriter aac(48000); FinalMp4Writer mux(output, *encoder, aac.context());
    VideoCfrScheduler cfr({60, 1}, {60, 1}); std::array<float, 1600> silence{};
    unsigned submitted = 0, reanchors = 0, nextAudio = 101; Sample ptsShift = 0, previousTime = -1;
    const auto receive = [&]
    {
        auto packet = ffPacket();
        for (;;)
        {
            const auto result = avcodec_receive_packet(encoder.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            ffCheck(result, "Receive recovery packet"); packet->duration = 1; mux.video(*packet); av_packet_unref(packet.get());
        }
    };
    for (unsigned n = 0; n < 3600; ++n)
    {
        while (nextAudio <= (n + 60) * 100 / 60 + 1) audioBlock(master, nextAudio++);
        const bool reset = n < 3 || n % 113 == 0 || n % 127 == 0;
        if (reset) ptsShift = (++reanchors % 2 ? 1 : -1) * (900000000LL + Sample(reanchors) * hz);
        const auto f = stamp(n + 60, direct, ptsShift);
        if (reset)
        {
            require(camera.reanchor(f), "Explicit discontinuity/type-change gets a fresh valid anchor");
            mapper->reanchor();
        }
        else require(camera.observe(f), "Continuous frame after reanchor");
        const auto time = mapper->map(f), expected = Sample(n) * hz / 60;
        require(std::abs(time - expected) <= 210 && time > previousTime, "Reanchor preserves origin and increasing frame time");
        previousTime = time;
        // No reset of the scheduler, so its output grid covers the full 60s.
        cfr.push({f.frame, time, int(n)}); const auto selected = cfr.select(mapper->now(f.callback), true);
        require(selected && selected->pts == n && selected->input.sourceId == f.frame, "CFR continues selecting new images after every reset");
        picture->pts = selected->pts; ffCheck(avcodec_send_frame(encoder.get(), picture.get()), "Submit recovery frame");
        receive(); ++submitted; aac.append(silence.data(), 800, [&](const AVPacket& p) { mux.audio(p); });
    }
    require(reanchors > 50 && submitted == 3600 && cfr.counters().outputs == 3600, "Full 60s survives repeated source resets");
    auto reconnect = stamp(3660, direct, ptsShift); ++reconnect.generation;
    mapper->reanchor(); camera.reanchor(reconnect);
    recorder_test::rejects([&] { mapper->map(reconnect); });
    ffCheck(avcodec_send_frame(encoder.get(), nullptr), "Drain recovery codec"); receive();
    aac.finishInput([&](const AVPacket& p) { mux.audio(p); }); mux.finish();
    ExportActivity activity; ExportControl control(activity); unsigned decoded = 0;
    ExportVerificationObserver oracle; oracle.video = [&](Sample, const AVFrame&) { ++decoded; };
    FinalVideoExporter::verify(output, 3600, 2880000, 48000, {60, 1}, control, oracle, AV_CODEC_ID_MPEG4);
    require(decoded == submitted, "Finalized MP4 decodes all 3600 frames");
    require(output.moveFileTo(directory.getChildFile("recovered.mp4")), "Publish verified recovery MP4");
    std::cout << (dubbing ? "dubbing" : "recording") << (direct ? " device" : " PTS-only")
              << ": reanchors=" << reanchors << ", submitted=" << submitted << ", decoded=" << decoded << '\n';
}
}
int runSourceReanchorTests()
{
    recorder_test::Suite tests;
    tests.test("Production stream discontinuity requests never latch a source failure or clear a real failure", []
    {
        // Exercise LiveTakeVideo itself before preparation, without opening a
        // GPU. The old implementation called sourceFailed on the first request.
        auto stream = TakeController::createVideoStream(nullptr, "cam1");
        for (unsigned n = 0; n < 62; ++n)
        {
            stream->discontinuity();
            require(!stream->failed(), "A recoverable source notification must not permanently fail LiveTakeVideo");
        }
        require(std::int64_t(stream->report()["reanchorRequests"]) == 62, "Requests retained until a valid queued frame arrives");
        stream->sourceFailed(0); stream->discontinuity();
        require(stream->failed(), "A real source failure is never revived by a later reanchor request");
    });
    for (bool dubbing : {false, true}) for (bool direct : {false, true})
    {
        const auto name = std::string(dubbing ? "Dubbing" : "Recording") + (direct ? " device timestamp" : " PTS-only")
            + " encodes and decodes 3600 frames across repeated source resets";
        tests.test(name.c_str(), [=] { encodeRecovery(dubbing, direct); });
    }
    tests.test("Queued frames retain the reanchor boundary despite later producer requests", []
    {
        NvencFramePool pool(4, 16, 16); VideoSurface input; input.prepare(16, 16);
        for (unsigned n = 0; n < 4; ++n)
        { input.stamp = stamp(n, true); require(pool.copy(input, n < 2 ? 0 : n - 1), "Queue multiple clock segments"); }
        for (unsigned n = 0; n < 4; ++n)
        {
            int slot = -1; require(pool.pop(slot), "Pop queued image");
            require(pool.stamp(slot).frame == n + 1 && pool.clockRevision(slot) == (n < 2 ? 0 : n - 1),
                    "A later notification must not reanchor an earlier queued frame");
            pool.release(slot);
        }
        require(pool.occupied() == 0, "All queue slots released");
    });
    tests.test("Invalid stamps do not consume recovery and a reanchor cannot accept a new ASIO epoch", []
    {
        ClockMapper master(hz); CameraClockMapper camera(master, hz, {60, 1}, latency);
        const auto audio = warm(master, camera, false);
        CameraSampleTimeMapper mapper(camera, origin, 48000, audio.epoch, *camera.snapshot());
        const auto epoch = camera.snapshot()->epoch;
        require(!camera.reanchor({}) && camera.snapshot()->epoch == epoch, "Wait for a valid frame without resetting the clock");
        const auto f = stamp(60, false, -900000000);
        require(camera.reanchor(f), "Next valid frame anchors immediately without another warmup");
        mapper.reanchor(); require(mapper.map(f) == 0, "N0 preserved for PTS-only recovery");
        master.reset(); mapper.reanchor(); recorder_test::rejects([&] { mapper.map(f); });
    });
    return tests.result("source-reanchor");
}
