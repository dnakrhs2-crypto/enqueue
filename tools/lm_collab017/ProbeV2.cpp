// Headless production MixEngine/CollabSession driver. Never opens an AudioIODevice or a window.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MixEngine.h"
#include "CollabSession.h"
#include "LiveMixSettings.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include "../../livemix/shared/lm_obs_protocol.h"
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <avrt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <new>
#include <stdexcept>

namespace {
using namespace gocue::livemix;
using namespace gocue::livemix::collab;
namespace c = gocue::livemix::collab::contracts;
namespace r = gocue::livemix::recording;
using Object = juce::DynamicObject;
double now() { return juce::Time::getMillisecondCounterHiRes() / 1000.0; }
void pump()
{
    MSG msg;
    for (int i = 0; i < 128 && PeekMessageW (&msg, nullptr, 0, 0, PM_REMOVE); ++i)
    { TranslateMessage (&msg); DispatchMessageW (&msg); }
    Sleep (5);
}
juce::String json (const juce::var& v) { return juce::JSON::toString (v); }
void save (const juce::File& path, const juce::var& v)
{
    // Atomic replacement allows the runner to read while the message thread publishes.
    juce::TemporaryFile tmp (path);
    if (! tmp.getFile().replaceWithText (json (v)) || ! tmp.overwriteTargetFileWithTemporary())
        throw std::runtime_error ("cannot publish " + path.getFullPathName().toStdString());
}
double cpuSeconds()
{
    FILETIME created {}, ended {}, kernel {}, user {};
    if (! GetProcessTimes (GetCurrentProcess(), &created, &ended, &kernel, &user))
        throw std::runtime_error ("GetProcessTimes failed");
    const auto ticks = [] (FILETIME f) { return (uint64_t (f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return double (ticks (kernel) + ticks (user)) / 1.0e7;
}
juce::var recorderJson (const r::Snapshot& s)
{
    auto* o = new Object; juce::var v (o);
    o->setProperty ("state", (int) s.state); o->setProperty ("error", (int) s.error);
    o->setProperty ("errorDetail", juce::String::fromUTF8 (s.errorDetail.c_str()));
    o->setProperty ("capturedFrames", (juce::int64) s.capturedFrames);
    o->setProperty ("collabAtStart", s.collabAtStart); o->setProperty ("fileCount", (int) s.fileCount);
    juce::Array<juce::var> files;
    for (unsigned i = 0; i < s.fileCount; ++i)
    {
        const auto& f = s.files[i]; auto* p = new Object;
        p->setProperty ("track", (int) f.track); p->setProperty ("path", juce::String::fromUTF8 (f.pathUtf8.c_str()));
        p->setProperty ("rate", (int) f.sampleRate); p->setProperty ("part", (int) f.part);
        p->setProperty ("writtenFrames", (juce::int64) f.writtenFrames);
        p->setProperty ("checkpointFrames", (juce::int64) f.checkpointFrames);
        p->setProperty ("clippedSamples", (juce::int64) f.clippedSamples); files.add (juce::var (p));
    }
    o->setProperty ("files", files); return v;
}
class Ring
{
public:
    ~Ring() { if (header) UnmapViewOfFile (header); if (handle) CloseHandle (handle); }
    void open (const juce::String& name)
    {
        handle = OpenFileMappingW (FILE_MAP_READ, FALSE, name.toWideCharPointer());
        if (handle) header = (const lm_obs_ring_header*) MapViewOfFile (handle, FILE_MAP_READ, 0, 0, 0);
        if (! header || header->magic != LM_OBS_MAGIC || header->capacity_frames != LM_OBS_CAPACITY_FRAMES)
            throw std::runtime_error ("isolated OBS ring not available");
    }
    bool latest (float* out, int count)
    {
        const auto epoch = lm_obs_load_acquire (&header->epoch);
        int64_t position = lm_obs_load_acquire (&header->write_frames) - count;
        const auto* pcm = (const float*) ((const uint8_t*) header + header->data_offset);
        return lm_obs_read (header, pcm, epoch, &position, out, (uint32_t) count) == count;
    }
private:
    HANDLE handle = nullptr;
    const lm_obs_ring_header* header = nullptr;
};

class EngineClock : public juce::Thread
{
public:
    EngineClock (MixEngine& e, int hz, int frames, int voice, int seconds, bool echo)
        : Thread ("R9 engine clock"), engine (e), rate (hz), block (frames), identity (voice), reflect (echo),
          input (2, frames), output (4, frames)
    {
        for (auto& b : ringPcm) b.resize ((size_t) block * 2);
        capture.resize ((size_t) rate * (size_t) seconds * 8); // allocate before timed callbacks
        for (unsigned i = 0; i < 2; ++i) rings[i].open (engine.getPeerRingNames()[i]);
        input.clear(); output.clear();
    }
    ~EngineClock() override { stopThread (-1); }
    std::atomic<int64_t> frame { 0 };
    std::atomic<int> lateBlocks { 0 }, ringErrors { 0 };
    void writeCapture (const juce::File& path)
    {
        juce::FileOutputStream file (path);
        if (! file.openedOk() || ! file.setPosition (0) || file.truncate().failed()
            || ! file.write (capture.data(), stored * sizeof (float)))
            throw std::runtime_error ("capture write failed");
        file.flush();
    }
    int64_t captured() const { return (int64_t) (stored / 8); }
private:
    void run() override
    {
        DWORD taskIndex = 0;
        const auto task = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);
        HANDLE timer = CreateWaitableTimerExW (nullptr, nullptr, 2, TIMER_ALL_ACCESS);
        if (! timer) timeBeginPeriod (1);
        double next = now(); const double period = double (block) / rate;
        while (! threadShouldExit())
        {
            next += period; const double wait = next - now();
            if (wait > 0 && timer)
            {
                LARGE_INTEGER due; due.QuadPart = -(LONGLONG) (wait * 1.0e7);
                if (SetWaitableTimer (timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject (timer, 1000);
            }
            else if (wait > 0) Sleep ((DWORD) (wait * 1000));
            else if (wait < -period) { ++lateBlocks; if (wait < -.25) next = now(); }
            render();
        }
        if (timer) CloseHandle (timer); else timeEndPeriod (1);
        if (task) AvRevertMmThreadCharacteristics (task);
    }
    void render()
    {
        constexpr double frequencies[4][2] {{700,1100},{1700,2300},{2900,3100},{3700,3700}};
        const int64_t first = frame.load();
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < block; ++i)
            {
                const auto f = first + i; const auto phase = (int) (f % (rate / 2));
                const double length = std::round (rate * .002);
                double value = .12 * std::sin (juce::MathConstants<double>::twoPi * frequencies[identity][ch] * double (f) / rate);
                if (phase < length) value += .6 * (.5 - .5 * std::cos (juce::MathConstants<double>::twoPi * phase / (length - 1)))
                    * std::sin (juce::MathConstants<double>::twoPi * (6000 + identity * 2000) * phase / rate);
                input.setSample (ch, i, reflect ? output.getSample (ch + 2, i) : (float) value);
            }
        engine.renderBlock (input.getArrayOfReadPointers(), 2, output.getArrayOfWritePointers(), 4, block);
        for (unsigned i = 0; i < 2; ++i)
            if (! rings[i].latest (ringPcm[i].data(), block))
            { ++ringErrors; std::fill (ringPcm[i].begin(), ringPcm[i].end(), 0.0f); }
        if (stored + (size_t) block * 8 <= capture.size())
            for (int i = 0; i < block; ++i)
            {
                for (int ch = 0; ch < 4; ++ch) capture[stored++] = output.getSample (ch, i);
                for (unsigned slot = 0; slot < 2; ++slot)
                    for (int ch = 0; ch < 2; ++ch) capture[stored++] = ringPcm[slot][(size_t) i * 2 + (size_t) ch];
            }
        frame.store (first + block);
    }
    MixEngine& engine;
    int rate, block, identity; bool reflect;
    juce::AudioBuffer<float> input, output;
    std::array<Ring, 2> rings;
    std::array<std::vector<float>, 2> ringPcm;
    std::vector<float> capture;
    size_t stored = 0;
};

juce::var sessionJson (const CollabSession::Snapshot& s)
{
    auto* o = new Object; juce::var v (o);
    o->setProperty ("state", (int) s.state); o->setProperty ("code", s.code);
    o->setProperty ("reason", s.reason); o->setProperty ("message", s.message);
    o->setProperty ("selfId", (int) s.selfId); o->setProperty ("roomMode", (int) s.roomMode);
    o->setProperty ("rosterRevision", (int) s.rosterRevision);
    juce::Array<juce::var> people;
    for (size_t i = 0; i < s.personCount; ++i)
    {
        const auto& person = s.people[i]; auto* p = new Object;
        p->setProperty ("id", (int) person.participantId); p->setProperty ("slot", person.slot);
        p->setProperty ("self", person.isSelf); p->setProperty ("nick", person.nickname);
        p->setProperty ("state", (int) person.state); p->setProperty ("gain", person.monitorVolume);
        people.add (juce::var (p));
    }
    o->setProperty ("people", people); return v;
}

int drive (const juce::File& configFile)
{
    const auto cfg = juce::JSON::parse (configFile);
    const auto output = juce::File (cfg["output"].toString());
    if (! output.isDirectory()) throw std::runtime_error ("runner must create disposable output directory");
    const int rate = cfg.getProperty ("deviceRate", 48000), block = cfg.getProperty ("block", 128);
    const int voice = cfg.getProperty ("voice", 0), maxSeconds = cfg.getProperty ("maxSeconds", 180);
    if ((rate != 44100 && rate != 48000 && rate != 96000) || block < 16 || block > 2048 || voice < 0 || voice > 3
        || maxSeconds < 1 || maxSeconds > 1800) throw std::runtime_error ("invalid clock configuration");
    const bool host = cfg["host"], legacy = cfg["legacy"], reflect = cfg["reflect"];
    const auto nick = cfg["nick"].toString();
    // A missing override must never attach this test to the user's production OBS ring.
    for (const char* key : {"LIVEMIX_OBS_RING", "LIVEMIX_OBS_PEER_RING", "LIVEMIX_OBS_PEER2_RING"})
    {
        const auto name = juce::SystemStats::getEnvironmentVariable (key, {});
        if (! name.startsWith ("Local\\LiveMixR9.") || name.length() > 104)
            throw std::runtime_error ("R9 isolated ring overrides required");
    }
    LiveMixSettings settings (output.getChildFile ("profile"));
    settings.setCollabNick (nick); settings.setCollabRate ((int) cfg.getProperty ("wireRate", 48000));
    settings.setCollabPort ((int) cfg.getProperty ("port", 49731));
    settings.setCollabMonitorVolume (1); settings.setCollabMonitorOn (true);
    settings.setCollabRoute (1); settings.setCollabRouteFirst (2);
    MixEngine engine;
    const auto limit = (juce::int64) cfg.getProperty ("splitBytes", (juce::int64) 0);
    if (limit)
    {
        if (limit < 8192) throw std::runtime_error ("split limit too small");
        // Public R5 TestHooks, replacing the idle Recorder BEFORE prepare/callbacks. No app source patch.
        auto& recorder = engine.getRecorder(); recorder.~Recorder();
        r::Recorder::TestHooks hooks; hooks.wavLimitBytes = (uint64_t) limit;
        new (&recorder) r::Recorder (hooks);
    }
    MixSession mix; mix.addChannel(); mix.channels[0].stereo = true;
    engine.applySession (mix); engine.prepare (rate, block); engine.setObsEnabled (true);
    engine.getCollab().setMonitor (1, true); engine.getCollab().setRoute (CollabAudio::Route::direct, 2);
    EngineClock clock (engine, rate, block, voice, maxSeconds, reflect);
    auto* root = new Object; juce::var result (root);
    root->setProperty ("schema", 2); root->setProperty ("rate", rate); root->setProperty ("block", block);
    root->setProperty ("channels", 8); root->setProperty ("voice", voice); root->setProperty ("nick", nick);
    root->setProperty ("legacy", legacy); root->setProperty ("reflect", reflect);
    root->setProperty ("computer", juce::SystemStats::getComputerName());
    root->setProperty ("cpu", juce::SystemStats::getCpuModel()); root->setProperty ("logicalCpus", juce::SystemStats::getNumCpus());
    juce::Array<juce::var> rings;
    for (const auto& name : engine.getPeerRingNames()) rings.add (name);
    root->setProperty ("rings", rings);
    CollabRoom room; CollabTransport oldTransport;
    std::unique_ptr<CollabSession> session;
    double started = now(), connected = 0;
    if (legacy)
    {
        if (host) throw std::runtime_error ("v1 emulation is a guest; use the original probe for v1 host RTT");
        const bool loopback = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_COLLAB_LOOPBACK", {}) == "1";
        if (oldTransport.open (0, loopback).isNotEmpty()) throw std::runtime_error ("v1 UDP open failed");
        const auto joined = room.join (cfg["code"].toString(), nick, juce::Uuid().toString(), {}, oldTransport.getLocalPort());
        if (! joined.ok()) throw std::runtime_error (joined.error.toStdString());
        engine.getCollab().setWireRate ((uint32_t) (int) cfg.getProperty ("wireRate", 48000));
        engine.getCollab().attach (oldTransport); engine.getCollab().setCollabOpen (true);
        PeerInfo me; me.nickname = nick; me.appVersion = "0.16.0"; me.deviceRate = (uint32_t) rate;
        me.wireRate = engine.getCollab().getWireRate();
        oldTransport.startGuest (joined.session, joined.candidates, me);
    }
    else
    {
        session = std::make_unique<CollabSession> (engine, settings);
        if (host) session->createRoom (nick); else session->joinRoom (cfg["code"].toString(), nick);
    }
    clock.startThread (juce::Thread::Priority::highest);
    int commandId = 0, code = 0; bool stopping = false, invited = false;
    double nextStatus = 0, recCpu = 0, recWall = 0;
    juce::var activeTake; juce::Array<juce::var> takes;
    juce::StringArray accepted;
    const auto finishTake = [&]
    {
        if (auto* take = activeTake.getDynamicObject())
        {
            const double wall = now() - recWall, cpu = cpuSeconds() - recCpu;
            engine.getRecorder().requestStop(); engine.getRecorder().finish();
            take->setProperty ("snapshot", recorderJson (engine.getRecorder().snapshot()));
            take->setProperty ("endFrame", (juce::int64) clock.frame.load());
            take->setProperty ("lateBlocksEnd", clock.lateBlocks.load());
            take->setProperty ("wallSeconds", wall); take->setProperty ("cpuSeconds", cpu);
            take->setProperty ("cpuPercentOneCore", 100 * cpu / wall);
            take->setProperty ("cpuPercentMachine", 100 * cpu / wall / juce::SystemStats::getNumCpus());
            take->setProperty ("writerCpuSeconds", engine.getRecorder().writerCpuSeconds());
            takes.add (activeTake); activeTake = juce::var();
        }
    };
    try
    {
        while (! stopping && now() - started < maxSeconds - 2)
        {
            pump();
            if (now() < nextStatus) continue;
            nextStatus = now() + .10;
            const auto command = juce::JSON::parse (output.getChildFile ("command.json"));
            if (command.isObject() && (int) command["id"] > commandId)
            {
                commandId = (int) command["id"]; const auto action = command["action"].toString();
                if (action == "stop") stopping = true;
                else if (action == "leave") { if (session) session->leave(); else oldTransport.stop(); }
                else if (action == "join" && session) session->joinRoom (command["code"].toString(), nick);
                else if (action == "gain" && session) session->setPersonMonitorVolume ((uint32_t) (int) command["person"], (float) (double) command["gain"]);
                else if (action == "record")
                {
                    if (activeTake.isObject()) throw std::runtime_error ("take already open");
                    const auto label = command["label"].toString();
                    if (label.isEmpty() || label.containsAnyOf ("/\\:.")) throw std::runtime_error ("invalid take label");
                    auto folder = output.getChildFile ("recordings").getChildFile (label); folder.createDirectory();
                    auto* t = new Object; activeTake = juce::var (t);
                    t->setProperty ("label", label); t->setProperty ("directory", folder.getFullPathName());
                    t->setProperty ("startFrameLower", (juce::int64) clock.frame.load());
                    t->setProperty ("format", command["format"]); t->setProperty ("kbps", command["kbps"]);
                    t->setProperty ("lateBlocksStart", clock.lateBlocks.load());
                    r::Options opts; opts.directoryUtf8 = folder.getFullPathName().toStdString();
                    opts.format = command["format"].toString() == "mp3" ? r::Format::mp3 : r::Format::wav;
                    opts.mp3Kbps = (unsigned) (int) command.getProperty ("kbps", 320);
                    recCpu = cpuSeconds(); recWall = now();
                    if (! engine.startRecording (opts)) throw std::runtime_error ("engine startRecording rejected");
                }
                else if (action == "recordStop") finishTake();
                else if (action != "mark") throw std::runtime_error ("unknown command");
            }
            if (session)
            {
                const auto s = session->getSnapshot();
                root->setProperty ("session", sessionJson (s));
                if (! connected && s.state == CollabSession::State::connected) connected = now();
                if (host && (bool) cfg["web"] && ! invited && s.canCreateWebInvite)
                { session->createWebInvite(); invited = true; }
                if (s.webInviteUrl.isNotEmpty())
                    output.getChildFile ("invite.txt").replaceWithText (s.webInviteUrl); // disposable control file only, never logged
                for (size_t i = 0; i < s.webRequestCount; ++i)
                    if (s.webRequests[i].canAccept && ! accepted.contains (s.webRequests[i].requestId))
                    { accepted.add (s.webRequests[i].requestId); session->acceptWebRequest (s.webRequests[i].requestId); }
            }
            else
            {
                const auto stats = oldTransport.getStats(); auto* s = new Object;
                s->setProperty ("state", stats.state == CollabTransport::State::connected ? 5 : 4);
                s->setProperty ("v2", stats.v2); s->setProperty ("received", (juce::int64) stats.received);
                root->setProperty ("session", juce::var (s));
                if (! connected && stats.state == CollabTransport::State::connected) connected = now();
            }
            if (connected) root->setProperty ("connectMs", (connected - started) * 1000);
            const auto frameBeforeSnapshot = clock.frame.load();
            const auto recording = engine.getRecorder().snapshot();
            const auto frameAfterSnapshot = clock.frame.load();
            if (auto* t = activeTake.getDynamicObject())
                if (! t->hasProperty ("startFrameUpper") && recording.state == r::State::recording)
                {
                    // The recorder's accepted-frame counter ties the file origin to this callback clock.
                    // One callback can be between Recorder::push and publishing clock.frame.
                    const auto acceptedFrames = (juce::int64) recording.capturedFrames;
                    t->setProperty ("startFrameLower", std::max ((juce::int64) t->getProperty ("startFrameLower"),
                                       (juce::int64) frameBeforeSnapshot - acceptedFrames - block));
                    t->setProperty ("startFrameUpper", (juce::int64) frameAfterSnapshot - acceptedFrames + block);
                    t->setProperty ("observedCapturedFrames", acceptedFrames);
                }
            root->setProperty ("recorder", recorderJson (recording));
            root->setProperty ("frame", (juce::int64) clock.frame.load());
            root->setProperty ("lateBlocks", clock.lateBlocks.load()); root->setProperty ("ringReadErrors", clock.ringErrors.load());
            root->setProperty ("commandId", commandId); root->setProperty ("takes", takes);
            save (output.getChildFile ("status.json"), result);
        }
        if (! stopping) { code = 3; root->setProperty ("error", "driver deadline"); }
        finishTake();
    }
    catch (const std::exception& e) { code = 4; root->setProperty ("error", juce::String (e.what())); finishTake(); }
    clock.stopThread (-1);
    if (session) session->leave(); else oldTransport.stop();
    session.reset(); engine.getRecorder().finish();
    clock.writeCapture (output.getChildFile ("capture.f32"));
    root->setProperty ("captureFrames", (juce::int64) clock.captured()); root->setProperty ("takes", takes);
    root->setProperty ("lateBlocks", clock.lateBlocks.load()); root->setProperty ("ringReadErrors", clock.ringErrors.load());
    root->setProperty ("exit", code); save (output.getChildFile ("result.json"), result);
    output.getChildFile ("invite.txt").deleteFile();
    return code;
}
int decode (const juce::File& path, const juce::File& out)
{
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (path));
    if (! reader || reader->numChannels != 2) throw std::runtime_error ("cannot decode stereo recording");
    juce::FileOutputStream file (out);
    if (! file.openedOk() || ! file.setPosition (0) || file.truncate().failed())
        throw std::runtime_error ("decode output unavailable");
    juce::AudioBuffer<float> block (2, 4096); std::vector<float> interleaved (8192);
    for (juce::int64 first = 0; first < reader->lengthInSamples; first += 4096)
    {
        const int count = (int) std::min<juce::int64> (4096, reader->lengthInSamples - first);
        if (! reader->read (&block, 0, count, first, true, true)) throw std::runtime_error ("decode read failed");
        for (int i = 0; i < count; ++i) for (int ch = 0; ch < 2; ++ch) interleaved[(size_t) i * 2 + ch] = block.getSample (ch, i);
        if (! file.write (interleaved.data(), (size_t) count * 8)) throw std::runtime_error ("decode write failed");
    }
    file.flush(); auto* o = new Object; juce::var result (o);
    o->setProperty ("frames", reader->lengthInSamples); o->setProperty ("rate", reader->sampleRate);
    o->setProperty ("bits", (int) reader->bitsPerSample); save (out.withFileExtension ("json"), result); return 0;
}
}
int runCollabProbeV2 (int argc, char** argv)
{
    try
    {
        juce::ignoreUnused (argc, argv);
        // D_전체 and nicknames are Unicode even when the Windows ANSI code page is not UTF-8.
        int count = 0; auto** wide = CommandLineToArgvW (GetCommandLineW(), &count);
        if (! wide) throw std::runtime_error ("cannot read Unicode arguments");
        juce::StringArray args;
        for (int i = 0; i < count; ++i) args.add (juce::String (wide[i]));
        LocalFree (wide);
        if (count == 3 && args[1] == "--v2") return drive (juce::File (args[2]));
        if (count == 4 && args[1] == "--decode") return decode (juce::File (args[2]), juce::File (args[3]));
        throw std::runtime_error ("usage: --v2 CONFIG.json | --decode INPUT OUTPUT.f32");
    }
    catch (const std::exception& error) { std::fprintf (stderr, "R9: %s\n", error.what()); return 4; }
}
