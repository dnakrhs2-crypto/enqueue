#include "CollabAudio.h"
#include "CollabTransport.h"
#include "LiveMixCollabTestHelpers.h"
#include "lm_obs_protocol.h"
#include <chrono>
#include <thread>
#include <windows.h>
#if defined (_DEBUG)
#include <crtdbg.h>
#endif

namespace gocue::livemix::collab
{
struct CollabSourcesTestAccess
{
    static juce::CriticalSection& lifecycle (CollabAudio& audio) { return audio.lifecycle; }
};
}

namespace gocue::tests
{
using namespace collabtest;
namespace c = livemix::collab::contracts;
namespace
{
constexpr uint64_t session = 0x1020304050607080ULL;
struct SourcePacket
{
    c::HeaderV2 header;
    std::array<uint8_t, audioBytes> payload {};
    uint64_t arrival = 0;
};
SourcePacket sourcePacket (uint32_t id, bool web, uint64_t number, float l = 0.25f, float r = -0.125f,
                           double hzL = 0, double hzR = 0, uint32_t epoch = 1, uint32_t revision = 1,
                           uint32_t rate = 48000)
{
    SourcePacket p;
    auto& h = p.header;
    h.session = session; h.origin = id; h.linkSender = 1; h.seq = (uint32_t) number;
    h.streamEpoch = epoch; h.rosterRevision = revision; h.firstFrame = number * framesPerPacket;
    h.frames = framesPerPacket; h.payloadBytes = web ? (uint16_t) c::monoPcm16Bytes : audioBytes;
    h.flags = web ? c::Flags::monoPcm16 : c::Flags::stereoPcm24;
    std::array<float, framesPerPacket> left, right;
    for (int n = 0; n < framesPerPacket; ++n)
    {
        const double t = (h.firstFrame + n) / (double) rate;
        left[(size_t) n] = hzL == 0 ? l : l * (float) std::sin (juce::MathConstants<double>::twoPi * hzL * t);
        right[(size_t) n] = hzR == 0 ? r : r * (float) std::sin (juce::MathConstants<double>::twoPi * hzR * t);
    }
    if (web) packPcm16 (p.payload.data(), left.data(), framesPerPacket);
    else packPcm24 (p.payload.data(), left.data(), right.data(), framesPerPacket);
    p.arrival = (uint64_t) (h.firstFrame * 1.0e9 / rate);
    return p;
}
void receive (CollabAudio& audio, const SourcePacket& p) { audio.receiveV2 (p.header, p.payload.data(), p.arrival); }
RosterSnapshot room (std::initializer_list<uint32_t> ids = { 1, 2, 3 }, uint32_t revision = 1, uint32_t web = 3)
{
    RosterSnapshot r; r.session = session; r.revision = revision;
    for (auto id : ids)
    {
        auto& m = r.members[r.memberCount++]; m.participantId = id;
        m.kind = id == web ? c::ParticipantKind::web : c::ParticipantKind::native;
        m.state = c::ConnectionState::connected; m.capabilities = c::allCapabilities;
        m.nickname = "person " + juce::String (id); m.appVersion = "0.17.0";
    }
    return r;
}
struct RingView
{
    explicit RingView (const juce::String& name)
    {
        handle = OpenFileMappingW (FILE_MAP_READ, FALSE, name.toWideCharPointer());
        if (handle != nullptr) header = static_cast<lm_obs_ring_header*> (MapViewOfFile (handle, FILE_MAP_READ, 0, 0, 0));
    }
    ~RingView() { if (header != nullptr) UnmapViewOfFile (header); if (handle != nullptr) CloseHandle (handle); }
    int64_t frames() const { return lm_obs_load_acquire (&header->write_frames); }
    bool matches (const c::AudioSourceView& source, int count) const
    {
        const auto start = frames() - count;
        const auto* pcm = reinterpret_cast<const float*> (header + 1);
        for (int n = 0; n < count; ++n)
        {
            const auto at = (size_t) ((start + n) % LM_OBS_CAPACITY_FRAMES) * 2;
            if (pcm[at] != (source.left ? source.left[n] : 0.0f)
                || pcm[at + 1] != (source.right ? source.right[n] : 0.0f)) return false;
        }
        return true;
    }
    HANDLE handle = nullptr;
    lm_obs_ring_header* header = nullptr;
};
bool silent (const c::AudioSourceView& source, int frames = 120)
{
    for (int n = 0; n < frames; ++n)
        if ((source.left && source.left[n] != 0.0f) || (source.right && source.right[n] != 0.0f)) return false;
    return true;
}
bool sameStats (const CollabPlayout::Stats& a, const CollabPlayout::Stats& b)
{
    return a.state == b.state && a.targetMs == b.targetMs && a.fillMs == b.fillMs
        && a.correctionPpm == b.correctionPpm && a.lossPercent10s == b.lossPercent10s
        && a.received == b.received && a.lost == b.lost && a.late == b.late && a.underruns == b.underruns;
}
#if defined (_DEBUG)
thread_local bool countAllocations = false;
thread_local unsigned allocationCalls = 0;
int __cdecl allocationHook (int, void*, size_t, int, long, const unsigned char*, int)
{
    if (countAllocations) ++allocationCalls;
    return TRUE;
}
#endif
}

class LiveMixCollabSourcesTests : public juce::UnitTest
{
public:
    LiveMixCollabSourcesTests() : UnitTest ("LiveMix Collab sources stereo, mono and monitor", "LiveMix") {}
    void runTest() override
    {
        beginTest ("two views and OBS rings contain only their person's pre-headphone stereo/mono samples");
        const auto mappingBase = "Local\\LiveMix.R4." + juce::Uuid().toString();
        CollabAudio audio (mappingBase + ".Peer", mappingBase + ".Peer2");
        audio.prepare (48000, 120); audio.applyRoster (room(), 1); audio.setCollabOpen (true);
        RingView ring1 (mappingBase + ".Peer"), ring2 (mappingBase + ".Peer2");
        expect (ring1.header != nullptr && ring2.header != nullptr);
        if (ring1.header == nullptr || ring2.header == nullptr) return;
        expect (&audio.getPeerObs() == &audio.getPeerObs (0));
        std::array<std::vector<float>, 3> captured;
        c::RenderedSources sources;
        uint64_t block = 0;
        bool ringsExact = true;
        const auto render = [&]
        {
            receive (audio, sourcePacket (2, false, block, 0.25f, 0.375f, 700, 1300));
            receive (audio, sourcePacket (3, true, block, 0.125f, 0, 2300));
            ++block; sources = audio.renderSources (120);
            ringsExact = ringsExact && ring1.matches (sources.remote[0], 120) && ring2.matches (sources.remote[1], 120);
        };
        // A web person's buffer starts at 100 ms and steps down to its need over about 64 s, slightly speeding playback
        // meanwhile (0.17.4); the tones are compared after it has settled, one second at 90 s.
        constexpr int blocks = 36400;
        for (int n = 0; n < blocks; ++n)
        {
            render();
            if (n >= blocks - 400)
            {
                captured[0].insert (captured[0].end(), sources.remote[0].left, sources.remote[0].left + 120);
                captured[1].insert (captured[1].end(), sources.remote[0].right, sources.remote[0].right + 120);
                captured[2].insert (captured[2].end(), sources.remote[1].left, sources.remote[1].left + 120);
            }
        }
        expect (ringsExact); expectEquals (sources.frames, 120);
        expectEquals (sources.remote[0].participantId, uint32_t (2)); expectEquals (sources.remote[1].participantId, uint32_t (3));
        expectEquals ((int) sources.remote[0].channels, 2); expectEquals ((int) sources.remote[1].channels, 1);
        expect (sources.remote[0].kind == c::ParticipantKind::native && sources.remote[1].kind == c::ParticipantKind::web);
        expect (sources.remote[0].connected && sources.remote[1].connected);
        expect (sources.remote[0].left != sources.remote[0].right && sources.remote[1].left == sources.remote[1].right);
        // The web person's playout corrects its clock within a few tens of ppm after the step-down, a tenth of a hertz at
        // 2300 Hz; the level is the peak within 1 Hz, while the crosstalk checks below stay at the exact frequencies.
        const auto peak = [] (const std::vector<float>& signal, double hz)
        {
            double best = 0;
            for (double f = hz - 1.0; f <= hz + 1.0; f += 0.05) best = std::max (best, amplitude (signal, 48000, f));
            return best;
        };
        for (int channel = 0; channel < 3; ++channel)
        {
            const double hz[] { 700, 1300, 2300 }, level[] { 0.25, 0.375, 0.125 };
            expectWithinAbsoluteError (peak (captured[(size_t) channel], hz[channel]), level[channel], 0.001);
            for (int other = 0; other < 3; ++other)
                if (other != channel) expectLessThan (amplitude (captured[(size_t) channel], 48000, hz[other]), 0.0001);
        }
        expectEquals (audio.getStats().received, block); expectEquals (audio.getStats (1).received, block);

        beginTest ("seat 1 at 50 percent ramps for 5 ms; seat 2, views and OBS stay at unity");
        std::array<std::array<float, 120>, 4> output {};
        float* outputs[] { output[0].data(), output[1].data(), output[2].data(), output[3].data() };
        const auto checkMix = [&] (int first, float gain1, float gain2)
        {
            bool equal = true;
            for (int n = 0; n < 120; ++n)
            {
                const float expectedL = sources.remote[0].left[n] * gain1 + sources.remote[1].left[n] * gain2;
                const float expectedR = sources.remote[0].right[n] * gain1 + sources.remote[1].right[n] * gain2;
                equal = equal && std::abs (output[(size_t) first][(size_t) n] - expectedL) < 0.000001f
                              && std::abs (output[(size_t) first + 1][(size_t) n] - expectedR) < 0.000001f;
            }
            expect (equal);
        };
        audio.setSlotMonitorVolume (0, 0.5f);
        for (int n = 0; n < 2; ++n)
        {
            render(); output = {};
            const auto written = ring1.frames();
            audio.renderMonitor (sources, outputs, 4, 2);
            expectEquals (ring1.frames(), written); // monitor/recorder consumers never pull or write OBS a second time
            const float gain = n == 0 ? 0.75f : 0.5f;
            expectWithinAbsoluteError (output[2].back(), sources.remote[0].left[119] * gain + sources.remote[1].left[119], 0.00001f);
        }
        render(); output = {}; audio.renderMonitor (sources, outputs, 4, 2); checkMix (2, 0.5f, 1.0f);
        expect (std::all_of (output[0].begin(), output[0].end(), [] (float v) { return v == 0; }));
        expect (ringsExact);

        beginTest ("direct and OBS routes, shared OFF and compatibility volume leave both raw taps running");
        audio.setRoute (CollabAudio::Route::direct, 0);
        render(); output = {}; audio.renderMonitor (sources, outputs, 4, 2); checkMix (0, 0.5f, 1.0f);
        expect (std::all_of (output[2].begin(), output[2].end(), [] (float v) { return v == 0; }));
        audio.setRoute (CollabAudio::Route::obs, 0);
        render(); output = {}; audio.renderMonitor (sources, outputs, 4, 2);
        expect (output == std::array<std::array<float, 120>, 4> {});
        audio.setRoute (CollabAudio::Route::master, 0);
        audio.setMonitor (0.25f, false);
        for (int n = 0; n < 3; ++n) { render(); output = {}; audio.renderMonitor (sources, outputs, 4, 2); }
        expect (output == std::array<std::array<float, 120>, 4> {});
        audio.setMonitor (0.25f, true);
        for (int n = 0; n < 3; ++n) { render(); output = {}; audio.renderMonitor (sources, outputs, 4, 2); }
        checkMix (2, 0.25f, 0.25f); expect (ringsExact);
        expectEquals (ring1.frames(), (int64_t) block * 120); expectEquals (ring2.frames(), ring1.frames());
        audio.setCollabOpen (false); expect (! audio.getPeerObs (0).isEnabled() && ! audio.getPeerObs (1).isEnabled());
        audio.setObsEnabled (true); expect (audio.getPeerObs (0).isEnabled() && audio.getPeerObs (1).isEnabled());
    }
};

class LiveMixCollabSourcesRosterTests : public juce::UnitTest
{
public:
    LiveMixCollabSourcesRosterTests() : UnitTest ("LiveMix Collab sources roster and epochs", "LiveMix") {}
    void runTest() override
    {
        beginTest ("a guest excludes self and initially gives the host seat 1");
        {
            CollabAudio guest; guest.prepare (48000, 120); guest.applyRoster (room(), 2);
            const auto view = guest.renderSources (120);
            expectEquals (view.remote[0].participantId, uint32_t (1)); expectEquals (view.remote[1].participantId, uint32_t (3));
            expect (view.remote[0].left != nullptr && view.remote[1].left == view.remote[1].right);
        }

        beginTest ("roster publication retains the unaffected buffer, stats and current callback view");
        const auto mappingBase = "Local\\LiveMix.R4.roster." + juce::Uuid().toString();
        CollabAudio audio (mappingBase + ".Peer", mappingBase + ".Peer2");
        audio.prepare (48000, 120); audio.applyRoster (room(), 1); audio.setObsEnabled (true);
        RingView ring1 (mappingBase + ".Peer"), ring2 (mappingBase + ".Peer2");
        expect (ring1.header != nullptr && ring2.header != nullptr);
        if (ring1.header == nullptr || ring2.header == nullptr) return;
        const auto ringEpoch = lm_obs_load_acquire (&ring1.header->epoch);
        uint64_t webFrame = 0;
        const auto web = [&] { receive (audio, sourcePacket (3, true, webFrame++, 0.5f)); };
        c::RenderedSources sources;
        for (uint64_t n = 0; n < 800; ++n)
        {
            receive (audio, sourcePacket (2, false, n)); web(); sources = audio.renderSources (120);
        }
        const auto original = audio.getStats (0), survivor = audio.getStats (1);
        auto renamed = room ({ 1, 2, 3 }, 2); renamed.members[1].nickname = "new name";
        audio.applyRoster (renamed, 1);
        expect (sameStats (original, audio.getStats (0)) && sameStats (survivor, audio.getStats (1)));
        expectWithinAbsoluteError (sources.remote[0].left[119], 0.25f, 0.00001f);
        expectWithinAbsoluteError (sources.remote[1].left[119], 0.5f, 0.00001f);
        audio.applyRoster (renamed, 1); audio.applyRoster (room(), 1);
        expect (sameStats (original, audio.getStats (0)) && sameStats (survivor, audio.getStats (1)));

        beginTest ("leaving seat 1 becomes null/silent while seat 2 plays continuously at its old revision");
        audio.applyRoster (room ({ 1, 3 }, 3), 1);
        expect (sameStats (survivor, audio.getStats (1)));
        bool keptPlaying = true, rings = true;
        for (uint64_t n = 800; n < 1600; ++n)
        {
            receive (audio, sourcePacket (2, false, n)); web(); sources = audio.renderSources (120);
            keptPlaying = keptPlaying && sources.remote[0].left == nullptr && sources.remote[0].right == nullptr
                && sources.remote[0].participantId == 0 && sources.remote[1].participantId == 3
                && std::abs (sources.remote[1].left[119] - 0.5f) < 0.00001f;
            rings = rings && ring1.matches ({}, 120) && ring2.matches (sources.remote[1], 120);
        }
        expect (keptPlaying && rings); expect (audio.getStats (0).state == CollabPlayout::State::off);
        expectEquals (audio.getStats (0).received, uint64_t (0));
        expectEquals (audio.getStats (1).underruns, survivor.underruns);

        beginTest ("a new person uses the vacant seat with a fresh queue, leaving the other person's clock untouched");
        // Audio arriving before its roster is discarded, including a valid future origin/epoch.
        receive (audio, sourcePacket (4, false, 0, -0.25f, 0.125f, 0, 0, 1, 4));
        auto before = audio.getStats (1);
        audio.applyRoster (room ({ 1, 3, 4 }, 4), 1);
        expect (sameStats (before, audio.getStats (1)));
        expectEquals (audio.getStats (0).received, uint64_t (0));
        web(); sources = audio.renderSources (120);
        expectEquals (sources.remote[0].participantId, uint32_t (4));
        expect (sources.remote[0].left != nullptr && silent (sources.remote[0]));
        expectWithinAbsoluteError (sources.remote[1].left[119], 0.5f, 0.00001f);
        for (uint64_t n = 0; n < 800; ++n)
        {
            receive (audio, sourcePacket (4, false, n, -0.25f, 0.125f, 0, 0, 1, 4));
            web(); sources = audio.renderSources (120);
        }
        expectWithinAbsoluteError (sources.remote[0].left[119], -0.25f, 0.00001f);
        expectWithinAbsoluteError (sources.remote[0].right[119], 0.125f, 0.00001f);
        expectEquals (sources.remote[1].participantId, uint32_t (3));

        beginTest ("only the roster can restart a person's epoch; old PCM, PLC and measurements cannot leak into it");
        const auto current = audio.getStats (0);
        receive (audio, sourcePacket (4, false, 0, 0.75f, -0.5f, 0, 0, 2, 4));
        receive (audio, sourcePacket (1, false, 0)); // self never occupies a seat
        expect (sameStats (current, audio.getStats (0)));
        auto restarted = room ({ 1, 3, 4 }, 5); restarted.members[2].streamEpoch = 2;
        before = audio.getStats (1); audio.applyRoster (restarted, 1);
        expect (sameStats (before, audio.getStats (1)));
        expectWithinAbsoluteError (sources.remote[0].left[119], -0.25f, 0.00001f); // still this callback's view
        expectEquals (audio.getStats (0).received, uint64_t (0));
        receive (audio, sourcePacket (4, false, 800, -0.25f, 0.125f, 0, 0, 1, 4));
        expectEquals (audio.getStats (0).received, uint64_t (0));
        web(); sources = audio.renderSources (120);
        expectEquals (sources.remote[0].streamEpoch, uint32_t (2)); expect (silent (sources.remote[0]));
        expect (ring1.matches (sources.remote[0], 120));
        expectWithinAbsoluteError (sources.remote[1].left[119], 0.5f, 0.00001f);
        for (uint64_t n = 0; n < 800; ++n)
        {
            receive (audio, sourcePacket (4, false, n, 0.75f, -0.5f, 0, 0, 2, 5));
            web(); sources = audio.renderSources (120);
        }
        expectWithinAbsoluteError (sources.remote[0].left[119], 0.75f, 0.00001f);
        expectWithinAbsoluteError (sources.remote[0].right[119], -0.5f, 0.00001f);
        expectEquals (audio.getStats (0).received, uint64_t (800)); expectEquals (audio.getStats (0).late, uint64_t (0));
        expectEquals (audio.getStats (1).received, webFrame); expectEquals (audio.getStats (1).underruns, survivor.underruns);
        expectEquals (lm_obs_load_acquire (&ring1.header->epoch), ringEpoch);
        expectEquals (ring1.frames(), ring2.frames());
        audio.applyRoster ({}, 1); sources = audio.renderSources (120);
        expect (sources.remote[0].left == nullptr && sources.remote[1].left == nullptr);
        expect (ring1.matches ({}, 120) && ring2.matches ({}, 120));
    }
};

class LiveMixCollabSourcesClockTests : public juce::UnitTest
{
public:
    LiveMixCollabSourcesClockTests() : UnitTest ("LiveMix Collab sources independent device clocks", "LiveMix") {}
    void runTest() override
    {
        // The web person's buffer first steps down from 100 ms (about 64 s, 0.17.4); its clock is read once that is done.
        beginTest ("native +500 ppm and web -1000 ppm use separate ASRC and loss histories for two and a half simulated minutes");
        CollabAudio audio; audio.prepare (48000, 120); audio.applyRoster (room(), 1);
        const double skew[] { 500, -1000 };
        uint64_t next[] { 0, 0 };
        for (int block = 0; block < 60000; ++block)
        {
            const double now = block * 120.0 / 48000;
            for (int slot = 0; slot < 2; ++slot)
            {
                const double interval = 120.0 / (48000 * (1.0 + skew[slot] * 1.0e-6));
                while (next[slot] * interval <= now + 1.0e-10)
                {
                    auto p = sourcePacket ((uint32_t) slot + 2, slot == 1, next[slot]);
                    p.arrival = (uint64_t) (next[slot] * interval * 1.0e9);
                    if (slot != 0 || next[slot] != 800) receive (audio, p);
                    ++next[slot];
                }
            }
            audio.renderSources (120);
        }
        for (unsigned slot = 0; slot < 2; ++slot)
        {
            const auto stats = audio.getStats (slot);
            expectWithinAbsoluteError (stats.correctionPpm, skew[slot], 80.0);
            expectEquals (stats.underruns, uint64_t (0));
            expectEquals (stats.lost, uint64_t (slot == 0 ? 1 : 0));
            expectWithinAbsoluteError (stats.fillMs, stats.targetMs, 2.0);
        }
    }
};

class LiveMixCollabSourcesLegacyTests : public juce::UnitTest
{
public:
    LiveMixCollabSourcesLegacyTests() : UnitTest ("LiveMix Collab sources v1 compatibility and restart", "LiveMix") {}
    void runTest() override
    {
        beginTest ("legacy renderRemote preserves playout, additive output, pre-routed pairs and disabled seat 2");
        const auto mappingBase = "Local\\LiveMix.R4.legacy." + juce::Uuid().toString();
        CollabAudio audio (mappingBase); audio.setMonitor (0.5f, true); audio.prepare (48000, 120);
        audio.setCollabOpen (true); audio.setConnected (true); audio.setRoute (CollabAudio::Route::direct, 6);
        expect (! audio.getPeerObs (1).isEnabled());
        RingView ring (mappingBase); expect (ring.header != nullptr); if (ring.header == nullptr) return;
        CollabPlayout reference; reference.prepare (48000, 120); reference.rebuffer();
        std::array<float, 120> l {}, r {};
        std::array<std::array<float, 120>, 4> output {};
        float* outputs[] { output[0].data(), output[1].data(), output[2].data(), output[3].data() };
        bool exact = true, raw = true;
        for (uint64_t block = 0; block < 1200; ++block)
        {
            auto p = tone (block, 0.25f, 700, false);
            if (block != 800) { audio.receive (p.header, p.payload.data(), p.arrival); push (reference, p); }
            for (auto& channel : output) channel.fill (0.125f);
            audio.renderRemote (outputs, 4, 2, 120); reference.pull (l.data(), r.data(), 120);
            for (int n = 0; n < 120; ++n)
                exact = exact && output[2][(size_t) n] == 0.125f + l[(size_t) n] * 0.5f
                    && output[3][(size_t) n] == 0.125f + r[(size_t) n] * 0.5f
                    && output[0][(size_t) n] == 0.125f && output[1][(size_t) n] == 0.125f;
            c::AudioSourceView view; view.left = l.data(); view.right = r.data(); raw = raw && ring.matches (view, 120);
        }
        expect (exact && raw); expect (sameStats (audio.getStats(), reference.getStats()));
        beginTest ("the v1 disconnect still releases the last sample over 5 ms, then leaves signed-zero output untouched");
        const float last = l.back(); audio.setConnected (false);
        bool release = true;
        for (int block = 0; block < 3; ++block)
        {
            for (auto& channel : output) channel.fill (-0.0f);
            audio.renderRemote (outputs, 4, 2, 120);
            for (int n = 0; n < 120; ++n)
            {
                const int remaining = std::max (0, 239 - block * 120 - n);
                const float expected = last * ((float) remaining / 240.0f) * 0.5f;
                release = release && output[2][(size_t) n] == expected;
            }
        }
        expect (release); expect (std::signbit (output[2][0]));
        expect (audio.getStats().state == CollabPlayout::State::off);

        beginTest ("a same-rate v2 device restart resets seq/firstFrame, while the existing v1 prepare keeps its clock");
        for (bool v2 : { false, true })
        {
            CollabTransport transport; CollabAudio senderAudio;
            senderAudio.prepare (48000, 120); senderAudio.attach (transport);
            if (v2) senderAudio.applyRoster (room(), 1);
            uint64_t lastFrame = 0; uint32_t sequence = 0;
            const CollabSender::Sink sink = [&] (const Header& h, const uint8_t*) { lastFrame = h.firstFrame; sequence = h.seq; };
            for (int n = 0; n < 800; ++n) { senderAudio.pushLocalSilence (120); senderAudio.drain (sink); }
            expectEquals (lastFrame, uint64_t (799 * 120));
            senderAudio.prepare (48000, 120); senderAudio.pushLocalSilence (120); senderAudio.drain (sink);
            expectEquals (lastFrame, uint64_t (v2 ? 0 : 800 * 120));
            expectEquals (sequence, uint32_t (v2 ? 0 : 800));
        }

        beginTest ("R3 sends exactly one new epoch for both a changed-rate and a same-rate device restart");
        {
            CollabTransport host, guest;
            CollabAudio hostAudio, guestAudio;
            hostAudio.prepare (48000, 120); guestAudio.prepare (48000, 120);
            hostAudio.attach (host); guestAudio.attach (guest);
            const auto roster = room ({ 1, 2 });
            hostAudio.applyRoster (roster, 1); guestAudio.applyRoster (roster, 2);
            const auto hostError = host.open (0, true), guestError = guest.open (0, true);
            expect (hostError.isEmpty() && guestError.isEmpty());
            if (hostError.isNotEmpty() || guestError.isNotEmpty()) return;
            const CollabTransport::Address hostAddress { "127.0.0.1", host.getLocalPort() };
            const CollabTransport::Address guestAddress { "127.0.0.1", guest.getLocalPort() };
            std::atomic<uint32_t> advertisedEpoch { 0 }, receivedEpoch { 0 };
            std::atomic<uint64_t> packetCount { 0 }, latestFrame { 0 };
            std::atomic<bool> packetClock { true };
            CollabTransport::V2Callbacks hostCallbacks, guestCallbacks;
            hostCallbacks.onRoster = [&] (const RosterSnapshot& r)
            {
                if (const auto* member = findMember (r, 1)) advertisedEpoch.store (member->streamEpoch);
            };
            // The production R8 binding queues applyRoster on its control thread. Here the transport trace
            // checks its actual on-wire epoch/clock; the playout/roster binding is exercised above.
            guestCallbacks.onAudio = [&] (const c::HeaderV2& h, const uint8_t*, uint64_t)
            {
                if (h.origin != 1) return;
                if (h.seq != (uint32_t) (h.firstFrame / 120)) packetClock.store (false);
                receivedEpoch.store (h.streamEpoch); latestFrame.store (h.firstFrame); ++packetCount;
            };
            host.configureV2 (1, roster, { { 2, 1, { guestAddress } } }, std::move (hostCallbacks));
            guest.configureV2 (2, roster, { { 1, 1, { hostAddress } } }, std::move (guestCallbacks));
            host.startHost (session, { "host", "0.17.0" });
            guest.startGuest (session, { hostAddress }, { "guest", "0.17.0" });
            for (uint32_t epoch = 1; epoch <= 3; ++epoch)
            {
                if (epoch != 1) hostAudio.prepare (96000, 240); // 48 -> 96 kHz, then a restart at 96 kHz
                const auto before = packetCount.load();
                for (int n = 0; n < 750; ++n)
                {
                    hostAudio.pushLocalSilence (epoch == 1 ? 120 : 240); guestAudio.pushLocalSilence (120);
                    std::this_thread::sleep_for (std::chrono::milliseconds (3));
                }
                expectEquals (advertisedEpoch.load(), epoch); expectEquals (receivedEpoch.load(), epoch);
                expectGreaterThan (packetCount.load() - before, uint64_t (100));
                expectLessThan (latestFrame.load(), uint64_t (750 * 120)); expect (packetClock.load());
            }
            host.stop(); guest.stop();
        }
    }
};

class LiveMixCollabSourcesRealtimeTests : public juce::UnitTest
{
public:
    LiveMixCollabSourcesRealtimeTests() : UnitTest ("LiveMix Collab sources realtime callbacks", "LiveMix") {}
    void runTest() override
    {
        beginTest ("receiveV2/renderSources/renderMonitor run while the lifecycle lock is held elsewhere");
        const auto mappingBase = "Local\\LiveMix.R4.rt." + juce::Uuid().toString();
        CollabAudio audio (mappingBase + ".Peer", mappingBase + ".Peer2");
        audio.prepare (48000, 120); audio.applyRoster (room(), 1); audio.setObsEnabled (true);
        std::array<float, 120> left {}, right {};
        float* outputs[] { left.data(), right.data() };
        bool played = false;
#if defined (_DEBUG)
        // Like the existing ASRC allocation probe, count allocator calls only in the measured code.
        // The CRT hook also sees STL allocations; use Debug for this check, without global new/delete replacements.
        unsigned allocations = 0;
        const auto previousHook = _CrtSetAllocHook (allocationHook);
#endif
        {
            const juce::ScopedLock held (livemix::collab::CollabSourcesTestAccess::lifecycle (audio));
            std::thread callback ([&]
            {
#if defined (_DEBUG)
                allocationCalls = 0; countAllocations = true;
#endif
                for (uint64_t n = 0; n < 1600; ++n)
                {
                    receive (audio, sourcePacket (2, false, n)); receive (audio, sourcePacket (3, true, n, 0.5f));
                    const auto sources = audio.renderSources (120);
                    left.fill (0); right.fill (0); audio.renderMonitor (sources, outputs, 2, 0);
                }
#if defined (_DEBUG)
                countAllocations = false; allocations = allocationCalls;
#endif
                played = std::abs (left.back() - 0.75f) < 0.00001f && std::abs (right.back() - 0.375f) < 0.00001f;
            });
            callback.join();
        }
#if defined (_DEBUG)
        _CrtSetAllocHook (previousHook);
        beginTest ("Debug CRT: 1600 two-person callbacks perform zero allocations, reallocations or frees");
        expectEquals (allocations, 0u);
#endif
        expect (played); expectEquals (audio.getStats (0).received, uint64_t (1600));
        expectEquals (audio.getStats (1).received, uint64_t (1600));
    }
};

static LiveMixCollabSourcesTests liveMixCollabSourcesTests;
static LiveMixCollabSourcesRosterTests liveMixCollabSourcesRosterTests;
static LiveMixCollabSourcesClockTests liveMixCollabSourcesClockTests;
static LiveMixCollabSourcesLegacyTests liveMixCollabSourcesLegacyTests;
static LiveMixCollabSourcesRealtimeTests liveMixCollabSourcesRealtimeTests;
}
