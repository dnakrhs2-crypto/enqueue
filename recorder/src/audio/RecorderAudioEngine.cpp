#include "RecorderAudioEngine.h"
#include "RawAudioTap.h"
#include "NativePcmConverter.h"
#include "sync/ClockMath.h"
#include "storage/IoHealth.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <thread>

namespace gocue::recorder
{
namespace
{
void pauseWorker() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
void check(const juce::Result& result) { if (result.failed()) throw std::runtime_error(result.getErrorMessage().toStdString()); }
juce::Result failure(const char* message) { return juce::Result::fail(juce::String::fromUTF8(message)); }
juce::BigInteger mask(const std::vector<int>& channels) { juce::BigInteger v; for (auto i : channels) v.setBit(i); return v; }
std::atomic<void*> deviceOwner{nullptr};
static_assert(std::atomic<float>::is_always_lock_free && std::atomic<double>::is_always_lock_free
              && std::atomic<std::int64_t>::is_always_lock_free && std::atomic<void*>::is_always_lock_free);
}
struct RecorderAudioEngine::Impl final : juce::AudioIODeviceCallback
{
    struct Session
    {
        Impl& engine;
        TakeConfig config;
        DeviceInfo device;
        std::vector<unsigned> logical;
        std::vector<JournalDeviceMapping> mapping;
        unsigned pcmChannels = 0;
        std::vector<unsigned> channelOffsets;
        RawAudioTap raw;
        IoHealth ioHealth;
        RecordingJournal journal;
        std::mutex journalMutex;
        std::unique_ptr<WavTrackWriter> wav;
        std::shared_ptr<PeakCache> peakCache;
        std::unique_ptr<ReferenceMixWriter> reference;
        std::unique_ptr<PlaybackBlockQueue> referenceQueue;
        std::thread worker, referenceWorker;
        std::atomic<std::int64_t> requestStart{-1}, scheduledStart{-1}, requestStop{-1}, n0{-1}, nstop{-1}, end{-1};
        std::atomic<std::uint64_t> adoptedSequence{0};
        std::atomic<Error> fatal{Error::none};
        std::atomic<bool> finishing{false}, finished{false}, referenceInputDone{false}, referenceError{false}, journalClosed{false};
        juce::Uuid editId;
        std::array<std::atomic<float>, 8> peak{};
        std::array<double, 8> squares{};
        std::array<NativeFormat, 16> formats{};
        std::array<bool, 16> haveFormat{};
        std::uint64_t converted = 0;
        double referenceSquares = 0;
        juce::String workerError, referenceMessage;
        juce::var wavReport, referenceReport;
        bool startedJournal = false, stoppedJournal = false;
        AsioEventCounters baselineEvents{};
        std::unique_ptr<IPlaybackBlockProvider> dubReference;
        std::int64_t referenceStart = 0, inputCorrection = 0;
        bool dubbing = false;
        Session(Impl& parent, TakeConfig c, bool recordMics = true,
                std::unique_ptr<IPlaybackBlockProvider> dub = {}, std::int64_t pstart = 0, std::int64_t correction = 0)
            : engine(parent), config(std::move(c)), device(parent.info),
            logical(recordMics ? parent.armed() : std::vector<unsigned>{}),
            mapping(recordMics ? parent.mappings() : std::vector<JournalDeviceMapping>{}), ioHealth(config.faults), journal(&ioHealth),
            dubReference(std::move(dub)), referenceStart(pstart), inputCorrection(correction), dubbing(dubReference != nullptr)
        {
            if (config.projectDirectory == juce::File() || config.takeId.isNull()) throw std::invalid_argument("Missing audio take directory/ID");
            if (config.microphoneAssetIds.empty()) for (std::size_t i = 0; i < logical.size(); ++i) config.microphoneAssetIds.push_back(newId());
            if (config.microphoneAssetIds.size() != logical.size()) throw std::invalid_argument("Audio asset IDs do not match armed microphones");
            for (const auto& m : mapping)
            {
                channelOffsets.push_back(pcmChannels); pcmChannels += m.channels();
                for (int index : {m.activeIndex, m.rightActiveIndex})
                    if (index >= 0 && !NativePcmConverter::supports(nativeFormatForAsio(engine.nativeTypes[std::size_t(index)].load())))
                        throw std::invalid_argument("Selected ASIO native input format cannot be preserved as PCM24");
            }
            if (engine.device) AsioTimingBridge::readEvents(*engine.device, baselineEvents);
            raw.prepare(device.activeToPhysical, device.bufferFrames,
                        (std::uint64_t(device.sampleRate) * 4 + device.bufferFrames - 1) / device.bufferFrames);
            check(journal.open(config.projectDirectory.getChildFile("journal")));
            if (!logical.empty())
            {
                WavTrackWriter::Config w;
                w.projectDirectory = config.projectDirectory; w.takeId = config.takeId;
                w.sampleRate = device.sampleRate; w.framesPerBlock = device.bufferFrames; w.mics = unsigned(logical.size());
                w.devices = mapping; w.logicalMicrophones = logical; w.faults = &ioHealth;
                for (const auto& m : mapping) w.slotChannels.push_back(m.channels());
                peakCache = std::make_shared<PeakCache>(device.sampleRate, pcmChannels); w.peakCache = peakCache;
                w.checkpointSink = [this](const JournalCheckpoint& cp)
                { std::lock_guard<std::mutex> lock(journalMutex); return journal.append(cp); };
                wav = std::make_unique<WavTrackWriter>(std::move(w)); check(wav->start());
            }
            if (config.referencePackets)
            {
                reference = std::make_unique<ReferenceMixWriter>(device.sampleRate);
                referenceQueue = std::make_unique<PlaybackBlockQueue>(device.bufferFrames,
                    unsigned((std::uint64_t(device.sampleRate) * 4 + device.bufferFrames - 1) / device.bufferFrames));
            }
        }
        ~Session()
        {
            signal(Error::cancelled);
            finishing.store(true, std::memory_order_release);
            if (worker.joinable()) worker.join();
            referenceInputDone = true;
            if (referenceWorker.joinable()) referenceWorker.join();
        }
        void signal(Error reason) noexcept
        {
            auto expected = Error::none; fatal.compare_exchange_strong(expected, reason);
            // This is the already confirmed exclusive boundary, even when an
            // ASIO reset prevents another callback. Never invent a future sample.
            const auto start = n0.load(std::memory_order_acquire), last = end.load(std::memory_order_acquire);
            if (start >= 0 && last >= start) { std::int64_t unset = -1; nstop.compare_exchange_strong(unset, last); }
        }
        void launch()
        {
            if (reference) referenceWorker = std::thread([this] { runReference(); });
            worker = std::thread([this] { run(); });
        }
        void runReference()
        {
            try
            {
                std::vector<float> referenceLeft(device.bufferFrames), referenceRight(device.bufferFrames), stereo(std::size_t(device.bufferFrames) * 2);
                for (;;)
                {
                    const auto done = referenceInputDone.load(std::memory_order_acquire);
                    const auto count = referenceQueue->consume(referenceLeft.data(), referenceRight.data(), device.bufferFrames);
                    if (count)
                    {
                        for (unsigned i = 0; i < count; ++i) { stereo[i * 2] = referenceLeft[i]; stereo[i * 2 + 1] = referenceRight[i]; }
                        reference->append(stereo.data(), count, config.referencePackets);
                    }
                    else if (done) break;
                    else pauseWorker();
                }
                if (converted) reference->finishInput(config.referencePackets);
                referenceReport = reference->toJson();
            }
            catch (const std::exception& e) { referenceMessage = juce::String::fromUTF8(e.what()); referenceError = true; }
        }
        void startJournal(std::int64_t sample)
        {
            JournalTakeStarted s; s.takeId = config.takeId; s.n0 = sample; s.pstart = config.placementSample;
            if (dubbing) { s.o0 = sample; s.usesOutputOrigin = true; s.placementMode = "dub"; }
            s.pcm.sampleRate = device.sampleRate; s.devices = mapping; s.files = config.additionalFiles;
            if (!mapping.empty() && std::all_of(mapping.begin(), mapping.end(), [](const auto& m) { return m.channels() == 2; }))
            { s.pcm.channels = 2; s.pcm.blockAlign = 6; }
            for (auto& file : s.files) file.assetId = juce::Uuid(file.assetId).toDashedString();
            juce::String formatsText;
            for (std::size_t c = 0; c < logical.size(); ++c)
            {
                const auto path = WavTrackWriter::chunkPath(config.takeId, logical[c], 1);
                s.files.push_back({juce::Uuid(config.microphoneAssetIds[c]).toDashedString(), path, path.upToLastOccurrenceOf("/", true, false) + "{chunk}.wav", mapping[c].channels()});
                const auto sampleType = engine.nativeTypes[std::size_t(mapping[c].activeIndex)].load();
                formatsText += "mic" + juce::String(int(logical[c])) + " ASIO type " + juce::String(sampleType) + ": "
                    + juce::String(NativePcmConverter::policy(nativeFormatForAsio(sampleType))) + "; ";
            }
            s.pcm.nativeFormat = formatsText.isEmpty() ? "No microphone inputs; output callback clock" : formatsText;
            { std::lock_guard<std::mutex> lock(journalMutex); check(journal.append(s)); }
            startedJournal = true;
            if (sample < engine.cursor.load()) { signal(Error::missedStart); return; }
            scheduledStart.store(sample, std::memory_order_release);
        }
        void run()
        {
            try
            {
                std::vector<std::uint8_t> packed(std::size_t(device.bufferFrames) * 3);
                std::vector<std::int32_t> pcm(std::size_t(device.bufferFrames) * pcmChannels);
                std::vector<float> stereo(std::size_t(device.bufferFrames) * 2);
                for (;;)
                {
                    if (!startedJournal && requestStart.load(std::memory_order_acquire) >= 0 && fatal.load() == Error::none)
                        startJournal(requestStart.load());
                    const bool finish = finishing.load(std::memory_order_acquire);
                    if (finish && startedJournal && !stoppedJournal && nstop.load() >= 0)
                    {
                        std::lock_guard<std::mutex> lock(journalMutex);
                        check(journal.append(JournalTakeStopped{config.takeId, nstop.load(), editId})); stoppedJournal = true;
                    }
                    if (wav && wav->error() != WavTrackWriter::Error::none)
                        signal(wav->error() == WavTrackWriter::Error::queueOverflow ? Error::pcmOverflow : Error::writeFailed);
                    const auto* block = raw.front();
                    if (!block) { if (finish) break; pauseWorker(); continue; }
                    // Native bytes are published before transport, so wait for
                    // that same callback's adoption release before cropping.
                    if (adoptedSequence.load(std::memory_order_acquire) <= block->stamp.sequence) { pauseWorker(); continue; }
                    const auto start = n0.load(), stop = nstop.load();
                    const auto first = std::max(block->stamp.samplePosition, start);
                    auto last = block->stamp.samplePosition + block->stamp.numSamples;
                    if (stop >= 0) last = std::min(last, stop);
                    if (start < 0 || last <= first) { raw.release(); continue; }
                    const auto frames = unsigned(last - first), skip = unsigned(first - block->stamp.samplePosition);
                    if (std::uint64_t(first - start) != converted) { raw.release(); signal(Error::invalidNative); continue; }
                    std::fill(stereo.begin(), stereo.end(), 0.0f);
                    bool valid = true;
                    for (std::size_t c = 0; c < logical.size() && valid; ++c)
                    {
                        const auto& slot = mapping[c]; const auto logicalIndex = logical[c] - 1;
                        for (unsigned ch = 0; ch < slot.channels(); ++ch)
                        {
                            const auto& view = block->channels[std::size_t(ch ? slot.rightActiveIndex : slot.activeIndex)];
                            const auto formatIndex = logicalIndex * 2 + ch;
                            if (haveFormat[formatIndex] && !sameNativeFormat(formats[formatIndex], view.format)) { valid = false; break; }
                            formats[formatIndex] = view.format; haveFormat[formatIndex] = true;
                            const auto* data = static_cast<const std::uint8_t*>(view.data) + std::size_t(skip) * view.format.strideBytes;
                            const auto bytes = std::size_t(frames - 1) * view.format.strideBytes + view.format.containerBytes;
                            const auto result = NativePcmConverter::pack(view.format, data, bytes, frames, packed.data(), packed.size());
                            if (!result) { valid = false; break; }
                            float maximum = peak[logicalIndex].load();
                            for (unsigned i = 0; i < frames; ++i)
                            {
                                const auto* p = packed.data() + i * 3;
                                const auto word = std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16;
                                const auto value = (word & 0x800000u) ? std::int32_t(word) - 16777216 : std::int32_t(word);
                                pcm[std::size_t(i) * pcmChannels + channelOffsets[c] + ch] = value;
                                const float normal = float(value) / 8388608.0f;
                                maximum = std::max(maximum, std::abs(normal));
                                stereo[i * 2 + ch] += normal / float(logical.size());
                                if (slot.channels() == 1) stereo[i * 2 + 1] += normal / float(logical.size());
                            }
                            squares[logicalIndex] += result.sumSquares / slot.channels(); peak[logicalIndex] = maximum;
                        }
                    }
                    if (!valid) { raw.release(); signal(Error::invalidNative); continue; }
                    if (wav && !wav->tryPush(pcm.data(), frames, converted, block->stamp.callbackQpc))
                    { raw.release(); signal(wav->error() == WavTrackWriter::Error::queueOverflow ? Error::pcmOverflow : Error::writeFailed); continue; }
                    if (dubReference) dubReference->render(stereo.data(), frames, referenceStart + std::int64_t(converted), device.sampleRate);
                    for (unsigned i = 0; i < frames; ++i)
                        referenceSquares += (double(stereo[i * 2]) * stereo[i * 2] + double(stereo[i * 2 + 1]) * stereo[i * 2 + 1]) * .5;
                    if (referenceQueue && !referenceError.load() && !referenceQueue->tryPush(stereo.data(), frames))
                        referenceError = true; // derived stream failure; WAV continues
                    converted += frames; raw.release();
                }
            }
            catch (const std::exception& e) { workerError = juce::String::fromUTF8(e.what()); signal(Error::writeFailed); }
            referenceInputDone.store(true, std::memory_order_release);
            if (referenceWorker.joinable()) referenceWorker.join();
            if (wav)
            {
                const auto result = wav->stop(std::int64_t(converted), editId); // external lifecycle: relative origin 0
                if (result.failed()) { workerError = result.getErrorMessage(); signal(Error::writeFailed); }
                wavReport = wav->telemetry();
            }
            finished.store(true, std::memory_order_release);
        }
    };

