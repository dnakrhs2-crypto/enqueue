#include "TestSupport.h"
#include "record/TakeController.h"
#include "media/ThumbnailCache.h"
#include "CrashFixtures.h"
#include <chrono>
#include <thread>

using namespace gocue::recorder;
using recorder_test::require;
void runDubbingFailureScenario(int);
namespace
{
void ok(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
template<class F> void until(F f)
{
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while (!f()) { require(std::chrono::steady_clock::now()<end,"Integration worker deadline"); Sleep(1); }
}
struct Lane
{
    std::atomic<bool> failed{false}, throwFinish{false}, finished{false};
    std::atomic<Sample> n0{-1}, end{-1}, available{0}, latency{0};
    std::atomic<unsigned> offers{0};
};
class Video final : public ITakeVideoStream
{
public:
    explicit Video(std::shared_ptr<Lane> s) : lane(std::move(s)) {}
    void configureClock(const ClockMapper&, std::int64_t latency) override { lane->latency=latency; }
    void prepare(const juce::File& f,NvencProfile,Rational,const AVCodecContext&) override { output=f; }
    void startAt(ClockMapping,Sample n,unsigned,std::function<Sample()>) override { lane->n0=n; }
    void offer(const VideoSurface&) noexcept override { if (!lane->failed) ++lane->offers; }
    void audioPacket(const AVPacket&) override {}
    bool ready() const noexcept override { return true; }
    void sourceFailed(Sample s) noexcept override { if (!lane->failed.exchange(true)) lane->available=s; }
    void endAt(Sample s) noexcept override { lane->end=s; if (!lane->failed) lane->available=s; }
    void audioDone() noexcept override {}
    void finish() override
    {
        lane->finished=true; if (lane->throwFinish) throw std::runtime_error("Injected camera finalizer failure");
        require(output.replaceWithText("Explicit lifecycle double; not MP4"),"Write lifecycle double");
    }
    bool failed() const noexcept override { return lane->failed; }
    Sample availableSamples() const noexcept override { return lane->available; }
    bool thumbnailReady() const noexcept override { return false; }
    juce::var report() const override { auto v=jsonObject(); jsonSet(v,"source","lifecycle double; no encoded-media claim"); return v; }
private:
    std::shared_ptr<Lane> lane; juce::File output;
};
struct Fixture
{
    RecorderDocument doc; RecorderAudioEngine audio;
    std::array<std::shared_ptr<Lane>,2> lanes{std::make_shared<Lane>(),std::make_shared<Lane>()};
    unsigned factory=0; TakeController take{doc,audio,[this]{return std::make_unique<Video>(lanes.at(factory++%2));}};
    TakeController::Config config; Sample position=0; std::int64_t base=qpcNow(); std::uint64_t sequence=0;
    bool mics = true;
    explicit Fixture(bool microphones=true)
    {
        mics = microphones;
        std::array<int,8> map; map.fill(-1); if(microphones)map[0]=0;
        ok(audio.setInputMap(map)); ok(audio.openSynthetic(8000,80,1,2)); if(microphones)ok(audio.arm(0,true));
        doc.newProject("Round 25+27",8000,{60,1});
        config.projectDirectory=crashFixture::directory("dual-integration"); config.synthetic=true;
        config.cameraSymbolicLink="synthetic:cam1"; config.cameraMode=CameraMode::parse("MJPEG 1920x1080 60/1"); config.cameraGeneration=11;
        config.camera2.enabled=config.camera2.synthetic=true; config.camera2.symbolicLink="synthetic:cam2";
        config.camera2.mode=CameraMode::parse("NV12 1920x1080 30/1"); config.camera2.generation=22; config.outputMapping={0,1};
        for(unsigned i=0;i<2;++i)
        {
            CalibrationProfile p; p.key=calibrationKey(i?config.camera2.symbolicLink:config.cameraSymbolicLink,
                i?config.camera2.mode:config.cameraMode,"uncontrolled",audio.deviceInfo().name.toStdString(),8000,80,{0,1},audio.calibrationInputMapping());
            p.cameraResidualLatency100ns=i?300000:100000; config.calibration[i]=p;
        }
        for(int i=0;i<30;++i)feed(); until([&]{return audio.clockReady();});
    }
    void feed()
    {
        std::array<std::uint8_t,240> pcm{}; std::array<float,80> in{},l{},r{};
        for(unsigned i=0;i<80;++i)WavTrackWriter::packPcm24(int(position+i+1234),pcm.data()+i*3);
        NativeInputView view{pcm.data(),0,0,nativeFormatForAsio(17)}; const float* inputs[]{in.data()}; float* outputs[]{l.data(),r.data()};
        BlockStamp s{}; s.flags=samplePositionValid; s.sequence=sequence++; s.samplePosition=position; s.sampleRate=8000; s.numSamples=80;
        s.callbackQpc=base+rescaleRound(position,qpcFrequency(),8000); audio.processBlock(s,mics?&view:nullptr,mics?1:0,inputs,outputs,2); position+=80;
    }
    Sample begin()
    {
        ok(take.prepare(config)); until([&]{take.tick();return take.state()==TakeController::State::armed;});
        const auto n0=position+81; ok(take.start(n0)); until([&]{return audio.startCommitted();});
        until([&]{if(audio.startSample()>=0)return true;feed();take.tick();require(audio.error()==RecorderAudioEngine::Error::none,"Synthetic callback failed before N0");return false;}); return n0;
    }
    void finish(Sample nstop)
    {
        ok(take.stop(nstop)); until([&]{if(audio.stopSample()>=0)return true;feed();take.tick();require(audio.error()==RecorderAudioEngine::Error::none,"Synthetic callback failed before Nstop");return false;});
        until([&]{take.tick();return take.state()==TakeController::State::done||take.state()==TakeController::State::partialFailure;});
    }
};
}
int runDualTakeIntegrationTests()
{
    recorder_test::Suite tests;
    tests.test("Product profiles reach both streams and reserve identical non-grid boundaries",[]
    {
        Fixture f;const auto n=f.begin(); f.finish(n+1601);
        require(f.lanes[0]->latency==100000&&f.lanes[1]->latency==300000,"Independent Lcam reaches production seam");
        require(f.lanes[0]->n0==n&&f.lanes[1]->n0==n&&f.lanes[0]->end==1601&&f.lanes[1]->end==1601,"Common exact N0/Nstop");
        const auto& t=f.doc.getProject().media->takes.front(); require(t.capture.cameraOffsetSamples[0]==80&&t.capture.cameraOffsetSamples[1]==240,"Capture snapshot preserves both applied offsets");
        require(bool(f.take.report()["cameras"][1]["calibrationKeyMatched"]),"Full-key match reported");
    });
    tests.test("Changed output order rejects old calibration before assets or journal are created",[]
    {
        Fixture f;f.config.outputMapping={1,0};require(f.take.prepare(f.config).failed(),"Wrong output profile rejected");
        require(f.doc.getProject().media->takes.empty()&&!f.config.projectDirectory.getChildFile("journal").exists(),"No side effects for rejected calibration");
    });
    for(unsigned bad:{0u,1u})tests.test("One finalizer exception still finishes the healthy lane and raw input",[bad]
    {
        Fixture f;const auto n=f.begin(); f.lanes[bad]->throwFinish=true;f.finish(n+1601);
        const auto& p=f.doc.getProject();const auto& t=p.media->takes.front();
        const auto good=p.media->findAsset(bad?t.cam1AssetId:t.cam2AssetId);
        require(f.lanes[0]->finished&&f.lanes[1]->finished&&f.take.state()==TakeController::State::partialFailure,"Both joins complete independently");
        require(good->logicalLength==1601&&good->gaps.empty()&&p.media->findAsset(t.microphoneAssetIds[0])->gaps.empty(),"Healthy media keeps full common end");
    });
    tests.test("Cam2 explicit discontinuity and stale reconnection cannot extend its take",[]
    {
        Fixture f;const auto n=f.begin();VideoSurface frame;frame.stamp.generation=22;f.take.offer(1,frame);
        f.take.cameraDiscontinuity(1,22);frame.stamp.generation=23;f.take.offer(1,frame);f.take.tick();
        require(f.take.state()==TakeController::State::recording&&f.audio.error()==RecorderAudioEngine::Error::none,"ASIO and healthy camera continue");
        f.finish(n+1601);const auto& p=f.doc.getProject();const auto& t=p.media->takes.front();
        require(f.lanes[1]->offers==1&&p.media->findAsset(t.cam2AssetId)->gaps.size()==1&&p.media->findAsset(t.cam1AssetId)->gaps.empty(),"New generation is discarded; only cam2 gap");
        require(Sample(f.take.report()["cameras"][1]["staleOffers"])==1,"Stale generation report");
    });
    tests.test("Single camera with zero microphones creates no cam2 or WAV asset",[]
    {
        Fixture f(false);f.config.camera2.enabled=false;const auto n=f.begin();f.finish(n+1601);
        const auto& t=f.doc.getProject().media->takes.front();require(t.cam2AssetId.isEmpty()&&t.microphoneAssetIds.empty()&&t.state==TakeState::complete,"Optional paths remain absent");
    });
    tests.test("Recovery does not promote CFR padding to logical duration or truncate healthy lanes",[]
    {
        const auto root=crashFixture::directory("dual-padding");RecorderProject initial;RecoveryScanner::writeCheckpoint(root.getChildFile("project.recorder"),initial);
        auto c=crashFixture::wavConfig(root,1);const auto prefix="media/takes/"+c.takeId.toDashedString()+"/";
        for(int i=1;i<=2;++i)c.additionalFiles.push_back({juce::Uuid().toDashedString(),prefix+"cam"+juce::String(i)+".recording.mp4",prefix+"cam"+juce::String(i)+".mp4"});
        for(int i=1;i<=2;++i){crashFixture::Camera cam(root.getChildFile(prefix+"cam"+juce::String(i)+".mp4"));for(int j=0;j<(i==1?31:15);++j)cam.frame();cam.finish();}
        WavTrackWriter wav(c);ok(wav.start());
        for(unsigned at=0;at<48001;at+=1600)crashFixture::push(wav,at,std::min(1600u,48001-at),1);
        ok(wav.stop(48001,juce::Uuid()));
        const auto hashes=crashFixture::hashes(root,true);RecoveryReport r;ok(RecoveryScanner().run(root,r));
        const auto* t=r.project.media->findTake(c.takeId.toString());require(t&&t->logicalLength==48001,"MP4 ceiling cannot extend exclusive Nstop");
        require(r.project.media->findAsset(t->cam1AssetId)->gaps.empty()&&r.project.media->findAsset(t->microphoneAssetIds[0])->gaps.empty(),"Healthy cam1/WAV stay at exact Nstop");
        const auto* b=r.project.media->findAsset(t->cam2AssetId);require(b->gaps.size()==1&&b->gaps[0].start==24000&&b->gaps[0].length==24001,"Only cam2 short range is a gap");
        require(hashes==crashFixture::hashes(root,true),"Recovery never changes original bytes");RecoveryReport again;ok(RecoveryScanner().run(root,again));require(again.changedTakes==0,"Idempotent recovered generation");
    });
    tests.test("Two-camera dubbing failure keeps common Ostop and other camera",[]{runDubbingFailureScenario(5);});
    tests.test("Background index work pauses until recording is released",[]
    {
        ThumbnailCache worker;worker.setRecording(true);std::atomic<bool> ran{false};
        require(worker.enqueue("two-camera-index",[&](const auto& yield){if(!yield())ran=true;}),"Background task queued");
        Sleep(20);require(!ran&&!worker.mayRun(),"Derived work suspended during capture");worker.setRecording(false);until([&]{return ran.load();});
    });
    return tests.result("partial-camera-take");
}
