#include "WavTrackWriter.h"
#include "diagnostics/CaptureTelemetry.h"
#include "storage/StorageEncoding.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <thread>

namespace gocue::recorder
{
namespace
{
// Runtime-sized equivalent of support/BoundedSpscQueue's reserve/commit protocol.
// Slot storage is fixed at preparation, and a consumer holds its slot throughout I/O.
class PcmQueue
{
public:
    struct Block { std::uint32_t frames = 0; std::uint64_t first = 0; std::int64_t qpc = 0; };
    PcmQueue(std::uint32_t rate, std::uint32_t channels, std::uint32_t blockFrames)
        : mics(channels), maxFrames(blockFrames), slots((static_cast<std::uint64_t>(rate) * WavTrackWriter::queueSeconds + blockFrames - 1) / blockFrames),
          blocks(static_cast<std::size_t>(slots)), pcm(static_cast<std::size_t>(slots) * maxFrames * mics, 0) {}
    bool push(const std::int32_t* source, std::uint32_t frames, std::uint64_t first, std::int64_t qpc) noexcept
    {
        const auto w = written.load(std::memory_order_relaxed);
        if (w - read.load(std::memory_order_acquire) == slots) return false;
        const auto i = static_cast<std::size_t>(w % slots);
        std::memcpy(pcm.data() + i * maxFrames * mics, source, static_cast<std::size_t>(frames) * mics * sizeof(*source));
        blocks[i] = {frames, first, qpc};
        const auto queued = pendingFrames.fetch_add(frames, std::memory_order_relaxed) + frames;
        highWater = std::max(highWater, queued); // Producer only, read after join.
        written.store(w + 1, std::memory_order_release);
        return true;
    }
    const std::int32_t* peek(Block& block) const noexcept
    {
        const auto r = read.load(std::memory_order_relaxed);
        if (r == written.load(std::memory_order_acquire)) return nullptr;
        const auto i = static_cast<std::size_t>(r % slots); block = blocks[i];
        return pcm.data() + i * maxFrames * mics;
    }
    void release(std::uint32_t frames) noexcept
    {
        pendingFrames.fetch_sub(frames, std::memory_order_relaxed);
        read.fetch_add(1, std::memory_order_release);
    }
    const std::uint32_t mics, maxFrames;
    const std::uint64_t slots;
    std::atomic<std::uint64_t> pendingFrames{0};
    std::uint64_t highWater = 0;
private:
    std::vector<Block> blocks;
    std::vector<std::int32_t> pcm;
    alignas(64) std::atomic<std::uint64_t> written{0};
    alignas(64) std::atomic<std::uint64_t> read{0};
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<WavTrackWriter::State>::is_always_lock_free);
static_assert(std::atomic<WavTrackWriter::Error>::is_always_lock_free);

WavTrackWriter::Config checkedConfig(WavTrackWriter::Config c)
{
    if (c.sampleRate == 0 || c.mics == 0 || c.mics > 8 || c.framesPerBlock == 0 || c.framesPerBlock > 16384
        || static_cast<std::uint64_t>(c.sampleRate) * 30 * 3 + 37 >= std::numeric_limits<std::uint32_t>::max()
        || c.takeId.isNull() || c.devices.size() != c.mics || c.projectDirectory == juce::File())
        throw std::invalid_argument("Invalid WAV configuration (Fs, 1..8 mics, block size, mapping, take ID, or RIFF size)");
    std::array<bool, 8> seen{};
    for (const auto& d : c.devices)
    {
        if (d.mic < 1 || d.mic > static_cast<int>(c.mics) || seen[static_cast<std::size_t>(d.mic - 1)]
            || d.activeIndex < 0 || d.physicalIndex < 0 || d.deviceId.isEmpty())
            throw std::invalid_argument("Invalid microphone/device mapping");
        seen[static_cast<std::size_t>(d.mic - 1)] = true;
    }
    return c;
}
std::array<std::uint8_t, 44> wavHeader(std::uint32_t rate, std::uint64_t frames)
{
    using storageEncoding::put;
    std::array<std::uint8_t, 44> h{};
    const auto size = static_cast<std::uint32_t>(frames * 3);
    std::memcpy(h.data(), "RIFF", 4); put(h.data() + 4, 36u + size + (size & 1u));
    std::memcpy(h.data() + 8, "WAVEfmt ", 8); put(h.data() + 16, 16u);
    put(h.data() + 20, std::uint16_t{1}); put(h.data() + 22, std::uint16_t{1});
    put(h.data() + 24, rate); put(h.data() + 28, rate * 3);
    put(h.data() + 32, std::uint16_t{3}); put(h.data() + 34, std::uint16_t{24});
    std::memcpy(h.data() + 36, "data", 4); put(h.data() + 40, size);
    return h;
}
}

struct WavTrackWriter::Impl
{
    explicit Impl(Config c) : config(checkedConfig(std::move(c))), queue(config.sampleRate, config.mics, config.framesPerBlock),
        journal(config.faults), packed(static_cast<std::size_t>(config.framesPerBlock) * 3), frequency(qpcFrequency()) {}
    struct Track
    {
        explicit Track(FileIoFaultAdapter* fault) : file(fault) {}
        DurableFile file;
        juce::String path;
        bool pad = false;
    };
    struct CheckpointStamp { std::uint64_t samples = 0; std::int64_t header = 0, media = 0, journal = 0; };
    Config config;
    PcmQueue queue;
    RecordingJournal journal;
    std::vector<std::unique_ptr<Track>> tracks;
    std::vector<std::uint8_t> packed;
    std::thread worker;
    std::atomic<State> current{State::idle};
    std::atomic<Error> failure{Error::none};
    std::atomic<bool> stopRequested{false};
    std::atomic<std::uint64_t> written{0}, mediaDurable{0}, journalDurable{0};
    mutable std::mutex messageMutex;
    juce::String errorMessage;
    std::int64_t nstop = 0;
    juce::Uuid editId;
    std::uint64_t chunk = 1, inChunk = 0, checkpointCount = 0, headerUpdates = 0, closedChunkSets = 0;
    bool dirty = false;
    std::int64_t lastHeader = 0, lastCheckpoint = 0, frequency;
    Distribution queueWait, completionDelay, headerIntervals, flushTimes;
    std::array<CheckpointStamp, 128> trace{};
    double ms(std::int64_t ticks) const { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency); }
    std::int64_t origin() const { return config.usesOutputOrigin ? config.o0 : config.n0; }
    void fail(Error code, const juce::String& message)
    {
        { std::lock_guard<std::mutex> lock(messageMutex); if (errorMessage.isEmpty()) errorMessage = message; }
        auto expected = Error::none;
        failure.compare_exchange_strong(expected, code, std::memory_order_release);
        current.store(State::failed, std::memory_order_release);
    }
    bool io(const juce::Result& r)
    {
        if (r.failed()) { fail(Error::io, r.getErrorMessage()); return false; }
        return true;
    }
    juce::Result result() const
    {
        std::lock_guard<std::mutex> lock(messageMutex);
        if (!errorMessage.isEmpty()) return juce::Result::fail(errorMessage);
        switch (failure.load(std::memory_order_acquire))
        {
            case Error::none: return juce::Result::ok();
            case Error::queueOverflow: return juce::Result::fail("PCM queue overflow; capture must stop, no silence inserted");
            case Error::invalidBlock: return juce::Result::fail("Invalid PCM input block");
            default: return juce::Result::fail("WAV writer failed");
        }
    }
    bool openChunk()
    {
        tracks.clear();
        const auto header = wavHeader(config.sampleRate, 0);
        for (unsigned mic = 1; mic <= config.mics; ++mic)
        {
            auto t = std::make_unique<Track>(config.faults);
            t->path = WavTrackWriter::chunkPath(config.takeId, mic, chunk);
            const auto file = config.projectDirectory.getChildFile(t->path);
            if (!io(file.getParentDirectory().createDirectory()) || !io(t->file.open(file, DurableFile::OpenMode::createNew))
                || !io(t->file.write(header.data(), header.size())) || !io(t->file.flushData())) return false;
            tracks.push_back(std::move(t));
        }
        inChunk = 0;
        return true;
    }
    bool prepare()
    {
        if (!io(journal.open(config.projectDirectory.getChildFile("journal"), config.journalRotationBytes)) || !openChunk()) return false;
        JournalTakeStarted start;
        start.takeId = config.takeId; start.pcm.sampleRate = config.sampleRate; start.pcm.nativeFormat = config.nativeFormat;
        start.n0 = config.n0; start.o0 = config.o0; start.pstart = config.pstart; start.usesOutputOrigin = config.usesOutputOrigin;
        start.placementMode = config.placementMode;
        start.devices = config.devices;
        start.files = config.additionalFiles;
        if (config.testChunkFrames && (!config.faults || config.testChunkFrames % config.sampleRate != 0))
        { fail(Error::invalidBlock, "Test chunk boundary requires a fault adapter and whole seconds"); return false; }
        for (const auto& t : tracks)
            start.files.push_back({juce::Uuid().toDashedString(), t->path, t->path.upToLastOccurrenceOf("/", true, false) + "{chunk}.wav"});
        if (!io(journal.append(start))) return false;
        lastHeader = lastCheckpoint = qpcNow();
        return true;
    }
    bool checkpoint()
    {
        if (!dirty) return true;
        const auto header = wavHeader(config.sampleRate, inChunk);
        const auto began = qpcNow();
        for (auto& t : tracks)
        {
            if ((inChunk & 1u) && !t->pad)
            {
                const std::uint8_t zero = 0;
                if (!io(t->file.write(&zero, 1))) return false;
                t->pad = true;
            }
            if (!io(t->file.writeAt(0, header.data(), header.size())) || !io(t->file.flushApplicationBuffers())) return false;
            ++headerUpdates;
        }
        const auto headerAt = qpcNow();
        headerIntervals.add(ms(headerAt - lastHeader)); lastHeader = headerAt;
        JournalCheckpoint cp; cp.takeId = config.takeId;
        const auto end = written.load(std::memory_order_relaxed);
        for (auto& t : tracks)
        {
            if (!io(t->file.flushData())) return false;
            cp.files.push_back({t->path, t->file.durableBytes(), inChunk, end - inChunk,
                static_cast<std::int64_t>(end), 1, config.sampleRate, 44, 3});
        }
        const auto mediaAt = qpcNow(); mediaDurable.store(end, std::memory_order_release);
        if (!io(journal.append(cp))) return false;
        const auto journalAt = qpcNow(); journalDurable.store(end, std::memory_order_release);
        trace[checkpointCount++ % trace.size()] = {end, headerAt, mediaAt, journalAt};
        flushTimes.add(ms(journalAt - began)); lastCheckpoint = journalAt; dirty = false;
        return true;
    }
    bool closeTracks()
    {
        bool ok = true;
        for (auto& t : tracks) if (!io(t->file.close())) ok = false;
        return ok;
    }
    bool process(const PcmQueue::Block& block, const std::int32_t* pcm)
    {
        const auto end = written.load(std::memory_order_relaxed);
        if (block.first != end) { fail(Error::discontinuity, "PCM sample sequence gap/duplicate; take stopped"); return false; }
        if (end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - block.frames)
        { fail(Error::invalidBlock, "Sample position exceeds int64"); return false; }
        for (std::size_t i = 0; i < static_cast<std::size_t>(block.frames) * config.mics; ++i)
            if (pcm[i] < -8388608 || pcm[i] > 8388607)
            { fail(Error::invalidPcm, "Input is outside signed PCM24; no implicit clipping/conversion"); return false; }
        const auto chunkFrames = config.testChunkFrames ? config.testChunkFrames : static_cast<std::uint64_t>(config.sampleRate) * chunkSeconds;
        std::uint32_t consumed = 0;
        while (consumed < block.frames)
        {
            if (failure.load(std::memory_order_acquire) != Error::none) return false;
            if (inChunk == chunkFrames)
            {
                if (!checkpoint() || !closeTracks()) return false;
                ++closedChunkSets; ++chunk;
                if (!openChunk()) return false;
            }
            const auto total = written.load(std::memory_order_relaxed);
            const auto untilCheckpoint = config.sampleRate - total % config.sampleRate;
            const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>({block.frames - consumed, chunkFrames - inChunk, untilCheckpoint}));
            for (std::size_t mic = 0; mic < tracks.size(); ++mic)
            {
                for (std::uint32_t i = 0; i < frames; ++i)
                    WavTrackWriter::packPcm24(pcm[(static_cast<std::size_t>(consumed) + i) * config.mics + mic], packed.data() + i * 3);
                auto& t = *tracks[mic];
                const auto bytes = static_cast<std::size_t>(frames) * 3;
                std::size_t offset = 0;
                if (t.pad)
                {
                    if (!io(t.file.writeAt(t.file.writtenBytes() - 1, packed.data(), 1))) return false;
                    offset = 1; t.pad = false;
                }
                if (!io(t.file.write(packed.data() + offset, bytes - offset))) return false;
            }
            consumed += frames; inChunk += frames; written.store(total + frames, std::memory_order_release); dirty = true;
            if ((total + frames) % config.sampleRate == 0 && !checkpoint()) return false;
        }
        return true;
    }
    void run(std::promise<juce::Result> ready)
    {
        bool announced = false;
        try
        {
            if (prepare())
            {
                current.store(State::running, std::memory_order_release); ready.set_value(juce::Result::ok()); announced = true;
                while (failure.load(std::memory_order_acquire) == Error::none)
                {
                    PcmQueue::Block block;
                    if (const auto* pcm = queue.peek(block))
                    {
                        if (block.qpc) queueWait.add(ms(qpcNow() - block.qpc));
                        const auto ok = process(block, pcm);
                        if (block.qpc) completionDelay.add(ms(qpcNow() - block.qpc));
                        queue.release(block.frames);
                        if (!ok) break;
                    }
                    else if (stopRequested.load(std::memory_order_acquire)) break;
                    else
                    {
                        if (dirty && qpcNow() - lastCheckpoint >= frequency && !checkpoint()) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                if (failure.load(std::memory_order_acquire) == Error::none)
                {
                    const auto samples = static_cast<std::int64_t>(written.load());
                    if (origin() > std::numeric_limits<std::int64_t>::max() - samples || nstop != origin() + samples)
                        fail(Error::discontinuity, "Nstop does not match the accepted PCM sample range");
                    else if (io(journal.append(JournalTakeStopped{config.takeId, nstop, editId}))
                             && checkpoint() && closeTracks() && io(journal.append(JournalTakeFinalized{config.takeId})))
                        ++closedChunkSets;
                }
            }
        }
        catch (const std::exception& e) { fail(Error::internal, juce::String::fromUTF8(e.what())); }
        catch (...) { fail(Error::internal, "Unknown writer worker exception"); }
        closeTracks(); io(journal.close());
        if (failure.load(std::memory_order_acquire) == Error::none) current.store(State::stopped, std::memory_order_release);
        if (!announced) ready.set_value(result());
    }
};

WavTrackWriter::WavTrackWriter(Config c) : impl(std::make_unique<Impl>(std::move(c))) {}
WavTrackWriter::~WavTrackWriter()
{
    // Destruction is not a successful Stop transaction. A forgotten stop leaves a
    // recoverable unfinished take, and never silently invents a placement edit ID.
    if (impl->worker.joinable())
    {
        if (!impl->stopRequested.load()) impl->fail(Error::internal, "Writer destroyed without explicit stop");
        impl->stopRequested.store(true, std::memory_order_release); impl->worker.join();
    }
}
juce::Result WavTrackWriter::start()
{
    auto expected = State::idle;
    if (!impl->current.compare_exchange_strong(expected, State::starting)) return juce::Result::fail("WAV writer is single-use");
    std::promise<juce::Result> ready; auto future = ready.get_future();
    try { impl->worker = std::thread([this, ready = std::move(ready)]() mutable { impl->run(std::move(ready)); }); }
    catch (const std::exception& e) { impl->fail(Error::internal, e.what()); return impl->result(); }
    return future.get();
}
bool WavTrackWriter::tryPush(const std::int32_t* pcm, std::uint32_t frames, std::uint64_t first, std::int64_t qpc) noexcept
{
    if (impl->current.load(std::memory_order_acquire) != State::running) return false;
    Error fault = Error::none;
    if (!pcm || frames == 0 || frames > impl->config.framesPerBlock) fault = Error::invalidBlock;
    else if (!impl->queue.push(pcm, frames, first, qpc)) fault = Error::queueOverflow;
    if (fault == Error::none) return true;
    auto expected = Error::none; impl->failure.compare_exchange_strong(expected, fault, std::memory_order_release);
    impl->current.store(State::failed, std::memory_order_release); return false;
}
juce::Result WavTrackWriter::stop(std::int64_t nstop, const juce::Uuid& editId)
{
    if (!impl->worker.joinable()) return impl->current.load() == State::idle ? juce::Result::fail("WAV writer was not started") : impl->result();
    impl->nstop = nstop; impl->editId = editId;
    auto expected = State::running; impl->current.compare_exchange_strong(expected, State::stopping);
    impl->stopRequested.store(true, std::memory_order_release); impl->worker.join(); return impl->result();
}
WavTrackWriter::State WavTrackWriter::state() const noexcept { return impl->current.load(std::memory_order_acquire); }
WavTrackWriter::Error WavTrackWriter::error() const noexcept { return impl->failure.load(std::memory_order_acquire); }
juce::Result WavTrackWriter::status() const { return impl->result(); }
std::uint64_t WavTrackWriter::queueFrames() const noexcept { return impl->queue.pendingFrames.load(std::memory_order_relaxed); }
std::uint64_t WavTrackWriter::queueCapacityFrames() const noexcept { return impl->queue.slots * impl->config.framesPerBlock; }
std::uint64_t WavTrackWriter::writtenSamples() const noexcept { return impl->written.load(std::memory_order_acquire); }
std::uint64_t WavTrackWriter::mediaDurableSamples() const noexcept { return impl->mediaDurable.load(std::memory_order_acquire); }
std::uint64_t WavTrackWriter::journalDurableSamples() const noexcept { return impl->journalDurable.load(std::memory_order_acquire); }
juce::String WavTrackWriter::chunkPath(const juce::Uuid& take, unsigned mic, std::uint64_t chunk)
{
    return "media/takes/" + take.toDashedString() + "/audio/mic" + juce::String(mic).paddedLeft('0', 2)
        + "/" + juce::String(static_cast<juce::int64>(chunk)).paddedLeft('0', 6) + ".wav";
}
bool WavTrackWriter::packPcm24(std::int32_t sample, std::uint8_t* bytes) noexcept
{
    if (sample < -8388608 || sample > 8388607 || !bytes) return false;
    const auto bits = static_cast<std::uint32_t>(sample);
    bytes[0] = static_cast<std::uint8_t>(bits); bytes[1] = static_cast<std::uint8_t>(bits >> 8); bytes[2] = static_cast<std::uint8_t>(bits >> 16);
    return true;
}
juce::var WavTrackWriter::telemetry() const
{
    if (impl->worker.joinable()) throw std::logic_error("Join writer before reading telemetry");
    auto v = jsonObject();
    jsonSet(v, "writtenSamplesPerMic", jsonInt(writtenSamples())); jsonSet(v, "mediaDurableSamplesPerMic", jsonInt(mediaDurableSamples()));
    jsonSet(v, "journalDurableSamplesPerMic", jsonInt(journalDurableSamples()));
    jsonSet(v, "queueCapacityFrames", jsonInt(queueCapacityFrames())); jsonSet(v, "queueHighWaterFrames", jsonInt(impl->queue.highWater));
    jsonSet(v, "checkpoints", jsonInt(impl->checkpointCount)); jsonSet(v, "headerUpdates", jsonInt(impl->headerUpdates));
    jsonSet(v, "closedChunkSets", jsonInt(impl->closedChunkSets)); jsonSet(v, "queueWait", impl->queueWait.toJson());
    jsonSet(v, "writerCompletionDelay", impl->completionDelay.toJson()); jsonSet(v, "headerUpdateInterval", impl->headerIntervals.toJson());
    jsonSet(v, "checkpointFlushDuration", impl->flushTimes.toJson()); jsonSet(v, "errorCode", static_cast<int>(error()));
    jsonSet(v, "error", status().getErrorMessage().toStdString()); jsonSet(v, "qpcFrequency", jsonInt(static_cast<std::uint64_t>(impl->frequency)));
    jsonSet(v, "timingDefinition", "QPC wall time; header cadence is per microphone set; one checkpoint per Fs samples plus dirty idle >=1s and final tail; stall may exceed 1s");
    jsonSet(v, "percentileDefinition", "nearest rank; 0.1ms upper buckets <=2000ms; larger values use observed max; empty=null");
    jsonSet(v, "tracePolicy", "last 128 committed checkpoints; samples are take-relative exclusive ends; QPC ticks");
    juce::Array<juce::var> trace;
    const auto kept = std::min<std::uint64_t>(impl->checkpointCount, impl->trace.size());
    for (std::uint64_t i = impl->checkpointCount - kept; i < impl->checkpointCount; ++i)
    {
        const auto& stamp = impl->trace[i % impl->trace.size()]; auto s = jsonObject();
        jsonSet(s, "sampleEnd", jsonInt(stamp.samples)); jsonSet(s, "headerQpc", jsonInt(static_cast<std::uint64_t>(stamp.header)));
        jsonSet(s, "mediaFlushQpc", jsonInt(static_cast<std::uint64_t>(stamp.media))); jsonSet(s, "journalFlushQpc", jsonInt(static_cast<std::uint64_t>(stamp.journal))); trace.add(s);
    }
    jsonSet(v, "checkpointsTrace", trace); return v;
}
}
