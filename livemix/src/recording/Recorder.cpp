#include "Recorder.h"
#include "RecordingQueue.h"
#include "WavSink.h"
#include "MfMp3Sink.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

namespace gocue::livemix::recording
{
namespace
{
juce::String cleanNickname(const std::string& name, unsigned track)
{
    juce::String result;
    for (auto c : juce::String::fromUTF8(name.c_str()))
        if (c >= 32 && !(c >= 127 && c <= 159)) result += juce::String::charToString(c);
    result = result.trim();
    for (auto c : juce::String("<>:\"/\\|?*")) result = result.replaceCharacter(c, '_');
    result = result.trimCharactersAtEnd(" .");
    if (result.isEmpty())
        result = track == 0 ? juce::String::fromUTF8("내소리")
                            : juce::String::fromUTF8(track == 1 ? "합방상대1" : "합방상대2");
    return result;
}
std::uint64_t threadCpuTicks()
{
    FILETIME creation {}, end {}, kernel {}, user {};
    if (!GetThreadTimes(GetCurrentThread(), &creation, &end, &kernel, &user)) return 0;
    auto ticks = [](FILETIME t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return ticks(kernel) + ticks(user);
}
}
struct Recorder::Impl
{
    explicit Impl(TestHooks h) : hooks(h) {}
    const TestHooks hooks;
    mutable std::mutex mutex;
    std::thread worker;
    std::atomic<State> state { State::idle };
    std::atomic<Error> failure { Error::none };
    std::atomic<bool> done { true };
    std::atomic<unsigned> callbacks { 0 };
    std::atomic<std::uint64_t> captured { 0 }, cpuTicks { 0 };
    std::atomic<double> elapsed { 0.0 };
    std::atomic<RecordingQueue*> active { nullptr };
    std::vector<std::unique_ptr<RecordingQueue>> queues; // ownership moves only on control/writer threads
    Snapshot published;
    StartRequest request;
    std::array<std::string, 2> firstOccupant;
    std::array<bool, 2> haveOccupant {};
    std::array<bool, 3> seen {};
    std::array<juce::String, 4> bases;
    juce::File directory;
    unsigned part = 1;
    double currentRate = 0;
    std::uint32_t currentEpoch = 0;
    struct File
    {
        FileSnapshot info;
        std::unique_ptr<WavSink> wav;
        std::unique_ptr<MfMp3Sink> mp3;
    };
    std::vector<File> files;
    struct Saved { unsigned track; juce::File path; unsigned part; };
    std::vector<Saved> saved;
    std::chrono::steady_clock::time_point lastCheckpoint;

