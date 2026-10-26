#include "MixEngine.h"
#include "LiveMixCollabTestHelpers.h"
#include "livemix017/r5/RecorderChecks.h"
#include <thread>

namespace gocue::livemix
{
struct RecordingEngineTestAccess
{
    static juce::CriticalSection& graphLock (MixEngine& engine) { return engine.lock; }
};
}
namespace gocue::tests
{
using namespace livemix;
using namespace collabtest;
namespace rc = livemix::recording::checks;
namespace c = livemix::collab::contracts;

class LiveMixRecordingEngineTests : public juce::UnitTest
{
public:
    LiveMixRecordingEngineTests() : UnitTest ("LiveMix Recorder engine integration", "LiveMix") {}
    void runTest() override
    {
        const auto report = [this] (const rc::Result& result)
        {
            beginTest (juce::String (result.name));
            for (const auto& assertion : result.assertions) expect (assertion.first, juce::String (assertion.second));
        };
        rc::run (report, "master -> two sources -> recorder -> monitor, including busy graph and shutdown", [&] (rc::Result& result)
        {
            rc::Temp folder;
            MixEngine engine ("Local\\LiveMixR8Record-" + juce::Uuid().toString());
            MixSession mix; mix.addChannel(); engine.applySession (mix); engine.prepare (48000, 120);
            auto roster = rosterFor ({ 1, 2, 3 }, 1, 3); engine.getCollab().applyRoster (roster, 1);
            std::array<recording::TrackIdentity, 3> identities {{ { 1, c::ParticipantKind::native, "Host" },
                { 2, c::ParticipantKind::native, "Guest" }, { 3, c::ParticipantKind::web, "Web" } }};
            engine.setRecordingContext (true, identities);
            engine.getCollab().setMonitor (0.0f, false);
            juce::AudioBuffer<float> output (2, 120);
            std::array<float, 120> input; input.fill (0.125f); const float* inputs[] { input.data() };
            uint64_t number = 0;
            const auto render = [&]
            {
                receiveTone (engine.getCollab(), 2, number, 0.25f);
                receiveTone (engine.getCollab(), 3, number, -0.0625f, true);
                engine.renderBlock (inputs, 1, output.getArrayOfWritePointers(), 2, 120); ++number;
            };
            for (int i = 0; i < 100; ++i) render();
            result.require (engine.startRecording ({ recording::Format::wav, 320, folder.path.getFullPathName().toStdString() }), "start accepted");
            auto& recorder = engine.getRecorder();
            result.require (rc::waitFor ([&] { return recorder.snapshot().state == recording::State::recording; }), "recording began");
            result.require (recorder.snapshot().collabAtStart && recorder.snapshot().fileCount == 4, "A/B/C/D opened from room context");
            for (int i = 0; i < 20; ++i) render();
            result.require (std::abs (output.getSample (0, 119) - 0.125f) < 1e-6f, "headphones contain master only");
            {
                const juce::ScopedLock lock (RecordingEngineTestAccess::graphLock (engine));
                std::thread callback ([&] { for (int i = 0; i < 20; ++i) render(); }); callback.join();
            }
            engine.shutdown();
            result.require (recorder.snapshot().state == recording::State::idle, "shutdown joined and closed recorder");
            result.require (recorder.snapshot().capturedFrames == 4800, "one recording push per rendered frame on both paths");
            auto a = rc::wave (result, rc::named (folder.path, "A_Host.wav"));
            auto b = rc::wave (result, rc::named (folder.path, "B_Guest.wav"));
            auto cc = rc::wave (result, rc::named (folder.path, "C_Web.wav"));
            auto d = rc::wave (result, rc::named (folder.path, juce::String::fromUTF8 ("D_전체.wav")));
            result.require (a.left.size() == 4800 && b.left.size() == 4800 && cc.left.size() == 4800 && d.left.size() == 4800, "all files aligned");
            bool same = true, raw = true;
            for (size_t i = 0; i < a.left.size(); ++i)
            {
                same &= d.left[i] == a.left[i] + b.left[i] + cc.left[i];
                raw &= a.left[i] == (i < 2400 ? rc::q24 (0.125f) : 0)
                    && std::abs (b.left[i] - rc::q24 (0.25f)) <= 2 && std::abs (cc.left[i] - rc::q24 (-0.0625f)) <= 2;
            }
            result.require (same && raw, "recording uses pre-monitor sources; busy graph master is silent");
        });
        rc::run (report, "waiting room opens four tracks; new occupant names the seat; device rate starts next part", [&] (rc::Result& result)
        {
            rc::Temp folder; MixEngine engine ("Local\\LiveMixR8Seat-" + juce::Uuid().toString());
            engine.prepare (48000, 120);
            std::array<recording::TrackIdentity, 3> identities {}; identities[0] = { 1, c::ParticipantKind::native, "Host" };
            engine.setRecordingContext (true, identities);
            result.require (engine.startRecording ({ recording::Format::wav, 320, folder.path.getFullPathName().toStdString() }), "start waiting room");
            auto& recorder = engine.getRecorder();
            result.require (rc::waitFor ([&] { return recorder.snapshot().state == recording::State::recording; }), "started");
            juce::AudioBuffer<float> output (2, 120);
            for (int i = 0; i < 10; ++i) engine.renderBlock (nullptr, 0, output.getArrayOfWritePointers(), 2, 120);
            engine.getCollab().applyRoster (rosterFor ({ 1, 2 }), 1);
            identities[1] = { 2, c::ParticipantKind::native, "Late" }; engine.setRecordingContext (true, identities);
            for (int i = 0; i < 30; ++i)
            {
                receiveTone (engine.getCollab(), 2, (uint64_t) i, 0.25f);
                engine.renderBlock (nullptr, 0, output.getArrayOfWritePointers(), 2, 120);
            }
            engine.prepare (44100, 120); // offline callback is stopped, the same boundary used by a device restart
            for (int i = 0; i < 10; ++i) engine.renderBlock (nullptr, 0, output.getArrayOfWritePointers(), 2, 120);
            engine.shutdown();
            auto first = rc::wave (result, rc::named (folder.path, "B_Late.wav"));
            auto second = rc::wave (result, rc::named (folder.path, "B_Late_2.wav"));
            result.require (first.rate == 48000 && second.rate == 44100, "prepareFormat splits at the new rate");
            result.require (first.left.size() == 4800 && second.left.size() == 1200, "callback lengths remain exact across formats");
            result.require (rc::paths (folder.path).size() == 6, "empty C removed in both parts, A/B/D retained");
        });
        rc::run (report, "solo take remains one summed file when collaboration starts", [&] (rc::Result& result)
        {
            rc::Temp folder; MixEngine engine ("Local\\LiveMixR8Solo-" + juce::Uuid().toString()); engine.prepare (48000, 120);
            result.require (engine.startRecording ({ recording::Format::wav, 320, folder.path.getFullPathName().toStdString() }), "solo start");
            result.require (rc::waitFor ([&] { return engine.getRecorder().snapshot().state == recording::State::recording; }), "solo recording");
            engine.getCollab().applyRoster (rosterFor ({ 1, 2 }), 1);
            engine.setRecordingContext (true, {});
            juce::AudioBuffer<float> output (2, 120);
            for (uint64_t i = 0; i < 80; ++i)
            {
                receiveTone (engine.getCollab(), 2, i, 0.25f);
                engine.renderBlock (nullptr, 0, output.getArrayOfWritePointers(), 2, 120);
            }
            engine.shutdown(); const auto files = rc::paths (folder.path);
            result.require (files.size() == 1 && ! engine.getRecorder().snapshot().collabAtStart, "file roles frozen at start");
            const auto wave = rc::wave (result, files.front());
            result.require (wave.left.size() == 9600 && std::abs (wave.left.back() - rc::q24 (0.25f)) <= 2, "solo file includes later guest");
        });
    }
};
static LiveMixRecordingEngineTests liveMixRecordingEngineTests;
}
