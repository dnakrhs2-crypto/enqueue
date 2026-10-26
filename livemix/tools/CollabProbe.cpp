// LiveMixCollabProbe: the 합방 network path measured with a test signal, without an audio device, a window or a
// person at either PC. Two copies (one hosts, one joins) run LiveMix's own CollabSession, CollabAudio,
// CollabTransport, UPnP mapping and room server; a high-resolution timer thread stands in for the audio device
// (48 kHz, --block frames per callback).
//
// The measuring side sends a 1 kHz tone on the left and a 2 ms marker burst every 500 ms on the right. The
// reflecting side sends back exactly what it hears, so the measuring side times its own markers on one clock:
// loop = its packetising + network + partner's jitter buffer + partner's packetising + network + its jitter buffer.
// Half the loop is a real 합방's one-way path before either PC's own audio interface latency. Both sides watch the
// tone for dropouts (level) and jumps (phase): the reflecting side hears the measuring side's leg, the measuring
// side both legs.
//
//   LiveMixCollabProbe --host [--port 49731] [--rate 48000|44100]   prints "PROBE_CODE nnnn" once the room is open
//   LiveMixCollabProbe --join nnnn
//   options: --role measure|reflect (default: the host measures, the guest reflects)  --seconds 60 (after connecting)
//            --wait 180 (for the partner)  --block 128  --nick NAME  --record FILE.wav (first 20 s heard)
//            --out FILE.json  --log FILE (the per-second lines too, in case the remote session drops)
// Prints "PROBE_SECOND {...}" every second and "PROBE_RESULT {...}" at the end, ASCII only.
// Exit 0 = connected and measured, 2 = this PC cannot host, 3 = no partner or could not connect, 4 = other error.
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include "CollabSession.h"
#include "LiveMixSettings.h"
#include <juce_events/juce_events.h>
#include <windows.h>
#include <avrt.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>

// R9 modes live in the R9-owned tool directory; the original two-PC probe below stays compatible.
int runCollabProbeV2 (int argc, char** argv);

