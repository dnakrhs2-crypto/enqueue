#include "AsioProbe.h"
#include "AsioTimingBridge.h"
#include "RawAudioTap.h"
#include "NativePcmConverter.h"
#include "diagnostics/CaptureTelemetry.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <thread>

namespace gocue::recorder
{
namespace
{
struct Options
{
    std::map<std::string, std::string> values;
    std::set<std::string> flags;
    bool has(const std::string& key) const { return values.count(key) || flags.count(key); }
    std::string get(const std::string& key, const std::string& fallback = {}) const
    { const auto it = values.find(key); return it == values.end() ? fallback : it->second; }
};
int integer(const std::string& text, int minimum, int maximum)
{
    int value = 0; const auto p = std::from_chars(text.data(), text.data() + text.size(), value);
    if (p.ec != std::errc{} || p.ptr != text.data() + text.size() || value < minimum || value > maximum)
        throw std::invalid_argument("Integer outside allowed range: " + text);
    return value;
}
Options parseOptions(int argc, wchar_t** argv)
{
    Options result;
    const std::set<std::string> flags{"--list", "--select", "--measure-position-poll"};
    const std::set<std::string> values{"--asio-device", "--asio-inputs", "--seconds", "--report", "--test-output-map", "--sample-rate", "--buffer-size"};
    for (int i = 2; i < argc; ++i)
    {
        const auto key = juce::String(argv[i]).toStdString();
        if (result.has(key)) throw std::invalid_argument("Duplicate option: " + key);
        if (flags.count(key)) result.flags.insert(key);
        else if (values.count(key) && i + 1 < argc) result.values.emplace(key, juce::String(argv[++i]).toStdString());
        else throw std::invalid_argument("Unknown/incomplete asio option: " + key);
    }
    return result;
}
std::vector<int> parseInputs(const std::string& text, int available)
{
    if (text == "none" || text == "0") return {};
    std::vector<int> result;
    std::size_t start = 0;
    do
    {
        const auto end = text.find(',', start);
        result.push_back(integer(text.substr(start, end == std::string::npos ? end : end - start), 1, available) - 1);
        if (end == std::string::npos) break;
        start = end + 1;
    } while (true);
    std::sort(result.begin(), result.end());
    if (result.size() > 8 || std::adjacent_find(result.begin(), result.end()) != result.end())
        throw std::invalid_argument("Select at most 8 unique physical inputs");
    return result;
}
juce::BigInteger mask(const std::vector<int>& channels)
{
    juce::BigInteger result; for (auto c : channels) result.setBit(c); return result;
}
juce::var channelMap(const std::vector<int>& channels)
{
    juce::Array<juce::var> result;
    for (std::size_t i = 0; i < channels.size(); ++i)
    {
        auto item = jsonObject(); jsonSet(item, "activeIndex", static_cast<int>(i));
        jsonSet(item, "physicalIndex", channels[i]); jsonSet(item, "physicalChannel", channels[i] + 1); result.add(item);
    }
    return result;
}
juce::var moments(const RunningMoments& m, const char* unit)
{
    auto v = jsonObject(); jsonSet(v, "count", jsonInt(m.count)); jsonSet(v, "unit", unit);
    jsonSet(v, "mean", m.mean); jsonSet(v, "stddevPopulation", m.standardDeviation());
    jsonSet(v, "min", m.minimum); jsonSet(v, "max", m.maximum); return v;
}
juce::var regression(const AsioRegression& r)
{
    auto v = jsonObject(); jsonSet(v, "valid", r.valid); jsonSet(v, "observations", jsonInt(r.observations));
    jsonSet(v, "originQpc", std::to_string(r.originQpc)); jsonSet(v, "originSamplePosition", std::to_string(r.originSamplePosition));
    jsonSet(v, "samplesPerQpcTick", r.samplesPerQpcTick); jsonSet(v, "samplesPerSecond", r.samplesPerSecond);
    jsonSet(v, "interceptSamplesAtOrigin", r.interceptAtOrigin); jsonSet(v, "residualRmsSamples", r.residualRmsSamples);
    jsonSet(v, "residualMaxAbsSamples", r.residualMaxAbsSamples); jsonSet(v, "residualP95AbsSamples", r.residualP95AbsSamples);
    jsonSet(v, "residualP99AbsSamples", r.residualP99AbsSamples);
    jsonSet(v, "definition", "OLS over latest uninterrupted epoch, last <=10 seconds / <=4096 pairs; arrival jitter retained; no clock correction or outlier removal");
    return v;
}
juce::var stampJson(const BlockStamp& s)
{
    auto v = jsonObject(); jsonSet(v, "flags", jsonInt(s.flags)); jsonSet(v, "asioTimeInfoFlags", jsonInt(s.asioTimeInfoFlags));
    jsonSet(v, "samplePosition", std::to_string(s.samplePosition)); jsonSet(v, "systemTimeRaw", std::to_string(s.systemTimeRaw));
    jsonSet(v, "callbackQpc", std::to_string(s.callbackQpc)); jsonSet(v, "sequence", std::to_string(s.sequence));
    jsonSet(v, "sampleRate", s.sampleRate); jsonSet(v, "bufferIndex", s.bufferIndex); jsonSet(v, "numSamples", jsonInt(s.numSamples));
    jsonSet(v, "inputLatencySamples", s.inputLatencySamples); jsonSet(v, "outputLatencySamples", s.outputLatencySamples);
    jsonSet(v, "xruns", jsonInt(s.xruns)); jsonSet(v, "resets", jsonInt(s.resets)); jsonSet(v, "resyncs", jsonInt(s.resyncs));
    jsonSet(v, "latencyChanges", jsonInt(s.latencyChanges)); return v;
}
juce::var timingJson(const AsioTimingBridge& bridge)
{
    const auto& s = bridge.statistics(); auto v = jsonObject();
    jsonSet(v, "blocks", jsonInt(s.blocks)); jsonSet(v, "samples", jsonInt(s.samples));
    jsonSet(v, "timeInfoBlocks", jsonInt(s.timeInfoBlocks)); jsonSet(v, "samplePositionValidBlocks", jsonInt(s.samplePositionBlocks));
    jsonSet(v, "systemTimeValidBlocks", jsonInt(s.systemTimeBlocks));
    jsonSet(v, "timeInfoSupport", s.timeInfoBlocks ? "observed" : s.blocks ? "not observed; legacy bufferSwitch" : "unavailable; no callbacks");
    jsonSet(v, "qpcRegressions", jsonInt(s.qpcRegressions)); jsonSet(v, "sampleRegressions", jsonInt(s.sampleRegressions));
    jsonSet(v, "sampleDuplicates", jsonInt(s.sampleDuplicates)); jsonSet(v, "sampleJumps", jsonInt(s.sampleJumps));
    jsonSet(v, "observationGaps", jsonInt(s.observationGaps)); jsonSet(v, "rateChanges", jsonInt(s.rateChanges));
    jsonSet(v, "xrunEvents", jsonInt(s.xrunEvents)); jsonSet(v, "resetEvents", jsonInt(s.resetEvents));
    jsonSet(v, "resyncEvents", jsonInt(s.resyncEvents)); jsonSet(v, "latencyChangeEvents", jsonInt(s.latencyChangeEvents));
    jsonSet(v, "overloadReportingSupported", (s.last.flags & overloadReportingSupported) != 0);
    jsonSet(v, "callbackInterval", moments(s.callbackIntervalMs, "ms")); jsonSet(v, "callbackJitter", moments(s.callbackJitterMs, "ms"));
    jsonSet(v, "sampleInterval", moments(s.sampleInterval, "samples")); jsonSet(v, "callbackRegression", regression(s.regression()));
    jsonSet(v, "pollCost", moments(s.pollCostUs, "us")); jsonSet(v, "pollFailures", jsonInt(s.pollFailures));
    jsonSet(v, "pollRegression", regression(s.pollRegression()));
    jsonSet(v, "pollDefinition", "Control-thread getSamplePosition at <=10 Hz; QPC brackets driver call, midpoint used only for separate approximate regression. Not the callback's block timestamp.");
    juce::Array<juce::var> examples;
    for (std::size_t i = 0; i < s.systemTimeExampleCount; ++i) examples.add(juce::String(std::to_string(s.systemTimeExamples[i])));
    jsonSet(v, "systemTimeRawExamples", examples);
    jsonSet(v, "systemTimeInterpretation", "Raw unsigned 64-bit decimal strings; driver unit, epoch and buffer reference unverified. Never treated as QPC or hostTimeNs.");
    jsonSet(v, "firstBlock", stampJson(s.first)); jsonSet(v, "lastBlock", stampJson(s.last));
    jsonSet(v, "stampQueueCapacity", jsonInt(AsioTimingBridge::stampQueueCapacity)); jsonSet(v, "stampQueueHighWater", jsonInt(bridge.stampHighWater()));
    jsonSet(v, "stampQueueDrops", jsonInt(bridge.droppedStamps())); jsonSet(v, "pollQueueDrops", jsonInt(bridge.droppedPolls())); return v;
}
// Output tables are prepared off callback. The callback only clears/copies;
// table position is callback-owned, failure is a lock-free scalar signal.
class ToneOutput final : public juce::AudioIODeviceCallback
{
public:
    void prepare(double rate, const std::vector<int>& outputs, int left, int right, bool tone)
    {
        tables.resize(outputs.size());
        const auto length = static_cast<std::size_t>(std::max(1.0, std::round(rate)));
        for (std::size_t c = 0; c < outputs.size(); ++c)
        {
            tables[c].resize(length, 0);
            const double hz = outputs[c] == left ? 440.0 : outputs[c] == right ? 660.0 : 0.0;
            if (tone) for (std::size_t i = 0; i < length; ++i) tables[c][i] = static_cast<float>(0.05 * std::sin(6.283185307179586 * hz * static_cast<double>(i) / rate));
        }
    }
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int count, int samples,
                                         const juce::AudioIODeviceCallbackContext&) override
    {
        for (int c = 0; c < count; ++c)
        {
            if (!outputs[c]) continue;
            if (static_cast<std::size_t>(c) >= tables.size() || tables[c].empty())
            { std::memset(outputs[c], 0, static_cast<std::size_t>(samples) * sizeof(float)); continue; }
            auto offset = cursor; std::size_t copied = 0;
            while (copied < static_cast<std::size_t>(samples))
            {
                const auto n = std::min(tables[c].size() - offset, static_cast<std::size_t>(samples) - copied);
                std::memcpy(outputs[c] + copied, tables[c].data() + offset, n * sizeof(float));
                copied += n; offset = (offset + n) % tables[c].size();
            }
        }
        if (!tables.empty()) cursor = (cursor + static_cast<std::size_t>(samples)) % tables.front().size();
        callbacks.fetch_add(1, std::memory_order_relaxed);
    }
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override { stopped.store(true, std::memory_order_relaxed); }
    void audioDeviceError(const juce::String&) override { failed.store(true, std::memory_order_relaxed); }
    std::atomic<bool> failed{false}, stopped{false};
    std::atomic<std::uint64_t> callbacks{0};
private:
    std::vector<std::vector<float>> tables;
    std::size_t cursor = 0;
};
struct ChannelMeter
{
    NativeFormat format{};
    std::uint64_t samples = 0, saturated = 0;
    double sumSquares = 0;
};
struct Collection
{
    RawAudioTap raw;
    AsioTimingBridge bridge{qpcFrequency(), &raw};
    ToneOutput tone;
    std::vector<std::uint8_t> packed;
    std::array<ChannelMeter, 8> meters{};
    std::uint64_t conversionErrors = 0;
    const char* firstConversionError = nullptr;
    std::uint32_t firstErrorSample = 0;
    std::int32_t firstErrorPhysicalIndex = -1;
    std::uint64_t firstErrorSequence = 0;
    std::atomic<bool> done{false}, fatal{false};
    std::thread worker;
    juce::AudioIODevice* device = nullptr;
    void consume() noexcept
    {
        bridge.drain();
        while (const auto* block = raw.front())
        {
            std::array<PcmConversionResult, 8> results{}; bool valid = true;
            for (std::uint32_t c = 0; c < block->numChannels; ++c)
            {
                const auto& view = block->channels[c]; auto& meter = meters[c];
                if (meter.samples && !sameNativeFormat(meter.format, view.format))
                {
                    if (!firstConversionError)
                    { firstConversionError = "native format changed"; firstErrorPhysicalIndex = view.physicalIndex; firstErrorSequence = block->stamp.sequence; }
                    valid = false; break;
                }
                const auto n = block->stamp.numSamples;
                results[c] = NativePcmConverter::pack(view.format, view.data, static_cast<std::size_t>(n - 1) * view.format.strideBytes + view.format.containerBytes,
                    n, packed.data() + c * (packed.size() / 8), packed.size() / 8);
                meter.format = view.format;
                if (!results[c])
                {
                    if (!firstConversionError)
                    {
                        firstConversionError = results[c].error == PcmConversionError::nonFinite ? "non-finite input; whole block rejected"
                            : results[c].error == PcmConversionError::unsupportedFormat ? "unsupported native format" : "invalid native buffer";
                        firstErrorSample = results[c].errorSample; firstErrorPhysicalIndex = view.physicalIndex; firstErrorSequence = block->stamp.sequence;
                    }
                    valid = false; break;
                }
            }
            if (valid)
            {
                for (std::uint32_t c = 0; c < block->numChannels; ++c)
                { meters[c].samples += results[c].samplesWritten; meters[c].sumSquares += results[c].sumSquares; meters[c].saturated += results[c].saturatedSamples; }
            }
            else { ++conversionErrors; fatal.store(true, std::memory_order_relaxed); }
            raw.release();
        }
    }
    void stop()
    {
        if (device) { device->close(); device = nullptr; } // ASIO stop/join before unpublishing tap
        bridge.unregisterAfterDeviceClosed(); done.store(true, std::memory_order_release);
        if (worker.joinable()) worker.join();
    }
    ~Collection() { stop(); }
};
void setStatus(juce::var& report, const char* result, const std::string& reason)
{
    jsonSet(report, "result", result); jsonSet(report, "status", std::string(result) == "PASS" ? "available" : std::string(result) == "FAIL" ? "failed" : "unavailable");
    jsonSet(report, "reason", reason);
}
void run(const Options& options, juce::var& report)
{
    juce::ScopedJuceInitialiser_GUI juceRuntime;
    std::unique_ptr<juce::AudioIODeviceType> type(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
    if (!type) { setStatus(report, "UNAVAILABLE", "JUCE was built without ASIO"); return; }
    type->scanForDevices(); const auto names = type->getDeviceNames(); juce::Array<juce::var> devices;
    for (int i = 0; i < names.size(); ++i)
    { auto item = jsonObject(); jsonSet(item, "deviceIndex", i); jsonSet(item, "name", names[i]); devices.add(item); }
    jsonSet(report, "devices", devices);
    if (names.isEmpty()) { setStatus(report, "UNAVAILABLE", "No ASIO devices enumerated"); return; }
    if (options.has("--list") || (!options.has("--asio-device") && !options.has("--select") && !options.has("--seconds")))
    { setStatus(report, "PASS", "ASIO registry enumeration only; no device opened"); return; }
    std::string deviceOption = options.get("--asio-device");
    if (deviceOption.empty() && options.has("--select"))
    {
        for (int i = 0; i < names.size(); ++i) std::cerr << i << ": " << names[i] << '\n';
        std::cerr << "ASIO device index (zero-based): "; std::getline(std::cin, deviceOption);
    }
    const int index = integer(deviceOption, 0, names.size() - 1);
    const int seconds = integer(options.get("--seconds", "10"), 1, 86400);
    jsonSet(report, "selectedDeviceIndex", index); jsonSet(report, "driverName", names[index]); jsonSet(report, "requestedSeconds", seconds);
    if (!AsioTimingBridge::hookCompiled())
    { setStatus(report, "UNAVAILABLE", "Shared JUCE lacks enabled 0002 timing/native patch; apply patch and rebuild Recorder targets"); return; }
    std::unique_ptr<juce::AudioIODevice> device(type->createDevice(names[index], names[index]));
    if (!device) { setStatus(report, "UNAVAILABLE", "ASIO device creation failed"); return; }
    auto inputNames = device->getInputChannelNames(), outputNames = device->getOutputChannelNames();
    juce::Array<juce::var> inputList, outputList;
    for (auto name : inputNames) inputList.add(name);
    for (auto name : outputNames) outputList.add(name);
    jsonSet(report, "inputChannelNames", inputList); jsonSet(report, "outputChannelNames", outputList);
    std::string inputsOption = options.get("--asio-inputs", "none");
    if (options.has("--select") && !options.has("--asio-inputs"))
    {
        for (int i = 0; i < inputNames.size(); ++i) std::cerr << i + 1 << ": " << inputNames[i] << '\n';
        std::cerr << "Physical inputs (one-based 1,2,... or none): "; std::getline(std::cin, inputsOption);
    }
    const auto inputs = parseInputs(inputsOption, inputNames.size());
    std::vector<int> outputs; int left = -1, right = -1;
    if (options.has("--test-output-map"))
    {
        const auto map = options.get("--test-output-map"); const auto colon = map.find(':');
        left = integer(map.substr(0, colon), 1, outputNames.size()) - 1;
        right = colon == std::string::npos ? left : integer(map.substr(colon + 1), 1, outputNames.size()) - 1;
        outputs = {left}; if (right != left) outputs.push_back(right); std::sort(outputs.begin(), outputs.end());
    }
    else if (!outputNames.isEmpty()) outputs = {0}; // silent output keeps mic-OFF clock running
    if (inputs.empty() && outputs.empty()) { setStatus(report, "UNAVAILABLE", "Mic OFF requires at least one ASIO output channel"); return; }
    jsonSet(report, "activeIndexToPhysicalIndex", channelMap(inputs)); jsonSet(report, "outputActiveIndexToPhysicalIndex", channelMap(outputs));
    jsonSet(report, "micOff", inputs.empty()); jsonSet(report, "toneEnabled", options.has("--test-output-map"));
    jsonSet(report, "toneLeftPhysicalChannel", left + 1); jsonSet(report, "toneRightPhysicalChannel", right + 1);
    jsonSet(report, "toneDefinition", "0.05 peak; L=440Hz, R=660Hz; L=R or single channel => mono 440Hz; other outputs silent");
    const double rate = options.has("--sample-rate") ? integer(options.get("--sample-rate"), 8000, 768000) : device->getCurrentSampleRate();
    const int preferredBlock = device->getDefaultBufferSize();
    const int block = options.has("--buffer-size") ? integer(options.get("--buffer-size"), 1, 262144) : preferredBlock;
    // JUCE may retry createBuffers at the preferred size. Reserve both choices,
    // not every advertised size (some drivers advertise enormous maxima).
    // A later change beyond this bound is an explicit rawInvalidBlocks failure.
    const int maximumBlock = std::max(block, preferredBlock);
    if (block <= 0 || maximumBlock <= 0 || maximumBlock > 262144 || !std::isfinite(rate) || rate <= 0 || rate > 768000)
        throw std::runtime_error("Driver reported invalid sample rate/buffer sizes");
    auto collection = std::make_unique<Collection>(); collection->device = device.get();
    const auto queueBlocks = static_cast<std::size_t>(std::ceil(4.0 * rate / block)) + 2;
    collection->raw.prepare(inputs, static_cast<std::uint32_t>(maximumBlock), queueBlocks);
    collection->packed.resize(static_cast<std::size_t>(maximumBlock) * 3 * 8);
    if (!collection->bridge.registerTap()) throw std::runtime_error("Another Recorder ASIO tap is registered");
    collection->worker = std::thread([&]
    {
        while (!collection->done.load(std::memory_order_acquire)) { collection->consume(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        collection->consume();
    });
    const auto error = device->open(mask(inputs), mask(outputs), rate, block);
    if (error.isNotEmpty()) { collection->stop(); setStatus(report, "UNAVAILABLE", error.toStdString()); return; }
    if (device->getActiveInputChannels() != mask(inputs) || device->getActiveOutputChannels() != mask(outputs))
    { collection->stop(); setStatus(report, "UNAVAILABLE", "Driver changed the requested physical channel map"); return; }
    jsonSet(report, "actualSampleRate", device->getCurrentSampleRate()); jsonSet(report, "actualBlockSize", device->getCurrentBufferSizeSamples());
    jsonSet(report, "preparedMaxBlockSize", maximumBlock); jsonSet(report, "inputLatencySamples", device->getInputLatencyInSamples());
    jsonSet(report, "outputLatencySamples", device->getOutputLatencyInSamples());
    collection->tone.prepare(device->getCurrentSampleRate(), outputs, left, right, options.has("--test-output-map"));
    device->start(&collection->tone);
    const auto start = std::chrono::steady_clock::now(), deadline = start + std::chrono::seconds(seconds);
    auto nextPoll = start;
    bool interrupted = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        { if (message.message == WM_QUIT) interrupted = true; else { TranslateMessage(&message); DispatchMessageW(&message); } }
        if (interrupted || collection->fatal.load() || collection->tone.failed.load() || collection->tone.stopped.load()
            || collection->raw.overflows() || collection->raw.invalidBlocks()) break;
        const auto now = std::chrono::steady_clock::now();
        if (options.has("--measure-position-poll") && now >= nextPoll)
        {
            SamplePositionPoll p{}; AsioTimingBridge::poll(*device, p); collection->bridge.enqueuePoll(p);
            nextPoll = now + std::chrono::milliseconds(100);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const bool deviceFailed = collection->tone.failed.load() || collection->tone.stopped.load();
    jsonSet(report, "elapsedSeconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    collection->stop();
    jsonSet(report, "timing", timingJson(collection->bridge));
    AsioEventCounters finalEvents{};
    const bool eventsAvailable = AsioTimingBridge::readEvents(*device, finalEvents);
    auto eventReport = jsonObject(); jsonSet(eventReport, "available", eventsAvailable);
    jsonSet(eventReport, "xruns", jsonInt(finalEvents.xruns)); jsonSet(eventReport, "resets", jsonInt(finalEvents.resets));
    jsonSet(eventReport, "resyncs", jsonInt(finalEvents.resyncs)); jsonSet(eventReport, "latencyChanges", jsonInt(finalEvents.latencyChanges));
    jsonSet(eventReport, "definition", "Device lifetime counters after ASIO close; includes events with no following bufferSwitch. First block counters are the observation baseline.");
    jsonSet(report, "finalDriverEvents", eventReport);
    jsonSet(report, "rawQueueCapacityBlocks", jsonInt(collection->raw.capacity())); jsonSet(report, "rawQueueBytes", jsonInt(collection->raw.allocatedBytes()));
    jsonSet(report, "rawQueueHighWater", jsonInt(collection->raw.highWater())); jsonSet(report, "rawQueueOverflows", jsonInt(collection->raw.overflows()));
    jsonSet(report, "rawInvalidBlocks", jsonInt(collection->raw.invalidBlocks())); jsonSet(report, "conversionErrors", jsonInt(collection->conversionErrors));
    if (collection->firstConversionError)
    {
        auto errorReport = jsonObject(); jsonSet(errorReport, "reason", collection->firstConversionError);
        jsonSet(errorReport, "sampleWithinBlock", jsonInt(collection->firstErrorSample));
        jsonSet(errorReport, "physicalIndex", collection->firstErrorPhysicalIndex); jsonSet(errorReport, "sequence", std::to_string(collection->firstErrorSequence));
        jsonSet(report, "firstConversionError", errorReport);
    }
    jsonSet(report, "outputCallbacks", jsonInt(collection->tone.callbacks.load()));
    juce::Array<juce::var> channels;
    for (std::size_t c = 0; c < inputs.size(); ++c)
    {
        const auto& m = collection->meters[c]; auto item = jsonObject();
        jsonSet(item, "activeIndex", static_cast<int>(c)); jsonSet(item, "physicalIndex", inputs[c]); jsonSet(item, "physicalChannel", inputs[c] + 1);
        jsonSet(item, "asioSampleType", m.format.asioSampleType); jsonSet(item, "validBits", jsonInt(m.format.validBits));
        jsonSet(item, "containerBytes", jsonInt(m.format.containerBytes)); jsonSet(item, "strideBytes", jsonInt(m.format.strideBytes));
        jsonSet(item, "byteOrder", m.format.byteOrder == NativeByteOrder::little ? "LE" : "BE");
        jsonSet(item, "alignment", m.format.alignment == NativeAlignment::leastSignificant ? "right" : "left");
        jsonSet(item, "encoding", m.format.encoding == NativeEncoding::signedInteger ? "signedInteger" : m.format.encoding == NativeEncoding::ieeeFloat ? "float32" : "unsupported");
        jsonSet(item, "conversionPolicy", NativePcmConverter::policy(m.format)); jsonSet(item, "samples", jsonInt(m.samples));
        jsonSet(item, "rmsPcm24Normalised", m.samples ? std::sqrt(m.sumSquares / static_cast<double>(m.samples)) : 0);
        jsonSet(item, "saturatedSamples", jsonInt(m.saturated)); channels.add(item);
    }
    jsonSet(report, "nativeInputs", channels);
    const auto& s = collection->bridge.statistics();
    const bool trailingEvent = eventsAvailable && s.blocks && (finalEvents.xruns != s.first.xruns || finalEvents.resets != s.first.resets || finalEvents.resyncs != s.first.resyncs);
    const bool failed = interrupted || deviceFailed || trailingEvent || collection->conversionErrors || collection->raw.overflows() || collection->raw.invalidBlocks()
        || collection->bridge.droppedStamps() || collection->bridge.droppedPolls() || s.invalidBlocks || s.sampleRegressions || s.sampleDuplicates
        || s.sampleJumps || s.qpcRegressions || s.rateChanges || s.resetEvents || s.resyncEvents || s.xrunEvents;
    if (!s.blocks || !collection->tone.callbacks.load()) setStatus(report, "UNAVAILABLE", "No ASIO block/tone callbacks observed");
    else setStatus(report, failed ? "FAIL" : "PASS", failed ? "Observed capture/timing errors; inspect counters" : "Requested software observation completed; hardware loss, clock accuracy and audible routing require independent measurement");
    jsonSet(report, "outputOnlyClockObserved", inputs.empty() && s.blocks > 0 && collection->tone.callbacks.load() > 0);
    jsonSet(report, "captureInterval", "Raw/timing collection includes ASIO open/start priming until close; elapsedSeconds is the requested running interval after start(callback)");
}
}
int runAsioProbe(int argc, wchar_t** argv)
{
    const auto options = parseOptions(argc, argv);
    auto report = jsonObject(); juce::Array<juce::var> command;
    for (int i = 0; i < argc; ++i) command.add(juce::String(argv[i]));
    jsonSet(report, "schemaVersion", 1); jsonSet(report, "command", command); jsonSet(report, "sourceKind", "hardware");
    jsonSet(report, "startedUtc", utcNowIso8601()); jsonSet(report, "qpcFrequency", std::to_string(qpcFrequency()));
    jsonSet(report, "juceVersion", juce::SystemStats::getJUCEVersion()); jsonSet(report, "os", juce::SystemStats::getOperatingSystemName());
    jsonSet(report, "hookEnabled", AsioTimingBridge::hookCompiled());
    jsonSet(report, "indexConvention", "device index zero-based; CLI physical channels one-based; activeIndex/physicalIndex zero-based");
    jsonSet(report, "driverVersion", "unavailable via JUCE AudioIODevice; record installed driver version with measurement evidence");
    jsonSet(report, "realtimeScope", "Recorder tap/queues do no allocation, locks, I/O, COM or logging. Existing JUCE callbackLock and driver outputReady remain outside the tap.");
    try { run(options, report); }
    catch (const std::invalid_argument& e) { setStatus(report, "FAIL", e.what()); }
    catch (const std::exception& e) { setStatus(report, "UNAVAILABLE", e.what()); }
    jsonSet(report, "endedUtc", utcNowIso8601());
    if (options.has("--report"))
        CaptureTelemetry::writeJson(juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(options.get("--report"))), report);
    std::cout << juce::JSON::toString(report, false) << '\n';
    return report["result"].toString() == "PASS" ? 0 : report["result"].toString() == "UNAVAILABLE" ? 2 : 1;
}
}
