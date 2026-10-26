#include "MixEngine.h"
#include "MonitorOutput.h"
#include "CollabTransport.h"
#include "LiveMixCollabTestHelpers.h"
#include "TestGainPlugin.h"
#include "lm_obs_protocol.h"
#include <deque>
#include <thread>
#include <windows.h>

namespace gocue::livemix
{
struct CollabEngineTestAccess
{
    static juce::CriticalSection& graphLock (MixEngine& e) { return e.lock; }
    static MonitorOutput& split (MixEngine& e)
    {
        e.monitor = std::make_unique<MonitorOutput>();
        e.monitor->prepareForTest (48000, 120, 48000, 120);
        e.publishedMonitor.store (e.monitor.get()); e.splitMonitor.store (true); e.stereoOutputsOnly.store (true);
        return *e.monitor;
    }
};
}
namespace gocue::tests
{
using namespace livemix;
using namespace collabtest;
namespace
{
struct RingView
{
    explicit RingView (const juce::String& name)
    {
        handle = OpenFileMappingW (FILE_MAP_READ, FALSE, name.toWideCharPointer());
        if (handle != nullptr) header = static_cast<lm_obs_ring_header*> (MapViewOfFile (handle, FILE_MAP_READ, 0, 0, 0));
    }
    ~RingView() { if (header != nullptr) UnmapViewOfFile (header); if (handle != nullptr) CloseHandle (handle); }
    float last (int ch) const
    {
        const auto frames = lm_obs_load_acquire (&header->write_frames);
        const auto* pcm = reinterpret_cast<const float*> (header + 1);
        return pcm[((frames - 1) % LM_OBS_CAPACITY_FRAMES) * 2 + ch];
    }
    HANDLE handle = nullptr;
    lm_obs_ring_header* header = nullptr;
};
}
class LiveMixCollabEngineTests : public juce::UnitTest
{
public:
    LiveMixCollabEngineTests() : UnitTest ("LiveMix Collab engine", "LiveMix") {}
    void runTest() override
    {
        beginTest ("two engines with delayed/lost packets: master pair, mix-minus and both OBS taps");
        const auto mappingBase = "Local\\LiveMix.CollabTest." + juce::Uuid().toString();
        MixEngine a (mappingBase + ".A"), b (mappingBase + ".B");
        MixSession sa, sb; sa.addChannel(); sb.addChannel(); sb.master.outputFirst = 2;
        a.applySession (sa); b.applySession (sb); a.prepare (48000, 120); b.prepare (48000, 120);
        CollabTransport ta, tb;
        a.getCollab().attach (ta); b.getCollab().attach (tb);
        a.getCollab().setConnected (true); b.getCollab().setConnected (true);
        b.setObsEnabled (true);
        RingView local (mappingBase + ".B"), remote (mappingBase + ".B.Peer");
        expect (local.header != nullptr && remote.header != nullptr);
        if (local.header == nullptr || remote.header == nullptr) return;
        std::array<float, 120> inputA, inputB;
        inputA.fill (0.25f); inputB.fill (-0.125f);
        const float* inputsA[] { inputA.data() }; const float* inputsB[] { inputB.data() };
        juce::AudioBuffer<float> outA (4, 120), outB (4, 120);
        std::deque<Packet> ab, ba;
        uint64_t block = 0, sentA = 0, sentB = 0;
        bool minus = true, clockA = true, clockB = true;
        auto queue = [&] (CollabAudio& audio, std::deque<Packet>& packets, uint32_t sender, uint64_t& next, bool& clock, bool checkMinus, bool silent)
        {
            audio.drain ([&] (const Header& header, const uint8_t* payload)
            {
                clock = clock && header.firstFrame == next; next += 120;
                Packet p; p.header = header; p.header.sender = sender;
                std::copy_n (payload, audioBytes, p.payload.data()); p.arrival = (block + 2) * 2500000ULL;
                if (checkMinus)
                {
                    std::array<float, 240> decoded; unpackPcm24 (payload, decoded.data(), 120);
                    for (float sample : decoded) minus = minus && std::abs (sample - (silent ? 0.0f : -0.125f)) < 1.0e-6f;
                }
                if (header.firstFrame != 200 * 120) packets.push_back (p); // one loss in each direction
            });
        };
        auto render = [&] (bool busy = false)
        {
            auto deliver = [&] (std::deque<Packet>& packets, CollabAudio& audio)
            {
                while (! packets.empty() && packets.front().arrival <= block * 2500000ULL)
                {
                    const auto& p = packets.front(); audio.receive (p.header, p.payload.data(), p.arrival); packets.pop_front();
                }
            };
            deliver (ab, b.getCollab()); deliver (ba, a.getCollab());
            a.renderBlock (inputsA, 1, outA.getArrayOfWritePointers(), 4, 120);
            b.renderBlock (inputsB, 1, outB.getArrayOfWritePointers(), 4, 120);
            queue (a.getCollab(), ab, 1, sentA, clockA, false, false);
            queue (b.getCollab(), ba, 2, sentB, clockB, true, busy);
            ++block;
        };
        for (int n = 0; n < 1600; ++n) render();
        expect (minus && clockA && clockB);
        expectWithinAbsoluteError (outB.getSample (2, 119), 0.125f, 1.0e-5f);
        expectWithinAbsoluteError (outB.getSample (3, 119), 0.125f, 1.0e-5f);
        expectEquals (outB.getMagnitude (0, 0, 120), 0.0f);
        expectWithinAbsoluteError (outA.getSample (0, 119), 0.125f, 1.0e-5f);
        expectWithinAbsoluteError (local.last (0), -0.125f, 1.0e-6f);
        expectWithinAbsoluteError (remote.last (0), 0.25f, 1.0e-5f);
        expect (b.getCollab().getStats().lost >= 1);

        beginTest ("headphone volume and OFF ramp affect only monitor, peer OBS stays before gain");
        b.getCollab().readRemoteMeter();
        for (bool on : { true, false })
        {
            b.getCollab().setMonitor (on ? 0.0f : 1.0f, on);
            for (int n = 0; n < 20; ++n) render();
            expectWithinAbsoluteError (outB.getSample (2, 119), -0.125f, 1.0e-6f);
            expectWithinAbsoluteError (remote.last (0), 0.25f, 1.0e-5f);
            const auto peaks = b.getCollab().readRemoteMeter();
            expectWithinAbsoluteError (peaks.left, 0.25f, 1.0e-5f);
            expectWithinAbsoluteError (peaks.right, 0.25f, 1.0e-5f);
            const auto empty = b.getCollab().readRemoteMeter();
            expectEquals (empty.left, 0.0f); expectEquals (empty.right, 0.0f);
        }
        b.getCollab().setMonitor (1.0f, true);
        render(); const float half = outB.getSample (2, 119); render();
        expectWithinAbsoluteError (half, 0.0f, 1.0e-5f);
        expectWithinAbsoluteError (outB.getSample (2, 119), 0.125f, 1.0e-5f);

        beginTest ("partner route: a direct pair of its own, or no output when heard in OBS, the peer ring in every route");
        b.getCollab().setRoute (CollabAudio::Route::direct, 0);
        for (int n = 0; n < 20; ++n) render();
        expectWithinAbsoluteError (outB.getSample (0, 119), 0.25f, 1.0e-5f);
        expectWithinAbsoluteError (outB.getSample (1, 119), 0.25f, 1.0e-5f);
        expectWithinAbsoluteError (outB.getSample (2, 119), -0.125f, 1.0e-6f);
        expectWithinAbsoluteError (local.last (0), -0.125f, 1.0e-6f);   // the master bus and its OBS source stay without the partner
        b.getCollab().setRoute (CollabAudio::Route::obs, 0);
        for (int n = 0; n < 20; ++n) render();
        expectEquals (outB.getMagnitude (0, 0, 120), 0.0f);
        expectWithinAbsoluteError (outB.getSample (2, 119), -0.125f, 1.0e-6f);
        expectWithinAbsoluteError (remote.last (0), 0.25f, 1.0e-5f);
        b.getCollab().setRoute (CollabAudio::Route::master, 0);
        for (int n = 0; n < 20; ++n) render();
        expectWithinAbsoluteError (outB.getSample (2, 119), 0.125f, 1.0e-5f);
        expectEquals (outB.getMagnitude (0, 0, 120), 0.0f);

        beginTest ("busy graph still plays remote and sends local silence with uninterrupted packet clock");
        {
            const juce::ScopedLock held (CollabEngineTestAccess::graphLock (b));
            std::thread audio ([&] { for (int n = 0; n < 10; ++n) render (true); }); audio.join();
        }
        expect (minus && clockA && clockB);
        expectWithinAbsoluteError (outB.getSample (2, 119), 0.25f, 1.0e-5f);
        expectEquals (local.last (0), 0.0f);
        expectWithinAbsoluteError (remote.last (0), 0.25f, 1.0e-5f);
        expectEquals (lm_obs_load_acquire (&local.header->write_frames), (int64_t) (block * 120));
        expectEquals (lm_obs_load_acquire (&remote.header->write_frames), (int64_t) (block * 120));

        beginTest ("split monitor receives remote on normal and busy graph blocks");
        auto& monitor = CollabEngineTestAccess::split (b);
        std::array<float, 240> monitorOut {};
        for (int n = 0; n < 100; ++n) { render(); monitor.pullForTest (monitorOut.data(), 120); }
        expectWithinAbsoluteError (monitorOut.back(), 0.125f, 1.0e-5f);
        expectEquals (outB.getMagnitude (0, 120), 0.0f);
        {
            const juce::ScopedLock held (CollabEngineTestAccess::graphLock (b));
            std::thread audio ([&]
            {
                for (int n = 0; n < 100; ++n) { render (true); monitor.pullForTest (monitorOut.data(), 120); }
            }); audio.join();
        }
        expectWithinAbsoluteError (monitorOut.back(), 0.25f, 1.0e-5f); expect (minus && clockB);
        b.setObsEnabled (false); expect (! b.getObsSender().isEnabled() && ! b.getCollab().getPeerObs().isEnabled());

        beginTest ("without collab the pre-existing stereo master path is bit-identical, peer ring gets silence");
        {
            MixEngine engine (mappingBase + ".dry"); MixSession session; session.addChannel();
            session.channels[0].stereo = true; session.master.outputFirst = 2;
            engine.applySession (session); engine.prepare (48000, 120);
            engine.getMasterChain().addPlugin (std::make_unique<TestGainPlugin> (0.5f)); engine.setObsEnabled (true);
            RingView peerRing (mappingBase + ".dry.Peer"); expect (peerRing.header != nullptr);
            std::array<float, 120> l, r;
            for (int n = 0; n < 120; ++n) { l[(size_t) n] = (n - 60) / 128.0f; r[(size_t) n] = -l[(size_t) n] * 0.5f; }
            const float* ins[] { l.data(), r.data() };
            juce::AudioBuffer<float> output (4, 120); engine.renderBlock (ins, 2, output.getArrayOfWritePointers(), 4, 120);
            bool exact = true;
            for (int n = 0; n < 120; ++n) exact = exact && output.getSample (2, n) == l[(size_t) n] * 0.5f && output.getSample (3, n) == r[(size_t) n] * 0.5f;
            expect (exact); expectEquals (output.getMagnitude (0, 0, 120), 0.0f);
            if (peerRing.header != nullptr)
            {
                expectEquals (peerRing.last (0), 0.0f);
                expectEquals (lm_obs_load_acquire (&peerRing.header->write_frames), int64_t (120));
            }
        }
        beginTest ("R8 resolves two independent peer overrides and empty overrides from the base ring");
        {
            struct Environment
            {
                const char* name;
                juce::String old;
                explicit Environment (const char* key) : name (key), old (juce::SystemStats::getEnvironmentVariable (key, {})) {}
                ~Environment() { SetEnvironmentVariableA (name, old.isEmpty() ? nullptr : old.toRawUTF8()); }
                void set (const juce::String& value) { SetEnvironmentVariableA (name, value.isEmpty() ? nullptr : value.toRawUTF8()); }
            } base ("LIVEMIX_OBS_RING"), peer1 ("LIVEMIX_OBS_PEER_RING"), peer2 ("LIVEMIX_OBS_PEER2_RING");
            base.set (mappingBase + ".Env"); peer1.set (mappingBase + ".ExplicitOne"); peer2.set (mappingBase + ".ExplicitTwo");
            {
                MixEngine engine;
                expectEquals (engine.getPeerRingNames()[0], mappingBase + ".ExplicitOne");
                expectEquals (engine.getPeerRingNames()[1], mappingBase + ".ExplicitTwo");
                engine.setObsEnabled (true);
                RingView one (engine.getPeerRingNames()[0]), two (engine.getPeerRingNames()[1]);
                expect (one.header != nullptr && two.header != nullptr);
            }
            peer1.set ({}); peer2.set ({});
            MixEngine engine;
            expectEquals (engine.getPeerRingNames()[0], mappingBase + ".Env.Peer");
            expectEquals (engine.getPeerRingNames()[1], mappingBase + ".Env.Peer2");
        }
    }
};
static LiveMixCollabEngineTests liveMixCollabEngineTests;
}