namespace
{
using namespace gocue::livemix;
using namespace gocue::livemix::collab;
using State = CollabSession::State;

constexpr double deviceRate = 48000.0;
constexpr double toneHz = 1000.0;            // exactly 10 cycles per analysis window
constexpr float toneLevel = 0.25f;
constexpr int64_t markerPeriod = 24000;      // 500 ms
constexpr int markerLength = 96;             // 2 ms
constexpr double markerHz = 3000.0;
constexpr float markerLevel = 0.5f, markerThreshold = 0.1f;
constexpr int windowFrames = 480;            // 10 ms
constexpr int64_t settleFrames = 3 * 48000;  // the first 3 s of tone (connect ramp, buffer settling) are not judged
constexpr double twoPi = juce::MathConstants<double>::twoPi;

float markerSample (int n)
{
    const double window = 0.5 - 0.5 * std::cos (twoPi * n / (markerLength - 1));
    return (float) (markerLevel * window * std::sin (twoPi * markerHz * n / deviceRate));
}
int markerCrossing()
{
    for (int n = 0; n < markerLength; ++n)
        if (std::abs (markerSample (n)) > markerThreshold) return n;
    return 0;
}
double tonePhase (int64_t frame) { return twoPi * (double) (frame % 48) / 48.0; }   // 1 kHz at 48 kHz
double wrap (double x)
{
    while (x > juce::MathConstants<double>::pi) x -= twoPi;
    while (x < -juce::MathConstants<double>::pi) x += twoPi;
    return x;
}
double round1 (double x) { return std::round (x * 10.0) / 10.0; }
double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

juce::String toJson (const juce::var& v)
{
    return juce::JSON::toString (v, juce::JSON::FormatOptions {}.withSpacing (juce::JSON::Spacing::none)
                                        .withEncoding (juce::JSON::Encoding::ascii).withMaxDecimalPlaces (6));
}
void printLine (const juce::String& line)
{
    const auto text = line + "\n";
    std::fwrite (text.toRawUTF8(), 1, text.getNumBytesAsUTF8(), stdout);
    std::fflush (stdout);
}
void pump (int ms)
{
    const double until = nowMs() + ms;
    do
    {
        MSG message;
        for (int n = 0; n < 64 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++n)
        {
            TranslateMessage (&message); DispatchMessageW (&message);
        }
        Sleep (5);
    } while (nowMs() < until);
}
const char* stateName (State s)
{
    switch (s)
    {
        case State::off: return "off";
        case State::checkingHost: return "checkingHost";
        case State::hostWaiting: return "hostWaiting";
        case State::hostUnavailable: return "hostUnavailable";
        case State::joining: return "joining";
        case State::connected: return "connected";
        case State::disconnected: return "disconnected";
        case State::error: return "error";
    }
    return "?";
}

struct Second   // what the clock thread saw since the last report
{
    std::vector<double> loopMs;
    int markers = 0, windows = 0, jumps = 0, quiet = 0, lateBlocks = 0;
    double maxJump = 0, minLevel = 9, maxLevel = 0;
};

class Clock : public juce::Thread
{
public:
    Clock (CollabAudio& a, bool measure, int block, int recordSeconds)
        : juce::Thread ("probe clock"), audio (a), measuring (measure), blockSize (block),
          recordLimit ((size_t) recordSeconds * (size_t) deviceRate * 2)
    {
        left.resize ((size_t) block); right.resize ((size_t) block);
        sendLeft.resize ((size_t) block); sendRight.resize ((size_t) block);
        record.reserve (recordLimit);
    }
    Second take()
    {
        const juce::SpinLock::ScopedLockType hold (lock);
        Second result = std::move (current);
        current = {};
        return result;
    }
    void stopJudging() { judging = false; }
    std::vector<float> takeRecording() { return std::move (record); }   // after stopThread

private:
    void run() override
    {
        DWORD taskIndex = 0;
        const auto task = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);
        HANDLE timer = CreateWaitableTimerExW (nullptr, nullptr, 0x00000002 /* high resolution */, TIMER_ALL_ACCESS);
        if (timer == nullptr) timeBeginPeriod (1);
        LARGE_INTEGER frequency; QueryPerformanceFrequency (&frequency);
        const auto now = [&] { LARGE_INTEGER t; QueryPerformanceCounter (&t); return (double) t.QuadPart / (double) frequency.QuadPart; };
        const double period = blockSize / deviceRate;
        double next = now();
        while (! threadShouldExit())
        {
            next += period;
            const double wait = next - now();
            if (wait > 0)
            {
                if (timer != nullptr)
                {
                    LARGE_INTEGER due; due.QuadPart = -(LONGLONG) (wait * 1.0e7);
                    if (SetWaitableTimer (timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject (timer, 1000);
                }
                else Sleep ((DWORD) std::floor (wait * 1000.0));
            }
            else if (wait < -period)
            {
                { const juce::SpinLock::ScopedLockType hold (lock); ++current.lateBlocks; }   // a device would have glitched
                if (wait < -0.25) next = now();   // after a long stall, do not race through the backlog
            }
            process();
        }
        if (timer != nullptr) CloseHandle (timer); else timeEndPeriod (1);
        if (task != nullptr) AvRevertMmThreadCharacteristics (task);
    }

    void process()
    {
        const int n = blockSize;
        std::fill (left.begin(), left.end(), 0.0f); std::fill (right.begin(), right.end(), 0.0f);
        float* outputs[] { left.data(), right.data() };
        audio.renderRemote (outputs, 2, 0, n);
        if (measuring)
        {
            for (int i = 0; i < n; ++i)
            {
                const int64_t f = frame + i, inPeriod = f % markerPeriod;
                sendLeft[(size_t) i] = toneLevel * (float) std::sin (tonePhase (f));
                sendRight[(size_t) i] = inPeriod < markerLength ? markerSample ((int) inPeriod) : 0.0f;
                if (inPeriod == crossing) sent.push_back (f);
            }
            if (sent.size() > 16) sent.erase (sent.begin(), sent.end() - 16);
            audio.pushLocal (sendLeft.data(), sendRight.data(), n);
        }
        else audio.pushLocal (left.data(), right.data(), n);   // send back exactly what was heard
        analyse (n);
        frame += n;
    }

    bool settled (int64_t f) const { return toneSeen && f - toneSeenAt >= settleFrames; }

    void analyse (int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const int64_t f = frame + i;
            if (std::abs (right[(size_t) i]) > markerThreshold && f - lastMarker > markerPeriod / 2)
            {
                lastMarker = f;
                if (! settled (f) || ! judging.load()) continue;
                double loop = -1;
                if (measuring)
                    for (auto it = sent.rbegin(); it != sent.rend(); ++it)
                        if (*it <= f)
                        {
                            if (f - *it < markerPeriod - markerLength) loop = (double) (f - *it) * 1000.0 / deviceRate;
                            break;
                        }
                const juce::SpinLock::ScopedLockType hold (lock);
                ++current.markers;
                if (loop >= 0) current.loopMs.push_back (loop);
            }
        }
        for (int i = 0; i < n; ++i)
        {
            const double phase = tonePhase (frame + i);
            re += left[(size_t) i] * std::cos (phase); im += left[(size_t) i] * std::sin (phase);
            if (++filled < windowFrames) continue;
            const double level = 2.0 * std::hypot (re, im) / windowFrames / toneLevel, angle = std::atan2 (im, re);
            re = im = 0; filled = 0;
            if (! toneSeen && level > 0.5) { toneSeen = true; toneSeenAt = frame + i; }
            if (! settled (frame + i) || ! judging.load()) { lastAngle = angle; haveAngle = level >= 0.1; continue; }
            const juce::SpinLock::ScopedLockType hold (lock);
            ++current.windows;
            current.minLevel = std::min (current.minLevel, level); current.maxLevel = std::max (current.maxLevel, level);
            if (level < 0.1) ++current.quiet;
            else if (haveAngle)
            {
                const double jump = std::abs (wrap (angle - lastAngle));
                current.maxJump = std::max (current.maxJump, jump);
                if (jump > 0.35 || level < 0.7 || level > 1.3) ++current.jumps;
            }
            lastAngle = angle; haveAngle = level >= 0.1;
        }
        if (toneSeen && record.size() < recordLimit)
            for (int i = 0; i < n && record.size() < recordLimit; ++i)
            {
                record.push_back (left[(size_t) i]); record.push_back (right[(size_t) i]);
            }
    }

