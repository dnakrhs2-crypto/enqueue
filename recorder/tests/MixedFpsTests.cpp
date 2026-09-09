#include "TestSupport.h"
#include "record/TakeController.h"
#include "sync/CameraClockMapper.h"
#include "FramePatternSource.h"
#include <thread>

using namespace gocue::recorder;
using recorder_test::require;
namespace
{
FrameStamp stamp(unsigned frame, unsigned fps, std::int64_t base = 10000000)
{
    FrameStamp s; s.frame = frame + 1; s.generation = 7;
    s.pts100ns = std::int64_t(frame) * 10000000 / fps;
    s.deviceTimestamp100ns = base + s.pts100ns; s.hasDeviceTimestamp = s.deviceTimestampValid = true;
    s.callback = base + s.pts100ns + 1000; return s;
}
void masterBlock(ClockMapper& master, unsigned block)
{
    BlockStamp s{}; s.flags = samplePositionValid; s.sequence = block; s.samplePosition = block * 480;
    s.sampleRate = 48000; s.numSamples = 480; s.callbackQpc = 10000000 + std::int64_t(block) * 100000;
    master.observe(s);
}
}
int runMixedFpsTests()
{
    recorder_test::Suite tests;
    for (const auto phase : {0LL, 70000LL, 200000LL}) tests.test("30 to 60 native repeats respect independent camera phase", [phase]
    {
        VideoCfrScheduler scheduler({30,1}, {60,1}); unsigned source = 0;
        for (int output = 0; output < 120; ++output)
        {
            const auto grid = VideoCfrScheduler::gridTime(output,{60,1});
            while (source < 62 && VideoCfrScheduler::gridTime(source,{30,1}) + phase <= grid + 333334)
            { scheduler.push({source + 1, VideoCfrScheduler::gridTime(source,{30,1}) + phase, int(source % 16)}); ++source; }
            require(scheduler.select(grid + 333334).has_value(), "Every project grid has a frame");
        }
        const auto& c = scheduler.counters();
        require(c.outputs == 120 && c.repeated >= 59, "Time preserved at 60 CFR");
        require(c.reasons[0].repeated == c.repeated && c.reasons[1].repeated == 0, "Native repeats do not become clock errors at another phase");
        require(c.reasons[2].missing == 0 && c.reasons[3].missing == 0, "No capture/encode error inflation");
    });
    tests.test("Two camera Lcam values map through one live master and reserved N0", []
    {
        ClockMapper master(10000000); for (unsigned i=0;i<=100;++i) masterBlock(master,i);
        CameraClockMapper a(master,10000000,{60,1},100000), b(master,10000000,{30,1},300000);
        const auto first=stamp(60,60), second=stamp(30,30); a.observe(first); b.observe(second);
        const auto epoch=master.snapshot()->epoch;
        CameraSampleTimeMapper ma(a,24000,48000,epoch,*a.snapshot()), mb(b,24000,48000,epoch,*b.snapshot());
        require(ma.map(first)==4900000 && mb.map(second)==4700000, "Separate 10/30ms delays subtract once from capture time");
        require(ma.now(20000000)==5000000 && mb.now(20000000)==5000000, "Lcam never shifts deadlines/N0");
        const auto before=a.snapshot()->epoch;
        auto reset=stamp(0,30,21000000); reset.generation=8; b.observe(reset);
        recorder_test::rejects([&]{ mb.map(reset); });
        require(a.snapshot()->epoch==before && ma.map(first)==4900000, "Cam2 restart fails only its old epoch");
    });
    tests.test("Calibration full key includes native mode/rate and ordered output mapping", []
    {
        const auto mode=CameraMode::parse("MJPEG 1920x1080 60/1");
        CalibrationProfile p; p.key=calibrationKey("uvc:A",mode,"uncontrolled","ASIO",48000,256,{2,3});
        p.cameraResidualLatency100ns=100000;
        const auto restored=CalibrationProfile::deserialize(p.serialize()); restored.requireMatch(p.key);
        for (unsigned field=0;field<8;++field)
        {
            auto key=p.key;
            switch(field) { case 0:key.cameraId="uvc:B";break; case 1:key.nativeMode="NV12 1920x1080 60/1";break;
                case 2:key.fps={30,1};break; case 3:key.asioDriver="Other ASIO";break; case 4:key.sampleRate=44100;break;
                case 5:key.bufferSamples=512;break; case 6:key.outputMapping={3,2};break; case 7:key.exposure="locked";break; }
            recorder_test::rejects([&]{ restored.requireMatch(key); });
        }
    });
    tests.test("Common exclusive sample end is preserved for both native rates", []
    {
        for (const unsigned fps : {30u,60u}) for (const auto length : {1LL,48001LL,2880001LL})
        {
            const auto n=TakeController::frameCount(length,48000,{fps,1});
            require(n==(length*fps+47999)/48000, "One common Nstop gives exact ceil frame count");
        }
    });
    tests.test("MJPEG warmup followed by repeated pixel ID keeps encoder timestamps monotonic", []
    {
        VideoSurfacePool pool(1920,1080); auto stats=std::make_shared<CaptureTelemetry>(Rational{30,1});
        probe::FramePatternSource::Config config{2,30,1,CaptureSubtype::mjpeg}; config.colourDevice=ColourDevice::warpForTests;
        probe::FramePatternSource source(config,pool,stats,{});
        CaptureFrameDecoder decoder(source.mode(),1,ColourDevice::warpForTests); VideoSurface decoded; decoded.prepare(1920,1080);
        for (const auto id : {1u,1u,2u,1u})
        {
            std::vector<std::uint8_t> bytes; source.makePacket(id,bytes); FrameStamp s;
            decoder.decodeBytes(bytes.data(),bytes.size(),decoded,s);
            const auto pixel=probe::readPattern(decoded.y(),1920,1920,1080);
            require(pixel && pixel->camera==2 && pixel->frame==id, "Real JPEG decode preserves warmup/live pixel IDs");
        }
    });
    return tests.result("mixed-fps");
}
