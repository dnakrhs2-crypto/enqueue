#include "HardeningChecks.h"
#include "TestSupport.h"
#include "sync/CameraClockMapper.h"
#include "export/FinalVideoExporter.h"
#include "record/ReferenceMixWriter.h"
#include "support/Platform.h"

namespace gocue::recorder::hardening
{
juce::var dubbingEpoch(const juce::File& directory)
{
    using recorder_test::require;
    exportCheck(directory.createDirectory());
    constexpr Sample hz = 10000000, baseQpc = 1000000000, O0 = 19200, Pstart = 12345;
    ClockMapper master(hz);
    const auto audioStamp = [](unsigned n)
    {
        BlockStamp s{}; s.sequence = n; s.samplePosition = Sample(n) * 480;
        s.sampleRate = 48000; s.numSamples = 480; s.flags = samplePositionValid;
        s.callbackQpc = baseQpc + Sample(n) * 100000; return s;
    };
    for (unsigned n = 0; n <= 30; ++n) master.observe(audioStamp(n));
    const auto preparedMaster = *master.snapshot(); require(preparedMaster.valid, "Prepare ASIO model");
    CameraClockMapper camera(master, hz, {60,1});
    FrameStamp warm; warm.frame = 1; warm.generation = 1; warm.pts100ns = 3000000;
    warm.callback = baseQpc + warm.pts100ns; warm.hasDeviceTimestamp = warm.deviceTimestampValid = true;
    warm.deviceTimestamp100ns = warm.callback; camera.observe(warm);
    const auto preparedCamera = *camera.snapshot();
    AnchoredCameraTimeMapper prerollMapper(preparedMaster, preparedCamera, O0, 48000);
    const auto preroll = prerollMapper.map(warm);
    require(preroll < 0 && prerollMapper.map(warm) == preroll, "LiveTake may remap its retained negative preroll");
    AnchoredCameraTimeMapper mapper(preparedMaster, preparedCamera, O0, 48000);
    // Exactly the start()/first-frame boundary: both models advance an epoch.
    // No native sample reset or capture generation change is introduced.
    master.reset(ClockEpochReason::observationGap); camera.reset();
    for (unsigned n = 31; n <= 60; ++n) master.observe(audioStamp(n));
    require(master.snapshot()->epoch == preparedMaster.epoch + 1, "Inject master epoch +1");

    const auto codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4); require(codec != nullptr, "CPU test codec available");
    CodecPtr encoder(avcodec_alloc_context3(codec)); require(encoder != nullptr, "Allocate CPU codec");
    encoder->width = encoder->height = 16; encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->time_base = {1,60}; encoder->framerate = {60,1}; encoder->gop_size = 60;
    encoder->max_b_frames = 0; encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ffCheck(avcodec_open2(encoder.get(), codec, nullptr), "Open CPU regression codec");
    auto picture = ffFrame(); picture->format = encoder->pix_fmt; picture->width = picture->height = 16;
    ffCheck(av_frame_get_buffer(picture.get(), 32), "Allocate tiny regression frame");
    for (int plane = 0; plane < 3; ++plane) for (int row = 0; row < (plane ? 8 : 16); ++row)
        std::fill_n(picture->data[plane] + row * picture->linesize[plane], plane ? 8 : 16, std::uint8_t(plane ? 128 : 80));
    ReferenceMixWriter aac(48000); const auto output = directory.getChildFile("epoch-regression.mp4.partial");
    FinalMp4Writer mux(output, *encoder, aac.context()); VideoCfrScheduler cfr({60,1},{60,1});
    std::array<float,1600> silence{}; unsigned submitted = 0;
    const auto receive = [&]
    {
        auto packet = ffPacket();
        for (;;)
        {
            const auto result = avcodec_receive_packet(encoder.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            ffCheck(result, "Receive CPU regression packet"); packet->duration = 1; mux.video(*packet); av_packet_unref(packet.get());
        }
    };
    FrameStamp last;
    for (unsigned n = 0; n < 60; ++n)
    {
        auto frame = warm; frame.frame = n + 2; frame.pts100ns = 4000000 + Sample(n) * hz / 60;
        frame.callback = baseQpc + frame.pts100ns; frame.deviceTimestamp100ns = frame.callback;
        camera.observe(frame); const auto mapped = mapper.map(frame);
        require(std::abs(mapped - Sample(n) * hz / 60) <= 1, "O0 and MF PTS delta mapping");
        require(projectVideoSample(Pstart, O0, O0) == Pstart, "Off-grid Pstart stays fixed");
        cfr.push({frame.frame, mapped, int(n)}); const auto selected = cfr.select(mapper.now(frame.callback), true);
        require(selected && selected->pts == n && selected->input.sourceId == frame.frame, "Real CFR selects current source");
        picture->pts = selected->pts; ffCheck(avcodec_send_frame(encoder.get(), picture.get()), "Submit CPU regression frame");
        receive(); ++submitted; aac.append(silence.data(), 800, [&](const AVPacket& p) { mux.audio(p); }); last = frame;
    }
    require(camera.snapshot()->epoch == preparedCamera.epoch + 1, "Inject camera epoch +1");
    auto broken = last; ++broken.frame; broken.pts100ns -= hz;
    recorder_test::rejects([&] { mapper.map(broken); });
    broken = last; ++broken.frame; ++broken.generation;
    recorder_test::rejects([&] { mapper.map(broken); });
    broken = last; ++broken.frame; broken.pts100ns += hz / 60; broken.callback += hz / 60; broken.deviceTimestamp100ns -= hz / 2;
    recorder_test::rejects([&] { mapper.map(broken); });
    ffCheck(avcodec_send_frame(encoder.get(), nullptr), "Drain CPU regression codec"); receive();
    aac.finishInput([&](const AVPacket& p) { mux.audio(p); }); mux.finish();
    ExportActivity activity; ExportControl control(activity); unsigned decoded = 0;
    ExportVerificationObserver oracle; oracle.video = [&](Sample, const AVFrame&) { ++decoded; };
    const auto verification = FinalVideoExporter::verify(output, 60, 48000, 48000, {60,1}, control, oracle, AV_CODEC_ID_MPEG4);
    require(submitted == 60 && decoded == submitted, "All epoch regression frames encoded and decoded");
    const auto finalFile = directory.getChildFile("epoch-regression.mp4");
    require(MoveFileExW(output.getFullPathName().toWideCharPointer(), finalFile.getFullPathName().toWideCharPointer(), MOVEFILE_WRITE_THROUGH) != FALSE,
        "Publish verified CPU epoch regression without replacing an existing file");
    auto report = jsonObject(); jsonSet(report,"status","PASS"); jsonSet(report,"masterEpochIncrement",1); jsonSet(report,"cameraEpochIncrement",1);
    jsonSet(report,"framesSubmitted",submitted); jsonSet(report,"framesDecoded",decoded); jsonSet(report,"availableSamples",48000);
    jsonSet(report,"Pstart",Pstart); jsonSet(report,"O0",O0); jsonSet(report,"discontinuitiesRejected",3);
    jsonSet(report,"sourceKind","synthetic clocks + production anchored mapper/CFR + 16x16 CPU MPEG4/AAC; no MF/ASIO/GPU device opened");
    jsonSet(report,"verification",verification); jsonSet(report,"physicalSync","UNAVAILABLE; Claude off/on 60s and studio loopback required");
    jsonSet(report,"videoFile",finalFile.getFullPathName());
    return report;
}
}