    CollabAudio& audio;
    const bool measuring;
    const int blockSize;
    const size_t recordLimit;
    const int64_t crossing = markerCrossing();
    std::vector<float> left, right, sendLeft, sendRight, record;
    std::vector<int64_t> sent;
    int64_t frame = 0, lastMarker = -markerPeriod;
    double re = 0, im = 0, lastAngle = 0;
    int filled = 0;
    int64_t toneSeenAt = 0;
    bool toneSeen = false, haveAngle = false;
    std::atomic<bool> judging { true };
    juce::SpinLock lock;
    Second current;
};

bool writeWav (const juce::File& file, const std::vector<float>& interleaved)
{
    file.deleteFile();
    juce::FileOutputStream out (file);
    if (! out.openedOk()) return false;
    const auto frames = (int) (interleaved.size() / 2), dataBytes = frames * 4;
    out.write ("RIFF", 4); out.writeInt (36 + dataBytes); out.write ("WAVE", 4);
    out.write ("fmt ", 4); out.writeInt (16); out.writeShort (1); out.writeShort (2);
    out.writeInt ((int) deviceRate); out.writeInt ((int) deviceRate * 4); out.writeShort (4); out.writeShort (16);
    out.write ("data", 4); out.writeInt (dataBytes);
    for (const float s : interleaved) out.writeShort ((short) juce::jlimit (-32767, 32767, (int) std::lround (s * 32767.0f)));
    out.flush();
    return out.getStatus().wasOk();
}

juce::var percentiles (std::vector<double> values)
{
    auto* o = new juce::DynamicObject();
    juce::var result (o);
    o->setProperty ("count", (int) values.size());
    if (values.empty()) return result;
    std::sort (values.begin(), values.end());
    const auto at = [&] (double q) { return values[(size_t) std::min (values.size() - 1, (size_t) std::floor (q * (double) (values.size() - 1) + 0.5))]; };
    o->setProperty ("min", round1 (values.front())); o->setProperty ("median", round1 (at (0.5)));
    o->setProperty ("p95", round1 (at (0.95))); o->setProperty ("max", round1 (values.back()));
    return result;
}

struct Args
{
    bool host = false;
    juce::String code, role, nick, record, out, log;
    int port = 49731, rate = 48000, seconds = 60, wait = 180, block = 128;
};

int run (const Args& args)
{
    const bool measure = args.role.isNotEmpty() ? args.role == "measure" : args.host;
    const auto directory = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LiveMixProbe-" + juce::Uuid().toString());
    directory.createDirectory();
    int exitCode = 0;
    auto* result = new juce::DynamicObject();
    juce::var resultVar (result);
    result->setProperty ("mode", args.host ? "host" : "join");
    result->setProperty ("role", measure ? "measure" : "reflect");
    result->setProperty ("computer", juce::SystemStats::getComputerName());
    result->setProperty ("nick", args.nick); result->setProperty ("block", args.block);
    {
        LiveMixSettings settings (directory);
        settings.setCollabNick (args.nick); settings.setCollabPort (args.port); settings.setCollabRate (args.rate);
        settings.setCollabMonitorVolume (1.0f); settings.setCollabMonitorOn (true); settings.setCollabRoute (0);
        CollabAudio audio ("Local\\LiveMixProbe.Peer." + juce::Uuid().toString());   // never the user's LiveMix OBS ring
        audio.prepare (deviceRate, args.block);
        Clock clock (audio, measure, args.block, args.record.isNotEmpty() ? 20 : 0);
        clock.startThread (juce::Thread::Priority::highest);
        std::vector<double> loops, rtts, estimates, targets, fills;
        int measured = 0, markers = 0, windows = 0, jumps = 0, quiet = 0, lateBlocks = 0;
        double maxJump = 0, minLevel = 9, maxLoss = 0;
        {
            CollabSession session (audio, settings, std::make_unique<CollabRoom>(), CollabUpnp::makeMiniUpnp(), CollabSession::Options {});
            const double started = nowMs();
            if (args.host) session.createRoom (args.nick);
            else session.joinRoom (args.code, args.nick);
            bool codePrinted = false;
            double connectedAt = 0;
            for (;;)
            {
                pump (50);
                const auto s = session.getSnapshot();
                if (args.host && ! codePrinted && s.state == State::hostWaiting && s.code.isNotEmpty())
                {
                    printLine ("PROBE_CODE " + s.code); codePrinted = true;
                    result->setProperty ("code", s.code); result->setProperty ("roomOpenMs", juce::roundToInt (nowMs() - started));
                }
                if (s.state == State::connected) { connectedAt = nowMs(); result->setProperty ("partner", s.peerNick); break; }
                if (s.state == State::hostUnavailable || s.state == State::error || (! args.host && s.state == State::off))
                {
                    result->setProperty ("failed", stateName (s.state));
                    result->setProperty ("reason", s.reason); result->setProperty ("message", s.message);
                    exitCode = s.state == State::hostUnavailable ? 2 : (args.host ? 4 : 3);
                    break;
                }
                if (nowMs() - started > args.wait * 1000.0)
                {
                    result->setProperty ("failed", juce::String ("timeout in ") + stateName (s.state)); exitCode = 3; break;
                }
            }
            if (exitCode == 0)
            {
                result->setProperty ("connectMs", juce::roundToInt (connectedAt - started));
                double nextReport = connectedAt + 1000.0;
                while (measured < args.seconds)
                {
                    pump (20);
                    const auto s = session.getSnapshot();
                    if (s.state != State::connected && s.state != State::disconnected)
                    {
                        result->setProperty ("ended", juce::String (stateName (s.state)) + " " + s.message); break;
                    }
                    if (nowMs() < nextReport) continue;
                    nextReport += 1000.0; ++measured;
                    const auto stats = audio.getStats();
                    const auto second = clock.take();
                    auto* line = new juce::DynamicObject();
                    juce::var lineVar (line);
                    line->setProperty ("t", measured); line->setProperty ("state", stateName (s.state));
                    line->setProperty ("rttMs", round1 (s.rttMs)); line->setProperty ("estOneWayMs", round1 (s.estimatedOneWayMs));
                    line->setProperty ("targetMs", round1 (stats.targetMs)); line->setProperty ("fillMs", round1 (stats.fillMs));
                    line->setProperty ("ppm", round1 (stats.correctionPpm)); line->setProperty ("loss10s", round1 (stats.lossPercent10s));
                    line->setProperty ("received", (juce::int64) stats.received); line->setProperty ("lost", (juce::int64) stats.lost);
                    line->setProperty ("late", (juce::int64) stats.late); line->setProperty ("underruns", (juce::int64) stats.underruns);
                    line->setProperty ("markers", second.markers);
                    juce::Array<juce::var> loopList;
                    for (const auto l : second.loopMs) { loopList.add (round1 (l)); loops.push_back (l); }
                    line->setProperty ("loopMs", loopList);
                    line->setProperty ("windows", second.windows); line->setProperty ("jumps", second.jumps);
                    line->setProperty ("quiet", second.quiet); line->setProperty ("maxJump", round1 (second.maxJump * 100.0) / 100.0);
                    line->setProperty ("minLevel", second.windows > 0 ? round1 (second.minLevel * 100.0) / 100.0 : 0.0);
                    line->setProperty ("lateBlocks", second.lateBlocks);
                    printLine ("PROBE_SECOND " + toJson (lineVar));
                    if (args.log.isNotEmpty()) juce::File (args.log).appendText ("PROBE_SECOND " + toJson (lineVar) + juce::newLine);
                    if (s.state == State::connected)
                    {
                        rtts.push_back (s.rttMs); estimates.push_back (s.estimatedOneWayMs);
                        targets.push_back (stats.targetMs); fills.push_back (stats.fillMs);
                    }
                    markers += second.markers; windows += second.windows; jumps += second.jumps; quiet += second.quiet;
                    lateBlocks += second.lateBlocks; maxJump = std::max (maxJump, second.maxJump);
                    if (second.windows > 0) minLevel = std::min (minLevel, second.minLevel);
                    maxLoss = std::max (maxLoss, stats.lossPercent10s);
                }
                clock.stopJudging();
                // keep sending 2 s more, so the partner's last measured second does not hear this side leaving
                for (const double until = nowMs() + 2000.0; nowMs() < until;)
                {
                    pump (20);
                    const auto s = session.getSnapshot().state;
                    if (s != State::connected && s != State::disconnected) break;
                }
                const auto stats = audio.getStats();
                auto* playout = new juce::DynamicObject();
                playout->setProperty ("received", (juce::int64) stats.received); playout->setProperty ("lost", (juce::int64) stats.lost);
                playout->setProperty ("late", (juce::int64) stats.late); playout->setProperty ("underruns", (juce::int64) stats.underruns);
                playout->setProperty ("maxLoss10s", round1 (maxLoss)); playout->setProperty ("ppm", round1 (stats.correctionPpm));
                result->setProperty ("playout", juce::var (playout));
                result->setProperty ("measuredSeconds", measured);
                result->setProperty ("loopMs", percentiles (loops));
                if (! loops.empty())
                {
                    auto sorted = loops; std::sort (sorted.begin(), sorted.end());
                    result->setProperty ("oneWayMs", round1 (sorted[sorted.size() / 2] / 2.0));
                }
                result->setProperty ("rttMs", percentiles (rtts)); result->setProperty ("estOneWayMs", percentiles (estimates));
                result->setProperty ("targetMs", percentiles (targets)); result->setProperty ("fillMs", percentiles (fills));
                auto* tone = new juce::DynamicObject();
                tone->setProperty ("windows", windows); tone->setProperty ("jumps", jumps); tone->setProperty ("quiet", quiet);
                tone->setProperty ("maxJump", round1 (maxJump * 100.0) / 100.0);
                tone->setProperty ("minLevel", windows > 0 ? round1 (minLevel * 100.0) / 100.0 : 0.0);
                result->setProperty ("tone", juce::var (tone));
                result->setProperty ("markers", markers); result->setProperty ("lateBlocks", lateBlocks);
            }
            session.leave();
            pump (1500);   // the worker closes the room and removes the UPnP mapping
        }
        clock.stopThread (2000);
        if (args.record.isNotEmpty())
        {
            const auto recording = clock.takeRecording();
            result->setProperty ("recordedSeconds", round1 ((double) recording.size() / 2.0 / deviceRate));
            result->setProperty ("recordSaved", writeWav (juce::File (args.record), recording));
        }
    }
    directory.deleteRecursively();
    result->setProperty ("exit", exitCode);
    const auto text = toJson (resultVar);
    if (args.out.isNotEmpty()) juce::File (args.out).replaceWithText (text + "\n");
    printLine ("PROBE_RESULT " + text);
    return exitCode;
}
}

