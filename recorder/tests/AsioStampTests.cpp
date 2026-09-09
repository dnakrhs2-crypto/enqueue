#include "audio/AsioTimingBridge.h"
#include "audio/RawAudioTap.h"
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace gocue::recorder;
namespace
{
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
BlockStamp block(std::uint64_t n)
{
    BlockStamp s{}; s.flags = timeInfoPresent | samplePositionValid | systemTimeValid | sampleRateValid;
    s.asioTimeInfoFlags = 7; s.sequence = n; s.numSamples = 480; s.sampleRate = 48000;
    s.samplePosition = (std::int64_t{1} << 60) + static_cast<std::int64_t>(n * 480);
    s.callbackQpc = (std::int64_t{1} << 60) + static_cast<std::int64_t>(n * 10000);
    s.systemTimeRaw = 0xffff000000000000ull + n; s.bufferIndex = static_cast<int>(n % 2);
    return s;
}
}
int runAsioStampTests()
{
    int passed = 0, failed = 0;
    auto test = [&](const char* name, auto fn)
    {
        try { fn(); ++passed; }
        catch (const std::exception& e) { ++failed; std::cerr << "ASIO FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("large origins, exact slope, raw system time and intervals", []
    {
        auto stats = std::make_unique<AsioStampStatistics>(1000000);
        for (std::uint64_t i = 0; i < 501; ++i) stats->observe(block(i));
        const auto fit = stats->regression();
        require(fit.valid && fit.observations == 501, "valid bounded fit");
        require(std::abs(fit.samplesPerSecond - 48000) < 1e-8 && fit.residualMaxAbsSamples < 1e-8, "integer origins retain precision beyond 2^53");
        require(stats->sampleJumps == 0 && stats->sampleRegressions == 0 && stats->sampleDuplicates == 0, "continuous samples");
        require(stats->callbackIntervalMs.mean == 10 && stats->callbackJitterMs.maximum == 0, "interval and expected-duration jitter");
        require(stats->systemTimeExamples[0] == 0xffff000000000000ull, "systemTime never converted to signed/time units");
    });
    test("jitter produces measured residual, without fake jumps", []
    {
        auto stats = std::make_unique<AsioStampStatistics>(1000000);
        for (std::uint64_t i = 0; i < 600; ++i) { auto s = block(i); s.callbackQpc += (i & 1) ? 100 : -100; stats->observe(s); }
        const auto fit = stats->regression();
        require(fit.valid && fit.residualRmsSamples > 4 && fit.residualRmsSamples < 5.5, "arrival jitter expressed in samples");
        require(stats->callbackJitterMs.standardDeviation() > 0.19 && !stats->sampleJumps, "jitter separate from sample loss");
    });
    test("regression duplicate jump reset and queue gap classification", []
    {
        auto stats = std::make_unique<AsioStampStatistics>(1000000);
        stats->observe(block(0)); stats->observe(block(1));
        auto s = block(2); s.samplePosition = block(1).samplePosition - 1; stats->observe(s);
        require(stats->sampleRegressions == 1 && !stats->regression().valid, "backwards starts fresh epoch");
        auto next = block(3); next.samplePosition = s.samplePosition; stats->observe(next);
        require(stats->sampleDuplicates == 1, "duplicate samples");
        next = block(4); stats->observe(next); require(stats->sampleJumps == 1, "noncontiguous positive step");
        next = block(8); stats->observe(next);
        require(stats->observationGaps == 1 && stats->sampleJumps == 1, "missing telemetry not labelled driver jump");
        next = block(9); next.resets = 1; next.xruns = 2; next.latencyChanges = 1; stats->observe(next);
        require(stats->resetEvents == 1 && stats->xrunEvents == 2 && stats->latencyChangeEvents == 1, "event counters retained");
    });
    test("invalid flags never create a sample clock; output-only QPC still observed", []
    {
        auto stats = std::make_unique<AsioStampStatistics>(1000000);
        for (std::uint64_t i = 0; i < 10; ++i) { auto s = block(i); s.flags = 0; stats->observe(s); }
        require(stats->blocks == 10 && stats->callbackIntervalMs.count == 9, "legacy callback arrival clock exists");
        require(!stats->timeInfoBlocks && !stats->samplePositionBlocks && !stats->regression().valid, "invalid fields not synthesized");
        auto s = block(10); stats->observe(s); s = block(11); s.callbackQpc = block(10).callbackQpc; stats->observe(s);
        require(stats->qpcRegressions == 1, "QPC duplicate/backwards detection");
    });
    test("rate changes and variable block sizes", []
    {
        auto stats = std::make_unique<AsioStampStatistics>(1000000);
        auto s = block(0); s.numSamples = 128; stats->observe(s);
        auto next = block(1); next.samplePosition = s.samplePosition + 128; next.numSamples = 256; stats->observe(next);
        require(stats->sampleJumps == 0, "previous block length determines next position");
        s = block(2); s.samplePosition = next.samplePosition + 256; s.sampleRate = 96000; stats->observe(s);
        require(stats->rateChanges == 1 && !stats->regression().valid, "rate epoch boundary");
    });
    test("events before first observation are a baseline, not in-run failures", []
    {
        auto stats = std::make_unique<AsioStampStatistics>(1000000);
        for (std::uint64_t i = 0; i < 5; ++i)
        { auto s = block(i); s.resets = 7; s.xruns = 4; s.resyncs = 2; stats->observe(s); }
        require(!stats->resetEvents && !stats->xrunEvents && !stats->resyncEvents, "startup counters baselined");
        require(stats->regression().valid && stats->first.resets == 7, "baseline retained in first stamp");
    });
    test("observation retention and polling cost are bounded and separate", []
    {
        auto stats = std::make_unique<AsioStampStatistics>(1000000);
        for (std::uint64_t i = 0; i < 9000; ++i) { auto s = block(i); s.callbackQpc = 1000000 + static_cast<std::int64_t>(i * 100); stats->observe(s); }
        require(stats->regression().observations == AsioStampStatistics::observationCapacity, "4096 retention cap");
        for (int i = 0; i < 4; ++i) stats->observePoll({100000 + i * 100000, 100020 + i * 100000, i * 4800, 123, 0, 1});
        require(stats->pollCostUs.mean == 20 && stats->pollRegression().valid, "driver call bracketing");
        require(std::abs(stats->pollRegression().samplesPerSecond - 48000) < 1e-6, "independent poll regression");
        stats->observePoll({0, 1, 0, 0, -1, 0});
        require(stats->pollFailures == 1 && !stats->pollRegression().valid, "poll failure validity");
    });
    test("stamp and poll queues refuse overflow, drain and reuse", []
    {
        auto bridge = std::make_unique<AsioTimingBridge>(1000000);
        for (std::size_t i = 0; i < AsioTimingBridge::stampQueueCapacity; ++i) require(bridge->enqueueStamp(block(i)), "exact queue capacity");
        require(!bridge->enqueueStamp(block(2000)) && bridge->droppedStamps() == 1, "bounded overflow");
        require(bridge->stampHighWater() == AsioTimingBridge::stampQueueCapacity, "high water");
        SamplePositionPoll p{1, 2, 3, 4, 0, 1};
        for (std::size_t i = 0; i < AsioTimingBridge::pollQueueCapacity; ++i) require(bridge->enqueuePoll(p), "poll capacity");
        require(!bridge->enqueuePoll(p) && bridge->droppedPolls() == 1, "poll bound");
        std::uint64_t workerObservations = 0;
        bridge->drain([](void* context, const BlockStamp&) noexcept { ++*static_cast<std::uint64_t*>(context); }, &workerObservations);
        require(bridge->statistics().blocks == AsioTimingBridge::stampQueueCapacity && workerObservations == AsioTimingBridge::stampQueueCapacity, "sync worker receives every stamp");
        require(bridge->enqueueStamp(block(2001)), "queue reusable after drain"); bridge->drain();
    });
    test("global tap registration, mic-off raw block and unregister lifecycle", []
    {
        RawAudioTap raw; raw.prepare({}, 480, 2);
        auto bridge = std::make_unique<AsioTimingBridge>(1000000, &raw), second = std::make_unique<AsioTimingBridge>(1000000);
        require(bridge->registerTap() && !second->registerTap(), "one device owner");
        recorderAsioTap.load(std::memory_order_acquire)(block(0), nullptr, 0);
        bridge->drain(); require(bridge->statistics().blocks == 1 && raw.front() && raw.front()->numChannels == 0, "output-only keeps timing/raw boundary");
        raw.release(); bridge->unregisterAfterDeviceClosed();
        require(recorderAsioTap.load() == nullptr && second->registerTap(), "quiescent owner transfer"); second->unregisterAfterDeviceClosed();
    });
    test("concurrent stamp producer and sync consumer", []
    {
        auto bridge = std::make_unique<AsioTimingBridge>(1000000); std::atomic<bool> done{false};
        std::thread consumer([&] { while (!done.load(std::memory_order_acquire)) { bridge->drain(); std::this_thread::yield(); } bridge->drain(); });
        std::uint64_t accepted = 0;
        for (std::uint64_t i = 0; i < 30000; ++i) accepted += bridge->enqueueStamp(block(i));
        done.store(true, std::memory_order_release); consumer.join();
        require(bridge->statistics().blocks == accepted && accepted + bridge->droppedStamps() == 30000, "exact cross-thread accounting");
    });
    std::cout << "AsioStampTests: " << passed << " passed, " << failed << " failed; no device opened\n";
    return failed ? 1 : 0;
}
int runNativePcmTests();
int recorderCaptureTestMain(int argc, char** argv);
int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--suite" && std::string(argv[2]) == "asio-native-pcm")
    { const int a = runAsioStampTests(), b = runNativePcmTests(); return a | b; }
    if (argc == 1)
    { const int a = recorderCaptureTestMain(argc, argv), b = runAsioStampTests(), c = runNativePcmTests(); return a | b | c; }
    return recorderCaptureTestMain(argc, argv);
}
