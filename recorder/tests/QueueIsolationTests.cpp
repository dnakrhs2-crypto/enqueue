#include "TestSupport.h"
#include "FramePatternSource.h"
#include "DualAudioLoad.h"
#include "audio/RawAudioTap.h"
#include "record/Mp4TakeWriter.h"
#include "record/NvencEncoder.h"
#include "record/ReferenceMixWriter.h"
#include "record/WavTrackWriter.h"
#include "storage/StorageEncoding.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>

using namespace gocue::recorder;
using namespace gocue::recorder::probe;
using namespace recorder_test;
namespace
{
template<class Ready> void until(Ready ready)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready()) { require(std::chrono::steady_clock::now() < end, "Timed out waiting for fixture worker"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
juce::File temporary(const char* name)
{ return juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(juce::String(name) + "-" + juce::Uuid().toString()); }
WavTrackWriter::Config wavConfig(const juce::File& directory)
{
    WavTrackWriter::Config c; c.projectDirectory = directory;
    for (int i = 0; i < 8; ++i) c.devices.push_back({"fixture", "Input", i + 1, i, i}); return c;
}
struct BlockingWrite : FileIoFaultAdapter
{
    std::atomic<bool> armed{false}, entered{false}, released{false};
    juce::Result beforeIo(FileIoOperation op, const juce::File& f, std::uint64_t offset, std::size_t) override
    {
        if (armed && f.hasFileExtension("wav") && op == FileIoOperation::append && offset >= 44 && !entered.exchange(true))
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!released && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!released) return juce::Result::fail("Fixture release timeout");
        }
        return juce::Result::ok();
    }
};
// LGPL software MPEG4 fixture exercises the *same* MP4 writer and full-decoding
// oracle without opening NVENC. 60 output packets can still conceal wrong IDs.
void makeOracleMp4(const juce::File& file, bool dropId)
{
    const auto* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4); require(codec != nullptr, "Software MPEG4 fixture encoder available");
    CodecPtr context(avcodec_alloc_context3(codec)); require(context != nullptr, "Allocate fixture encoder");
    context->width = 320; context->height = 192; context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = {1,60}; context->framerate = {60,1}; context->gop_size = 60; context->max_b_frames = 0;
    context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; context->bit_rate = 4000000; context->thread_count = 1;
    ffCheck(avcodec_open2(context.get(), codec, nullptr), "Open fixture encoder");
    ReferenceMixWriter reference; Mp4TakeWriter writer(file, *context, reference.context());
    VideoSurface surface; surface.prepare(320,192); auto frame = ffFrame(); auto packet = ffPacket();
    frame->width = 320; frame->height = 192; frame->format = AV_PIX_FMT_YUV420P;
    ffCheck(av_frame_get_buffer(frame.get(),32), "Allocate fixture frame");
    const PacketSink audio = [&](const AVPacket& p) { writer.audio(p); };
    const auto receive = [&]
    {
        for (;;)
        {
            const int result = avcodec_receive_packet(context.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            ffCheck(result,"Receive fixture video"); if (!packet->duration) packet->duration = 1; writer.video(*packet); av_packet_unref(packet.get());
        }
    };
    for (int i = 0; i < 60; ++i)
    {
        paintPattern(surface, {static_cast<unsigned>(dropId && i == 20 ? 20 : i + 1),1});
        ffCheck(av_frame_make_writable(frame.get()),"Writable fixture frame");
        for (int row = 0; row < 192; ++row) std::memcpy(frame->data[0] + row * frame->linesize[0],surface.y() + row * 320,320);
        for (int row = 0; row < 96; ++row) for (int x = 0; x < 160; ++x)
        { frame->data[1][row * frame->linesize[1] + x] = surface.uv()[row * 320 + x * 2]; frame->data[2][row * frame->linesize[2] + x] = surface.uv()[row * 320 + x * 2 + 1]; }
        frame->pts = i; ffCheck(avcodec_send_frame(context.get(),frame.get()),"Submit fixture video"); receive(); reference.advance((i + 1) * 800,audio);
    }
    ffCheck(avcodec_send_frame(context.get(),nullptr),"Flush fixture video"); receive(); reference.finish(48000,audio); writer.finalize();
}
}
int runQueueIsolationTests()
{
    Suite suite;
    suite.test("camera identity survives telemetry reset", []
    {
        auto a = std::make_shared<CaptureTelemetry>(Rational{60,1}, "cam1"); auto b = std::make_shared<CaptureTelemetry>(Rational{30,1}, "cam2");
        a->loss(LossReason::decoderError); a->reset();
        require(a->toJson()["pipelineId"].toString() == "cam1" && b->toJson()["pipelineId"].toString() == "cam2", "Distinct immutable dimensions");
        require(b->count(LossReason::decoderError) == 0, "Camera counts do not cross lanes");
    });
    suite.test("held cam1 encoder surfaces cannot block cam2 capture queue or latest mailbox", []
    {
        NvencFramePool cam1(4,320,192), cam2(4,320,192); VideoSurfacePool preview1(320,192), preview2(320,192);
        BoundedSpscQueue<unsigned,2> capture1, capture2; VideoSurface frame; frame.prepare(320,192); paintPattern(frame,{1,1});
        std::array<int,4> held{};
        for (int i = 0; i < 4; ++i) { require(cam1.copy(frame) && cam1.pop(held[i]), "Hold every cam1 encoder slot"); }
        auto retained = ffFrame(); ffCheck(av_frame_ref(retained.get(), &cam1.frame(held[0])), "Simulate NVENC retained reference");
        require(!cam1.copy(frame), "cam1 saturates independently");
        require(capture1.push(1) && capture1.push(2) && !capture1.push(3), "Cam1 capture backlog reaches two");
        auto other = std::async(std::launch::async, [&]
        {
            for (unsigned number = 1; number <= 120; ++number)
            {
                require(capture2.push(number), "Cam2 capture remains writable while cam1 is stalled"); unsigned input = 0;
                require(capture2.pop(input) && input == number, "Cam2 independent ordered dequeue");
                const int slot = preview2.acquireWrite(); require(slot >= 0, "Cam2 preview slot available");
                auto& image = preview2.surface(slot); paintPattern(image,{input,2}); image.stamp.frame = input;
                require(cam2.copy(image), "Cam2 encoder bank available"); int encoded = -1; require(cam2.pop(encoded), "Cam2 encode receives");
                require(cam2.frame(encoded).data[0] != cam1.frame(held[0]).data[0], "No shared backing allocation"); cam2.release(encoded);
                preview2.publish(slot); preview2.uploadLatest([&](VideoSurface& s) { const auto id = readPattern(s.y(),320,320,192); require(id && id->frame == input && id->camera == 2,"Latest cam2 pixels remain independent"); });
            }
        });
        require(other.wait_for(std::chrono::seconds(10)) == std::future_status::ready, "Cam2 completes without releasing cam1"); other.get();
        for (const int slot : held) cam1.release(slot);
        require(cam1.occupied() == 0 && cam2.occupied() == 0, "Both frame pools drained independently");
        av_frame_unref(retained.get());
    });
    suite.test("raw queue overflow latches take-wide stop; upstream loss prevents WAV finalization", []
    {
        auto c = wavConfig(temporary("RecorderR05-raw-overflow")); WavTrackWriter writer(c); require(writer.start().wasOk(),"Prepare real writer");
        RawAudioTap raw; raw.prepare({0},1,1); const std::uint8_t pcm[3]{}; NativeInputView view{pcm,0,0,nativeFormatForAsio(17)};
        BlockStamp stamp{}; stamp.numSamples = 1; stamp.sampleRate = 48000;
        require(raw.onAsioBlock(stamp,&view,1),"Fill raw queue"); require(!raw.onAsioBlock(stamp,&view,1),"Raw overflow is rejected");
        TakeStopSignal stop; checkAudioQueues(raw,writer,stop); require(stop.get() == AudioStopReason::rawOverflow,"Harness emits take-stop signal");
        stop.request(AudioStopReason::writerFailure); require(stop.get() == AudioStopReason::rawOverflow,"First cause remains visible");
        writer.requestAbort(); require(writer.stop(0,juce::Uuid()).failed(),"Loss cannot be finalized as a normal take");
    });
    suite.test("realtime source workers keep cam2 preview advancing with a saturated cam1 bank", []
    {
        VideoSurfacePool a(1920,1080), b(1920,1080); NvencFramePool stalled(4);
        auto ta = std::make_shared<CaptureTelemetry>(Rational{60,1},"cam1"); auto tb = std::make_shared<CaptureTelemetry>(Rational{30,1},"cam2");
        std::atomic<unsigned> rejected{0}; std::atomic<std::uint64_t> newest{0};
        std::atomic<std::int64_t> origin{0}; std::promise<void> go; auto gate=go.get_future().share();
        FramePatternSource cam1({1,60,1,CaptureSubtype::nv12},a,ta,[&](const VideoSurface& frame) { if (!stalled.copy(frame)) ++rejected; });
        FramePatternSource cam2({2,30,1,CaptureSubtype::nv12},b,tb,[&](const VideoSurface& frame) { newest=frame.stamp.frame; });
        cam1.start(gate,origin); cam2.start(gate,origin); origin=qpcNow(); go.set_value();
        until([&]
        {
            a.uploadLatest([](VideoSurface&) {});
            b.uploadLatest([](VideoSurface& frame) { const auto id=readPattern(frame.y(),1920,1920,1080); require(id && id->camera==2,"Live cam2 payload identity"); });
            return (rejected.load()>0 && newest.load()>=5) || (cam1.finished() && cam2.finished());
        });
        cam1.stop(); cam2.stop(); require(rejected.load()>0 && newest.load()>=5,"Independent source/decode/mailbox progresses while cam1 encode stalls");
        require(ta->queueHighWater<=2 && tb->queueHighWater<=2,"Both live capture queues obey two-sample bound");
        int slot=-1; while(stalled.pop(slot)) stalled.release(slot);
    });
    suite.test("four-second WAV overflow stops take without retry or inserted silence", []
    {
        BlockingWrite fault; auto c = wavConfig(temporary("RecorderR05-wav-overflow")); c.faults = &fault;
        WavTrackWriter writer(c); require(writer.start().wasOk(),"Start writer");
        struct Release { BlockingWrite& fault; ~Release() { fault.released = true; } } release{fault};
        std::vector<std::int32_t> pcm(c.framesPerBlock * c.mics); fault.armed = true;
        require(writer.tryPush(pcm.data(),c.framesPerBlock,0),"First block accepted"); until([&] { return fault.entered.load(); });
        std::uint64_t accepted = c.framesPerBlock;
        while (writer.tryPush(pcm.data(),c.framesPerBlock,accepted)) accepted += c.framesPerBlock;
        require(accepted == writer.queueCapacityFrames(),"Prepared four-second bound includes the writer-held block");
        require(writer.error() == WavTrackWriter::Error::queueOverflow,"WAV failure explicitly identifies overflow");
        RawAudioTap raw; TakeStopSignal stop; checkAudioQueues(raw,writer,stop);
        require(stop.get() == AudioStopReason::writerFailure,"Writer overflow stops all capture");
        fault.released = true; require(writer.stop(static_cast<std::int64_t>(accepted),juce::Uuid()).failed(),"Overflow has no normal finalize");
    });
    suite.test("pixel IDs round-trip, CRC damage and wrong camera are detected", []
    {
        VideoSurface surface; surface.prepare(320,192);
        for (const unsigned id : {1u,255u,65536u,0xffffffffu})
        { paintPattern(surface,{id,2}); auto read = readPattern(surface.y(),320,320,192); require(read && read->camera == 2 && read->frame == id,"Pixel uint32 ID roundtrip"); }
        for (unsigned y = 32; y < 48; ++y) for (unsigned x = 32; x < 48; ++x) surface.y()[y * 320 + x] = 255 - surface.y()[y * 320 + x];
        require(!readPattern(surface.y(),320,320,192),"Checksum rejects one flipped cell");
        require(!readPattern(surface.y(),20,320,192),"Invalid stride rejected");
    });
    suite.test("oracle permits 30 to 60 repeats and 60 to 30 intentional omissions", []
    {
        FramePatternOracle up(2,{30,1},{60,1},1), down(1,{60,1},{30,1},1);
        for (unsigned i = 0; i < 60; ++i) up.observe(PatternId{i/2+1,2},i);
        for (unsigned i = 0; i < 30; ++i) down.observe(PatternId{i*2+1,1},i);
        require(up.finish()["result"].toString() == "PASS" && static_cast<int>(up.finish()["observedRepeats"]) == 30,"Expected repetitions are not capture loss");
        require(down.finish()["result"].toString() == "PASS" && static_cast<int>(down.finish()["intentionalNativeOmissions"]) == 30,"Expected omissions are not capture loss");
        FramePatternOracle gap(1,{60,1},{60,1},1);
        for (unsigned i = 0; i < 59; ++i) gap.observe(PatternId{i==20?20:i+1,1},i);
        require(static_cast<int>(gap.finish()["captureLoss"]) == 2,"Repeated lost ID plus missing tail detected despite plausible frame count");
        FramePatternOracle lost30(2,{30,1},{60,1},1);
        for (unsigned i=0;i<60;++i) lost30.observe(PatternId{i/2==6?6:i/2+1,2},i);
        require(static_cast<int>(lost30.finish()["captureLoss"])==1 && static_cast<int>(lost30.finish()["badOutputPositions"])==2,"One missing 30fps native ID is not double-counted by 60fps repeats");
        FramePatternOracle cross(1,{30,1},{30,1},1); cross.observe(PatternId{1,2}); require(cross.finish()["result"].toString()=="FAIL","Wrong camera rejected");
    });
    suite.test("production NV12 and MJPEG decoders retain pixel oracle", []
    {
        for (const auto subtype : {CaptureSubtype::nv12,CaptureSubtype::mjpeg})
        {
            VideoSurfacePool pool(1920,1080); auto telemetry = std::make_shared<CaptureTelemetry>(Rational{30,1},"cam2");
            FramePatternSource source({2,30,1,subtype,ColourDevice::warpForTests},pool,telemetry,{});
            std::vector<std::uint8_t> bytes; source.makePacket(123456,bytes);
            CaptureFrameDecoder decoder(source.mode(),1,ColourDevice::warpForTests); VideoSurface output; output.prepare(1920,1080); FrameStamp stamp;
            decoder.decodeBytes(bytes.data(),bytes.size(),output,stamp);
            const auto id = readPattern(output.y(),1920,1920,1080); require(id && id->frame == 123456 && id->camera == 2,"Normalized decoder pixels identify source frame");
        }
    });
    suite.test("full MP4 decode catches hidden ID loss with equal CFR packet count", []
    {
        const auto dir = temporary("RecorderR05-oracle"); require(dir.createDirectory().wasOk(),"Create fixture directory");
        for (const bool bad : {false,true})
        {
            const auto path = dir.getChildFile(bad?"bad.mp4":"good.mp4"); makeOracleMp4(path,bad);
            const auto result = inspectPatternMp4(path,1,{60,1},{60,1},1);
            require(static_cast<int>(result["decodedFrames"])==60,"Both fixtures decode to 60 CFR frames");
            require(result["result"].toString()==(bad?"FAIL":"PASS"),"Only pixel ID continuity detects hidden repeated source");
        }
    });
    suite.test("8ch realtime synthetic tap writes bit-exact mono PCM24 originals", []
    {
        const auto dir = temporary("RecorderR05-audio"); std::atomic<std::int64_t> origin{0}; TakeStopSignal stop;
        DualAudioLoad audio({true,0,1,dir,nullptr},stop,origin); audio.prepare(); origin=qpcNow();
        until([&] { return audio.complete() || stop.requested(); }); audio.stopInput(); audio.finish();
        const auto report=audio.toJson(); require(static_cast<bool>(report["complete"]),"Synthetic audio completes without overflows");
        const juce::Uuid take(report["takeId"].toString());
        for (unsigned mic=0;mic<8;++mic)
        {
            juce::MemoryBlock bytes; require(dir.getChildFile(WavTrackWriter::chunkPath(take,mic+1,1)).loadFileAsData(bytes),"Read written mono track");
            const auto* p=static_cast<const std::uint8_t*>(bytes.getData()); require(bytes.getSize()==44+48000*3,"One second of PCM24");
            require(storageEncoding::get<std::uint16_t>(p+34)==24 && storageEncoding::get<std::uint16_t>(p+22)==1,"PCM24 mono header");
            for (unsigned i=0;i<48000;++i)
            {
                const auto* x=p+44+i*3; const auto got=std::uint32_t{x[0]}|(std::uint32_t{x[1]}<<8)|(std::uint32_t{x[2]}<<16);
                require(got==(static_cast<std::uint32_t>(audioPattern(i,mic))&0xffffffu),"Every original sample/channel survives tap, conversion and WAV writer");
            }
        }
    });
    suite.test("two-second real WAV I/O stall recovers inside the four-second queue", []
    {
        const auto dir=temporary("RecorderR05-audio-stall"); std::atomic<std::int64_t> origin{0}; TakeStopSignal stop;
        WriterStall stall(2000); DualAudioLoad audio({true,0,4,dir,&stall},stop,origin); audio.prepare();
        const auto zero=qpcNow(); stall.arm(zero+qpcFrequency()); origin=zero;
        until([&] { return audio.complete() || stop.requested(); }); audio.stopInput(); audio.finish();
        const auto report=audio.toJson();
        require(static_cast<bool>(stall.toJson()["injected"]),"Real file-owner stall occurred");
        require(static_cast<bool>(report["complete"]),"2s stall recovers with all 8ch input samples");
        const auto high=static_cast<juce::int64>(report["writer"]["queueHighWaterFrames"]);
        require(high>=48000 && high<=192000,"Measured backlog remains inside four-second PCM budget");
        require(static_cast<juce::int64>(report["writer"]["writtenSamplesPerMic"])==192000,"No silence or lost samples in recovered writer");
    });
    return suite.result("QueueIsolationTests");
}