int main (int argc, char* argv[])
{
    const juce::ScopedJuceInitialiser_GUI messageManager;   // the session's timers run on this thread
    if (argc > 1 && (juce::String (argv[1]) == "--v2" || juce::String (argv[1]) == "--decode"))
        return runCollabProbeV2 (argc, argv);
    Args args;
    args.nick = "probe-" + juce::SystemStats::getComputerName();
    for (int i = 1; i < argc; ++i)
    {
        const auto a = juce::String (juce::CharPointer_UTF8 (argv[i]));
        const auto next = [&] { return i + 1 < argc ? juce::String (juce::CharPointer_UTF8 (argv[++i])) : juce::String(); };
        if (a == "--host") args.host = true;
        else if (a == "--join") args.code = next();
        else if (a == "--role") args.role = next();
        else if (a == "--nick") args.nick = next();
        else if (a == "--record") args.record = next();
        else if (a == "--out") args.out = next();
        else if (a == "--log") args.log = next();
        else if (a == "--port") args.port = next().getIntValue();
        else if (a == "--rate") args.rate = next().getIntValue();
        else if (a == "--seconds") args.seconds = next().getIntValue();
        else if (a == "--wait") args.wait = next().getIntValue();
        else if (a == "--block") args.block = next().getIntValue();
        else { printLine ("PROBE_RESULT {\"exit\":4,\"failed\":\"unknown argument\"}"); return 4; }
    }
    const bool validRole = args.role.isEmpty() || args.role == "measure" || args.role == "reflect";
    if (args.host == args.code.isNotEmpty() || ! validRole || (args.rate != 48000 && args.rate != 44100)
        || args.block < 16 || args.block > 2048 || args.seconds < 1 || args.port < 1024 || args.port > 65535)
    {
        printLine ("PROBE_RESULT {\"exit\":4,\"failed\":\"usage: --host | --join CODE  [--role measure|reflect] [--seconds N] "
                   "[--wait N] [--rate 48000|44100] [--block N] [--port N] [--nick NAME] [--record FILE.wav] [--out FILE.json] [--log FILE]\"}");
        return 4;
    }
    return run (args);
}