    DeviceInfo info;
    std::array<int, 8> inputs{-1,-1,-1,-1,-1,-1,-1,-1};
    std::array<bool, 8> stereoSlots{}, armedFlags{};
    OutputMapping outputs;
    std::array<std::atomic<int>, 16> nativeTypes{};
    std::unique_ptr<juce::AudioIODeviceType> type;
    std::unique_ptr<juce::AudioIODevice> device;
    bool ownsTap = false;
    std::atomic<bool> closing{false}, syncStop{false};
    std::shared_ptr<IClockMapper> mapper;
    AsioTimingBridge bridge{qpcFrequency()};
    ClockMapper robustClock{qpcFrequency()};
    BoundedSpscQueue<BlockStamp, 1024> originalClockStamps;
    std::thread syncWorker;
    std::atomic<std::int64_t> cursor{0};
    std::atomic<std::int64_t> mappingEpochQpc{(std::numeric_limits<std::int64_t>::max)()};
    std::atomic<unsigned> callbackRate{0}, callbackBlock{0};
    std::atomic<unsigned> stableBlocks{0}, callbacksInFlight{0}, outputInFlight{0};
    std::atomic<std::uint64_t> softwareClockBlocks{0}, nativeClockBlocks{0}, xruns{0};
    std::array<std::atomic<double>, 2> outputSquares{};
    std::atomic<std::uint64_t> outputSamples{0};
    std::atomic<bool> haveBlock{false};
    std::atomic<bool> reopenRequired{false};
    std::atomic<std::uint64_t> deviceEpoch{1};
    BlockStamp previous{}; // native callback only, reset after close/join
    std::vector<float> left, right;
    std::atomic<std::uint32_t> listen{0x00ff0000}; // bit24 enabled; selected<<16, solo<<8, mute
    float monitorGain = 0;
    std::array<float, 8> monitorWeights{};
    std::atomic<PlaybackBlockQueue*> playback{nullptr};
    std::atomic<IAudioOutputClient*> dubbingOutput{nullptr};
    std::atomic<IAudioOutputClient*> playbackClient{nullptr};
    std::array<std::atomic<float>, 8> inputMeter{};
    std::unique_ptr<Session> session;
    std::atomic<Session*> active{nullptr};
    std::shared_ptr<std::atomic<bool>> shutdownBusy = std::make_shared<std::atomic<bool>>(false);

