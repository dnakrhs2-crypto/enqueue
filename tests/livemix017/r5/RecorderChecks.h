#pragma once
#include "recording/Recorder.h"
#include "recording/RecordingQueue.h"
#include "recording/WavSink.h"
#include "recording/MfMp3Sink.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <thread>

// The probe and unit tests exercise the same production path and independently decode its files.
namespace gocue::livemix::recording::checks
{
struct Result
{
    std::string name;
    std::vector<std::pair<bool, std::string>> assertions;
    bool ok() const { return std::all_of(assertions.begin(), assertions.end(), [](const auto& a) { return a.first; }); }
    void require(bool value, const std::string& message)
    {
        assertions.emplace_back(value, message);
        if (!value) throw std::runtime_error(message);
    }
};
using Report = std::function<void(const Result&)>;
template<class Fn> void run(const Report& report, const std::string& name, Fn fn)
{
    Result result { name, {} };
    try { fn(result); }
    catch (const std::exception& e)
    { if (result.ok()) result.assertions.emplace_back(false, e.what()); }
    report(result);
}
struct Temp
{
    juce::File path = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("LiveMixRecorder-" + juce::Uuid().toString(), "", false);
    Temp() { if (path.createDirectory().failed()) throw std::runtime_error("temporary directory unavailable"); }
    ~Temp() { path.deleteRecursively(); } // only the freshly created, owned temporary directory
};
inline bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 15000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    while (std::chrono::steady_clock::now() < end);
    return predicate();
}
inline StartRequest request(const juce::File& folder, bool inCollab = true, Format format = Format::wav, unsigned kbps = 320)
{
    StartRequest req;
    req.options = { format, kbps, folder.getFullPathName().toStdString() };
    req.collabAtStart = inCollab;
    req.people[0] = { 1, collab::contracts::ParticipantKind::native, "내소리" };
    req.people[1] = { 2, collab::contracts::ParticipantKind::native, "상대1" };
    req.people[2] = { 3, collab::contracts::ParticipantKind::web, "웹손님" };
    return req;
}
inline void start(Result& r, Recorder& rec, const StartRequest& req, unsigned rate = 48000, int block = 257)
{
    r.require(rec.prepareFormat({ static_cast<double>(rate), block, 1 }), "prepare format");
    r.require(rec.start(req), "accept start request");
    r.require(waitFor([&] { const auto s = rec.snapshot().state; return s == State::recording || s == State::error; }), "start completes");
    const auto s = rec.snapshot();
    r.require(s.state == State::recording, "recording: " + s.errorDetail);
    r.require(s.fileCount == (req.collabAtStart ? 4u : 1u), "all file roles open before recording");
}
struct Signal
{
    unsigned rate;
    int frames;
    std::array<juce::AudioBuffer<float>, 3> audio;
    explicit Signal(unsigned hz, int n) : rate(hz), frames(n)
    {
        for (int t = 0; t < 3; ++t)
        {
            audio[static_cast<std::size_t>(t)].setSize(2, n);
            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < 2; ++ch)
                {
                    const auto frequency = 233.0 + t * 181.0 + (t == 2 ? 0.0 : ch * 53.0);
                    float value = static_cast<float>(0.11 * std::sin(juce::MathConstants<double>::twoPi * frequency * i / hz));
                    // Common, clearly detectable marker at 0.6s; distinct tones identify each person/channel.
                    if (i >= static_cast<int>(hz * 0.6) && i < static_cast<int>(hz * 0.6) + 48) value = 0.65f;
                    // Each person also has a different marker time, width and amplitude.
                    const int personal = static_cast<int>(hz * (0.72 + t * 0.08));
                    if (i >= personal && i < personal + 32 + t * 8) value = 0.45f + static_cast<float>(t) * 0.04f;
                    audio[static_cast<std::size_t>(t)].setSample(ch, i, value);
                }
        }
    }
};
inline void feed(Recorder& rec, const Signal& signal, int first, int count, unsigned mask = 7, int block = 257)
{
    for (int offset = first; offset < first + count;)
    {
        const int n = std::min(block, first + count - offset);
        std::array<collab::contracts::AudioSourceView, 3> views;
        for (unsigned t = 0; t != 3; ++t)
            if ((mask & (1u << t)) != 0)
            {
                views[t].left = signal.audio[t].getReadPointer(0, offset);
                views[t].right = signal.audio[t].getReadPointer(1, offset);
                views[t].channels = t == 2 ? 1 : 2;
            }
        rec.push(views[0], views[1], views[2], n);
        offset += n;
    }
}
inline std::vector<juce::File> paths(const juce::File& folder, const juce::String& pattern = "*.wav")
{
    auto list = folder.findChildFiles(juce::File::findFiles, true, pattern);
    std::vector<juce::File> result;
    for (const auto& file : list) result.push_back(file);
    return result;
}
inline juce::File named(const juce::File& folder, const juce::String& name)
{
    for (const auto& f : paths(folder, "*")) if (f.getFileName() == name) return f;
    return {};
}
struct Wave
{
    unsigned rate = 0, bits = 0, channels = 0;
    std::vector<int> left, right; // unshifted PCM24
};
inline Wave wave(Result& r, const juce::File& file)
{
    {
        juce::FileInputStream header(file);
        char magic[4] {};
        r.require(header.read(magic, 4) == 4 && std::string(magic, 4) == "RIFF", "ordinary RIFF WAV, never automatic RF64");
    }
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(file.createInputStream().release(), true));
    r.require(reader != nullptr, "open WAV " + file.getFileName().toStdString());
    Wave result;
    result.rate = static_cast<unsigned>(reader->sampleRate); result.bits = reader->bitsPerSample; result.channels = reader->numChannels;
    result.left.resize(static_cast<std::size_t>(reader->lengthInSamples)); result.right.resize(result.left.size());
    int* dest[] { result.left.data(), result.right.data() };
    r.require(reader->read(dest, 2, 0, static_cast<int>(result.left.size()), false), "decode WAV samples");
    for (auto* channel : { &result.left, &result.right }) for (auto& sample : *channel) sample /= 256;
    return result;
}
inline int q24(float sample)
{
    const auto scaled = std::llround(static_cast<double>(sample) * 8388608.0);
    return static_cast<int>(std::clamp<std::int64_t>(scaled, -8388608, 8388607));
}
inline void verifyWave(Result& r, const Wave& w, const Signal& source, unsigned track, int first = 0, int lateC = 0,
                       unsigned mask = 7)
{
    r.require(w.rate == source.rate && w.channels == 2 && w.bits == 24, "device rate / stereo / PCM24");
    r.require(first >= 0 && static_cast<std::size_t>(first) + w.left.size() <= static_cast<std::size_t>(source.frames), "decoded interval fits source clock");
    bool same = true;
    for (std::size_t i = 0; i < w.left.size(); ++i)
        for (int ch = 0; ch != 2; ++ch)
        {
            const int position = first + static_cast<int>(i);
            auto value = [&](unsigned t) { return (mask & (1u << t)) == 0 || (t == 2 && position < lateC)
                ? 0 : q24(source.audio[t].getSample(ch, position)); };
            const auto expected = track == 3 ? static_cast<int>(std::clamp<std::int64_t>(
                static_cast<std::int64_t>(value(0)) + value(1) + value(2), -8388608, 8388607)) : value(track);
            same = same && (ch == 0 ? w.left[i] : w.right[i]) == expected;
        }
    r.require(same, track == 3 ? "every D sample equals saturated integer A+B+C" : "every source sample / stereo / silence alignment");
}
inline void finished(Result& r, Recorder& rec, std::uint64_t frames)
{
    rec.requestStop(); rec.requestStop(); rec.finish();
    const auto s = rec.snapshot();
    r.require(s.state == State::idle && s.error == Error::none, "normal stop: " + s.errorDetail);
    r.require(s.capturedFrames == frames, "captured frame count");
}
inline void wavTests(const Report& report)
{
    for (unsigned rate : { 44100u, 48000u, 96000u })
        run(report, "WAV " + std::to_string(rate) + " stereo PCM24, four aligned files and exact sum", [rate](Result& r)
        {
            Temp dir; Recorder rec; auto req = request(dir.path); req.people[2] = {};
            req.people[0].nicknameUtf8 = " 내:<소리>. ";
            start(r, rec, req, rate);
            const int count = static_cast<int>(rate) + 97, join = static_cast<int>(rate / 2);
            Signal input(rate, count);
            feed(rec, input, 0, join, 3);
            rec.noteSeatOccupant(2, { 3, collab::contracts::ParticipantKind::web, "웹/손님." });
            feed(rec, input, join, count - join);
            finished(r, rec, count);
            r.require(paths(dir.path).size() == 4, "late third guest retained and renamed");
            r.require(named(dir.path, juce::String::fromUTF8("A_내__소리_.wav")).existsAsFile(), "nickname sanitisation preserves Unicode");
            r.require(named(dir.path, juce::String::fromUTF8("C_웹_손님.wav")).existsAsFile(), "first occupant rename");
            for (const auto& file : rec.snapshot().files)
            {
                auto w = wave(r, juce::File(juce::String::fromUTF8(file.pathUtf8.c_str())));
                r.require(w.left.size() == static_cast<std::size_t>(count), "same start and length");
                verifyWave(r, w, input, static_cast<unsigned>(file.track), 0, join);
                r.require(file.checkpointFrames == file.writtenFrames && file.writtenFrames == count, "final checkpoint length");
                if (file.track == Track::remote2) r.require(w.left == w.right, "web dual mono L=R");
                if (file.track == Track::mix) r.require(file.clippedSamples > 0, "mix overload counted without gain reduction");
            }
        });
    run(report, "WAV frame-boundary bundle splitting, sample continuity and all-part cleanup", [](Result& r)
    {
        Temp dir; Recorder::TestHooks hooks; hooks.wavLimitBytes = 8192; Recorder rec(hooks);
        auto req = request(dir.path); req.people[1] = {}; req.people[2] = {};
        start(r, rec, req); Signal input(48000, 7501);
        feed(rec, input, 0, 1001, 1);
        rec.noteSeatOccupant(1, { 8, collab::contracts::ParticipantKind::native, "첫사람" });
        feed(rec, input, 1001, 2000, 3);
        rec.noteSeatOccupant(1, { 9, collab::contracts::ParticipantKind::web, "다음사람" });
        feed(rec, input, 3001, 4500, 3);
        finished(r, rec, 7501);
        const auto files = paths(dir.path);
        const auto parts = rec.snapshot().files[0].part;
        r.require(parts > 2 && files.size() == parts * 3, "A/B/D in every part, C removed in every part");
        std::array<int, 4> offsets {};
        for (unsigned part = 1; part <= parts; ++part)
        {
            const auto suffix = (part > 1 ? "_" + juce::String(part) : "") + ".wav";
            std::size_t partLength = 0;
            for (unsigned t : { 0u, 1u, 3u })
            {
                const auto base = juce::String::fromUTF8(t == 0 ? "A_내소리" : t == 1 ? "B_첫사람" : "D_전체");
                auto file = named(dir.path, base + suffix);
                r.require(file.getSize() <= 8192, "whole WAV size stays under injected split limit");
                auto w = wave(r, file);
                if (t == 0) partLength = w.left.size();
                r.require(w.left.size() == partLength, "bundle splits on same frame");
                bool exact = true;
                for (std::size_t i = 0; i < w.left.size(); ++i)
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const int n = offsets[t] + static_cast<int>(i);
                        const int a = q24(input.audio[0].getSample(ch, n));
                        const int b = n >= 1001 ? q24(input.audio[1].getSample(ch, n)) : 0;
                        exact = exact && (ch == 0 ? w.left[i] : w.right[i]) == (t == 0 ? a : t == 1 ? b : a+b);
                    }
                r.require(exact, "no missing/duplicate samples at block-internal split");
                offsets[t] += static_cast<int>(w.left.size());
            }
        }
        r.require(offsets[0] == 7501 && offsets[1] == 7501 && offsets[3] == 7501, "all parts total original length");
    });
    run(report, "WAV checkpoint reopens while writer lives; final tail and write failure", [](Result& r)
    {
        Temp dir; const auto path = dir.path.getChildFile("checkpoint.wav"); WavSink sink;
        r.require(sink.open(path, 48000), "open and flush empty header");
        r.require(wave(r, path).left.empty(), "empty checkpoint readable");
        std::vector<std::int32_t> pcm(10000, 12345);
        r.require(sink.write(pcm.data(), 3000) && sink.checkpoint(), "header flush then disk flush");
        r.require(sink.checkpointFrames() == 3000 && wave(r, path).left.size() == 3000, "completed checkpoint readable");
        r.require(sink.write(pcm.data(), 2000) && sink.close(), "close flushes tail");
        r.require(wave(r, path).left.size() == 5000, "final length includes tail");
        WavSink broken;
        r.require(broken.open(dir.path.getChildFile("failure.wav"), 48000, wavSplitBytes, 4096), "open failure-injected stream");
        r.require(!broken.write(pcm.data(), 3000) && !broken.close(), "disk write failure is observable");
        r.require(broken.checkpointFrames() == 0, "failed flush never advances checkpoint");
    });
    run(report, "PCM24 clipping, finite conversion and no limiter", [](Result& r)
    {
        std::uint64_t clipped = 0;
        r.require(WavSink::quantize(2.0f, clipped) == 8388607 && WavSink::quantize(-2.0f, clipped) == -8388608, "saturate both polarities");
        r.require(clipped == 2, "count each overloaded sample");
        r.require(WavSink::quantize(0.25f, clipped) == 2097152 && clipped == 2, "unity gain below clipping");
        r.require(WavSink::quantize(std::numeric_limits<float>::quiet_NaN(), clipped) == 0, "nonfinite input is bounded");
    });
}
inline void recorderTests(const Report& report)
{
    run(report, "Solo stays one mix file when remote seats arrive", [](Result& r)
    {
        Temp dir; Recorder rec; start(r, rec, request(dir.path, false)); Signal input(48000, 24001);
        r.require(!rec.start(request(dir.path)), "busy start rejected without replacing current take");
        feed(rec, input, 0, 12000, 1); feed(rec, input, 12000, 12001);
        finished(r, rec, 24001);
        const auto s = rec.snapshot();
        r.require(paths(dir.path).size() == 1 && s.files[0].track == Track::mix, "solo layout fixed at start");
        auto w = wave(r, juce::File(juce::String::fromUTF8(s.files[0].pathUtf8.c_str())));
        r.require(w.left.size() == 24001, "solo file contains every captured frame");
        bool exact = true;
        for (int i = 0; i < 24001; ++i) for (int ch = 0; ch < 2; ++ch)
        {
            int expected = q24(input.audio[0].getSample(ch, i));
            if (i >= 12000) expected += q24(input.audio[1].getSample(ch, i)) + q24(input.audio[2].getSample(ch, i));
            exact = exact && (ch == 0 ? w.left[static_cast<std::size_t>(i)] : w.right[static_cast<std::size_t>(i)]) == expected;
        }
        r.require(exact, "solo includes remote sound after join");
    });
    run(report, "Two people keep silent occupied B; absent C is removed; late names stay fixed", [](Result& r)
    {
        Temp dir; Recorder rec; auto req = request(dir.path); req.people[1] = {}; req.people[2] = {};
        start(r, rec, req); Signal input(48000, 2000); input.audio[1].clear();
        rec.noteSeatOccupant(1, { 7, collab::contracts::ParticipantKind::web, "첫손님" });
        feed(rec, input, 0, 1000, 3);
        rec.noteSeatOccupant(1, { 8, collab::contracts::ParticipantKind::native, "다른손님" });
        feed(rec, input, 1000, 1000, 1);
        finished(r, rec, 2000);
        r.require(paths(dir.path).size() == 3 && rec.snapshot().fileCount == 3, "A/B/D only");
        auto b = wave(r, named(dir.path, juce::String::fromUTF8("B_첫손님.wav")));
        r.require(b.left.size() == 2000 && std::all_of(b.left.begin(), b.left.end(), [](int x) { return x == 0; }), "silent non-null view retained through departure");
        r.require(rec.snapshot().files[0].part == 1, "seat changes do not split the take");
    });
    run(report, "Rename destination exists: every original part keeps its default name", [](Result& r)
    {
        Temp dir; Recorder::TestHooks hooks; hooks.wavLimitBytes = 8192; Recorder rec(hooks);
        auto req = request(dir.path); req.people[1] = {}; req.people[2] = {};
        start(r, rec, req); const auto folder = juce::File(juce::String::fromUTF8(rec.snapshot().files[0].pathUtf8.c_str())).getParentDirectory();
        const auto conflict = folder.getChildFile(juce::String::fromUTF8("B_손님_2.wav"));
        r.require(conflict.replaceWithText("do not replace"), "prepare ordinary existing destination");
        rec.noteSeatOccupant(1, { 4, collab::contracts::ParticipantKind::native, "손님" });
        Signal input(48000, 5000); feed(rec, input, 0, 5000, 3); finished(r, rec, 5000);
        const auto parts = rec.snapshot().files[0].part;
        for (unsigned p = 1; p <= parts; ++p)
            r.require(folder.getChildFile(juce::String::fromUTF8("B_합방상대1") + (p > 1 ? "_" + juce::String(p) : "") + ".wav").existsAsFile(), "default name for every part");
        r.require(conflict.loadFileAsString() == "do not replace", "existing destination untouched");
    });
    run(report, "Web-to-native seat reuse keeps its original stereo file and part", [](Result& r)
    {
        Temp dir; Recorder rec; auto req = request(dir.path); req.people[1].kind = collab::contracts::ParticipantKind::web;
        req.people[2] = {}; start(r, rec, req);
        Signal input(48000, 6000);
        for (int first = 0; first < 3000;)
        {
            const int n = std::min(257, 3000 - first);
            collab::contracts::AudioSourceView mono;
            mono.left = input.audio[1].getReadPointer(0, first); mono.channels = 1;
            rec.push({}, mono, {}, n); first += n;
        }
        rec.noteSeatOccupant(1, { 8, collab::contracts::ParticipantKind::native, "교체된사람" });
        feed(rec, input, 3000, 3000, 2); finished(r, rec, 6000);
        auto b = wave(r, named(dir.path, juce::String::fromUTF8("B_상대1.wav")));
        r.require(b.left.size() == 6000 && rec.snapshot().files[0].part == 1, "initial name and one part retained");
        bool exact = true, stereo = false;
        for (int i = 0; i < 6000; ++i)
        {
            exact = exact && b.left[static_cast<std::size_t>(i)] == q24(input.audio[1].getSample(0, i))
                && b.right[static_cast<std::size_t>(i)] == q24(input.audio[1].getSample(i < 3000 ? 0 : 1, i));
            if (i >= 3000) stereo = stereo || b.left[static_cast<std::size_t>(i)] != b.right[static_cast<std::size_t>(i)];
        }
        r.require(exact && stereo, "mono duplicates L, subsequent native stereo keeps distinct channels");
    });
    run(report, "Device format epoch drains previous queue and starts next WAV bundle", [](Result& r)
    {
        Temp dir; Recorder rec; start(r, rec, request(dir.path));
        Signal first(48000, 5003), second(96000, 7001);
        feed(rec, first, 0, first.frames);
        r.require(rec.prepareFormat({ 96000, 257, 2 }), "prepare new device while callbacks stopped");
        feed(rec, second, 0, second.frames); finished(r, rec, 12004);
        r.require(std::abs(rec.snapshot().elapsedSeconds - (5003.0/48000 + 7001.0/96000)) < 1e-9, "elapsed time sums actual device intervals");
        r.require(paths(dir.path).size() == 8, "both complete bundles retained");
        for (unsigned p = 1; p <= 2; ++p) for (unsigned t = 0; t < 4; ++t)
        {
            const auto name = juce::String::fromUTF8(t == 0 ? "A_내소리" : t == 1 ? "B_상대1" : t == 2 ? "C_웹손님" : "D_전체")
                + (p == 2 ? "_2.wav" : ".wav");
            auto w = wave(r, named(dir.path, name)); const auto& expected = p == 1 ? first : second;
            r.require(w.left.size() == static_cast<std::size_t>(expected.frames), "old queue drained with no inserted device-gap silence");
            verifyWave(r, w, expected, t);
        }
    });
    run(report, "Prepared FIFO holds four seconds at variable block sizes", [](Result& r)
    {
        RecordingQueue queue({ 48000, 1000, 1 });
        r.require(queue.capacity() == 192000, "four-second frame capacity");
        for (int i = 0; i < 192; ++i) r.require(queue.push({}, {}, {}, 1000, static_cast<std::uint64_t>(i)*1000), "queue accepts its capacity");
        r.require(!queue.push({}, {}, {}, 1, 192000), "overflow returns false without dropping earlier audio");
        std::vector<RecordingQueue::Frame> data(1000); RecordingQueue::Block block;
        std::uint64_t expected = 0;
        while (queue.pop(block, data.data()))
        {
            r.require(block.firstFrame == expected && block.format.formatEpoch == 1 && block.format.sampleRate == 48000, "block clock and generation metadata");
            expected += static_cast<unsigned>(block.frames);
        }
        r.require(expected == 192000 && queue.empty(), "every accepted frame consumed");
        r.require(queue.push({}, {}, {}, 17, expected) && queue.pop(block, data.data()) && block.frames == 17, "ring wraps with short callbacks");
    });
    run(report, "FIFO overflow stops only recording, preserves unoccupied files and latched error", [](Result& r)
    {
        Temp dir; std::atomic<bool> gate { false }; Recorder::TestHooks hooks; hooks.writerGate = &gate;
        Recorder rec(hooks); auto req = request(dir.path); req.people[1] = {}; req.people[2] = {};
        start(r, rec, req, 48000, 1000);
        rec.noteSeatOccupant(2, { 5, collab::contracts::ParticipantKind::web, "새이름" });
        for (int i = 0; i < 193; ++i) rec.push({}, {}, {}, 1000);
        r.require(rec.snapshot().error == Error::queueOverflow, "overflow immediately visible");
        rec.finish(); const auto s = rec.snapshot();
        r.require(s.state == State::error && s.error == Error::queueOverflow, "error remains after finish");
        r.require(paths(dir.path).size() == 4 && named(dir.path, juce::String::fromUTF8("C_합방상대2.wav")).existsAsFile(), "error skips delete and rename");
        const auto captured = s.capturedFrames;
        for (int i = 0; i < 100; ++i) rec.push({}, {}, {}, 1000);
        r.require(rec.snapshot().capturedFrames == captured, "subsequent audio callbacks return after recording error");
        r.require(captured == 192000 && s.files[0].writtenFrames == captured, "already accepted queue drained");
    });
    run(report, "Disk failure ends whole bundle; invalid storage is a start error", [](Result& r)
    {
        Temp dir; Recorder::TestHooks hooks; hooks.failAfterBytes = 4096; Recorder rec(hooks);
        auto req = request(dir.path); req.people[2] = {};
        start(r, rec, req); Signal input(48000, 10000); feed(rec, input, 0, 10000, 3); rec.finish();
        r.require(rec.snapshot().error == Error::writeFailed && rec.snapshot().state == State::error, "write error retained");
        r.require(paths(dir.path).size() == 4, "no partial-success cleanup");
        auto blocker = dir.path.getChildFile("ordinary-file"); blocker.replaceWithText("x");
        Recorder denied; r.require(denied.prepareFormat({48000, 257, 1}), "prepare storage error case");
        r.require(denied.start(request(blocker.getChildFile("child"), false)), "start is asynchronous");
        denied.finish(); r.require(denied.snapshot().error == Error::openFailed, "invalid/unwritable storage reports open failure");
    });
    run(report, "Unavailable MP3 is isolated; next WAV take and repeated finish work", [](Result& r)
    {
        Temp dir; Recorder::TestHooks hooks; hooks.encoderUnavailable = true; Recorder rec(hooks);
        r.require(rec.prepareFormat({48000, 257, 1}), "prepare");
        r.require(rec.start(request(dir.path, false, Format::mp3)), "accept MP3 start"); rec.finish();
        r.require(rec.snapshot().error == Error::encoderUnavailable, "missing MF reported only at MP3 start");
        start(r, rec, request(dir.path, false)); Signal input(48000, 1000); feed(rec, input, 0, 1000);
        finished(r, rec, 1000); rec.finish(); r.require(rec.snapshot().state == State::idle, "WAV remains available after MP3 failure");
    });
}
struct Mp3
{
    unsigned rate = 0, kbps = 0;
    std::uint64_t mpegFrames = 0;
    juce::AudioBuffer<float> audio;
};
inline Mp3 mp3(Result& r, const juce::File& file, unsigned expectedRate, unsigned expectedKbps)
{
    juce::MemoryBlock bytes; r.require(file.loadFileAsData(bytes), "read MP3 bytes");
    const auto* data = static_cast<const unsigned char*>(bytes.getData());
    std::size_t pos = 0; Mp3 result;
    const unsigned bitrates[] { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
    const unsigned rates[] {44100,48000,32000,0};
    bool headers = bytes.getSize() > 0;
    while (pos + 4 <= bytes.getSize())
    {
        if (data[pos] != 0xff || (data[pos+1] & 0xfe) != 0xfa) { headers = false; break; }
        result.rate = rates[(data[pos+2] >> 2) & 3]; result.kbps = bitrates[data[pos+2] >> 4];
        if (!result.rate || !result.kbps) { headers = false; break; }
        headers = headers && result.rate == expectedRate && result.kbps == expectedKbps && (data[pos+3] >> 6) != 3;
        pos += 144000 * result.kbps / result.rate + ((data[pos+2] >> 1) & 1);
        result.mpegFrames += 1152;
    }
    r.require(headers && pos == bytes.getSize(), "every MPEG frame: requested rate / bitrate / stereo; complete tail");
    juce::MP3AudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(file.createInputStream().release(), true));
    r.require(reader != nullptr, "independent JUCE MP3 decoder opens output");
    r.require(reader->numChannels == 2 && reader->sampleRate == expectedRate, "decoded stereo / rate");
    result.audio.setSize(2, static_cast<int>(reader->lengthInSamples));
    r.require(reader->read(&result.audio, 0, result.audio.getNumSamples(), 0, true, true), "decode all MP3 samples");
    return result;
}
inline double tone(const juce::AudioBuffer<float>& audio, unsigned rate, int channel, double frequency, double startSeconds = .1)
{
    const int first = static_cast<int>(rate * startSeconds), count = static_cast<int>(rate / 4);
    double re = 0, im = 0;
    for (int i = first; i < first + count && i < audio.getNumSamples(); ++i)
    {
        const double angle = juce::MathConstants<double>::twoPi * frequency * i / rate;
        re += audio.getSample(channel, i) * std::cos(angle); im += audio.getSample(channel, i) * std::sin(angle);
    }
    return 2 * std::hypot(re, im) / count;
}
inline int marker(const juce::AudioBuffer<float>& audio, unsigned rate, double atSeconds = .6)
{
    int best = 0; float peak = 0;
    const int first = static_cast<int>(rate * (atSeconds - .05));
    const int end = std::min(audio.getNumSamples() - 48, static_cast<int>(rate * (atSeconds + .15)));
    for (int i = first; i < end; ++i)
    {
        float energy = 0;
        for (int j = 0; j < 48; ++j) energy += std::abs(audio.getSample(0, i+j));
        if (energy > peak) { peak = energy; best = i; }
    }
    return best;
}
inline void mp3Tests(const Report& report)
{
    for (unsigned rate : {32000u,44100u,48000u,96000u}) for (unsigned kbps : {128u,256u,320u})
        run(report, "MP3 " + std::to_string(rate) + " -> " + std::to_string(rate == 96000 ? 48000 : rate)
            + " / " + std::to_string(kbps) + " kbps / A B C D", [rate,kbps](Result& r)
        {
            Temp dir; Recorder rec; start(r, rec, request(dir.path, true, Format::mp3, kbps), rate);
            Signal input(rate, static_cast<int>(rate) + 173); feed(rec, input, 0, input.frames);
            finished(r, rec, input.frames);
            const unsigned hz = rate == 96000 ? 48000 : rate;
            const auto expected = static_cast<std::uint64_t>(std::llround(input.frames * static_cast<double>(hz) / rate));
            std::uint64_t length = 0; int firstMarker = -1;
            r.require(paths(dir.path, "*.mp3").size() == 4, "four MP3 files");
            for (const auto& info : rec.snapshot().files)
            {
                auto decoded = mp3(r, juce::File(juce::String::fromUTF8(info.pathUtf8.c_str())), hz, kbps);
                r.require(decoded.mpegFrames >= expected && decoded.mpegFrames - expected <= 3456, "drained duration within encoder delay/padding");
                if (!length) length = decoded.mpegFrames;
                r.require(decoded.mpegFrames == length && info.writtenFrames == length && info.checkpointFrames == length, "equal encoded intervals and final checkpoints");
                const int found = marker(decoded.audio, hz);
                r.require(std::abs(found - static_cast<int>(hz * .6)) <= 2304, "start marker has bounded codec delay");
                if (firstMarker < 0) firstMarker = found;
                r.require(std::abs(found - firstMarker) <= 64, "four file markers align after encoding");
                if (info.track != Track::mix)
                {
                    const double time = .72 + static_cast<unsigned>(info.track) * .08;
                    const int personal = marker(decoded.audio, hz, time);
                    r.require(std::abs(personal - found - static_cast<int>(std::llround((time - .6) * hz))) < 96,
                              "person-specific marker retains its sample-clock position");
                }
                for (unsigned t = 0; t < 3; ++t)
                {
                    const auto amplitude = tone(decoded.audio, hz, 0, 233.0+t*181.0);
                    const bool own = info.track == Track::mix || static_cast<unsigned>(info.track) == t;
                    r.require(own ? amplitude > .075 && amplitude < .15 : amplitude < .012, "person-specific tone content");
                }
                if (info.track == Track::remote2)
                {
                    float difference = 0;
                    for (int i = 0; i < decoded.audio.getNumSamples(); ++i)
                        difference = std::max(difference, std::abs(decoded.audio.getSample(0,i)-decoded.audio.getSample(1,i)));
                    r.require(difference < .0001f, "web L=R after independent decode");
                }
            }
        });
    for (bool multi : { false, true })
        run(report, multi ? "MP3 late C guest and departed B keep their common timeline"
                          : "MP3 solo mix includes guests joining during recording", [multi](Result& r)
        {
            Temp dir; Recorder rec; auto req = request(dir.path, multi, Format::mp3); req.people[2] = {};
            start(r, rec, req);
            Signal input(48000, 96000);
            constexpr int join = 38400;
            feed(rec, input, 0, join, multi ? 3 : 1);
            rec.noteSeatOccupant(2, { 7, collab::contracts::ParticipantKind::web, "늦은손님" });
            feed(rec, input, join, input.frames - join, multi ? 5 : 7);
            finished(r, rec, input.frames);
            const auto snapshot = rec.snapshot();
            r.require(paths(dir.path, "*.mp3").size() == (multi ? 4u : 1u), "fixed layout at start");
            std::uint64_t length = 0;
            for (unsigned f = 0; f < snapshot.fileCount; ++f)
            {
                const auto& info = snapshot.files[f];
                auto decoded = mp3(r, juce::File(juce::String::fromUTF8(info.pathUtf8.c_str())), 48000, 320);
                if (!length) length = decoded.mpegFrames;
                r.require(decoded.mpegFrames == length, "late/departed files have the same total length");
                if (info.track == Track::remote2)
                {
                    r.require(juce::String::fromUTF8(info.pathUtf8.c_str()).contains(juce::String::fromUTF8("C_늦은손님.mp3")), "MP3 first occupant rename");
                    r.require(decoded.audio.getRMSLevel(0, 0, 30000) < .001f, "late seat leading interval is silence");
                    r.require(tone(decoded.audio, 48000, 0, 595, 1.2) > .075, "late guest content in original timeline");
                }
                if (info.track == Track::remote1)
                    r.require(decoded.audio.getRMSLevel(0, 57600, 12000) < .001f, "departed seat remains silent without ending file");
                if (info.track == Track::mix)
                {
                    r.require(tone(decoded.audio, 48000, 0, 233, 1.2) > .075
                           && tone(decoded.audio, 48000, 0, 595, 1.2) > .075, "late guest mixed with master");
                    const auto b = tone(decoded.audio, 48000, 0, 414, 1.2);
                    r.require(multi ? b < .012 : b > .075, "mix follows current occupants");
                }
            }
        });
    run(report, "MP3 device-rate change drains all four old encoders", [](Result& r)
    {
        Temp dir; Recorder rec; start(r, rec, request(dir.path, true, Format::mp3), 44100);
        Signal one(44100, 22050), two(96000, 48000);
        feed(rec, one, 0, one.frames);
        r.require(rec.prepareFormat({96000,257,2}), "prepare 96k device generation");
        feed(rec, two, 0, two.frames); finished(r, rec, 70050);
        r.require(paths(dir.path, "*.mp3").size() == 8, "two MP3 bundles");
        for (const auto& file : paths(dir.path, "*.mp3"))
        {
            const unsigned rate = file.getFileNameWithoutExtension().endsWith("_2") ? 48000 : 44100;
            auto decoded = mp3(r, file, rate, 320);
            r.require(decoded.mpegFrames >= rate/2 && decoded.mpegFrames <= rate/2+3456, "each generation drained at correct rate");
        }
    });
    run(report, "MP3 float clipping/dither and storage failure are surfaced", [](Result& r)
    {
        Temp dir; MfMp3Sink sink;
        r.require(sink.open(dir.path.getChildFile("clip.mp3"), 48000, 128), "open direct MP3 sink");
        std::vector<float> overloaded(48000, 2.0f);
        r.require(sink.write(overloaded.data(), 24000) && sink.close(), "encode and drain overloaded input");
        r.require(sink.clippedSamples() == 48000, "count saturated PCM16 samples");
        MfMp3Sink edge;
        r.require(edge.open(dir.path.getChildFile("dither.mp3"), 48000, 128), "open full-scale dither check");
        std::fill(overloaded.begin(), overloaded.end(), 32767.0f / 32768.0f);
        r.require(edge.write(overloaded.data(), 24000) && edge.close(), "encode full-scale PCM16 edge");
        r.require(edge.clippedSamples() > 0 && edge.clippedSamples() < 48000, "dither-induced saturation is also counted");
        MfMp3Sink broken;
        r.require(broken.open(dir.path.getChildFile("failed.mp3"), 48000, 320, 1000), "open injected MP3 storage failure");
        r.require(!broken.write(overloaded.data(), 24000) && !broken.close() && broken.errorCode() == Error::writeFailed, "MP3 storage failure retained");
    });
}
}