    void fail(Error error, const juce::String& detail)
    {
        auto expected = Error::none;
        if (failure.compare_exchange_strong(expected, error))
        {
            std::lock_guard<std::mutex> lock(mutex);
            published.errorDetail = detail.toStdString();
        }
    }
    void publish()
    {
        std::lock_guard<std::mutex> lock(mutex);
        published.fileCount = static_cast<unsigned>(files.size());
        published.files = {};
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            auto& f = files[i];
            if (f.wav)
            {
                f.info.writtenFrames = f.wav->writtenFrames();
                f.info.checkpointFrames = f.wav->checkpointFrames();
            }
            if (f.mp3)
            {
                f.info.writtenFrames = f.mp3->writtenFrames();
                f.info.checkpointFrames = f.mp3->checkpointFrames();
                f.info.clippedSamples = f.mp3->clippedSamples();
            }
            published.files[i] = f.info;
        }
    }
    juce::String extension() const { return request.options.format == Format::wav ? ".wav" : ".mp3"; }
    juce::File pathFor(unsigned track, unsigned number, const juce::String& base) const
    {
        juce::ignoreUnused(track);
        return directory.getChildFile(base + (number > 1 ? "_" + juce::String(number) : "") + extension());
    }
    bool choosePaths()
    {
        juce::File parent = request.options.directoryUtf8.empty()
            ? juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile(juce::String::fromUTF8("LiveMix 녹음"))
            : juce::File(juce::String::fromUTF8(request.options.directoryUtf8.c_str()));
        const auto created = parent.createDirectory();
        if (created.failed()) { fail(Error::openFailed, created.getErrorMessage()); return false; }
        const auto stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d_%H-%M-%S");
        if (request.collabAtStart)
        {
            directory = parent.getNonexistentChildFile(stamp, "", true);
            const auto made = directory.createDirectory();
            if (made.failed()) { fail(Error::openFailed, made.getErrorMessage()); return false; }
            for (unsigned i = 0; i < 3; ++i)
                bases[i] = juce::String::charToString(static_cast<juce::juce_wchar>('A' + i)) + "_"
                    + cleanNickname(request.people[i].participantId || i == 0 ? request.people[i].nicknameUtf8 : "", i);
            bases[3] = juce::String::fromUTF8("D_전체");
        }
        else
        {
            directory = parent;
            bases[3] = parent.getNonexistentChildFile("LiveMix_" + stamp, extension(), true).getFileNameWithoutExtension();
        }
        return true;
    }
    bool openPart(const DeviceFormat& format)
    {
        files.clear();
        currentRate = format.sampleRate; currentEpoch = format.formatEpoch;
        const unsigned first = request.collabAtStart ? 0u : 3u;
        for (unsigned t = first; t != 4; ++t)
        {
            File f;
            const auto path = pathFor(t, part, bases[t]);
            f.info.track = static_cast<Track>(t); f.info.part = part;
            f.info.pathUtf8 = path.getFullPathName().toStdString();
            f.info.sampleRate = static_cast<unsigned>(format.sampleRate);
            bool ok;
            if (request.options.format == Format::wav)
            {
                f.wav = std::make_unique<WavSink>();
                ok = f.wav->open(path, f.info.sampleRate, hooks.wavLimitBytes, hooks.failAfterBytes);
                if (!ok) fail(Error::openFailed, "WAV open: " + path.getFullPathName() + " " + f.wav->error());
            }
            else
            {
                f.mp3 = std::make_unique<MfMp3Sink>();
                ok = f.mp3->open(path, format.sampleRate, request.options.mp3Kbps, hooks.failAfterBytes, hooks.encoderUnavailable);
                f.info.sampleRate = f.mp3->sampleRate();
                if (!ok) fail(f.mp3->errorCode(), f.mp3->error());
            }
            files.push_back(std::move(f));
            if (path.existsAsFile()) saved.push_back({ t, path, part });
            if (!ok) return false;
        }
        lastCheckpoint = std::chrono::steady_clock::now();
        publish();
        return true;
    }
    bool closePart()
    {
        bool ok = true;
        for (auto& f : files)
        {
            if (f.wav && !f.wav->close()) { fail(Error::writeFailed, f.wav->error()); ok = false; }
            if (f.mp3 && !f.mp3->close()) { fail(f.mp3->errorCode(), f.mp3->error()); ok = false; }
        }
        publish();
        return ok;
    }
    bool checkpoint()
    {
        for (auto& f : files)
        {
            if (f.wav && !f.wav->checkpoint()) { fail(Error::writeFailed, f.wav->error()); return false; }
            if (f.mp3 && !f.mp3->checkpoint()) { fail(f.mp3->errorCode(), f.mp3->error()); return false; }
        }
        lastCheckpoint = std::chrono::steady_clock::now();
        publish();
        return true;
    }
    bool writeBlock(const RecordingQueue::Block& block, const RecordingQueue::Frame* audio)
    {
        for (unsigned t = 0; t != 3; ++t) seen[t] = seen[t] || (block.occupied & (1u << t)) != 0;
        int offset = 0;
        while (offset < block.frames)
        {
            int count = block.frames - offset;
            if (request.options.format == Format::wav)
            {
                const auto remaining = files.front().wav->remainingFrames();
                if (!remaining)
                {
                    if (!closePart()) return false;
                    ++part;
                    if (!openPart(block.format)) return false;
                    continue;
                }
                count = static_cast<int>(std::min<std::uint64_t>(count, remaining));
            }
            std::array<std::vector<std::int32_t>, 4> pcm;
            std::array<std::vector<float>, 4> floats;
            std::array<std::uint64_t, 4> clips {};
            if (request.options.format == Format::wav)
            {
                for (auto& p : pcm) p.resize(static_cast<std::size_t>(count) * 2);
                for (int i = 0; i < count; ++i)
                    for (int ch = 0; ch != 2; ++ch)
                    {
                        std::int64_t sum = 0;
                        for (unsigned t = 0; t != 3; ++t)
                        {
                            const auto q = WavSink::quantize(audio[offset+i].samples[t*2+ch], clips[t]);
                            pcm[t][static_cast<std::size_t>(i*2+ch)] = q; sum += q;
                        }
                        pcm[3][static_cast<std::size_t>(i*2+ch)] = WavSink::saturate(sum, clips[3]);
                    }
            }
            else
            {
                for (auto& p : floats) p.resize(static_cast<std::size_t>(count) * 2);
                for (int i = 0; i < count; ++i)
                    for (int ch = 0; ch != 2; ++ch)
                    {
                        float sum = 0;
                        for (unsigned t = 0; t != 3; ++t)
                        {
                            const auto value = audio[offset+i].samples[t*2+ch];
                            floats[t][static_cast<std::size_t>(i*2+ch)] = value; sum += value;
                        }
                        floats[3][static_cast<std::size_t>(i*2+ch)] = sum;
                    }
            }
            for (auto& f : files)
            {
                const auto t = static_cast<unsigned>(f.info.track);
                if (f.wav)
                {
                    if (!f.wav->write(pcm[t].data(), count)) { fail(Error::writeFailed, f.wav->error()); return false; }
                    f.info.clippedSamples += clips[t];
                }
                if (f.mp3 && !f.mp3->write(floats[t].data(), count))
                { fail(f.mp3->errorCode(), f.mp3->error()); return false; }
            }
            offset += count;
        }
        return true;
    }
    void cleanup()
    {
        if (!request.collabAtStart) return;
        std::array<std::string, 2> names;
        std::array<bool, 2> known;
        { std::lock_guard<std::mutex> lock(mutex); names = firstOccupant; known = haveOccupant; }
        for (unsigned seat = 1; seat != 3; ++seat)
        {
            if (!seen[seat])
            {
                for (const auto& s : saved)
                    if (s.track == seat && !s.path.deleteFile())
                    { fail(Error::writeFailed, "could not remove unused seat file: " + s.path.getFullPathName()); return; }
                files.erase(std::remove_if(files.begin(), files.end(), [seat](const File& f)
                    { return static_cast<unsigned>(f.info.track) == seat; }), files.end());
                continue;
            }
            if (request.people[seat].participantId || !known[seat-1]) continue;
            const auto base = juce::String::charToString(static_cast<juce::juce_wchar>('A' + seat)) + "_"
                + cleanNickname(names[seat-1], seat);
            if (base == bases[seat]) continue;
            std::vector<std::pair<juce::File, juce::File>> renames;
            bool available = true;
            for (const auto& s : saved)
                if (s.track == seat)
                {
                    auto to = pathFor(seat, s.part, base);
                    if (to.exists()) { available = false; break; }
                    renames.emplace_back(s.path, to);
                }
            if (!available) continue;
            std::size_t moved = 0;
            // MoveFileW never replaces an existing destination, unlike File::moveFileTo.
            for (; moved < renames.size(); ++moved)
                if (!MoveFileW(renames[moved].first.getFullPathName().toWideCharPointer(),
                               renames[moved].second.getFullPathName().toWideCharPointer())) break;
            if (moved != renames.size())
            {
                while (moved > 0)
                {
                    --moved;
                    MoveFileW(renames[moved].second.getFullPathName().toWideCharPointer(),
                              renames[moved].first.getFullPathName().toWideCharPointer());
                }
                continue; // best effort; rename failure does not turn a completed take into an error
            }
            for (auto& f : files)
                if (static_cast<unsigned>(f.info.track) == seat)
                    f.info.pathUtf8 = pathFor(seat, f.info.part, base).getFullPathName().toStdString();
        }
    }
    void run()
    {
        const auto initialCpu = threadCpuTicks();
        try
        {
            RecordingQueue* queue;
            { std::lock_guard<std::mutex> lock(mutex); queue = queues.front().get(); }
            if (choosePaths() && openPart(queue->format))
            {
                auto expected = State::starting;
                state.compare_exchange_strong(expected, State::recording);
                std::size_t index = 0;
                std::vector<RecordingQueue::Frame> buffer(static_cast<std::size_t>(queue->format.maxBlockFrames));
                for (;;)
                {
                    // Deterministic FIFO-pressure seam. Never used by production.
                    if (hooks.writerGate && !hooks.writerGate->load() && state == State::recording && failure == Error::none)
                    { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
                    RecordingQueue::Block block;
                    if (queue->pop(block, buffer.data()))
                    {
                        if (!writeBlock(block, buffer.data())) break;
                        publish();
                    }
                    else if (queue->sealed.load(std::memory_order_acquire))
                    {
                        if (!queue->empty()) continue;
                        // A sealed generation cannot receive more blocks. All old samples precede the next part.
                        RecordingQueue* next = nullptr;
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            if (index + 1 < queues.size())
                            {
                                next = queues[index + 1].get();
                                queues[index++].reset(); // sealed, consumed, no longer visible to audio producer
                            }
                        }
                        if (next)
                        {
                            queue = next;
                            if (queue->format.sampleRate != currentRate || queue->format.formatEpoch != currentEpoch)
                            {
                                if (!closePart()) break;
                                ++part;
                                if (!openPart(queue->format)) break;
                            }
                            buffer.resize(static_cast<std::size_t>(queue->format.maxBlockFrames));
                            continue;
                        }
                    }
                    else if ((state == State::stopping || failure != Error::none) && callbacks.load() == 0)
                    {
                        // Recheck after acquiring callback completion, including its final queue publication.
                        if (queue->empty()) break;
                    }
                    else std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    if (std::chrono::steady_clock::now() - lastCheckpoint >= std::chrono::milliseconds(checkpointMs))
                        if (!checkpoint()) break;
                    cpuTicks.store(threadCpuTicks() - initialCpu);
                }
            }
            closePart();
            if (failure == Error::none) cleanup();
            publish();
        }
        catch (const std::exception& e)
        {
            fail(Error::writeFailed, juce::String::fromUTF8(e.what()));
            closePart();
        }
        // Release every sink on its COM-owning thread, including failed startup.
        files.clear();
        if (failure == Error::queueOverflow)
        { std::lock_guard<std::mutex> lock(mutex); published.errorDetail = "recording FIFO overflow"; }
        cpuTicks.store(threadCpuTicks() - initialCpu);
        state.store(failure == Error::none ? State::idle : State::error);
        done.store(true);
    }
};
Recorder::Recorder() : Recorder(TestHooks {}) {}
Recorder::Recorder(TestHooks hooks) : impl(std::make_unique<Impl>(hooks)) {}
Recorder::~Recorder() { finish(); }
bool Recorder::prepareFormat(const DeviceFormat& format)
{
    if (!std::isfinite(format.sampleRate) || format.sampleRate < 1
        || format.sampleRate > static_cast<double>(std::numeric_limits<int>::max()) / queueSeconds
        || std::floor(format.sampleRate) != format.sampleRate || format.maxBlockFrames <= 0
        || format.maxBlockFrames > format.sampleRate * queueSeconds || format.formatEpoch == 0) return false;
    std::unique_ptr<RecordingQueue> queue;
    try { queue = std::make_unique<RecordingQueue>(format); }
    catch (const std::bad_alloc&) { return false; }
    auto& p = *impl;
    std::lock_guard<std::mutex> lock(p.mutex);
    auto* old = p.active.load();
    if (p.done) { p.queues.clear(); old = nullptr; }
    p.queues.push_back(std::move(queue));
    p.active.store(p.queues.back().get());
    if (old) old->sealed.store(true, std::memory_order_release);
    return true;
}
bool Recorder::start(const StartRequest& request)
{
    auto& p = *impl;
    if (!p.done.load() || (p.state != State::idle && p.state != State::error)) return false;
    if (p.worker.joinable()) p.worker.join();
    if (!p.active || (request.options.format != Format::wav && request.options.format != Format::mp3)
        || std::find(mp3BitratesKbps.begin(), mp3BitratesKbps.end(), request.options.mp3Kbps) == mp3BitratesKbps.end()
        || (!request.options.directoryUtf8.empty()
            && !juce::File::isAbsolutePath(juce::String::fromUTF8(request.options.directoryUtf8.c_str()))))
    {
        p.failure = Error::invalidOptions; p.state = State::error;
        std::lock_guard<std::mutex> lock(p.mutex); p.published.errorDetail = "invalid recording options or device format";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        auto prepared = std::move(p.queues.back()); p.queues.clear(); prepared->reset();
        p.queues.push_back(std::move(prepared));
        p.published = {}; p.published.options = request.options; p.published.collabAtStart = request.collabAtStart;
        p.request = request; p.firstOccupant = {}; p.haveOccupant = {}; p.seen = {}; p.saved.clear();
    }
    p.part = 1; p.captured = 0; p.elapsed = 0; p.cpuTicks = 0; p.failure = Error::none;
    p.done = false; p.state = State::starting;
    p.worker = std::thread([&p] { p.run(); });
    return true;
}
void Recorder::requestStop() noexcept
{
    auto& p = *impl;
    auto state = p.state.load();
    while (state == State::starting || state == State::recording)
        if (p.state.compare_exchange_weak(state, State::stopping)) break;
}
void Recorder::push(collab::contracts::AudioSourceView master, collab::contracts::AudioSourceView remote1,
                    collab::contracts::AudioSourceView remote2, int n) noexcept
{
    if (n <= 0) return;
    auto& p = *impl;
    p.callbacks.fetch_add(1);
    if (p.state.load() == State::recording && p.failure.load() == Error::none)
    {
        auto* queue = p.active.load();
        const auto first = p.captured.load(std::memory_order_relaxed);
        if (!queue->push(master, remote1, remote2, n, first))
        {
            auto expected = Error::none;
            p.failure.compare_exchange_strong(expected, Error::queueOverflow);
        }
        else
        {
            p.captured.store(first + static_cast<unsigned>(n), std::memory_order_relaxed);
            p.elapsed.store(p.elapsed.load(std::memory_order_relaxed) + n / queue->format.sampleRate, std::memory_order_relaxed);
        }
    }
    p.callbacks.fetch_sub(1);
}
void Recorder::noteSeatOccupant(unsigned slot, const TrackIdentity& identity)
{
    if (slot < 1 || slot > 2 || !identity.participantId) return;
    auto& p = *impl;
    std::lock_guard<std::mutex> lock(p.mutex);
    if ((p.state == State::starting || p.state == State::recording) && p.request.collabAtStart
        && !p.request.people[slot].participantId && !p.haveOccupant[slot-1])
    { p.firstOccupant[slot-1] = identity.nicknameUtf8; p.haveOccupant[slot-1] = true; }
}
Snapshot Recorder::snapshot() const
{
    auto& p = *impl;
    std::lock_guard<std::mutex> lock(p.mutex);
    auto result = p.published;
    result.error = p.failure.load(); result.state = result.error == Error::none ? p.state.load() : State::error;
    result.capturedFrames = p.captured.load(); result.elapsedSeconds = p.elapsed.load();
    return result;
}
void Recorder::finish()
{
    requestStop();
    // Also cover storage/encoder errors, where the writer can exit before an in-flight copy finishes.
    while (impl->callbacks.load() != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (impl->worker.joinable()) impl->worker.join();
}
double Recorder::writerCpuSeconds() const noexcept { return static_cast<double>(impl->cpuTicks.load()) / 10000000.0; }
static_assert(std::atomic<double>::is_always_lock_free);
}
