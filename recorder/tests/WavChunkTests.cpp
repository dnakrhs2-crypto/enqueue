#include "record/WavTrackWriter.h"
#include "storage/StorageEncoding.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace gocue::recorder;
namespace
{
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void success(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
template<class Predicate> void until(Predicate ready)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready())
    {
        require(std::chrono::steady_clock::now() < end, "Worker did not reach expected state within 10s");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
WavTrackWriter::Config config(unsigned rate = 1001, unsigned mics = 3, unsigned block = 137)
{
    WavTrackWriter::Config c;
    c.projectDirectory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("RecorderR06-wav-" + juce::Uuid().toString());
    c.sampleRate = rate; c.mics = mics; c.framesPerBlock = block; c.n0 = 123456; c.pstart = 765;
    for (unsigned mic = 1; mic <= mics; ++mic)
        c.devices.push_back({"synthetic-fixture", "Mic " + juce::String(mic), static_cast<int>(mic), static_cast<int>(mic - 1), static_cast<int>(mic * 2)});
    return c;
}
std::int32_t pattern(std::uint64_t sample, unsigned mic)
{
    const auto bits = (static_cast<std::uint32_t>(sample) * 7919u + mic * 104729u) & 0xffffffu;
    return bits & 0x800000u ? static_cast<std::int32_t>(bits) - 16777216 : static_cast<std::int32_t>(bits);
}
void feed(WavTrackWriter& w, const WavTrackWriter::Config& c, std::uint64_t first, std::uint64_t count)
{
    std::vector<std::int32_t> pcm(static_cast<std::size_t>(c.framesPerBlock) * c.mics);
    for (std::uint64_t i = 0; i < count;)
    {
        const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(c.framesPerBlock, count - i));
        for (unsigned frame = 0; frame < n; ++frame)
            for (unsigned mic = 0; mic < c.mics; ++mic) pcm[static_cast<std::size_t>(frame) * c.mics + mic] = pattern(first + i + frame, mic);
        // Accelerated non-RT fixture producer: wait BEFORE push, never retry a lost block.
        until([&] { return w.queueFrames() + n <= w.queueCapacityFrames() || w.error() != WavTrackWriter::Error::none; });
        success(w.status()); require(w.tryPush(pcm.data(), n, first + i), "Prepared queue accepted fixture block"); i += n;
    }
}
juce::MemoryBlock read(const juce::File& file)
{
    juce::MemoryBlock b; require(file.loadFileAsData(b), "Read WAV file"); return b;
}
void verify(const juce::File& file, unsigned rate, unsigned mic, std::uint64_t first, std::uint64_t samples)
{
    const auto b = read(file); const auto* p = static_cast<const std::uint8_t*>(b.getData());
    const auto dataBytes = samples * 3;
    require(b.getSize() == 44 + dataBytes + (dataBytes & 1), "WAV length including RIFF odd-byte padding");
    require(std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WAVEfmt ", 8) == 0 && std::memcmp(p + 36, "data", 4) == 0, "RIFF/fmt/data identity");
    require(storageEncoding::get<std::uint32_t>(p + 4) + 8 == b.getSize() && storageEncoding::get<std::uint32_t>(p + 40) == dataBytes, "Header lengths match actual data");
    require(storageEncoding::get<std::uint16_t>(p + 20) == 1 && storageEncoding::get<std::uint16_t>(p + 22) == 1
        && storageEncoding::get<std::uint16_t>(p + 32) == 3 && storageEncoding::get<std::uint16_t>(p + 34) == 24, "Mono PCM24 format");
    require(storageEncoding::get<std::uint32_t>(p + 24) == rate && storageEncoding::get<std::uint32_t>(p + 28) == rate * 3, "Interface rate preserved");
    for (std::uint64_t i = 0; i < samples; ++i)
    {
        const auto bits = static_cast<std::uint32_t>(pattern(first + i, mic)) & 0xffffffu;
        const auto* s = p + 44 + i * 3;
        require((s[0] | (std::uint32_t{s[1]} << 8) | (std::uint32_t{s[2]} << 16)) == bits, "PCM sample oracle: no missing, duplicate, changed or reordered samples");
    }
    if (dataBytes & 1) require(p[b.getSize() - 1] == 0, "RIFF padding is zero, not an audio sample");
}
struct Fault : FileIoFaultAdapter
{
    std::atomic<bool> armed{false}, entered{false}, release{false};
    bool gate = false, journalFlush = false;
    FileIoOperation operation = FileIoOperation::append;
    juce::Result beforeIo(FileIoOperation op, const juce::File& f, std::uint64_t offset, std::size_t) override
    {
        if (!armed.load()) return juce::Result::ok();
        const bool target = journalFlush ? f.hasFileExtension("log") : f.hasFileExtension("wav");
        if (!target || op != operation || (op == FileIoOperation::append && offset < 44)) return juce::Result::ok();
        entered.store(true);
        if (gate)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!release.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return release.load() ? juce::Result::ok() : juce::Result::fail("Fixture gate timeout");
        }
        return juce::Result::fail("Injected storage I/O failure");
    }
};
struct ReleaseGate { Fault& fault; ~ReleaseGate() { fault.release.store(true); } };
bool finalized(const juce::File& dir)
{
    JournalReplay replay; success(RecordingJournal::replay(dir.getChildFile("journal"), replay));
    for (const auto& r : replay.records) if (r.kind == JournalKind::TakeFinalized) return true;
    return false;
}
}
int runWavChunkTests()
{
    unsigned passed = 0, failed = 0;
    const auto test = [&](const char* name, const std::function<void()>& body)
    {
        try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("PCM24 LE golden bytes: zero, LSB, signs and extrema", []
    {
        const std::int32_t values[] = {0, 1, -1, 8388607, -8388608, 256, -256};
        const std::uint8_t golden[][3] = {{0,0,0}, {1,0,0}, {255,255,255}, {255,255,127}, {0,0,128}, {0,1,0}, {0,255,255}};
        for (std::size_t i = 0; i < std::size(values); ++i)
        {
            std::uint8_t bytes[3]{}; require(WavTrackWriter::packPcm24(values[i], bytes), "Valid PCM24 sample");
            require(std::memcmp(bytes, golden[i], 3) == 0, "PCM24 golden bytes");
        }
        std::uint8_t bytes[3]{};
        require(!WavTrackWriter::packPcm24(8388608, bytes) && !WavTrackWriter::packPcm24(-8388609, bytes), "Out-of-range is an error, no implicit saturation");
    });
    test("Thirty-second chunks split straddling blocks at identical microphone boundaries", []
    {
        const auto c = config(); WavTrackWriter w(c); success(w.start());
        const std::uint64_t total = c.sampleRate * 60ull + 17;
        feed(w, c, 0, total); success(w.stop(c.n0 + static_cast<std::int64_t>(total), juce::Uuid()));
        require(w.state() == WavTrackWriter::State::stopped && w.journalDurableSamples() == total, "Full range committed");
        for (unsigned mic = 1; mic <= c.mics; ++mic)
            for (std::uint64_t chunk = 1; chunk <= 3; ++chunk)
                verify(c.projectDirectory.getChildFile(WavTrackWriter::chunkPath(c.takeId, mic, chunk)), c.sampleRate, mic - 1,
                    (chunk - 1) * c.sampleRate * 30ull, chunk == 3 ? 17 : c.sampleRate * 30ull);
        JournalReplay journal; success(RecordingJournal::replay(c.projectDirectory.getChildFile("journal"), journal));
        require(journal.records.front().kind == JournalKind::TakeStarted && journal.records.back().kind == JournalKind::TakeFinalized, "Start/finalize lifecycle");
        std::uint64_t checkpoints = 0;
        for (const auto& r : journal.records)
            if (r.kind == JournalKind::Checkpoint)
            {
                ++checkpoints; const auto* files = r.payload["files"].getArray(); require(files && files->size() == static_cast<int>(c.mics), "All microphones represented");
                const auto samples = static_cast<juce::int64>((*files)[0]["validSamples"]);
                for (const auto& f : *files) require(static_cast<juce::int64>(f["validSamples"]) == samples, "Common sample boundary in journal");
            }
        require(checkpoints == 61, "Sixty exact sample-second checkpoints plus final tail");
    });
    test("Exact thirty-second stop produces one chunk without an empty successor", []
    {
        const auto c = config(100, 2, 64); WavTrackWriter w(c); success(w.start()); feed(w, c, 0, 3000);
        success(w.stop(c.n0 + 3000, juce::Uuid()));
        for (unsigned mic = 1; mic <= c.mics; ++mic)
        {
            verify(c.projectDirectory.getChildFile(WavTrackWriter::chunkPath(c.takeId, mic, 1)), c.sampleRate, mic - 1, 0, 3000);
            require(!c.projectDirectory.getChildFile(WavTrackWriter::chunkPath(c.takeId, mic, 2)).exists(), "No empty successor at exact boundary");
        }
    });
    test("Live one-second header is flushed before checkpoint; odd PCM padding is overwritten on append", []
    {
        const auto c = config(101, 2, 101); WavTrackWriter w(c); success(w.start()); feed(w, c, 0, 101);
        until([&] { return w.journalDurableSamples() == 101 || w.error() != WavTrackWriter::Error::none; }); success(w.status());
        for (unsigned mic = 1; mic <= c.mics; ++mic)
            verify(c.projectDirectory.getChildFile(WavTrackWriter::chunkPath(c.takeId, mic, 1)), c.sampleRate, mic - 1, 0, 101);
        feed(w, c, 101, 1); success(w.stop(c.n0 + 102, juce::Uuid()));
        for (unsigned mic = 1; mic <= c.mics; ++mic)
            verify(c.projectDirectory.getChildFile(WavTrackWriter::chunkPath(c.takeId, mic, 1)), c.sampleRate, mic - 1, 0, 102);
        const auto telemetry = w.telemetry();
        for (const auto& cp : *telemetry["checkpointsTrace"].getArray())
            require(static_cast<juce::int64>(cp["headerQpc"]) <= static_cast<juce::int64>(cp["mediaFlushQpc"])
                && static_cast<juce::int64>(cp["mediaFlushQpc"]) <= static_cast<juce::int64>(cp["journalFlushQpc"]), "Header -> all media flush -> journal flush ordering");
    });
    test("WAV append failure is immediately visible and never finalizes/synthesizes silence", []
    {
        Fault fault; auto c = config(100, 2, 25); c.faults = &fault; WavTrackWriter w(c); success(w.start()); fault.armed.store(true);
        std::int32_t pcm[50]{}; require(w.tryPush(pcm, 25, 0), "Block queued before injected failure");
        until([&] { return w.state() == WavTrackWriter::State::failed; });
        require(w.error() == WavTrackWriter::Error::io && w.status().failed(), "Worker publishes error during capture");
        require(!w.tryPush(pcm, 25, 25), "Capture rejects subsequent blocks after failure");
        require(w.stop(c.n0 + 25, juce::Uuid()).failed(), "Stop propagates write failure");
        require(w.journalDurableSamples() == 0 && !finalized(c.projectDirectory), "No fabricated durable samples/finalized record");
    });
    test("Media and journal flush failures retain distinct conservative watermarks", []
    {
        for (const bool journalFault : {false, true})
        {
            Fault fault; fault.operation = FileIoOperation::flushData; fault.journalFlush = journalFault;
            auto c = config(100, 2, 100); c.faults = &fault; WavTrackWriter w(c); success(w.start()); fault.armed.store(true);
            std::int32_t pcm[200]{}; require(w.tryPush(pcm, 100, 0), "Input queued");
            until([&] { return w.state() == WavTrackWriter::State::failed; });
            require(w.stop(c.n0 + 100, juce::Uuid()).failed(), "OS flush error propagates");
            require(w.mediaDurableSamples() == (journalFault ? 100u : 0u) && w.journalDurableSamples() == 0, "Media flush cannot masquerade as journal commit");
            // A complete but unacknowledged journal commit may replay after a simulated
            // OS flush failure. Only the published durable acknowledgement is zero.
            require(!finalized(c.projectDirectory), "Failed flush never finalizes");
        }
    });
    test("Header update failure propagates", []
    {
        Fault fault; fault.operation = FileIoOperation::patch; auto c = config(100, 1, 100); c.faults = &fault;
        WavTrackWriter w(c); success(w.start()); fault.armed.store(true); std::int32_t pcm[100]{};
        require(w.tryPush(pcm, 100, 0), "Input queued"); until([&] { return w.error() != WavTrackWriter::Error::none; });
        require(w.stop(c.n0 + 100, juce::Uuid()).failed() && w.journalDurableSamples() == 0, "Header failure blocks checkpoint");
    });
    test("Queue overflow under a held I/O operation is reported without waiting for writer", []
    {
        Fault fault; fault.gate = true; auto c = config(8, 1, 1); c.faults = &fault; WavTrackWriter w(c); ReleaseGate release{fault};
        success(w.start()); fault.armed.store(true); const std::int32_t pcm = 7;
        require(w.tryPush(&pcm, 1, 0), "First block"); until([&] { return fault.entered.load(); });
        for (std::uint64_t i = 1; i < w.queueCapacityFrames(); ++i) require(w.tryPush(&pcm, 1, i), "Queue capacity is preallocated");
        require(!w.tryPush(&pcm, 1, w.queueCapacityFrames()), "Full queue rejects immediately");
        require(w.error() == WavTrackWriter::Error::queueOverflow && w.state() == WavTrackWriter::State::failed, "Callback publishes overflow without worker cooperation");
        fault.release.store(true); require(w.stop(c.n0 + 33, juce::Uuid()).failed(), "Overflow is not silently repaired");
        require(!finalized(c.projectDirectory), "Overflow leaves unfinished take");
    });
    test("Discontinuous sample numbers and invalid PCM stop the take", []
    {
        for (unsigned variant = 0; variant < 3; ++variant)
        {
            const auto c = config(100, 1, 4); WavTrackWriter w(c); success(w.start());
            std::int32_t pcm[4] = {1, 2, 3, 4}; if (variant == 2) pcm[1] = 8388608;
            if (variant == 1)
            {
                require(w.tryPush(pcm, 4, 0), "Initial block"); until([&] { return w.writtenSamples() == 4; });
            }
            require(w.tryPush(pcm, 4, variant == 0 ? 5 : 0), "Bad sequence/content is validated in worker");
            until([&] { return w.error() != WavTrackWriter::Error::none; });
            require(w.stop(c.n0 + 4, juce::Uuid()).failed(), "Bad input propagates");
            require(w.error() == (variant == 2 ? WavTrackWriter::Error::invalidPcm : WavTrackWriter::Error::discontinuity), "Specific diagnostic");
            require(!finalized(c.projectDirectory), "No finalized invalid original");
        }
    });
    test("Output-clock origin, stop boundary validation and existing-original protection", []
    {
        auto c = config(100, 1, 4); c.usesOutputOrigin = true; c.o0 = 9001;
        { WavTrackWriter w(c); success(w.start()); feed(w, c, 0, 4); success(w.stop(c.o0 + 4, juce::Uuid())); }
        const auto path = c.projectDirectory.getChildFile(WavTrackWriter::chunkPath(c.takeId, 1, 1)); const auto original = read(path);
        { WavTrackWriter w(c); require(w.start().failed(), "Existing take path is never overwritten"); }
        require(read(path) == original, "Original PCM and header unchanged");
        c = config(100, 1, 4); WavTrackWriter w(c); success(w.start()); feed(w, c, 0, 4);
        require(w.stop(c.n0 + 5, juce::Uuid()).failed() && !finalized(c.projectDirectory), "Mismatched Nstop cannot claim a complete take");
    });
    std::cout << "WAV chunk tests: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
