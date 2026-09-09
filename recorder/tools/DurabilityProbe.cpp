#include "record/WavTrackWriter.h"
#include "diagnostics/CaptureTelemetry.h"
#include "storage/StorageEncoding.h"
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace gocue::recorder;
namespace
{
constexpr unsigned sampleRate = 48000, blockFrames = 480;
struct Options
{
    bool synthetic = false;
    unsigned mics = 8, seconds = 60, stallMs = 2000;
    juce::File report;
    juce::String command;
};
unsigned number(const char* text)
{
    unsigned value = 0; const auto end = text + std::strlen(text); const auto result = std::from_chars(text, end, value);
    if (result.ec != std::errc{} || result.ptr != end) throw std::invalid_argument("Expected an unsigned decimal argument");
    return value;
}
Options parse(int argc, char** argv)
{
    Options o;
    for (int i = 0; i < argc; ++i) o.command += (i ? " " : "") + juce::String::fromUTF8(argv[i]).quoted();
    for (int i = 1; i < argc; ++i)
    {
        const std::string option(argv[i]);
        if (option == "--synthetic") { o.synthetic = true; continue; }
        if (i + 1 >= argc) throw std::invalid_argument("Missing option value");
        if (option == "--mics") o.mics = number(argv[++i]);
        else if (option == "--seconds") o.seconds = number(argv[++i]);
        else if (option == "--stall-ms") o.stallMs = number(argv[++i]);
        else if (option == "--report") o.report = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[++i]));
        else throw std::invalid_argument("Unknown option: " + option);
    }
    if (!o.synthetic || o.mics == 0 || o.mics > 8 || o.seconds == 0 || o.stallMs > 60000 || o.report == juce::File())
        throw std::invalid_argument("Require --synthetic, --mics 1..8, --seconds >0, --stall-ms 0..60000 and --report PATH");
    return o;
}
// PRBS23, x^23 + x^18 + 1. Per-channel nonzero seeds and a channel parity bit
// form a signed PCM24 fixture. No floating-point tone/quantization ambiguity.
std::uint32_t seed(unsigned mic) { return ((mic + 1) * 104729u) & 0x7fffffu; }
std::int32_t nextSample(std::uint32_t& state, unsigned mic)
{
    const auto bits = (state << 1) | (mic & 1u);
    state = ((state << 1) | (((state >> 22) ^ (state >> 17)) & 1u)) & 0x7fffffu;
    return bits & 0x800000u ? static_cast<std::int32_t>(bits) - 16777216 : static_cast<std::int32_t>(bits);
}
struct Stall : FileIoFaultAdapter
{
    explicit Stall(unsigned delay) : delayMs(delay) {}
    const unsigned delayMs;
    std::atomic<bool> armed{false}, active{false}, fired{false};
    std::int64_t trigger = 0, entered = 0, left = 0;
    juce::Result beforeIo(FileIoOperation operation, const juce::File& file, std::uint64_t offset, std::size_t bytes) override
    {
        if (delayMs && armed.load(std::memory_order_acquire) && !fired.load() && operation == FileIoOperation::append
            && file.hasFileExtension("wav") && offset >= 44 && bytes > 1 && qpcNow() >= trigger)
        {
            entered = qpcNow(); fired.store(true); active.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            active.store(false, std::memory_order_release); left = qpcNow();
        }
        return juce::Result::ok();
    }
};
struct Metrics
{
    struct Observation { std::uint64_t sampleEnd = 0, queued = 0, media = 0, journal = 0; std::int64_t qpc = 0; bool stall = false; };
    Distribution callbackTime, callbackInterval, schedulerLate, queueBuffered, mediaLag, journalLag;
    std::array<Observation, 512> delayedTrace{};
    std::uint64_t callbacks = 0, accepted = 0, rejected = 0, callbacksDuringStall = 0, delayedCount = 0;
    std::int64_t previous = 0, lastCallback = 0;
};
juce::var validate(const WavTrackWriter::Config& c, std::uint64_t total, bool& valid)
{
    auto out = jsonObject(); juce::Array<juce::var> files;
    std::uint64_t mismatches = 0, checked = 0, errors = 0;
    const auto perChunk = static_cast<std::uint64_t>(c.sampleRate) * WavTrackWriter::chunkSeconds;
    const auto chunks = (total + perChunk - 1) / perChunk;
    for (unsigned mic = 1; mic <= c.mics; ++mic)
    {
        auto prbs = seed(mic - 1);
        for (std::uint64_t chunk = 1; chunk <= chunks; ++chunk)
        {
            const auto path = WavTrackWriter::chunkPath(c.takeId, mic, chunk);
            const auto file = c.projectDirectory.getChildFile(path);
            const auto expected = std::min(perChunk, total - (chunk - 1) * perChunk);
            auto item = jsonObject(); jsonSet(item, "path", path.toStdString()); jsonSet(item, "expectedSamples", jsonInt(expected));
            juce::FileInputStream in(file); std::array<std::uint8_t, 44> h{};
            const bool headerRead = !in.failedToOpen() && in.read(h.data(), static_cast<int>(h.size())) == static_cast<int>(h.size());
            const auto dataBytes = storageEncoding::get<std::uint32_t>(h.data() + 40);
            const bool header = headerRead && std::memcmp(h.data(), "RIFF", 4) == 0 && std::memcmp(h.data() + 8, "WAVEfmt ", 8) == 0
                && storageEncoding::get<std::uint32_t>(h.data() + 16) == 16 && storageEncoding::get<std::uint16_t>(h.data() + 20) == 1
                && storageEncoding::get<std::uint16_t>(h.data() + 22) == 1 && storageEncoding::get<std::uint32_t>(h.data() + 24) == c.sampleRate
                && storageEncoding::get<std::uint32_t>(h.data() + 28) == c.sampleRate * 3 && storageEncoding::get<std::uint16_t>(h.data() + 32) == 3
                && storageEncoding::get<std::uint16_t>(h.data() + 34) == 24 && std::memcmp(h.data() + 36, "data", 4) == 0
                && dataBytes == expected * 3 && in.getTotalLength() == 44 + static_cast<std::int64_t>(dataBytes) + (dataBytes & 1)
                && static_cast<std::uint64_t>(storageEncoding::get<std::uint32_t>(h.data() + 4)) + 8 == static_cast<std::uint64_t>(in.getTotalLength());
            std::uint64_t fileMismatch = 0, samples = 0;
            if (header)
            {
                std::array<std::uint8_t, 3 * 4096> buffer{};
                while (samples < expected)
                {
                    const auto n = static_cast<unsigned>(std::min<std::uint64_t>(4096, expected - samples));
                    if (in.read(buffer.data(), static_cast<int>(n * 3)) != static_cast<int>(n * 3)) { ++errors; break; }
                    for (unsigned i = 0; i < n; ++i)
                    {
                        const auto* p = buffer.data() + i * 3;
                        const auto actual = p[0] | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16);
                        if (actual != (static_cast<std::uint32_t>(nextSample(prbs, mic - 1)) & 0xffffffu)) ++fileMismatch;
                    }
                    samples += n;
                }
                if (dataBytes & 1) { std::uint8_t pad = 255; if (in.read(&pad, 1) != 1 || pad != 0) ++errors; }
                if (in.getStatus().failed()) ++errors;
            }
            else
            {
                ++errors;
                // Preserve the oracle coordinate for following chunks on a failed file.
                for (std::uint64_t i = 0; i < expected; ++i) nextSample(prbs, mic - 1);
            }
            checked += samples; mismatches += fileMismatch;
            jsonSet(item, "headerAndLengthMatch", header); jsonSet(item, "checkedSamples", jsonInt(samples)); jsonSet(item, "pcmMismatchSamples", jsonInt(fileMismatch)); files.add(item);
        }
    }
    const auto audioRoot = c.projectDirectory.getChildFile("media/takes/" + c.takeId.toDashedString() + "/audio");
    const auto actualFiles = audioRoot.findChildFiles(juce::File::findFiles, true, "*.wav").size();
    if (static_cast<std::uint64_t>(actualFiles) != chunks * c.mics) ++errors;
    valid = errors == 0 && mismatches == 0 && checked == total * c.mics;
    jsonSet(out, "status", valid ? "PASS" : "FAIL"); jsonSet(out, "checkedSamplesAllMics", jsonInt(checked));
    jsonSet(out, "pcmMismatchSamples", jsonInt(mismatches)); jsonSet(out, "fileOrHeaderErrors", jsonInt(errors));
    jsonSet(out, "missingSamples", valid ? jsonInt(0) : juce::var()); jsonSet(out, "duplicateSamples", valid ? jsonInt(0) : juce::var());
    jsonSet(out, "oracle", "Byte-for-byte PRBS23 comparison at every absolute sample coordinate, per-channel seed; null loss counts on failure mean unclassified, not zero");
    jsonSet(out, "files", files); return out;
}
int run(const Options& o)
{
    const auto startedUtc = utcNowIso8601(); const auto frequency = qpcFrequency();
    const auto milliseconds = [&](std::int64_t ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency); };
    Stall stall(o.stallMs); WavTrackWriter::Config config; config.mics = o.mics; config.faults = &stall;
    config.projectDirectory = o.report.getParentDirectory().getChildFile("durability-media-" + config.takeId.toString());
    config.nativeFormat = "synthetic PRBS23 signed PCM24, right-aligned int32; no resampling/gain/dither";
    for (unsigned mic = 1; mic <= o.mics; ++mic)
        config.devices.push_back({"synthetic-prbs23", "Synthetic Mic " + juce::String(mic), static_cast<int>(mic), static_cast<int>(mic - 1), static_cast<int>(mic - 1)});
    WavTrackWriter writer(config); auto result = writer.start(); auto metrics = std::make_unique<Metrics>();
    const auto total = static_cast<std::uint64_t>(o.seconds) * sampleRate;
    const auto beginQpc = qpcNow(); const auto begin = std::chrono::steady_clock::now();
    stall.trigger = beginQpc + static_cast<std::int64_t>(o.seconds) * frequency / 2;
    stall.armed.store(true, std::memory_order_release);
    if (result.wasOk())
    {
        // Synthetic generation and measurement are outside the simulated callback.
        // The only operation inside the measured callback is writer.tryPush().
        std::thread producer([&]
        {
            std::array<std::uint32_t, 8> states{}; for (unsigned mic = 0; mic < o.mics; ++mic) states[mic] = seed(mic);
            std::array<std::int32_t, blockFrames * 8> pcm{};
            for (std::uint64_t sample = 0; sample < total; sample += blockFrames)
            {
                const auto frames = static_cast<unsigned>(std::min<std::uint64_t>(blockFrames, total - sample));
                for (unsigned f = 0; f < frames; ++f)
                    for (unsigned mic = 0; mic < o.mics; ++mic) pcm[static_cast<std::size_t>(f) * o.mics + mic] = nextSample(states[mic], mic);
                const auto sampleEnd = sample + frames;
                const auto due = begin + std::chrono::seconds(sampleEnd / sampleRate)
                    + std::chrono::nanoseconds((sampleEnd % sampleRate) * 1000000000ull / sampleRate);
                std::this_thread::sleep_until(due);
                const auto entered = qpcNow();
                const bool accepted = writer.tryPush(pcm.data(), frames, sample, entered);
                const auto returned = qpcNow();
                ++metrics->callbacks; metrics->callbackTime.add(milliseconds(returned - entered));
                if (metrics->previous) metrics->callbackInterval.add(milliseconds(entered - metrics->previous));
                metrics->previous = metrics->lastCallback = entered;
                metrics->schedulerLate.add(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - due).count());
                if (!accepted) { ++metrics->rejected; break; }
                metrics->accepted += frames;
                const auto queued = writer.queueFrames(), media = writer.mediaDurableSamples(), journal = writer.journalDurableSamples();
                const auto active = stall.active.load(std::memory_order_acquire);
                if (active) ++metrics->callbacksDuringStall;
                metrics->queueBuffered.add(static_cast<double>(queued) * 1000.0 / sampleRate);
                metrics->mediaLag.add(static_cast<double>(sampleEnd - std::min(sampleEnd, media)) * 1000.0 / sampleRate);
                metrics->journalLag.add(static_cast<double>(sampleEnd - std::min(sampleEnd, journal)) * 1000.0 / sampleRate);
                if (active || queued > blockFrames * 2 || sampleEnd - std::min(sampleEnd, journal) > sampleRate * 2)
                    metrics->delayedTrace[metrics->delayedCount++ % metrics->delayedTrace.size()] = {sampleEnd, queued, media, journal, returned, active};
            }
        });
        producer.join();
    }
    const auto suppliedAt = qpcNow();
    const auto stopped = writer.stop(static_cast<std::int64_t>(total), juce::Uuid());
    if (result.wasOk()) result = stopped;
    const auto finalizedAt = qpcNow();
    bool pcmValid = false; const auto verification = validate(config, total, pcmValid);
    JournalReplay journal; const auto replayResult = RecordingJournal::replay(config.projectDirectory.getChildFile("journal"), journal);
    bool journalValid = replayResult.wasOk() && !journal.ignoredTail && !journal.records.empty()
        && journal.records.front().kind == JournalKind::TakeStarted && journal.records.back().kind == JournalKind::TakeFinalized;
    std::uint64_t stopRecords = 0, checkpoints = 0;
    for (const auto& record : journal.records)
    {
        if (record.kind == JournalKind::TakeStopped)
        {
            ++stopRecords; journalValid &= static_cast<juce::int64>(record.payload["Nstop"]) == static_cast<juce::int64>(total);
        }
        if (record.kind == JournalKind::Checkpoint)
        {
            ++checkpoints;
            for (const auto& position : *record.payload["files"].getArray())
            {
                const auto validBytes = static_cast<juce::int64>(position["validBytes"]);
                const auto file = config.projectDirectory.getChildFile(position["path"].toString());
                journalValid &= file.existsAsFile() && file.getSize() >= validBytes;
            }
        }
    }
    journalValid &= stopRecords == 1 && writer.journalDurableSamples() == total;
    const bool callbackContinued = o.stallMs == 0 || (stall.fired.load() && metrics->callbacksDuringStall > 0);
    const bool callbackBudget = metrics->callbackTime.max() < 1000.0 * blockFrames / sampleRate;
    const bool passed = result.wasOk() && pcmValid && journalValid && callbackContinued && callbackBudget
        && metrics->rejected == 0 && metrics->accepted == total;
    auto report = jsonObject(); jsonSet(report, "schemaVersion", 1); jsonSet(report, "status", passed ? "PASS" : "FAIL");
    jsonSet(report, "source", "synthetic"); jsonSet(report, "command", o.command.toStdString()); jsonSet(report, "startedUtc", startedUtc); jsonSet(report, "endedUtc", utcNowIso8601());
    jsonSet(report, "os", juce::SystemStats::getOperatingSystemName().toStdString()); jsonSet(report, "compilerMscFullVer", _MSC_FULL_VER);
    jsonSet(report, "ffmpegBuild", RECORDER_FFMPEG_VERSION); jsonSet(report, "asioDriver", "UNAVAILABLE: no device opened by this synthetic probe");
    jsonSet(report, "mics", static_cast<int>(o.mics)); jsonSet(report, "sampleRate", static_cast<int>(sampleRate)); jsonSet(report, "blockFrames", static_cast<int>(blockFrames));
    jsonSet(report, "secondsRequested", jsonInt(o.seconds)); jsonSet(report, "supplyWallSeconds", milliseconds(suppliedAt - beginQpc) / 1000.0);
    jsonSet(report, "stopFlushMs", milliseconds(finalizedAt - suppliedAt)); jsonSet(report, "projectDirectory", config.projectDirectory.getFullPathName().toStdString());
    jsonSet(report, "takeId", config.takeId.toDashedString().toStdString()); jsonSet(report, "error", result.getErrorMessage().toStdString());
    jsonSet(report, "callbacks", jsonInt(metrics->callbacks)); jsonSet(report, "acceptedSamplesPerMic", jsonInt(metrics->accepted)); jsonSet(report, "rejectedBlocks", jsonInt(metrics->rejected));
    jsonSet(report, "callbackPushTime", metrics->callbackTime.toJson()); jsonSet(report, "callbackInterval", metrics->callbackInterval.toJson());
    jsonSet(report, "schedulerLateness", metrics->schedulerLate.toJson()); jsonSet(report, "queueBufferedMs", metrics->queueBuffered.toJson());
    jsonSet(report, "mediaWatermarkLagMs", metrics->mediaLag.toJson()); jsonSet(report, "journalWatermarkLagMs", metrics->journalLag.toJson());
    jsonSet(report, "watermarkDefinition", "At each accepted callback: (exclusive sample end - last all-microphone OS flush / journal-acknowledged sample end) / Fs. Includes callback block and scheduling, excludes an unflushed physical tail.");
    jsonSet(report, "callbackBudgetMs", 1000.0 * blockFrames / sampleRate); jsonSet(report, "callbackBudgetPass", callbackBudget);
    auto fault = jsonObject(); jsonSet(fault, "requestedMs", jsonInt(o.stallMs)); jsonSet(fault, "injected", stall.fired.load());
    jsonSet(fault, "actualMs", stall.fired.load() ? juce::var(milliseconds(stall.left - stall.entered)) : juce::var());
    jsonSet(fault, "startSeconds", stall.fired.load() ? juce::var(milliseconds(stall.entered - beginQpc) / 1000.0) : juce::var());
    jsonSet(fault, "callbacksDuringStall", jsonInt(metrics->callbacksDuringStall)); jsonSet(fault, "callbackContinued", callbackContinued);
    jsonSet(fault, "adapter", "Delay one worker WAV append at midpoint; no delay/lock in PCM queue producer"); jsonSet(report, "stall", fault);
    juce::Array<juce::var> observations; const auto kept = std::min<std::uint64_t>(metrics->delayedCount, metrics->delayedTrace.size());
    for (std::uint64_t i = metrics->delayedCount - kept; i < metrics->delayedCount; ++i)
    {
        const auto& item = metrics->delayedTrace[i % metrics->delayedTrace.size()]; auto v = jsonObject();
        jsonSet(v, "sampleEnd", jsonInt(item.sampleEnd)); jsonSet(v, "queueFrames", jsonInt(item.queued)); jsonSet(v, "mediaDurableSamples", jsonInt(item.media));
        jsonSet(v, "journalDurableSamples", jsonInt(item.journal)); jsonSet(v, "qpc", juce::var(static_cast<juce::int64>(item.qpc))); jsonSet(v, "stallActive", item.stall); observations.add(v);
    }
    jsonSet(report, "delayedTracePolicy", "Last 512 callback observations during I/O stall, queue >20ms, or journal lag >2s; histograms cover all callbacks");
    jsonSet(report, "delayedTrace", observations); jsonSet(report, "writer", writer.telemetry()); jsonSet(report, "pcmVerification", verification);
    auto journalReport = jsonObject(); jsonSet(journalReport, "status", journalValid ? "PASS" : "FAIL"); jsonSet(journalReport, "commits", jsonInt(journal.lastSequence));
    jsonSet(journalReport, "checkpoints", jsonInt(checkpoints)); jsonSet(journalReport, "segments", jsonInt(journal.fileCount));
    jsonSet(journalReport, "ignoredTail", journal.ignoredTail); jsonSet(journalReport, "error", replayResult.getErrorMessage().toStdString()); jsonSet(report, "journalReplay", journalReport);
    jsonSet(report, "unverified", "Physical ASIO callbacks, native input conversion, abrupt process/power loss, device caches, MP4 and RF64 export are not exercised.");
    CaptureTelemetry::writeJson(o.report, report);
    std::cout << (passed ? "PASS" : "FAIL") << " synthetic durability: " << metrics->accepted << " samples/mic, "
        << metrics->callbacksDuringStall << " callbacks during stall, report " << o.report.getFullPathName() << '\n';
    return passed ? 0 : 1;
}
}
int main(int argc, char** argv)
{
    try { return run(parse(argc, argv)); }
    catch (const std::invalid_argument& e)
    {
        std::cerr << e.what() << "\nRecorderDurabilityProbe --synthetic --mics 8 --seconds 60 --stall-ms 2000 --report PATH\n"; return 2;
    }
    catch (const std::exception& e) { std::cerr << "Durability probe failed: " << e.what() << '\n'; return 1; }
}