    explicit Impl(std::shared_ptr<IClockMapper> m) : mapper(m ? std::move(m) : std::make_shared<LinearClockMapper>())
    {
        for (auto& t : nativeTypes) t = 17;
        syncWorker = std::thread([this]
        {
            while (!syncStop.load()) { drainClock(); mapper->observe(bridge.statistics()); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            drainClock(); mapper->observe(bridge.statistics());
        });
    }
    ~Impl()
    {
        shutdownBusy->store(true);
        closePhysical(); detach(); session.reset(); syncStop = true; if (syncWorker.joinable()) syncWorker.join();
        shutdownBusy->store(false);
    }
    void drainClock() noexcept
    {
        bridge.drain(); BlockStamp stamp{};
        while (originalClockStamps.pop(stamp))
            try { robustClock.observe(stamp); } catch (...) { /* bounded extrapolation expires; coordinator aborts */ }
    }
    bool busy() const { return dubbingOutput.load() || (session && !session->journalClosed.load()); }
    void detach()
    {
        active.store(nullptr);
        while (callbacksInFlight.load()) pauseWorker();
    }
    std::vector<unsigned> armed() const
    { std::vector<unsigned> v; for (unsigned i = 0; i < 8; ++i) if (armedFlags[i] && inputs[i] >= 0) v.push_back(i + 1); return v; }
    std::vector<JournalDeviceMapping> mappings() const
    {
        std::vector<JournalDeviceMapping> v;
        for (auto mic : armed())
        {
            const int physical = inputs[mic - 1];
            const int index = int(std::find(info.activeToPhysical.begin(), info.activeToPhysical.end(), physical) - info.activeToPhysical.begin());
            v.push_back({info.name, "Microphone " + juce::String(int(mic)), int(mic), index, physical,
                         stereoSlots[mic - 1] ? activeIndex(physical + 1) : -1, stereoSlots[mic - 1] ? physical + 1 : -1});
        }
        return v;
    }
    int activeIndex(int physical) const noexcept
    {
        const auto found = std::lower_bound(info.activeToPhysical.begin(), info.activeToPhysical.end(), physical);
        return found != info.activeToPhysical.end() && *found == physical ? int(found - info.activeToPhysical.begin()) : -1;
    }
    std::vector<int> selectedInputs() const
    {
        std::vector<int> result;
        for (unsigned i = 0; i < 8; ++i) if (inputs[i] >= 0)
        { result.push_back(inputs[i]); if (stereoSlots[i]) result.push_back(inputs[i] + 1); }
        return result;
    }
    void prepareDeviceState()
    {
        reopenRequired = false; ++deviceEpoch;
        info.activeToPhysical = selectedInputs();
        std::sort(info.activeToPhysical.begin(), info.activeToPhysical.end());
        left.assign(info.bufferFrames, 0); right.assign(info.bufferFrames, 0);
        callbackRate = info.sampleRate; callbackBlock = info.bufferFrames;
        cursor = 0; stableBlocks = 0; haveBlock = false; previous = {}; monitorGain = 0; monitorWeights.fill(0);
        mappingEpochQpc = (std::numeric_limits<std::int64_t>::max)();
        nativeClockBlocks = 0; softwareClockBlocks = 0; xruns = 0; outputSamples = 0; for (auto& sum : outputSquares) sum = 0;
    }
    void closePhysical()
    {
        closing = true;
        if (device) { device->close(); device.reset(); }
        if (ownsTap) { recorderAsioTap.store(nullptr, std::memory_order_release); deviceOwner.store(nullptr); ownsTap = false; }
        while (callbacksInFlight.load() || outputInFlight.load()) pauseWorker();
        closing = false;
    }
    static void tap(const BlockStamp& s, const NativeInputView* v, std::uint32_t count) noexcept
    { if (auto* owner = static_cast<Impl*>(deviceOwner.load(std::memory_order_acquire))) owner->native(s, v, count); }
    void native(BlockStamp stamp, const NativeInputView* views, unsigned count) noexcept
    {
        callbacksInFlight.fetch_add(1);
        if (closing.load()) { callbacksInFlight.fetch_sub(1); return; }
        // Preserve the driver's validity bits for the round-04 quality/epoch
        // model. The legacy adapter below may synthesize a software position.
        originalClockStamps.push(stamp);
        auto* take = active.load();
        const bool hadPrevious = haveBlock.load(std::memory_order_relaxed);
        const bool nativePosition = (stamp.flags & samplePositionValid) != 0;
        if (nativePosition) ++nativeClockBlocks;
        else { stamp.samplePosition = cursor.load(); stamp.flags |= samplePositionValid; ++softwareClockBlocks; }
        Error boundary = Error::none;
        if (stamp.sampleRate != callbackRate.load()) boundary = Error::sampleRateChanged;
        else if (!stamp.numSamples || stamp.numSamples > callbackBlock.load() || stamp.samplePosition < 0
                 || stamp.samplePosition > (std::numeric_limits<std::int64_t>::max)() - stamp.numSamples) boundary = Error::clockDiscontinuity;
        else if (hadPrevious && (stamp.resets != previous.resets || stamp.resyncs != previous.resyncs)) boundary = Error::asioReset;
        else if (hadPrevious && (stamp.samplePosition != previous.samplePosition + previous.numSamples
                 || stamp.numSamples != previous.numSamples || stamp.callbackQpc <= previous.callbackQpc)) boundary = Error::clockDiscontinuity;
        if (hadPrevious && stamp.xruns >= previous.xruns) xruns.fetch_add(stamp.xruns - previous.xruns);
        if (!hadPrevious || boundary != Error::none) mappingEpochQpc = stamp.callbackQpc;
        if (boundary != Error::none) { stableBlocks = 0; if (!reopenRequired.exchange(true)) ++deviceEpoch; if (take) take->signal(boundary); }
        else stableBlocks.fetch_add(1);
        for (unsigned i = 0; i < std::min(count, unsigned(nativeTypes.size())); ++i) if (views) nativeTypes[i] = views[i].format.asioSampleType;
        bridge.enqueueStamp(stamp);
        auto inputStamp = stamp;
        if (take && take->dubbing)
        {
            const auto corrected = clock_math::subtract(stamp.samplePosition, take->inputCorrection);
            if (!corrected) { take->signal(Error::clockDiscontinuity); boundary = Error::clockDiscontinuity; }
            else inputStamp.samplePosition = *corrected;
        }
        if (take && boundary == Error::none && take->fatal.load() == Error::none && take->nstop.load() < 0)
        {
            const auto start = take->scheduledStart.load(std::memory_order_acquire), stop = take->requestStop.load(std::memory_order_acquire);
            const auto blockEnd = inputStamp.samplePosition + stamp.numSamples;
            if (start >= 0 && blockEnd > start)
            {
                if (take->n0.load() < 0 && inputStamp.samplePosition > start) take->signal(Error::missedStart);
                else if (stop >= 0 && stop < inputStamp.samplePosition) take->signal(Error::missedStop);
                else
                {
                    const auto first = std::max(start, inputStamp.samplePosition), last = stop < 0 ? blockEnd : std::min(stop, blockEnd);
                    // Raw copy FIRST. Transport adoption SECOND. No float input,
                    // monitor, gain or output can enter the original queue.
                    const bool copied = last <= first || take->raw.onAsioBlock(inputStamp, views, count);
                    if (!copied) take->signal(take->raw.overflows() ? Error::rawOverflow : Error::invalidNative);
                    else
                    {
                        if (take->n0.load() < 0) { take->end = start; take->n0.store(start, std::memory_order_release); }
                        take->end.store(std::max(first, last), std::memory_order_release);
                        if (stop >= 0 && stop <= blockEnd) take->nstop.store(std::max(first, last), std::memory_order_release);
                    }
                }
            }
        }
        if (take) take->adoptedSequence.store(stamp.sequence + 1, std::memory_order_release);
        if (stamp.samplePosition >= 0 && stamp.numSamples <= callbackBlock.load()
            && stamp.samplePosition <= (std::numeric_limits<std::int64_t>::max)() - stamp.numSamples)
            cursor.store(stamp.samplePosition + stamp.numSamples, std::memory_order_release);
        previous = stamp; haveBlock.store(true);
        callbacksInFlight.fetch_sub(1);
    }
    void output(const float* const* in, int inputCount, float* const* out, int outputCount, unsigned frames) noexcept
    {
        outputInFlight.fetch_add(1);
        for (int c = 0; c < outputCount; ++c) if (out[c]) std::fill(out[c], out[c] + frames, 0.0f);
        // Input metering is independent of the selected output and its buffer size.
        for (unsigned mic = 0; mic < 8; ++mic)
        {
            float peakValue = 0;
            for (unsigned ch = 0; ch < (stereoSlots[mic] ? 2u : 1u); ++ch)
            {
                const auto index = activeIndex(inputs[mic] + int(ch));
                if (inputs[mic] >= 0 && index >= 0 && index < inputCount && in && in[index])
                    for (unsigned i = 0; i < frames; ++i) peakValue = std::max(peakValue, std::abs(in[index][i]));
            }
            inputMeter[mic].store(peakValue, std::memory_order_relaxed);
        }
        if (frames <= left.size())
        {
            // Dubbing owns this block, including silence on a timing mismatch.
            if (auto* dub = dubbingOutput.load())
            {
                if (previous.numSamples == frames) dub->processOutput(previous, left.data(), right.data());
                else { std::fill_n(left.data(), frames, 0.0f); std::fill_n(right.data(), frames, 0.0f); signalReset(); }
            }
            else if (auto* client = playbackClient.load(); client && previous.numSamples == frames)
                client->processOutput(previous, left.data(), right.data());
            else if (auto* q = playback.load()) q->consume(left.data(), right.data(), frames);
            else { std::fill(left.begin(), left.begin() + frames, 0.0f); std::fill(right.begin(), right.begin() + frames, 0.0f); }
            const auto setting = listen.load(std::memory_order_relaxed);
            const auto mute = setting & 255, solo = (setting >> 8) & 255, selected = (setting >> 16) & 255;
            const bool enabled = (setting & 0x01000000) != 0;
            std::array<float, 8> target{}; unsigned count = 0;
            for (unsigned mic = 0; mic < 8; ++mic)
                if (inputs[mic] >= 0 && (selected & (1u << mic)) && !(mute & (1u << mic)) && (!solo || (solo & (1u << mic)))) { target[mic] = 1; ++count; }
            if (count) for (auto& weight : target) weight /= float(count);
            const float step = 1.0f / std::max(1.0f, float(info.sampleRate) * .003f);
            double dispatchedL = 0, dispatchedR = 0;
            for (unsigned i = 0; i < frames; ++i)
            {
                monitorGain += std::clamp((enabled ? 1.0f : 0.0f) - monitorGain, -step, step);
                float monitorL = 0, monitorR = 0;
                for (unsigned mic = 0; mic < 8; ++mic)
                {
                    monitorWeights[mic] += std::clamp(target[mic] - monitorWeights[mic], -step, step);
                    const auto lIndex = activeIndex(inputs[mic]);
                    const auto rIndex = stereoSlots[mic] ? activeIndex(inputs[mic] + 1) : lIndex;
                    if (inputs[mic] >= 0 && in)
                    {
                        if (lIndex >= 0 && lIndex < inputCount && in[lIndex]) monitorL += in[lIndex][i] * monitorWeights[mic];
                        if (rIndex >= 0 && rIndex < inputCount && in[rIndex]) monitorR += in[rIndex][i] * monitorWeights[mic];
                    }
                }
                const float l = left[i] + monitorL * monitorGain, r = right[i] + monitorR * monitorGain;
                if (outputs.mono)
                {
                    if (outputs.monoChannel >= 0 && outputs.monoChannel < outputCount && out[outputs.monoChannel])
                    { const auto mono = (l + r) * .5f; out[outputs.monoChannel][i] = mono; dispatchedL += double(mono) * mono; dispatchedR += double(mono) * mono; }
                }
                else
                {
                    if (outputs.left >= 0 && outputs.left < outputCount && out[outputs.left]) { out[outputs.left][i] = l; dispatchedL += double(l) * l; }
                    if (outputs.right >= 0 && outputs.right < outputCount && out[outputs.right]) { out[outputs.right][i] = r; dispatchedR += double(r) * r; }
                }
            }
            outputSquares[0].store(outputSquares[0].load() + dispatchedL); outputSquares[1].store(outputSquares[1].load() + dispatchedR);
            outputSamples.fetch_add(frames);
        }
        outputInFlight.fetch_sub(1);
    }
    void audioDeviceIOCallbackWithContext(const float* const* in, int ins, float* const* out, int outs, int frames, const juce::AudioIODeviceCallbackContext&) override
    { if (frames > 0) output(in, ins, out, outs, unsigned(frames)); }
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override { if (!closing.load()) signalReset(); }
    void audioDeviceError(const juce::String&) override { signalReset(); }
    void signalReset() noexcept
    {
        if (!reopenRequired.exchange(true)) ++deviceEpoch;
        stableBlocks = 0;
        callbacksInFlight.fetch_add(1); if (auto* s = active.load()) s->signal(Error::asioReset); callbacksInFlight.fetch_sub(1);
    }
};
RecorderAudioEngine::RecorderAudioEngine(std::shared_ptr<IClockMapper> mapper) : impl(std::make_unique<Impl>(std::move(mapper))) {}
RecorderAudioEngine::~RecorderAudioEngine() = default;
juce::StringArray RecorderAudioEngine::deviceNames()
{
    std::unique_ptr<juce::AudioIODeviceType> type(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
    if (!type) return {}; type->scanForDevices(); return type->getDeviceNames();
}
juce::Result RecorderAudioEngine::openDevice(const juce::String& name, unsigned Fs, int requestedBuffer)
{
    auto& s = *impl;
    if (s.busy()) return failure("테이크가 끝난 뒤 오디오 장치를 변경하세요.");
    if (Fs < 8000 || Fs > 768000 || requestedBuffer < 0 || requestedBuffer > 16384 || name.isEmpty()) return failure("Invalid ASIO device request");
    if (!AsioTimingBridge::hookCompiled()) return failure("Recorder ASIO native hook unavailable");
    // Vendor ASIO drivers are apartment-threaded COM objects: create/open/close them on the message thread only.
    jassert(juce::MessageManager::getInstanceWithoutCreating() == nullptr || juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto old = s.info;
    s.closePhysical(); s.detach(); s.session.reset();
    const auto attempt = [&](const juce::String& selected, unsigned rate, int block) -> juce::Result
    {
        try
        {
            if (deviceOwner.load() || recorderAsioTap.load()) return failure("Another Recorder ASIO owner is active");
            s.type.reset(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
            if (!s.type) return failure("ASIO support unavailable");
            s.type->scanForDevices();
            if (!s.type->getDeviceNames().contains(selected)) return failure("Selected ASIO device is missing; no automatic substitution");
            s.device.reset(s.type->createDevice(selected, selected));
            if (!s.device) return failure("ASIO device creation failed");
            // JUCE keeps the driver-level failure (No such device / Driver failed to initialise / ...)
            // in the device object; surface it instead of a misleading channel/buffer complaint.
            if (const auto driverError = s.device->getLastError(); driverError.isNotEmpty())
                throw std::runtime_error(("ASIO driver could not be opened: " + driverError).toStdString());
            s.info = {}; s.info.name = selected; s.info.sampleRate = rate;
            s.info.bufferFrames = unsigned(block ? block : s.device->getDefaultBufferSize());
            s.info.physicalInputs = s.device->getInputChannelNames().size(); s.info.physicalOutputs = s.device->getOutputChannelNames().size();
            s.info.inputNames = s.device->getInputChannelNames(); s.info.outputNames = s.device->getOutputChannelNames();
            for (auto size : s.device->getAvailableBufferSizes()) s.info.availableBuffers.push_back(size);
            if (!s.info.bufferFrames || s.info.bufferFrames > 16384 || s.info.physicalOutputs <= 0 || s.info.physicalOutputs > 256)
                throw std::runtime_error("ASIO requires an output callback and supported buffer size");
            for (auto p : s.selectedInputs()) if (p >= s.info.physicalInputs) throw std::runtime_error("Selected physical input is unavailable");
            for (auto p : {s.outputs.left, s.outputs.right, s.outputs.monoChannel}) if (p >= s.info.physicalOutputs) throw std::runtime_error("Selected physical output is unavailable");
            s.prepareDeviceState();
            juce::BigInteger outputMask; outputMask.setRange(0, s.info.physicalOutputs, true);
            void* absent = nullptr;
            if (!deviceOwner.compare_exchange_strong(absent, &s)) throw std::runtime_error("ASIO device is already owned");
            AsioTapFunction empty = nullptr;
            if (!recorderAsioTap.compare_exchange_strong(empty, &Impl::tap)) { deviceOwner = nullptr; throw std::runtime_error("ASIO tap is already owned"); }
            s.ownsTap = true;
            const auto error = s.device->open(mask(s.info.activeToPhysical), outputMask, rate, int(s.info.bufferFrames));
            if (error.isNotEmpty()) throw std::runtime_error(error.toStdString());
            if (s.device->getActiveInputChannels() != mask(s.info.activeToPhysical) || s.device->getActiveOutputChannels() != outputMask)
                throw std::runtime_error("ASIO driver changed the fixed active/physical channel map");
            // open itself primes native callbacks; close/start boundaries protect
            // the output scratch storage. No take exists during this negotiation.
            const auto actualRate = s.device->getCurrentSampleRate();
            const auto actualBlock = s.device->getCurrentBufferSizeSamples();
            if (!std::isfinite(actualRate) || actualRate < 8000 || actualRate > 768000 || actualRate != std::round(actualRate)
                || actualBlock <= 0 || actualBlock > 16384) throw std::runtime_error("Invalid actual ASIO rate/buffer");
            // open() already primes native taps, but there is no output callback
            // or take yet. Publish actual limits atomically, resize output scratch
            // before start(callback), and report the driver's chosen settings.
            s.info.sampleRate = unsigned(actualRate); s.info.bufferFrames = unsigned(actualBlock);
            s.callbackRate = unsigned(actualRate); s.callbackBlock = unsigned(actualBlock);
            // Priming may use a different driver-selected rate/buffer. This is
            // an explicit device reopen with no take, not an in-take reset.
            s.reopenRequired = false; ++s.deviceEpoch;
            s.left.assign(unsigned(actualBlock), 0); s.right.assign(unsigned(actualBlock), 0);
            s.info.inputLatency = s.device->getInputLatencyInSamples(); s.info.outputLatency = s.device->getOutputLatencyInSamples();
            s.device->start(&s); return juce::Result::ok();
        }
        catch (const std::exception& e) { s.closePhysical(); return juce::Result::fail(juce::String::fromUTF8(e.what())); }
    };
    const auto result = attempt(name, Fs, requestedBuffer);
    if (result.wasOk()) return result;
    s.closePhysical();
    if (old.sampleRate)
    {
        const auto restored = old.synthetic ? openSynthetic(old.sampleRate, old.bufferFrames, old.physicalInputs, old.physicalOutputs)
                                            : attempt(old.name, old.sampleRate, int(old.bufferFrames));
        if (restored.failed()) { s.info = {}; return juce::Result::fail(result.getErrorMessage() + "; previous device restore failed: " + restored.getErrorMessage()); }
    }
    else s.info = {};
    return result;
}
juce::Result RecorderAudioEngine::openSynthetic(unsigned Fs, unsigned block, int ins, int outs)
{
    auto& s = *impl;
    if (s.busy()) return failure("테이크가 끝난 뒤 샘플레이트를 변경하세요.");
    if (Fs < 8000 || Fs > 768000 || !block || block > 16384 || ins < 0 || ins > 256 || outs < 1 || outs > 256)
        return failure("Invalid synthetic device configuration");
    for (auto p : s.selectedInputs()) if (p >= ins) return failure("Synthetic physical input unavailable");
    for (auto p : {s.outputs.left, s.outputs.right, s.outputs.monoChannel}) if (p >= outs) return failure("Synthetic physical output unavailable");
    s.closePhysical(); s.detach(); s.session.reset();
    s.info = {}; s.info.name = "synthetic-native-PCM"; s.info.sampleRate = Fs; s.info.bufferFrames = block;
    s.info.physicalInputs = ins; s.info.physicalOutputs = outs; s.info.synthetic = true; s.prepareDeviceState();
    return juce::Result::ok();
}
juce::Result RecorderAudioEngine::closeDevice()
{
    if (impl->busy()) return failure("Stop and finish the take before closing ASIO");
    impl->closePhysical(); impl->detach(); impl->session.reset(); impl->info = {}; return juce::Result::ok();
}
RecorderAudioEngine::DeviceInfo RecorderAudioEngine::deviceInfo() const { return impl->info; }
juce::Result RecorderAudioEngine::setInputMap(const std::array<int, 8>& map, const std::array<bool, 8>& stereo)
{
    auto& s = *impl;
    if (s.busy()) return failure("Input mapping is fixed for the take");
    UserSettings settings; settings.physicalInputs.assign(map.begin(), map.end()); settings.stereoSlots = stereo;
    const auto valid = settings.validate(s.info.sampleRate ? s.info.physicalInputs : 256); if (valid.failed()) return valid;
    const auto previous = s.inputs; const auto previousStereo = s.stereoSlots; const auto old = s.info;
    // The old device must stop before changing any callback-visible map.
    s.closePhysical(); s.inputs = map; s.stereoSlots = stereo;
    if (!old.sampleRate) return juce::Result::ok();
    auto result = old.synthetic ? openSynthetic(old.sampleRate, old.bufferFrames, old.physicalInputs, old.physicalOutputs)
                               : openDevice(old.name, old.sampleRate, int(old.bufferFrames));
    if (result.failed())
    {
        s.closePhysical(); s.inputs = previous; s.stereoSlots = previousStereo;
        const auto restored = old.synthetic ? openSynthetic(old.sampleRate, old.bufferFrames, old.physicalInputs, old.physicalOutputs)
                                           : openDevice(old.name, old.sampleRate, int(old.bufferFrames));
        if (restored.failed()) return juce::Result::fail(result.getErrorMessage() + "; map restore failed: " + restored.getErrorMessage());
    }
    return result;
}
juce::Result RecorderAudioEngine::setOutputMap(OutputMapping map)
{
    auto& s = *impl;
    if (s.busy()) return failure("Output mapping is fixed for the take");
    for (auto p : {map.left, map.right, map.monoChannel})
        if (p < -1 || p > 255 || (s.info.sampleRate && p >= s.info.physicalOutputs)) return failure("Selected output is unavailable");
    if (!map.mono && map.left >= 0 && map.left == map.right) return failure("Left/right outputs must be different; use explicit mono mode");
    if (map.mono && (map.left != -1 || map.right != -1)) return failure("Mono mapping requires only monoChannel");
    if (!map.mono && map.monoChannel != -1) return failure("Stereo mapping cannot select a mono channel");
    // stop() detaches the JUCE output callback; the raw tap/clock remains alive.
    if (s.device) { s.closing = true; s.device->stop(); }
    while (s.outputInFlight.load()) pauseWorker();
    s.outputs = map;
    if (s.device) { s.device->start(&s); s.closing = false; }
    return juce::Result::ok();
}
juce::Result RecorderAudioEngine::arm(unsigned mic, bool enabled)
{
    if (impl->busy()) return failure("Armed microphone count is fixed for the take");
    if (mic >= 8 || (enabled && impl->inputs[mic] < 0)) return failure("Select a physical input before arming");
    impl->armedFlags[mic] = enabled; return juce::Result::ok();
}
std::vector<unsigned> RecorderAudioEngine::armedMicrophones() const { return impl->armed(); }
std::vector<JournalDeviceMapping> RecorderAudioEngine::microphoneMapping() const { return impl->mappings(); }
std::vector<int> RecorderAudioEngine::calibrationInputMapping() const
{
    std::vector<int> result;
    for (const auto& m : impl->mappings()) result.insert(result.end(), {m.mic, m.physicalIndex, m.rightPhysicalIndex});
    return result;
}
void RecorderAudioEngine::setInputMonitoring(bool enabled, std::uint8_t selected) noexcept
{
    auto old = impl->listen.load();
    while (!impl->listen.compare_exchange_weak(old, (old & 65535) | (std::uint32_t(selected) << 16) | (enabled ? 0x01000000u : 0))) {}
}
void RecorderAudioEngine::setListeningState(std::uint8_t mute, std::uint8_t solo) noexcept
{
    auto old = impl->listen.load();
    while (!impl->listen.compare_exchange_weak(old, (old & 0xffff0000) | mute | (std::uint32_t(solo) << 8))) {}
}
void RecorderAudioEngine::setPlaybackQueue(PlaybackBlockQueue* queue)
{
    if (impl->dubbingOutput.load()) return; // dubbing owns the prepared output until detach
    impl->playback.store(nullptr);
    while (impl->outputInFlight.load()) pauseWorker();
    impl->playback.store(queue);
}

juce::Result RecorderAudioEngine::prepareDubbing(TakeConfig config, bool recordMics,
    std::unique_ptr<IPlaybackBlockProvider> reference, std::int64_t pstart, std::int64_t residual)
{
    auto& s = *impl;
    if ((s.session && !s.session->journalClosed.load()) || !s.info.sampleRate || !reference
        || pstart < 0 || s.info.inputLatency < 0 || s.info.outputLatency < 0
        || (s.outputs.mono ? s.outputs.monoChannel < 0 : s.outputs.left < 0 && s.outputs.right < 0)
        || std::abs(double(residual)) > double(s.info.sampleRate) * 10)
        return failure("Dubbing device/reference is not ready");
    s.detach(); s.session.reset();
    s.shutdownBusy->store(true);
    try
    {
        s.session = std::make_unique<Impl::Session>(s, std::move(config), recordMics, std::move(reference), pstart,
                                                   std::int64_t(s.info.inputLatency) + residual);
        s.session->launch(); s.active.store(s.session.get(), std::memory_order_release); return juce::Result::ok();
    }
    catch (const std::exception& e) { s.session.reset(); s.shutdownBusy->store(s.dubbingOutput.load() != nullptr); return juce::Result::fail(juce::String::fromUTF8(e.what())); }
}
juce::Result RecorderAudioEngine::stopDubbingAt(std::int64_t sample)
{
    auto* s = impl->session.get();
    if (!s || !s->dubbing || sample <= s->requestStart.load() || s->requestStart.load() < 0
        || sample < currentSample() - s->inputCorrection || s->nstop.load() >= 0)
        return failure("Cannot schedule a past/empty dubbing stop");
    auto old = s->requestStop.load();
    do { if (old >= 0 && sample > old) return failure("Dubbing stop can only be shortened"); }
    while (!s->requestStop.compare_exchange_weak(old, sample));
    return juce::Result::ok();
}
void RecorderAudioEngine::setDubbingOutputClient(IAudioOutputClient* client)
{
    if (client) impl->shutdownBusy->store(true);
    impl->dubbingOutput.store(nullptr);
    while (impl->outputInFlight.load()) pauseWorker();
    if (client) impl->playback.store(nullptr);
    impl->dubbingOutput.store(client);
    if (!client) impl->shutdownBusy->store(impl->session && !impl->session->journalClosed.load());
}
const ClockMapper& RecorderAudioEngine::masterClock() const noexcept { return impl->robustClock; }

void RecorderAudioEngine::setPlaybackClient(IAudioOutputClient* client)
{
    impl->playbackClient.store(nullptr);
    while (impl->outputInFlight.load()) pauseWorker();
    impl->playbackClient.store(client);
}
juce::Result RecorderAudioEngine::showControlPanel()
{
    if (impl->busy() || !impl->device) return failure("테이크가 끝난 뒤 ASIO 장치를 확인하세요.");
    return impl->device->showControlPanel() ? juce::Result::ok() : failure("ASIO 제어판을 열 수 없습니다.");
}
std::array<float, 8> RecorderAudioEngine::inputPeaks() const noexcept
{ std::array<float, 8> p{}; for (unsigned i = 0; i < 8; ++i) p[i] = impl->inputMeter[i].load(); return p; }
std::shared_ptr<PeakCache> RecorderAudioEngine::peakCache() const
{ return impl->session ? impl->session->peakCache : nullptr; }
juce::Result RecorderAudioEngine::prepare(TakeConfig config)
{
    if (impl->busy() || !impl->info.sampleRate) return failure("ASIO device is not ready or a take is active");
    impl->detach(); impl->session.reset();
    impl->shutdownBusy->store(true);
    try
    {
        impl->session = std::make_unique<Impl::Session>(*impl, std::move(config));
        impl->session->launch(); impl->active.store(impl->session.get(), std::memory_order_release); return juce::Result::ok();
    }
    catch (const std::exception& e) { impl->session.reset(); impl->shutdownBusy->store(false); return juce::Result::fail(juce::String::fromUTF8(e.what())); }
}
juce::Result RecorderAudioEngine::startAt(std::int64_t sample)
{
    if (impl->reopenRequired.load()) return failure("Reopen the ASIO device after a clock discontinuity");
    auto* s = impl->session.get();
    if (!s || s->finishing.load() || sample < currentSample() || sample < 0) return failure("Cannot schedule a past/unprepared audio start");
    std::int64_t unset = -1;
    if (!s->requestStart.compare_exchange_strong(unset, sample)) return failure("Audio start already scheduled");
    return juce::Result::ok();
}
juce::Result RecorderAudioEngine::stopAt(std::int64_t sample)
{
    auto* s = impl->session.get();
    if (!s || sample < currentSample() || sample <= s->requestStart.load() || s->requestStart.load() < 0 || s->nstop.load() >= 0)
        return failure("Cannot schedule a past/empty audio stop");
    std::int64_t unset = -1;
    if (!s->requestStop.compare_exchange_strong(unset, sample)) return failure("Audio stop already scheduled");
    return juce::Result::ok();
}
void RecorderAudioEngine::abort(Error reason) noexcept { if (impl->session) impl->session->signal(reason); }
juce::Result RecorderAudioEngine::finishCapture(const juce::Uuid& edit)
{
    auto* s = impl->session.get();
    if (!s || (s->nstop.load() < 0 && s->fatal.load() == Error::none)) return failure("Audio stop has not been adopted");
    impl->detach();
    if (!s->finishing.load()) { s->editId = edit; s->finishing.store(true, std::memory_order_release); }
    if (s->worker.joinable()) s->worker.join();
    if (s->workerError.isNotEmpty()) return juce::Result::fail(s->workerError);
    return s->fatal.load() == Error::none ? juce::Result::ok() : failure("Audio capture stopped with an error; completed media preserved");
}
juce::Result RecorderAudioEngine::finishJournal(bool complete)
{
    auto* s = impl->session.get();
    if (!s || !s->finished.load()) return failure("Finish media before committing TakeFinalized");
    std::lock_guard<std::mutex> lock(s->journalMutex);
    auto result = juce::Result::ok();
    if (complete)
    {
        if (s->fatal.load() != Error::none || s->referenceError.load() || !s->stoppedJournal) result = failure("Failed/incomplete take cannot commit TakeFinalized");
        else result = s->journal.append(JournalTakeFinalized{s->config.takeId});
    }
    const auto closed = s->journal.close(); s->journalClosed = true;
    impl->shutdownBusy->store(impl->dubbingOutput.load() != nullptr); return result.failed() ? result : closed;
}
const AVCodecContext* RecorderAudioEngine::referenceContext() const { return impl->session && impl->session->reference ? &impl->session->reference->context() : nullptr; }
void RecorderAudioEngine::pollDeviceEvents()
{
    if (!impl->device || !impl->session) return;
    AsioEventCounters e{};
    if (AsioTimingBridge::readEvents(*impl->device, e)
        && (e.resets != impl->session->baselineEvents.resets || e.resyncs != impl->session->baselineEvents.resyncs)) deviceDiscontinuity(Error::asioReset);
}
bool RecorderAudioEngine::clockReady() const
{
    if (impl->reopenRequired.load() || impl->stableBlocks.load() < 3) return false;
    const auto mapping = impl->mapper->snapshot();
    // A device reopen/reset must not use the previous epoch's worker snapshot.
    return mapping.valid && mapping.originQpc >= impl->mappingEpochQpc.load();
}
ClockMapping RecorderAudioEngine::clockMapping() const { return impl->mapper->snapshot(); }
std::int64_t RecorderAudioEngine::currentSample() const noexcept { return impl->cursor.load(); }
std::int64_t RecorderAudioEngine::startSample() const noexcept { return impl->session ? impl->session->n0.load() : -1; }
bool RecorderAudioEngine::startCommitted() const noexcept { return impl->session && impl->session->scheduledStart.load(std::memory_order_acquire) >= 0; }
std::int64_t RecorderAudioEngine::stopSample() const noexcept { return impl->session ? impl->session->nstop.load() : -1; }
std::int64_t RecorderAudioEngine::acceptedEnd() const noexcept { return impl->session ? impl->session->end.load() : -1; }
RecorderAudioEngine::Error RecorderAudioEngine::error() const noexcept { return impl->session ? impl->session->fatal.load() : Error::none; }
bool RecorderAudioEngine::referenceFailed() const noexcept { return impl->session && impl->session->referenceError.load(); }
std::array<float, 8> RecorderAudioEngine::peaks() const noexcept
{ std::array<float, 8> p{}; if (impl->session) for (unsigned i = 0; i < 8; ++i) p[i] = impl->session->peak[i].load(); return p; }
juce::var RecorderAudioEngine::telemetry() const
{
    const auto* s = impl->session.get();
    if (!s || !s->finished.load()) throw std::logic_error("Finish capture before reading audio telemetry");
    auto v = jsonObject(); juce::Array<juce::var> tracks, activeMap;
    for (auto p : s->device.activeToPhysical) activeMap.add(p);
    for (std::size_t i = 0; i < s->logical.size(); ++i)
    {
        auto t = jsonObject(); const auto mic = s->logical[i];
        jsonSet(t, "mic", int(mic)); jsonSet(t, "physicalIndex", s->mapping[i].physicalIndex); jsonSet(t, "activeIndex", s->mapping[i].activeIndex);
        jsonSet(t, "channels", s->mapping[i].channels()); jsonSet(t, "rightPhysicalIndex", s->mapping[i].rightPhysicalIndex);
        jsonSet(t, "peak", s->peak[mic - 1].load()); jsonSet(t, "rms", s->converted ? std::sqrt(s->squares[mic - 1] / double(s->converted)) : 0.0);
        jsonSet(t, "nativeAsioType", s->formats[(mic - 1) * 2].asioSampleType); jsonSet(t, "conversionPolicy", NativePcmConverter::policy(s->formats[(mic - 1) * 2])); tracks.add(t);
    }
    jsonSet(v, "device", s->device.name); jsonSet(v, "sampleRate", int(s->device.sampleRate)); jsonSet(v, "bufferFrames", int(s->device.bufferFrames));
    jsonSet(v, "inputLatencySamples", s->device.inputLatency); jsonSet(v, "outputLatencySamples", s->device.outputLatency);
    jsonSet(v, "activeIndexToPhysicalIndex", activeMap); jsonSet(v, "tracks", tracks);
    jsonSet(v, "N0", jsonInt(s->n0.load())); jsonSet(v, "Nstop", jsonInt(s->nstop.load())); jsonSet(v, "convertedSamples", jsonInt(s->converted));
    jsonSet(v, "rawQueueBlocks", jsonInt(s->raw.capacity())); jsonSet(v, "rawQueueHighWater", jsonInt(s->raw.highWater()));
    jsonSet(v, "rawOverflow", jsonInt(s->raw.overflows())); jsonSet(v, "rawInvalid", jsonInt(s->raw.invalidBlocks()));
    jsonSet(v, "errorCode", int(s->fatal.load())); jsonSet(v, "error", s->workerError); jsonSet(v, "wav", s->wavReport);
    jsonSet(v, "reference", s->referenceReport); jsonSet(v, "referenceFailed", s->referenceError.load()); jsonSet(v, "referenceError", s->referenceMessage);
    jsonSet(v, "referenceInputRms", s->converted ? std::sqrt(s->referenceSquares / double(s->converted)) : 0.0);
    jsonSet(v, "xruns", jsonInt(impl->xruns.load())); jsonSet(v, "nativeClockBlocks", jsonInt(impl->nativeClockBlocks.load()));
    jsonSet(v, "softwareClockBlocks", jsonInt(impl->softwareClockBlocks.load())); jsonSet(v, "timingQueueDrops", jsonInt(impl->bridge.droppedStamps()));
    juce::Array<juce::var> outputRms;
    for (const auto& sum : impl->outputSquares) outputRms.add(impl->outputSamples.load() ? std::sqrt(sum.load() / double(impl->outputSamples.load())) : 0.0);
    jsonSet(v, "dispatchedOutputRmsLR", outputRms);
    jsonSet(v, "outputMeasurement", "Digital floats dispatched to selected ASIO outputs since device open; no DAC/headphone/loopback measurement");
    jsonSet(v, "clockPolicy", "Temporary round-03 OLS; missing native samplePosition uses explicit software block counter; no physical latency/exposure calibration");
    jsonSet(v, "realtimeScope", "Recorder native tap/transport/output: bounded copies/arithmetic and lock-free atomics; JUCE callbackLock/outputReady remain in upstream device callback");
    return v;
}
void RecorderAudioEngine::processBlock(const BlockStamp& stamp, const NativeInputView* in, unsigned count,
                                      const float* const* floats, float* const* out, unsigned outputs) noexcept
{
    if (!impl->info.synthetic) return;
    impl->native(stamp, in, count); impl->output(floats, int(count), out, int(outputs), stamp.numSamples);
}
void RecorderAudioEngine::endAtConfirmedBoundary()
{
    impl->detach(); // seq_cst pointer / in-flight handshake; no new native queue writes
    if (auto* s = impl->session.get())
    {
        const auto first = s->n0.load(), last = s->end.load();
        if (first >= 0 && last > first) s->nstop = last;
        else s->signal(Error::cancelled);
    }
}
void RecorderAudioEngine::deviceDiscontinuity(Error reason) noexcept
{
    if (!impl->reopenRequired.exchange(true)) ++impl->deviceEpoch;
    impl->stableBlocks = 0;
    impl->callbacksInFlight.fetch_add(1);
    if (auto* s = impl->active.load()) s->signal(reason);
    impl->callbacksInFlight.fetch_sub(1);
}
std::uint64_t RecorderAudioEngine::deviceGeneration() const noexcept { return impl->deviceEpoch.load(); }
bool RecorderAudioEngine::requiresDeviceReopen() const noexcept { return impl->reopenRequired.load(); }
bool RecorderAudioEngine::processingDelayed() const noexcept { return impl->session && impl->session->ioHealth.delayed(); }
std::shared_ptr<const std::atomic<bool>> RecorderAudioEngine::shutdownBlocker() const noexcept { return impl->shutdownBusy; }
}
