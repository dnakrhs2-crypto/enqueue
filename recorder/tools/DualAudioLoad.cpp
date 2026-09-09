#include "DualAudioLoad.h"
#include "audio/AsioTimingBridge.h"
#include "audio/RawAudioTap.h"
#include "audio/NativePcmConverter.h"
#include "record/WavTrackWriter.h"
#include "sync/ClockMapper.h"
#include "support/ThreadPriority.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <cstring>
#include <numeric>
#include <thread>

namespace gocue::recorder::probe
{
void checkAudioQueues(const RawAudioTap& raw, const WavTrackWriter& writer, TakeStopSignal& stop) noexcept
{
    if (raw.overflows()) stop.request(AudioStopReason::rawOverflow);
    if (raw.invalidBlocks()) stop.request(AudioStopReason::invalidRaw);
    if (writer.error() != WavTrackWriter::Error::none) stop.request(AudioStopReason::writerFailure);
}
std::int32_t audioPattern(std::uint64_t sample, unsigned channel) noexcept
{
    const auto bits = (static_cast<std::uint32_t>(sample) * 7919u + channel * 104729u) & 0xffffffu;
    return bits & 0x800000u ? static_cast<std::int32_t>(bits) - 16777216 : static_cast<std::int32_t>(bits);
}
struct DualAudioLoad::State : juce::AudioIODeviceCallback
{
    Config config;
    TakeStopSignal& stop;
    const std::atomic<std::int64_t>& origin;
    std::unique_ptr<juce::AudioIODeviceType> type;
    std::unique_ptr<juce::AudioIODevice> device;
    RawAudioTap raw;
    AsioTimingBridge bridge{qpcFrequency(), &raw};
    ClockMapper clock{qpcFrequency()};
    std::unique_ptr<WavTrackWriter> writer;
    WavTrackWriter::Config wavConfig;
    std::thread producer, worker;
    std::atomic<bool> inputStopped{false}, workerDone{false}, cancel{false};
    std::atomic<bool> targetComplete{false};
    unsigned block = 480;
    std::uint64_t samples = 0, blocks = 0, lastSequence = 0, verificationErrors = 0;
    std::int64_t lastPosition = 0, lastBlockSize = 0, firstCallback = 0, firstSequence = 0;
    bool havePrevious = false, prepared = false, closed = false, finished = false;
    DWORD workerPriorityError = 0, producerPriorityError = 0;
    std::string workerError, producerError, finalError;
    juce::String driver = "synthetic-8ch-PCM24";
    AsioEventCounters finalEvents{};
    bool eventsAvailable = false;
    juce::Array<juce::var> formats;
    std::vector<std::int32_t> interleaved;
    std::vector<std::uint8_t> packed;
    State(Config c, TakeStopSignal& s, const std::atomic<std::int64_t>& o) : config(std::move(c)), stop(s), origin(o) {}
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int channels, int frames,
                                         const juce::AudioIODeviceCallbackContext&) override
    { for (int c = 0; c < channels; ++c) if (outputs[c]) std::memset(outputs[c], 0, static_cast<std::size_t>(frames) * sizeof(float)); }
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
    void audioDeviceError(const juce::String&) override { stop.request(AudioStopReason::driverError); }
    void synthesize()
    {
        ScopedRecorderPriority priority(RecorderThreadRole::audio); producerPriorityError = priority.error;
        std::array<std::vector<std::uint8_t>, 8> channels;
        std::array<NativeInputView, 8> views{};
        for (int c = 0; c < 8; ++c) { channels[c].resize(block * 3); views[c] = {channels[c].data(), c, c, nativeFormatForAsio(17)}; }
        while (!cancel && !origin.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto zero = origin.load(), frequency = qpcFrequency();
        std::uint64_t sequence = 0;
        const auto target = static_cast<std::uint64_t>(config.seconds) * 48000;
        for (std::uint64_t first = 0; first < target && !cancel && !stop.requested(); first += block)
        {
            const auto due = zero + static_cast<std::int64_t>(first) * frequency / 48000;
            while (!cancel && !stop.requested() && qpcNow() < due) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (cancel || stop.requested()) break;
            const auto count = static_cast<unsigned>(std::min<std::uint64_t>(block, target - first));
            // Deterministic source work precedes the bounded tap callback.
            for (unsigned c = 0; c < 8; ++c) for (unsigned i = 0; i < count; ++i)
                WavTrackWriter::packPcm24(audioPattern(first + i, c), channels[c].data() + i * 3);
            BlockStamp stamp{}; stamp.flags = timeInfoPresent | samplePositionValid | sampleRateValid;
            stamp.samplePosition = static_cast<std::int64_t>(first); stamp.sampleRate = 48000; stamp.numSamples = count;
            stamp.sequence = ++sequence; stamp.callbackQpc = qpcNow();
            bridge.onAsioBlock(stamp, views.data(), 8);
            if (raw.overflows()) stop.request(AudioStopReason::rawOverflow);
            if (raw.invalidBlocks()) stop.request(AudioStopReason::invalidRaw);
        }
    }
    void consume()
    {
        ScopedRecorderPriority priority(RecorderThreadRole::audio); workerPriorityError = priority.error;
        try
        {
            for (;;)
            {
                bridge.drain([](void* context, const BlockStamp& stamp) noexcept
                {
                    auto& s = *static_cast<State*>(context);
                    try { s.clock.observe(stamp); } catch (...) { s.stop.request(AudioStopReason::discontinuity); }
                    if (s.origin.load() && (stamp.xruns || stamp.resets || stamp.resyncs || stamp.latencyChanges))
                        s.stop.request(AudioStopReason::driverReset);
                }, this);
                checkAudioQueues(raw, *writer, stop);
                if (bridge.droppedStamps()) stop.request(AudioStopReason::discontinuity);
                if (const auto* input = raw.front())
                {
                    const auto zero = origin.load();
                    if (!zero || input->stamp.callbackQpc < zero || targetComplete || stop.requested()) { raw.release(); continue; }
                    const auto& stamp = input->stamp;
                    if (stamp.sampleRate != 48000 || input->numChannels != 8 || stamp.numSamples > block
                        || (havePrevious && (stamp.sequence != lastSequence + 1
                            || ((stamp.flags & samplePositionValid) && stamp.samplePosition != lastPosition + lastBlockSize))))
                    { stop.request(AudioStopReason::discontinuity); raw.release(); continue; }
                    if (!havePrevious)
                    {
                        firstCallback = stamp.callbackQpc; firstSequence = static_cast<std::int64_t>(stamp.sequence);
                        for (unsigned c = 0; c < 8; ++c)
                        {
                            auto f = jsonObject(); jsonSet(f, "physicalIndex", input->channels[c].physicalIndex);
                            jsonSet(f, "asioSampleType", input->channels[c].format.asioSampleType);
                            jsonSet(f, "conversionPolicy", NativePcmConverter::policy(input->channels[c].format)); formats.add(f);
                        }
                    }
                    lastSequence = stamp.sequence; lastPosition = stamp.samplePosition; lastBlockSize = stamp.numSamples; havePrevious = true;
                    const auto count = static_cast<unsigned>(std::min<std::uint64_t>(stamp.numSamples, static_cast<std::uint64_t>(config.seconds) * 48000 - samples));
                    bool converted = true;
                    for (unsigned c = 0; c < 8; ++c)
                    {
                        const auto& ch = input->channels[c];
                        const auto bytes = static_cast<std::size_t>(count - 1) * ch.format.strideBytes + ch.format.containerBytes;
                        const auto result = NativePcmConverter::pack(ch.format, ch.data, bytes, count, packed.data(), packed.size());
                        if (!result) { converted = false; break; }
                        for (unsigned i = 0; i < count; ++i)
                        {
                            auto bits = static_cast<std::uint32_t>(packed[i * 3]) | (std::uint32_t{packed[i * 3 + 1]} << 8) | (std::uint32_t{packed[i * 3 + 2]} << 16);
                            interleaved[i * 8 + c] = bits & 0x800000 ? static_cast<std::int32_t>(bits) - 16777216 : static_cast<std::int32_t>(bits);
                            if (config.synthetic && interleaved[i * 8 + c] != audioPattern(samples + i, c)) ++verificationErrors;
                        }
                    }
                    if (!converted || verificationErrors) stop.request(AudioStopReason::invalidRaw);
                    else if (!writer->tryPush(interleaved.data(), count, samples, stamp.callbackQpc)) stop.request(AudioStopReason::writerFailure);
                    else { samples += count; ++blocks; if (samples == static_cast<std::uint64_t>(config.seconds) * 48000) targetComplete = true; }
                    raw.release();
                }
                else if (inputStopped) break;
                else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        catch (const std::exception& e) { workerError = e.what(); stop.request(AudioStopReason::invalidRaw); }
        workerDone = true;
    }
};
DualAudioLoad::DualAudioLoad(Config c, TakeStopSignal& s, const std::atomic<std::int64_t>& o) : state(std::make_unique<State>(std::move(c), s, o)) {}
DualAudioLoad::~DualAudioLoad() { try { stopInput(); finish(); } catch (...) {} }
void DualAudioLoad::prepare()
{
    auto& s = *state;
    if (s.prepared) throw std::logic_error("Audio load is single use");
    if (!s.config.synthetic)
    {
        if (!AsioTimingBridge::hookCompiled()) throw std::runtime_error("UNAVAILABLE: shared JUCE native ASIO hook is not compiled");
        s.type.reset(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
        if (!s.type) throw std::runtime_error("UNAVAILABLE: ASIO backend is not compiled");
        s.type->scanForDevices(); const auto names = s.type->getDeviceNames();
        if (s.config.asioDevice < 0 || s.config.asioDevice >= names.size()) throw std::runtime_error("UNAVAILABLE: selected ASIO device absent");
        s.driver = names[s.config.asioDevice]; s.device.reset(s.type->createDevice(s.driver, s.driver));
        if (!s.device) throw std::runtime_error("UNAVAILABLE: create ASIO device failed");
        if (s.device->getInputChannelNames().size() < 8) throw std::runtime_error("UNAVAILABLE: ASIO exposes fewer than 8 physical inputs; use --synthetic-audio for 8ch load");
        const int frames = s.device->getDefaultBufferSize();
        if (frames < 1 || frames > 16384) throw std::runtime_error("UNAVAILABLE: ASIO default block size outside 1..16384");
        s.block = static_cast<unsigned>(frames);
    }
    std::vector<int> mapping(8); std::iota(mapping.begin(), mapping.end(), 0);
    // Conversion staging is <= one second. The durable PCM writer owns the
    // separately reported four-second queue; neither can grow during recording.
    s.raw.prepare(mapping, s.block, (48000 + s.block - 1) / s.block);
    s.interleaved.resize(s.block * 8); s.packed.resize(s.block * 3);
    auto& c = s.wavConfig; c.projectDirectory = s.config.directory; c.sampleRate = 48000; c.mics = 8; c.framesPerBlock = s.block; c.faults = s.config.faults;
    c.nativeFormat = s.config.synthetic ? "synthetic signed PCM24 LE -> RawAudioTap -> PCM24, bit exact" : "native ASIO -> explicit NativePcmConverter PCM24 policy; see report formats";
    for (int mic = 1; mic <= 8; ++mic) c.devices.push_back({s.driver, "Input " + juce::String(mic), mic, mic - 1, mic - 1});
    s.writer = std::make_unique<WavTrackWriter>(c);
    const auto result = s.writer->start(); if (result.failed()) throw std::runtime_error(result.getErrorMessage().toStdString());
    s.prepared = true; s.worker = std::thread(&State::consume, &s);
    if (s.config.synthetic) s.producer = std::thread([&s]
    {
        try { s.synthesize(); }
        catch (const std::exception& e) { s.producerError = e.what(); s.stop.request(AudioStopReason::invalidRaw); }
    });
    else
    {
        if (!s.bridge.registerTap()) throw std::runtime_error("Another ASIO tap is registered");
        juce::BigInteger inputs, outputs; inputs.setRange(0, 8, true);
        if (s.device->getOutputChannelNames().size()) outputs.setBit(0);
        const auto error = s.device->open(inputs, outputs, 48000, static_cast<int>(s.block));
        if (error.isNotEmpty()) throw std::runtime_error("UNAVAILABLE: ASIO open: " + error.toStdString());
        if (s.device->getActiveInputChannels() != inputs || s.device->getCurrentSampleRate() != 48000
            || s.device->getCurrentBufferSizeSamples() != static_cast<int>(s.block)) throw std::runtime_error("UNAVAILABLE: ASIO changed the prepared 8ch/48k/block contract");
        s.device->start(&s);
    }
}
void DualAudioLoad::stopInput()
{
    auto& s = *state; if (s.closed) return; s.cancel = true;
    if (s.device) { s.eventsAvailable = AsioTimingBridge::readEvents(*s.device, s.finalEvents); s.device->stop(); s.device->close(); }
    s.bridge.unregisterAfterDeviceClosed();
    if (s.producer.joinable()) s.producer.join();
    s.inputStopped = true; s.closed = true;
    if (s.origin.load() && (s.finalEvents.xruns || s.finalEvents.resets || s.finalEvents.resyncs || s.finalEvents.latencyChanges)) s.stop.request(AudioStopReason::driverReset);
}
void DualAudioLoad::finish()
{
    auto& s = *state; if (s.finished) return;
    if (!s.closed) throw std::logic_error("Close audio producer before draining");
    if (s.worker.joinable()) s.worker.join();
    if (s.writer)
    {
        if (s.stop.requested()) s.writer->requestAbort();
        const auto result = s.writer->stop(static_cast<std::int64_t>(s.samples), juce::Uuid());
        if (result.failed()) { s.finalError = result.getErrorMessage().toStdString(); s.stop.request(AudioStopReason::writerFailure); }
    }
    s.finished = true;
}
bool DualAudioLoad::complete() const noexcept { return state->targetComplete.load(); }
std::uint64_t DualAudioLoad::queuedWavFrames() const noexcept { return state->writer ? state->writer->queueFrames() : 0; }
juce::var DualAudioLoad::toJson() const
{
    const auto& s = *state; auto v = jsonObject();
    jsonSet(v, "sourceKind", s.config.synthetic ? "synthetic" : "ASIO hardware"); jsonSet(v, "driver", s.driver);
    jsonSet(v, "hookCompiled", AsioTimingBridge::hookCompiled()); jsonSet(v, "channels", 8); jsonSet(v, "sampleRate", 48000); jsonSet(v, "bitsPerSample", 24);
    jsonSet(v, "framesPerBlock", s.block); jsonSet(v, "acceptedSamplesPerMic", s.samples); jsonSet(v, "blocks", s.blocks);
    jsonSet(v, "firstCallbackQpc", std::to_string(s.firstCallback)); jsonSet(v, "firstSequence", std::to_string(s.firstSequence));
    jsonSet(v, "rawQueueCapacityBlocks", s.raw.capacity()); jsonSet(v, "rawQueueBytes", s.raw.allocatedBytes());
    jsonSet(v, "rawQueueHighWater", s.raw.highWater()); jsonSet(v, "rawOverflows", s.raw.overflows()); jsonSet(v, "rawInvalidBlocks", s.raw.invalidBlocks());
    jsonSet(v, "rawQueueDefinition", "At most one second of bounded native conversion staging, ahead of WavTrackWriter's four-second PCM queue");
    jsonSet(v, "stampQueueOverflows", s.bridge.droppedStamps()); jsonSet(v, "formats", s.formats);
    jsonSet(v, "nativePcmOracleErrors", s.verificationErrors); jsonSet(v, "workerError", s.workerError); jsonSet(v, "finalError", s.finalError);
    jsonSet(v, "producerError", s.producerError);
    jsonSet(v, "workerPriorityError", s.workerPriorityError); jsonSet(v, "producerPriorityError", s.producerPriorityError);
    jsonSet(v, "driverCallbackPriority", "Preserved driver-owned scheduling; no priority/COM/I/O work added to ASIO callback");
    jsonSet(v, "takeStopRequested", s.stop.requested()); jsonSet(v, "takeStopReason", static_cast<int>(s.stop.get()));
    if (s.writer && s.finished) jsonSet(v, "writer", s.writer->telemetry());
    jsonSet(v, "takeId", s.wavConfig.takeId.toDashedString());
    if (s.finished)
    {
        const auto& stats = s.bridge.statistics();
        jsonSet(v, "sampleJumps", stats.sampleJumps); jsonSet(v, "sampleDuplicates", stats.sampleDuplicates); jsonSet(v, "sampleRegressions", stats.sampleRegressions);
        jsonSet(v, "callbackJitterStddevMs", stats.callbackJitterMs.standardDeviation()); jsonSet(v, "eventsAvailable", s.eventsAvailable);
        jsonSet(v, "xruns", std::max(s.finalEvents.xruns, stats.xrunEvents)); jsonSet(v, "resets", std::max(s.finalEvents.resets, stats.resetEvents));
        if (const auto clock = s.clock.snapshot())
        { jsonSet(v, "clockEpoch", clock->epoch); jsonSet(v, "clockPpm", clock->quality.ppm); jsonSet(v, "clockGrade", clockGradeName(clock->quality.grade)); }
    }
    jsonSet(v, "complete", s.finished && s.targetComplete && !s.stop.requested() && s.finalError.empty() && s.workerError.empty());
    return v;
}
}
