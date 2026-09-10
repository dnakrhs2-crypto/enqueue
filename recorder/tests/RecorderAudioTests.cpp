#include "TestSupport.h"
#include "audio/RecorderAudioEngine.h"
#include "storage/StorageEncoding.h"
#include <chrono>
#include <cmath>
#include <thread>

using namespace gocue::recorder;
using recorder_test::require;
namespace
{
void ok(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
template<class F> void until(F f)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!f()) { require(std::chrono::steady_clock::now() < deadline, "Timed out waiting for worker"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
std::int32_t pattern(std::int64_t sample, unsigned physical)
{
    const auto word = (std::uint32_t(sample) * 7919u + physical * 104729u) & 0xffffffu;
    return word & 0x800000u ? std::int32_t(word) - 16777216 : std::int32_t(word);
}
struct Fixture
{
    RecorderAudioEngine engine;
    RecorderAudioEngine::TakeConfig config;
    std::array<int, 8> map{7,2,5,0,6,1,4,3};
    std::int64_t position = 0, qpc = qpcNow();
    std::uint64_t sequence = 0;
    static constexpr unsigned Fs = 8000, block = 80;
    std::array<std::array<std::uint8_t, block * 3>, 16> raw{};
    std::array<std::array<float, block>, 16> input{};
    std::array<std::array<float, block>, 4> output{};
    bool stereo;
    Fixture(unsigned microphones = 8, bool stereoSlot = false) : stereo(stereoSlot)
    {
        if (stereo) { map[0] = 12; map[1] = 2; }
        for (unsigned i = microphones; i < 8; ++i) map[i] = -1;
        std::array<bool, 8> slots{}; slots[0] = stereo;
        ok(engine.setInputMap(map, slots)); ok(engine.openSynthetic(Fs, block, 16, 4));
        for (unsigned i = 0; i < microphones; ++i) ok(engine.arm(i, true));
        config.projectDirectory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("RecorderAudioTests-" + juce::Uuid().toString());
        for (int i = 0; i < 4; ++i) feed();
        until([&] { return engine.clockReady(); });
    }
    void feed(unsigned rate = Fs, std::uint64_t resets = 0, bool missingNativePosition = false)
    {
        const auto active = engine.deviceInfo().activeToPhysical;
        std::array<NativeInputView, 16> views{};
        std::array<const float*, 16> floatInputs{};
        std::array<float*, 4> floatOutputs{};
        for (unsigned c = 0; c < active.size(); ++c)
        {
            for (unsigned i = 0; i < block; ++i)
            {
                WavTrackWriter::packPcm24(pattern(position + i, unsigned(active[c])), raw[c].data() + i * 3);
                input[c][i] = stereo && active[c] == map[0] + 1 ? -.5f : .25f; // deliberately unrelated to native PCM
            }
            views[c] = {raw[c].data(), int(c), active[c], nativeFormatForAsio(17)}; floatInputs[c] = input[c].data();
        }
        for (unsigned c = 0; c < 4; ++c) floatOutputs[c] = output[c].data();
        BlockStamp stamp{}; stamp.flags = samplePositionValid | sampleRateValid; stamp.samplePosition = position;
        stamp.callbackQpc = qpc + position * qpcFrequency() / Fs; stamp.sequence = sequence++; stamp.sampleRate = rate;
        stamp.numSamples = block; stamp.bufferIndex = int(stamp.sequence % 2); stamp.resets = resets;
        if (missingNativePosition) { stamp.flags &= ~samplePositionValid; stamp.samplePosition = 0; }
        engine.processBlock(stamp, views.data(), unsigned(active.size()), floatInputs.data(), floatOutputs.data(), 4); position += block;
    }
    std::int64_t begin(unsigned lead = 11)
    {
        ok(engine.prepare(config)); const auto n0 = position + lead; ok(engine.startAt(n0));
        until([&] { return engine.startCommitted() || engine.error() != RecorderAudioEngine::Error::none; });
        require(engine.startCommitted(), "TakeStarted committed before input adoption"); return n0;
    }
    void verify(unsigned logical, std::int64_t n0, std::int64_t samples)
    {
        const auto file = config.projectDirectory.getChildFile(WavTrackWriter::chunkPath(config.takeId, logical + 1, 1));
        juce::MemoryBlock bytes; require(file.loadFileAsData(bytes), "WAV exists");
        const auto* p = static_cast<const std::uint8_t*>(bytes.getData());
        const auto channels = stereo && logical == 0 ? 2u : 1u;
        const auto dataBytes = samples * channels * 3;
        require(bytes.getSize() == std::size_t(44 + dataBytes + (dataBytes & 1)), "WAV exact logical sample count/pad");
        require(storageEncoding::get<std::uint16_t>(p + 22) == channels && storageEncoding::get<std::uint32_t>(p + 40) == dataBytes, "WAV channel/data header");
        for (std::int64_t i = 0; i < samples; ++i) for (unsigned ch = 0; ch < channels; ++ch)
        {
            const auto* sample = p + 44 + (i * channels + ch) * 3;
            const auto word = std::uint32_t(sample[0]) | std::uint32_t(sample[1]) << 8 | std::uint32_t(sample[2]) << 16;
            require(word == (std::uint32_t(pattern(n0 + i, unsigned(map[logical]) + ch)) & 0xffffffu), "Native PCM24 bytes preserved in logical/physical order");
        }
    }
};
struct Stall final : FileIoFaultAdapter
{
    std::atomic<bool> entered{false}, release{false};
    juce::Result beforeIo(FileIoOperation op, const juce::File& file, std::uint64_t offset, std::size_t) override
    {
        if (op == FileIoOperation::append && file.hasFileExtension("wav") && offset >= 44 && !release.load())
        { entered = true; until([&] { return release.load(); }); }
        return juce::Result::ok();
    }
};
}
int runRecorderAudioTests()
{
    recorder_test::Suite suite;
    suite.test("Eight stereo slots capture sixteen physical channels and peak cache round trip", []
    {
        Fixture f(8); ok(f.engine.closeDevice()); std::array<bool,8> stereo{}; stereo.fill(true);
        for (unsigned i = 0; i < 8; ++i) f.map[i] = int(i * 2);
        ok(f.engine.setInputMap(f.map,stereo)); ok(f.engine.openSynthetic(Fixture::Fs,Fixture::block,16,4));
        f.position = 0; f.sequence = 0; const auto start = f.begin(), length = 333LL; ok(f.engine.stopAt(start+length));
        while (f.engine.stopSample() < 0) f.feed(); ok(f.engine.finishCapture(juce::Uuid())); ok(f.engine.finishJournal(true));
        const auto peaks = f.engine.peakCache()->snapshot(); require(peaks.channels == 16 && peaks.samples == length, "Sixteen packed peak channels");
        const auto cacheFile = f.config.projectDirectory.getChildFile("stereo.peaks.json"); ok(PeakCache::write(cacheFile,peaks));
        const auto reread = PeakCache::read(cacheFile); require(reread.channels == 16 && reread.bins.back()[15].maximum == peaks.bins.back()[15].maximum, "Sixteenth peak channel persisted");
        for (unsigned mic = 0; mic < 8; ++mic)
        {
            juce::MemoryBlock bytes; require(f.config.projectDirectory.getChildFile(WavTrackWriter::chunkPath(f.config.takeId,mic+1,1)).loadFileAsData(bytes), "Stereo slot WAV exists");
            const auto* p = static_cast<const std::uint8_t*>(bytes.getData()); require(bytes.getSize() == 44 + length * 6 && storageEncoding::get<std::uint16_t>(p+22) == 2, "All slots are stereo");
            for (Sample i = 0; i < length; ++i) for (unsigned ch = 0; ch < 2; ++ch)
            {
                const auto* v = p+44+(i*2+ch)*3; const auto word = unsigned(v[0]) | unsigned(v[1]) << 8 | unsigned(v[2]) << 16;
                require(word == (unsigned(pattern(start+i,mic*2+ch)) & 0xffffffu), "Sixteen-channel native byte oracle");
            }
        }
        JournalReplay replay; ok(RecordingJournal::replay(f.config.projectDirectory.getChildFile("journal"),replay));
        require(int(replay.records[0].payload["pcm"]["channels"]) == 2, "Uniform stereo journal default");
    });
    suite.test("Stereo slot native L/R, physical bounds, listening and frozen mapping", []
    {
        Fixture f(2, true); OutputMapping out; out.left = 0; out.right = 1; ok(f.engine.setOutputMap(out));
        auto invalid = f.map; invalid[1] = 13; std::array<bool, 8> stereo{}; stereo[0] = true;
        require(f.engine.setInputMap(invalid, stereo).failed(), "Stereo right cannot be reused by a mono slot");
        invalid = f.map; invalid[0] = 15; require(f.engine.setInputMap(invalid, stereo).failed(), "Right channel must exist on device");
        f.engine.setInputMonitoring(true, 1); for (int i = 0; i < 4; ++i) f.feed();
        require(std::abs(f.output[0].back() - .25f) < 1e-6f && std::abs(f.output[1].back() + .5f) < 1e-6f, "Monitor L/R identity");
        require(f.engine.inputPeaks()[0] == .5f, "Stereo input meter is maximum of L/R");
        const auto start = f.begin(), length = 1739LL; ok(f.engine.stopAt(start + length));
        require(f.engine.setInputMap(f.map, {}).failed(), "Cannot change stereo during take");
        while (f.engine.stopSample() < 0) { f.engine.setListeningState(1, 2); f.feed(); }
        ok(f.engine.finishCapture(juce::Uuid())); ok(f.engine.finishJournal(true)); f.verify(0, start, length); f.verify(1, start, length);
        const auto peak = f.engine.peakCache()->snapshot(); require(peak.channels == 3 && peak.samples == length && peak.complete, "Mixed slot peak channels");
        double expectedSquares = 0;
        for (Sample i = 0; i < length; ++i)
        {
            const float mono = float(pattern(start+i,2))/8388608.0f;
            const float left = (float(pattern(start+i,12))/8388608.0f + mono) * .5f;
            const float right = (float(pattern(start+i,13))/8388608.0f + mono) * .5f;
            expectedSquares += (double(left)*left + double(right)*right) * .5;
        }
        require(std::abs(double(f.engine.telemetry()["referenceInputRms"]) - std::sqrt(expectedSquares / length)) < 1e-7, "Reference mix uses stereo L/R and a mono slot mean");
        JournalReplay replay; ok(RecordingJournal::replay(f.config.projectDirectory.getChildFile("journal"), replay));
        require(int(replay.records.front().payload["files"][0]["pcm"]["channels"]) == 2, "Engine journals stereo asset format");
    });
    suite.test("8ch native PCM / exact partial boundaries / mute solo monitor isolation", []
    {
        Fixture f; const auto n0 = f.begin(), length = 1539LL; ok(f.engine.stopAt(n0 + length));
        require(f.engine.setInputMap(f.map).failed() && f.engine.arm(0, false).failed(), "Map/arm frozen during take");
        require(f.engine.openSynthetic(44100, 80).failed(), "Rate changes require a new take");
        unsigned blocks = 0;
        while (f.engine.stopSample() < 0)
        {
            f.engine.setInputMonitoring((blocks & 1) != 0, std::uint8_t(blocks));
            f.engine.setListeningState(std::uint8_t(blocks * 7), std::uint8_t(blocks * 13)); f.feed(); ++blocks;
        }
        require(f.engine.startSample() == n0 && f.engine.stopSample() == n0 + length, "N0/Nstop adopted exactly");
        ok(f.engine.finishCapture(juce::Uuid())); ok(f.engine.finishJournal(true));
        for (unsigned c = 0; c < 8; ++c) f.verify(c, n0, length);
        require(f.engine.telemetry()["tracks"].getArray()->size() == 8, "Eight original track meters");
        double squares = 0;
        for (std::int64_t i = 0; i < length; ++i)
        {
            double sum = 0; for (unsigned c = 0; c < 8; ++c) sum += double(pattern(n0 + i, c)) / 8388608.0 / 8.0;
            squares += sum * sum;
        }
        require(std::abs(double(f.engine.telemetry()["referenceInputRms"]) - std::sqrt(squares / double(length))) < 1e-7, "Reference is fixed M=8 mean independent of mute/solo/monitor");
    });
    suite.test("Duplicate outputs rejected, independent sides, mono mean, unused outputs zero", []
    {
        Fixture f(0); OutputMapping duplicate; duplicate.left = 1; duplicate.right = 1;
        require(f.engine.setOutputMap(duplicate).failed(), "Reject duplicate L/R");
        OutputMapping oneSide; oneSide.right = 3; ok(f.engine.setOutputMap(oneSide));
        PlaybackBlockQueue queue(80, 4); std::array<float, 160> pcm{};
        for (unsigned i = 0; i < 80; ++i) { pcm[i * 2] = .2f; pcm[i * 2 + 1] = .6f; }
        require(queue.tryPush(pcm.data(), 80), "Prepared playback"); f.engine.setPlaybackQueue(&queue); f.feed();
        require(std::abs(f.output[3][0] - .6f) < 1e-6f && f.output[0][0] == 0, "Single selected side routed");
        OutputMapping mono; mono.mono = true; mono.monoChannel = 2; ok(f.engine.setOutputMap(mono));
        require(queue.tryPush(pcm.data(), 80), "Prepared mono playback"); f.feed();
        for (unsigned i = 0; i < 80; ++i) require(std::abs(f.output[2][i] - .4f) < 1e-6f && f.output[0][i] == 0 && f.output[1][i] == 0 && f.output[3][i] == 0, "Mono is L/R mean; unused outputs silent");
        f.engine.setPlaybackQueue(nullptr);
    });
    suite.test("Output-only take retains sample clock, no mic WAV or synthetic tone in reference", []
    {
        Fixture f(0); const auto n0 = f.begin(); ok(f.engine.stopAt(n0 + 201));
        while (f.engine.stopSample() < 0) f.feed(f.Fs, 0, true);
        ok(f.engine.finishCapture(juce::Uuid())); ok(f.engine.finishJournal(true));
        require(!f.config.projectDirectory.getChildFile("media/takes/" + f.config.takeId.toDashedString() + "/audio").exists(), "No microphone means no WAV directory");
        require(f.engine.currentSample() >= n0 + 201, "Output callbacks advance clock");
        require(std::int64_t(f.engine.telemetry()["softwareClockBlocks"]) > 0, "Missing ASIO native position uses explicitly reported callback sample counter");
    });
    suite.test("Fs change and ASIO reset stop at last confirmed input, preserve prefix", []
    {
        for (bool reset : {false, true})
        {
            Fixture f(1); const auto n0 = f.begin(0); f.feed(); const auto end = f.engine.acceptedEnd();
            f.feed(reset ? f.Fs : 44100, reset ? 1 : 0);
            require(f.engine.error() == (reset ? RecorderAudioEngine::Error::asioReset : RecorderAudioEngine::Error::sampleRateChanged), "Device boundary failure is explicit");
            require(f.engine.stopSample() == end, "Invalid block never extends original range");
            require(f.engine.finishCapture(juce::Uuid()).failed(), "Partial capture is not reported complete"); ok(f.engine.finishJournal(false));
            f.verify(0, n0, end - n0);
        }
    });
    suite.test("Four-second queues overflow stops complete take without silent replacement", []
    {
        Stall stall; Fixture f(1); f.config.faults = &stall; f.begin(0); f.feed();
        until([&] { return stall.entered.load(); });
        for (int i = 0; i < 2000 && f.engine.error() == RecorderAudioEngine::Error::none; ++i) f.feed();
        stall.release = true;
        until([&] { return f.engine.error() != RecorderAudioEngine::Error::none; });
        require(f.engine.error() == RecorderAudioEngine::Error::rawOverflow || f.engine.error() == RecorderAudioEngine::Error::pcmOverflow, "Original queue failure signal");
        require(f.engine.finishCapture(juce::Uuid()).failed(), "Overflow is partial failure"); ok(f.engine.finishJournal(false));
        JournalReplay replay; ok(RecordingJournal::replay(f.config.projectDirectory.getChildFile("journal"), replay));
        for (const auto& r : replay.records) require(r.kind != JournalKind::TakeFinalized, "Never commit a failed take as finalized");
    });
    suite.test("Temporary OLS mapper uses origin-relative arithmetic and invertible samples", []
    {
        AsioStampStatistics stats(10000000); LinearClockMapper mapper;
        for (int i = 0; i < 20; ++i)
        {
            BlockStamp b{}; b.numSamples = 480; b.sampleRate = 48000; b.flags = samplePositionValid; b.sequence = i;
            b.callbackQpc = 9000000000000000LL + i * 100000; b.samplePosition = 8000000000000000LL + i * 480;
            stats.observe(b);
        }
        mapper.observe(stats); const auto fit = mapper.snapshot(); require(fit.valid, "Clock regression ready");
        const auto sample = 8000000000007200LL; require(fit.mapToSample(fit.mapToQpc(sample)) == sample, "Exact large-origin roundtrip");
    });
    suite.test("Actual PCM AAC uses interface Fs and preserves priming / exact rescaled tail", []
    {
        for (unsigned rate : {8000u,44100u,48000u,96000u})
        {
            ReferenceMixWriter writer(rate); std::array<float, 514> stereo{};
            for (unsigned i = 0; i < 257; ++i) { stereo[i * 2] = .125f; stereo[i * 2 + 1] = -.25f; }
            std::int64_t first = AV_NOPTS_VALUE, last = 0; unsigned packets = 0;
            const PacketSink sink = [&](const AVPacket& packet)
            { if (!packets++) first = packet.pts; require(packet.pts == packet.dts, "Reference PTS equals DTS"); last = packet.pts + packet.duration; };
            for (unsigned n = 0; n < rate + 1;)
            { const auto count = std::min(257u, rate + 1 - n); writer.append(stereo.data(), count, sink); n += count; }
            writer.finishInput(sink);
            require(first == -1024 && last == rescaleRound(rate + 1, 48000, rate), "Input duration rescaled exactly once, AAC tail excludes padding");
            require(std::int64_t(writer.toJson()["inputSamples"]) == rate + 1, "No source samples fabricated or dropped");
        }
    });
    suite.test("Device reopen waits for a clock fit from the new sample-rate epoch", []
    {
        Fixture f(0); const auto previous = f.engine.clockMapping();
        ok(f.engine.openSynthetic(16000, 80, 0, 4));
        require(!f.engine.clockReady(), "No stale ready flag after reopening device");
        const auto origin = f.qpc + qpcFrequency();
        for (int i = 0; i < 4; ++i)
        {
            std::array<float, 80> left{}, right{}; float* outputs[] = {left.data(), right.data()};
            BlockStamp stamp{}; stamp.flags = samplePositionValid; stamp.sequence = i; stamp.numSamples = 80;
            stamp.sampleRate = 16000; stamp.samplePosition = i * 80; stamp.callbackQpc = origin + i * qpcFrequency() / 200;
            f.engine.processBlock(stamp, nullptr, 0, nullptr, outputs, 2);
        }
        until([&] { return f.engine.clockReady(); });
        const auto current = f.engine.clockMapping();
        require(current.originQpc >= origin && current.originQpc > previous.originQpc, "Clock snapshot comes from reopened device");
        require(std::abs(current.samplesPerTick * qpcFrequency() - 16000) < .01, "New actual Fs maps to QPC");
    });
    suite.test("WAV I/O failure propagates to whole-take collection stop", []
    {
        struct FailWrite final : FileIoFaultAdapter
        {
            juce::Result beforeIo(FileIoOperation op, const juce::File& file, std::uint64_t offset, std::size_t) override
            { return op == FileIoOperation::append && file.hasFileExtension("wav") && offset >= 44 ? juce::Result::fail("injected PCM disk write failure") : juce::Result::ok(); }
        } fault;
        Fixture f(1); f.config.faults = &fault; f.begin(0); f.feed();
        until([&] { return f.engine.error() != RecorderAudioEngine::Error::none; });
        require(f.engine.error() == RecorderAudioEngine::Error::writeFailed && f.engine.stopSample() >= 0, "Writer failure exposes a confirmed stop boundary");
        require(f.engine.finishCapture(juce::Uuid()).failed(), "Failed I/O never completes normally"); ok(f.engine.finishJournal(false));
    });
    return suite.result("RecorderAudioTests");
}
