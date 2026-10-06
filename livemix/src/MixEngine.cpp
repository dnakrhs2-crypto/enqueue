#include "MixEngine.h"
#include "AudioBackends.h"
#include "MonitorOutput.h"

#include <algorithm>
#include <utility>

namespace gocue::livemix
{

namespace
{
    std::pair<int, bool> preferredFormat (const juce::String& choice)
    {
        if (choice == "int16") return { 16, false };
        if (choice == "int24") return { 24, false };
        if (choice == "int32") return { 32, false };
        if (choice == "float32") return { 32, true };
        return { 0, false };
    }

    void applySampleFormat (const juce::String& choice)
    {
       #if JUCE_WINDOWS && JUCE_WASAPI
        const auto [bits, isFloat] = preferredFormat (choice);
        juce::setWasapiExclusivePreferredFormat (bits, isFloat);
       #else
        juce::ignoreUnused (choice);
       #endif
    }

    void selectDeviceType (juce::AudioDeviceManager& manager, const juce::String& type)
    {
        if (manager.getCurrentAudioDeviceType() == type) return;
        // JUCE's setCurrentAudioDeviceType opens remembered/default endpoints, possibly a duplex pair with
        // independent clocks. An empty internal setup selects ONLY the type; the explicit setup follows.
        // This is never persisted: LiveMix settings and its public startup API use MixDevice JSON.
        juce::XmlElement typeOnly ("DEVICESETUP");
        typeOnly.setAttribute ("deviceType", type);
        manager.initialise (0, 0, &typeOnly, false);
    }

    float clampedPan (double pan) noexcept
    {
        return (float) juce::jlimit (-1.0, 1.0, std::isfinite (pan) ? pan : 0.0);
    }

    std::array<float, 2> channelPanGains (float pan, bool stereo) noexcept
    {
        if (pan == 0.0f)
            return { 1.0f, 1.0f };   // exactly the old centre sound, including stereo produced by a mono mic's chain

        if (stereo)
        {
            const float attenuated = std::abs (pan) >= 1.0f ? 0.0f : (float) std::cos ((double) pan * juce::MathConstants<double>::halfPi);
            return pan < 0.0f ? std::array<float, 2> { 1.0f, attenuated } : std::array<float, 2> { attenuated, 1.0f };
        }

        // Mono already enters the chain at unity on BOTH sides. Constant power with sqrt(2) compensation keeps
        // that centre level: L = sqrt(2) cos(theta), R = sqrt(2) sin(theta), theta = (pan + 1) pi/4.
        const double theta = ((double) pan + 1.0) * juce::MathConstants<double>::pi / 4.0;
        const double gain = std::sqrt (2.0);
        return { pan >= 1.0f ? 0.0f : (float) (gain * std::cos (theta)),
                 pan <= -1.0f ? 0.0f : (float) (gain * std::sin (theta)) };
    }
}

// Sequential consistency orders reader entry before pointer acquisition and unpublishing before the
// reader-count check. A reader of the old pointer must finish before the message thread can reclaim it.
// No retry, allocation, lock or wait on the audio thread (including the busy-graph silence path).
struct MixEngine::MonitorAccess
{
    explicit MonitorAccess (MixEngine& e) noexcept : engine (e)
    {
        engine.monitorReaders.fetch_add (1);
        output = engine.publishedMonitor.load();
    }
    ~MonitorAccess() { engine.monitorReaders.fetch_sub (1); }
    MixEngine& engine;
    MonitorOutput* output = nullptr;
};
static_assert (std::atomic<MonitorOutput*>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free);

MixEngine::MixEngine (const juce::String& obsMappingName) : obsSender (obsMappingName)
{
    master.chain->setTimingHook (&transport);
    prepare (48000.0, 256);
}

MixEngine::~MixEngine()
{
    shutdown();
}

//==============================================================================
juce::String MixEngine::initialise (const MixDevice* saved)
{
    juce::String error;
    if (saved != nullptr && saved->input.isNotEmpty())
    {
        error = openDevice (*saved);
        if (error.isEmpty()) return {};
    }
    for (const auto& name : AudioBackends::availableTypes (deviceManager))
        if (name.containsIgnoreCase ("ASIO"))
        {
            auto* type = findType (name);
            type->scanForDevices();
            const auto names = type->getDeviceNames (false);
            if (! names.isEmpty())
            {
                error = openDevice ({ name, names[0], names[0], 0, 0.0 });
                if (error.isEmpty()) return {};
            }
        }
    if (auto* type = findType ("Windows Audio"))
    {
        type->scanForDevices();
        const auto inputs = type->getDeviceNames (true), outputs = type->getDeviceNames (false);
        if (! inputs.isEmpty())
            return openDevice ({ type->getTypeName(), inputs[juce::jmax (0, type->getDefaultDeviceIndex (true))],
                                 outputs[juce::jmax (0, type->getDefaultDeviceIndex (false))], 0, 0.0 });
    }
    return error.isNotEmpty() ? error : juce::String::fromUTF8 ("오디오 장치를 열지 못했습니다.");
}

juce::AudioIODeviceType* MixEngine::findType (const juce::String& name)
{
    for (auto* type : deviceManager.getAvailableDeviceTypes())
        if (type->getTypeName() == name) return type;
    return nullptr;
}

void MixEngine::ensureCallback()
{
    startTimer (50);
    if (! callbackAdded)
    {
        monitorRestartRequested.store (false, std::memory_order_release);
        publishedMonitor.store (monitor.get());
        deviceManager.addAudioCallback (this);
        callbackAdded = true;
    }
}

void MixEngine::removeCallback()
{
    if (callbackAdded)
    {
        deviceManager.removeAudioCallback (this);
        callbackAdded = false;
    }
    publishedMonitor.store (nullptr);
    monitorRestartRequested.store (false, std::memory_order_release);
}

juce::String MixEngine::openAllChannels()
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return {};

    const int ins = device->getInputChannelNames().size();
    const int outs = device->getOutputChannelNames().size();
    auto setup = deviceManager.getAudioDeviceSetup();
    juce::BigInteger allIn, allOut;
    if (setup.inputDeviceName.isNotEmpty())
        allIn.setRange (0, juce::jmin (ins, maxDeviceChannels), true);
    if (setup.outputDeviceName.isNotEmpty())
        allOut.setRange (0, juce::jmin (outs, device->getTypeName().containsIgnoreCase ("ASIO") ? maxDeviceChannels : 2), true);

    if (setup.inputChannels == allIn && setup.outputChannels == allOut && ! setup.useDefaultInputChannels && ! setup.useDefaultOutputChannels)
        return {};

    const auto previous = setup;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels = allIn;
    setup.outputChannels = allOut;
    const auto error = deviceManager.setAudioDeviceSetup (setup, true);

    if (error.isEmpty())
        return {};

    // JUCE closed the device on the refusal: back to the channels that worked
    const auto rollback = deviceManager.setAudioDeviceSetup (previous, true);
    return juce::String::fromUTF8 ("모든 채널을 열지 못했습니다: ") + error
           + (rollback.isNotEmpty() ? juce::String::fromUTF8 (" (이전 채널 구성으로 되돌리기도 실패: ") + rollback + ")"
                                    : juce::String::fromUTF8 (" (이전 채널 구성을 유지합니다)"));
}

juce::String MixEngine::openDevice (const MixDevice& requested)
{
    ++openCount;
    MixDevice wanted = requested;
    if (wanted.isAsio())
    {
        if (wanted.input.isEmpty()) wanted.input = getOpenDevice().input;
        wanted.output = wanted.input;
    }
    auto* type = findType (wanted.type);
    if (type == nullptr || (! wanted.isAsio() && ! AudioBackends::isWindows (wanted.type)))
        return juce::String::fromUTF8 ("오디오 장치 타입을 쓸 수 없습니다: ") + wanted.type;
    if (wanted.input.isEmpty())
        return juce::String::fromUTF8 ("입력 장치를 선택하세요.");
    type->scanForDevices();
    if (! type->getDeviceNames (true).contains (wanted.input)
        || (wanted.output.isNotEmpty() && ! type->getDeviceNames (false).contains (wanted.output)))
        return juce::String::fromUTF8 ("선택한 오디오 장치가 이 PC에 없습니다.");

    const bool split = ! wanted.isAsio() && wanted.output.isNotEmpty()
                       && ! AudioBackends::sameContainer (wanted.input, wanted.output);
    auto* previousDevice = deviceManager.getCurrentAudioDevice();
    const bool hadDevice = previousDevice != nullptr && previousDevice->isOpen();
    const auto previousType = deviceManager.getCurrentAudioDeviceType();
    const auto previous = deviceManager.getAudioDeviceSetup();
    const auto previousInfo = getOpenDevice();
    const bool previousSplit = isSplitMonitor();
    const bool previousStereoOnly = stereoOutputsOnly.load (std::memory_order_relaxed);

    removeCallback(); // joins the producer before replacing its monitor or staging storage
    auto previousMonitor = std::move (monitor);
    const double previousMonitorRate = previousMonitor != nullptr ? previousMonitor->getOutputSampleRate() : 0.0;
    const int previousMonitorPeriod = previousMonitor != nullptr ? previousMonitor->getOutputPeriod() : 0;
    if (previousMonitor != nullptr) previousMonitor->stop();
    deviceManager.closeAudioDevice();
    splitMonitor.store (false, std::memory_order_release);

    applySampleFormat (wanted.sampleFormat);
    selectDeviceType (deviceManager, wanted.type);
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = wanted.input;
    setup.outputDeviceName = split ? juce::String() : wanted.output;
    setup.sampleRate = wanted.sampleRate;
    setup.bufferSize = wanted.bufferSize;
    setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
    setup.inputChannels.setRange (0, maxDeviceChannels, true);
    if (setup.outputDeviceName.isNotEmpty())
        setup.outputChannels.setRange (0, wanted.isAsio() ? maxDeviceChannels : 2, true);
    auto error = deviceManager.setAudioDeviceSetup (setup, true);
    if (error.isEmpty())
    {
        auto* opened = deviceManager.getCurrentAudioDevice();
        if (opened == nullptr || ! opened->isOpen())
            error = juce::String::fromUTF8 ("오디오 장치를 시작하지 못했습니다.");
        else
            error = openAllChannels();
    }
    if (error.isEmpty() && split)
    {
        auto* input = deviceManager.getCurrentAudioDevice();
        monitor = std::make_unique<MonitorOutput>();
        error = monitor->start (*type, wanted.output, input->getCurrentSampleRate(), input->getCurrentBufferSizeSamples());
    }
    if (error.isNotEmpty())
    {
        if (monitor != nullptr) monitor->stop();
        monitor.reset();
        deviceManager.closeAudioDevice();
        juce::String rollback;
        applySampleFormat (previousInfo.sampleFormat);
        if (deviceManager.getCurrentAudioDeviceType() != previousType && findType (previousType) != nullptr)
            selectDeviceType (deviceManager, previousType);
        if (hadDevice)
        {
            rollback = deviceManager.setAudioDeviceSetup (previous, true);
            if (rollback.isEmpty() && deviceManager.getAudioDeviceSetup() != previous)
                rollback = juce::String::fromUTF8 ("이전 장치의 샘플레이트, 버퍼 또는 채널을 복원하지 못했습니다.");
            if (rollback.isEmpty() && previousMonitor != nullptr)
            {
                if (auto* oldType = findType (previousType))
                    rollback = previousMonitor->start (*oldType, previousInfo.output, previousInfo.sampleRate,
                                                        previousInfo.bufferSize, previousMonitorPeriod, previousMonitorRate);
                else
                    rollback = juce::String::fromUTF8 ("이전 모니터 장치 타입이 없습니다.");
                if (rollback.isEmpty() && (previousMonitor->getOutputSampleRate() != previousMonitorRate
                                           || previousMonitor->getOutputPeriod() != previousMonitorPeriod))
                    rollback = juce::String::fromUTF8 ("이전 모니터의 샘플레이트 또는 버퍼를 복원하지 못했습니다.");
            }
        }
        monitor = std::move (previousMonitor);
        openedDevice = previousInfo;
        splitMonitor.store (previousSplit && hadDevice, std::memory_order_release);
        stereoOutputsOnly.store (previousStereoOnly, std::memory_order_relaxed);
        // A failed rollback must not leave a half-working split graph that the UI reports as running.
        if (rollback.isNotEmpty())
        {
            deviceManager.closeAudioDevice();
            if (monitor != nullptr) monitor->stop();
        }
        ensureCallback();
        return juce::String::fromUTF8 ("오디오 장치를 열지 못했습니다: ") + error
            + (! hadDevice ? juce::String()
               : rollback.isNotEmpty() ? juce::String::fromUTF8 (" (이전 장치로 되돌리기도 실패: ") + rollback + ")"
                                       : juce::String::fromUTF8 (" (이전 장치로 되돌렸습니다)"));
    }

    openedDevice = wanted;
    splitMonitor.store (split, std::memory_order_release);
    stereoOutputsOnly.store (! wanted.isAsio(), std::memory_order_relaxed);
    ensureCallback();
    openedDevice = getOpenDevice();
    return {};
}

MixDevice MixEngine::getOpenDevice() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice(); device != nullptr && device->isOpen())
    {
        const auto setup = deviceManager.getAudioDeviceSetup();
        return { deviceManager.getCurrentAudioDeviceType(), setup.inputDeviceName,
                 isSplitMonitor() ? openedDevice.output : setup.outputDeviceName,
                 device->getCurrentBufferSizeSamples(), device->getCurrentSampleRate(), openedDevice.sampleFormat };
    }
    return {};
}

#if JUCE_WINDOWS && JUCE_WASAPI
MixEngine::DeviceFormat MixEngine::describeDeviceFormat (DeviceFormat::Kind kind, const juce::WasapiFormatInfo& input,
                                                        const juce::WasapiFormatInfo& output, const juce::String& choice, int asioBits)
{
    DeviceFormat result;
    result.kind = kind;
    if (kind == DeviceFormat::Kind::none) return result;
    if (kind == DeviceFormat::Kind::asio)
    {
        result.inputBits = result.outputBits = asioBits;
        return result;
    }
    const auto [wantedBits, wantedFloat] = preferredFormat (choice);
    auto direction = [&] (const juce::WasapiFormatInfo& info, int& bits, bool& isFloat, double& rate, int& accepted, bool& refused)
    {
        if (info.streamBits <= 0) return;
        if (kind == DeviceFormat::Kind::windowsShared)
        {
            bits = info.deviceBits;
            isFloat = bits > 0 && info.deviceIsFloat;
            rate = info.deviceSampleRate;
        }
        else
        {
            bits = info.streamBits;
            isFloat = info.streamIsFloat;
            accepted = info.exclusiveFormats;
            refused = wantedBits > 0 && (bits != wantedBits || isFloat != wantedFloat);
        }
    };
    direction (input, result.inputBits, result.inputFloat, result.inputDeviceRate, result.inputAccepted, result.inputRefused);
    direction (output, result.outputBits, result.outputFloat, result.outputDeviceRate, result.outputAccepted, result.outputRefused);
    return result;
}
#endif

MixEngine::DeviceFormat MixEngine::getDeviceFormat() const
{
    DeviceFormat result;
    auto* inputDevice = deviceManager.getCurrentAudioDevice();
    if (inputDevice == nullptr || ! inputDevice->isOpen() || ! isDeviceRunning()) return result;
    const auto type = inputDevice->getTypeName();
    if (type.containsIgnoreCase ("ASIO"))
    {
        result.kind = DeviceFormat::Kind::asio;
        result.inputBits = result.outputBits = inputDevice->getCurrentBitDepth();
        if (! inputDevice->getActiveInputChannels().isZero()) result.inputStreamRate = inputDevice->getCurrentSampleRate();
        if (! inputDevice->getActiveOutputChannels().isZero()) result.outputStreamRate = inputDevice->getCurrentSampleRate();
        return result;
    }
    if (! AudioBackends::isWindows (type)) return result;
    result.kind = type == "Windows Audio (Exclusive Mode)" ? DeviceFormat::Kind::windowsExclusive : DeviceFormat::Kind::windowsShared;
    auto* outputDevice = isSplitMonitor() ? (monitor != nullptr ? monitor->getDevice() : nullptr) : inputDevice;
    bool inputKnown = false, outputKnown = false;
   #if JUCE_WINDOWS && JUCE_WASAPI
    juce::WasapiFormatInfo input, output;
    inputKnown = outputKnown = juce::getWasapiFormatInfo (*inputDevice, input, output);
    if (isSplitMonitor())
    {
        output = {};
        juce::WasapiFormatInfo unused;
        outputKnown = outputDevice != nullptr && outputDevice->isOpen()
                      && juce::getWasapiFormatInfo (*outputDevice, unused, output);
    }
    result = describeDeviceFormat (result.kind, input, output, openedDevice.sampleFormat);
   #endif
    if (! inputDevice->getActiveInputChannels().isZero()) result.inputStreamRate = inputDevice->getCurrentSampleRate();
    if (outputDevice != nullptr && outputDevice->isOpen() && isMonitorRunning() && ! outputDevice->getActiveOutputChannels().isZero())
        result.outputStreamRate = outputDevice->getCurrentSampleRate();
    if (result.kind == DeviceFormat::Kind::windowsExclusive)
    {
        // Non-WASAPI devices (test fakes) know only their bit count, never supported formats or refusal.
        if (! inputKnown && ! inputDevice->getActiveInputChannels().isZero())
            result.inputBits = inputDevice->getCurrentBitDepth();
        if (! outputKnown && outputDevice != nullptr && outputDevice->isOpen() && ! outputDevice->getActiveOutputChannels().isZero())
            result.outputBits = outputDevice->getCurrentBitDepth();
    }
    return result;
}

juce::String MixEngine::setBufferSize (int samples)
{
    const auto current = getOpenDevice();
    if (current.type == "Windows Audio") return {}; // the shared-mode period is chosen by Windows
    if (current.input.isEmpty())
        return juce::String::fromUTF8 ("열린 오디오 장치가 없습니다.");
    if (samples <= 0 || current.bufferSize == samples) return {};
    auto wanted = current;
    wanted.bufferSize = samples;
    return openDevice (wanted);
}

juce::String MixEngine::restartDevice()
{
    auto current = getOpenDevice();
    if (current.input.isEmpty()) current = openedDevice;
    if (current.input.isEmpty())
        return juce::String::fromUTF8 ("장치를 다시 열지 못했습니다. 설정에서 장치를 다시 고르세요.");
    return openDevice (current);
}

juce::String MixEngine::openSessionDevice (const MixDevice& device)
{
    if (device.input.isEmpty()) return {};
    const auto current = getOpenDevice();
    if (isRunningWhole() && current.type == device.type && current.input == device.input
        && current.output == (device.isAsio() ? device.input : device.output)
        && (device.type != "Windows Audio (Exclusive Mode)" || current.sampleFormat == device.sampleFormat)
        && (device.bufferSize <= 0 || device.type == "Windows Audio" || current.bufferSize == device.bufferSize)
        && (device.sampleRate <= 0.0 || juce::approximatelyEqual (current.sampleRate, device.sampleRate)))
    {
        applySampleFormat (device.sampleFormat);
        openedDevice.sampleFormat = device.sampleFormat;
        const auto error = openAllChannels();
        ensureCallback();
        return error;
    }
    const auto error = openDevice (device);
    return error.isEmpty() ? juce::String() : juce::String::fromUTF8 ("세션의 ") + error;
}

void MixEngine::shutdown()
{
    stopTimer();
    removeCallback();
    obsSender.setEnabled (false);
    deviceManager.closeAudioDevice();
    if (monitor != nullptr) monitor->stop();
    monitor.reset();
    splitMonitor.store (false, std::memory_order_release);
}

bool MixEngine::isMonitorRunning() const noexcept
{
    return isDeviceRunning() && (isSplitMonitor() ? monitor != nullptr && monitor->isRunning()
                                && publishedMonitor.load() != nullptr && ! monitorRestartRequested.load (std::memory_order_acquire)
                                : getNumDeviceOutputs() > 0);
}

void MixEngine::timerCallback()
{
    if (! isSplitMonitor() || monitor == nullptr || ! isDeviceRunning()) return;
    auto* input = deviceManager.getCurrentAudioDevice();
    auto* type = findType (openedDevice.type);
    if (input == nullptr || type == nullptr || ! input->isOpen()) return;
    // Consume only requests made before this attempt; input restarts during close/open must survive it.
    const bool inputRestart = monitorRestartRequested.exchange (false, std::memory_order_acq_rel);
    if (! monitor->needsRestart() && ! inputRestart) return;

    // Leave input DSP and OBS running throughout output close/open/start. A callback holding the old
    // pointer finishes normally; new callbacks skip monitor delivery until the replacement is ready.
    publishedMonitor.store (nullptr);
    if (monitorReaders.load() != 0)
    {
        monitorRestartRequested.store (true, std::memory_order_release);
        return; // retry on the next timer tick, never wait for the graph
    }
    monitor->stop(); // join the output callback before reading its last announced format
    const auto rate = monitor->getRestartOutputRate();
    const auto period = monitor->getRestartOutputPeriod();
    monitor = std::make_unique<MonitorOutput>();
    monitor->start (*type, openedDevice.output, input->getCurrentSampleRate(), input->getCurrentBufferSizeSamples(), period, rate);
    publishedMonitor.store (monitor.get()); // a failed rebuild stays stopped and visible until the operator retries
    openedDevice = getOpenDevice();
}

double MixEngine::getLatencyMs() const
{
    return inputLatencyMs.load (std::memory_order_relaxed) + outputLatencyMs.load (std::memory_order_relaxed)
           + (isSplitMonitor() && monitor != nullptr ? monitor->getLatencyMs() : 0.0);
}

int MixEngine::getXRunCount() const
{
    int count = 0;
    if (auto* device = deviceManager.getCurrentAudioDevice())
        count = juce::jmax (0, device->getXRunCount());
    if (isSplitMonitor() && monitor != nullptr)
        count += (int) juce::jmin<std::uint64_t> (1000000000, monitor->getUnderruns() + monitor->getOverruns());
    return count;
}

//==============================================================================
void MixEngine::prepare (double newSampleRate, int newBlockSize)
{
    newBlockSize = juce::jmax (16, newBlockSize);
    const juce::ScopedLock sl (lock);
    transport.prepare (newSampleRate);
    sampleRate.store (newSampleRate, std::memory_order_relaxed);
    blockSize.store (newBlockSize, std::memory_order_relaxed);
    chBuf.setSize (2, newBlockSize, false, true, true);
    preBuf.setSize (2, newBlockSize, false, true, true);
    masterBus.setSize (2, newBlockSize, false, true, true);
    monitorStage.setSize (2, newBlockSize, false, true, true);

    for (auto& b : fxBus)
        b.setSize (2, newBlockSize, false, true, true);

    for (auto& c : channels)
        c->chain->prepare (newSampleRate, newBlockSize);

    for (auto& f : fxNodes)
        f->chain->prepare (newSampleRate, newBlockSize);

    master.chain->prepare (newSampleRate, newBlockSize);
    loudness.prepare (newSampleRate);
    obsSender.deviceStarted (newSampleRate);
}

void MixEngine::addToOutputs (float* const* outputs, int numOutputs, int first, const juce::AudioBuffer<float>& source, int offset, int numSamples) noexcept
{
    for (int k = 0; k < 2; ++k)
    {
        const int idx = first + k;

        if (idx >= 0 && idx < numOutputs && outputs[idx] != nullptr)
            juce::FloatVectorOperations::add (outputs[idx] + offset, source.getReadPointer (k), numSamples);
    }
}

void MixEngine::renderBlock (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples)
{
    const juce::ScopedNoDenormals noDenormals;   // a decaying tail must not slide into subnormal arithmetic on the callback
    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputs[ch], numSamples);

    if (numSamples <= 0)
        return;

    const MonitorAccess monitorAccess (*this);
    auto* const monitorOutput = monitorAccess.output;
    // the graph is swapped on the message thread under this lock (a structural edit, a session): the callback never
    // waits for it - this block stays silent (the outputs are cleared above) rather than stalling the driver
    const juce::ScopedTryLock sl (lock);

    if (! sl.isLocked())
    {
        obsSender.writeSilence (numSamples);
        if (isSplitMonitor() && monitorOutput != nullptr)
            monitorOutput->push (nullptr, nullptr, numSamples);
        return; // the graph did not run: the sample clock does not advance
    }

    const int chunkSize = juce::jmax (1, masterBus.getNumSamples());   // a driver may deliver more than announced: chunk, never grow
    transport.beginBlock(); // only the graph callback, after the early return on a busy graph
    const double sr = sampleRate.load (std::memory_order_relaxed);
    const float rampStepPerSample = (float) (1.0 / juce::jmax (1.0, onOffRampSeconds * sr));
    const int panRampSamples = juce::jmax (1, juce::roundToInt (panRampSeconds * sr));

    for (int offset = 0; offset < numSamples; offset += chunkSize)
    {
        transport.setChunkOffset (offset);
        const int n = juce::jmin (chunkSize, numSamples - offset);
        const bool split = isSplitMonitor();
        if (split) monitorStage.clear (0, n);
        auto* const* routedOutputs = split ? monitorStage.getArrayOfWritePointers() : outputs;
        const int routedCount = split ? 2 : numOutputs;
        const int routedOffset = split ? 0 : offset;
        masterBus.clear (0, n);
        const int numFx = juce::jmin (maxFx, (int) fxNodes.size());

        for (int f = 0; f < numFx; ++f)
            fxBus[(size_t) f].clear (0, n);

        for (auto& node : channels)
        {
            // the input: one device input on both sides, or a pair
            const int first = node->inputFirst.load (std::memory_order_relaxed);
            const bool stereo = node->stereo.load (std::memory_order_relaxed);
            const float* in0 = (first >= 0 && first < numInputs && inputs != nullptr && inputs[first] != nullptr) ? inputs[first] + offset : nullptr;
            const float* in1 = (stereo && first + 1 < numInputs && inputs != nullptr && inputs[first + 1] != nullptr) ? inputs[first + 1] + offset : nullptr;

            if (in0 != nullptr)
                chBuf.copyFrom (0, 0, in0, n);
            else
                chBuf.clear (0, 0, n);

            if (stereo)
            {
                if (in1 != nullptr)
                    chBuf.copyFrom (1, 0, in1, n);
                else
                    chBuf.clear (1, 0, n);
            }
            else
            {
                chBuf.copyFrom (1, 0, chBuf, 0, 0, n);
            }

            // mic ON/OFF: a short linear ramp (applied below, to the sends as well). Worked out first: a mic that is
            // fully off skips its chain when asked to - no plugin CPU for a mic nobody hears, like an archived track
            const float target = node->on.load (std::memory_order_relaxed) && ! node->muted.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
            const float start = node->onGain;
            float end = start;

            if (! juce::approximatelyEqual (start, target))
            {
                const float step = rampStepPerSample * (float) n;
                end = start < target ? juce::jmin (target, start + step) : juce::jmax (target, start - step);
            }

            node->onGain = end;
            const bool fullyOff = start <= 0.0f && end <= 0.0f;

            const auto panTarget = channelPanGains (node->pan.load (std::memory_order_relaxed), stereo);

            if (panTarget != node->panTarget)
            {
                node->panTarget = panTarget;
                node->panRemaining = panRampSamples;   // retarget from the gain the preceding block reached
            }

            if (fullyOff)
            {
                node->panCurrent = panTarget;   // a pan moved while off must not start from a stale position on unmute
                node->panRemaining = 0;
            }

            if (fullyOff && skipChainWhenOff.load (std::memory_order_relaxed))
            {
                node->chain->markProcessingSkipped();   // bypass changes made while silent must be settled before audio returns
                node->meter.push (chBuf.getMagnitude (0, 0, n), chBuf.getMagnitude (1, 0, n));   // the mic itself: it is alive

                for (int f = 0; f < numFx; ++f)   // a send fader moved while off: no ramp from a stale level when the mic returns
                    node->sends[(size_t) f].current = node->sends[(size_t) f].amount.load (std::memory_order_relaxed);

                continue;
            }

            // the sends, read once per block: the pre tap and the routing below see the same amounts and pre / post
            // flags. A moved fader ramps from the level the last block ended on (no zipper click).
            float sendAmount[(size_t) maxFx] = {};
            float sendFrom[(size_t) maxFx] = {};
            bool sendPre[(size_t) maxFx] = {};
            bool anyPre = false;

            for (int f = 0; f < numFx; ++f)
            {
                sendAmount[(size_t) f] = node->sends[(size_t) f].amount.load (std::memory_order_relaxed);
                sendFrom[(size_t) f] = node->sends[(size_t) f].current;
                sendPre[(size_t) f] = node->sends[(size_t) f].pre.load (std::memory_order_relaxed);
                anyPre = anyPre || (sendPre[(size_t) f] && (sendAmount[(size_t) f] > 0.0f || sendFrom[(size_t) f] > 0.0f));
            }

            if (anyPre)
            {
                preBuf.copyFrom (0, 0, chBuf, 0, 0, n);
                preBuf.copyFrom (1, 0, chBuf, 1, 0, n);
            }

            node->chain->process (chBuf, n);
            node->meter.push (chBuf.getMagnitude (0, 0, n), chBuf.getMagnitude (1, 0, n));   // what the chain delivers, before the switch

            if (fullyOff)
            {
                for (int f = 0; f < numFx; ++f)   // the sends follow their faders silently while the mic is off
                    node->sends[(size_t) f].current = sendAmount[(size_t) f];

                continue;   // off (the chain kept running, as asked): nothing reaches the buses, the sends included
            }

            if (! juce::approximatelyEqual (start, 1.0f) || ! juce::approximatelyEqual (end, 1.0f))
            {
                chBuf.applyGainRamp (0, n, start, end);

                if (anyPre)
                    preBuf.applyGainRamp (0, n, start, end);
            }

            for (int f = 0; f < numFx; ++f)
            {
                const float amount = sendAmount[(size_t) f];
                const float from = sendFrom[(size_t) f];
                node->sends[(size_t) f].current = amount;

                if (amount <= 0.0f && from <= 0.0f)
                    continue;

                const auto& source = sendPre[(size_t) f] ? preBuf : chBuf;
                fxBus[(size_t) f].addFromWithRamp (0, 0, source.getReadPointer (0), n, from, amount);
                fxBus[(size_t) f].addFromWithRamp (1, 0, source.getReadPointer (1), n, from, amount);
            }

            // The sends above keep their pre/post-chain stereo image. Only the pair routed to the master and
            // direct outputs is panned, after the chain and ON/OFF ramp. No allocation or extra lock here.
            const int panSamples = juce::jmin (n, node->panRemaining);

            for (int side = 0; side < 2; ++side)
            {
                const float to = panTarget[(size_t) side];

                if (panSamples > 0)
                {
                    const float from = node->panCurrent[(size_t) side];
                    const float next = panSamples == node->panRemaining ? to
                        : from + (to - from) * (float) panSamples / (float) node->panRemaining;
                    chBuf.applyGainRamp (side, 0, panSamples, from, next);
                    node->panCurrent[(size_t) side] = next;
                }

                if (n > panSamples && to != 1.0f)
                    chBuf.applyGain (side, panSamples, n - panSamples, to);
            }

            node->panRemaining -= panSamples;

            if (node->toMaster.load (std::memory_order_relaxed))
            {
                masterBus.addFrom (0, 0, chBuf, 0, 0, n);
                masterBus.addFrom (1, 0, chBuf, 1, 0, n);
            }

            if (node->direct.load (std::memory_order_relaxed))
                addToOutputs (routedOutputs, routedCount, outputFirst (node->directFirst.load (std::memory_order_relaxed)), chBuf, routedOffset, n);
        }

        for (int f = 0; f < numFx; ++f)
        {
            auto& fx = *fxNodes[(size_t) f];
            auto& bus = fxBus[(size_t) f];
            fx.chain->process (bus, n);
            const float ret = fx.muted.load (std::memory_order_relaxed) ? 0.0f : fx.returnAmount.load (std::memory_order_relaxed);
            const float retFrom = fx.returnCurrent;
            fx.returnCurrent = ret;

            if (! juce::approximatelyEqual (ret, 1.0f) || ! juce::approximatelyEqual (retFrom, 1.0f))
                bus.applyGainRamp (0, n, retFrom, ret);   // a moved return fader ramps across the block

            if (fx.mono.load (std::memory_order_relaxed))
            {
                // mono: the two sides summed at half level on both outputs (an identical pair keeps its level)
                bus.addFrom (0, 0, bus, 1, 0, n);
                bus.applyGain (0, 0, n, 0.5f);
                bus.copyFrom (1, 0, bus, 0, 0, n);
            }

            fx.meter.push (bus.getMagnitude (0, 0, n), bus.getMagnitude (1, 0, n));

            if (fx.toMaster.load (std::memory_order_relaxed))
            {
                masterBus.addFrom (0, 0, bus, 0, 0, n);
                masterBus.addFrom (1, 0, bus, 1, 0, n);
            }

            if (fx.direct.load (std::memory_order_relaxed))
                addToOutputs (routedOutputs, routedCount, outputFirst (fx.directFirst.load (std::memory_order_relaxed)), bus, routedOffset, n);
        }

        master.chain->process (masterBus, n);
        master.meter.push (masterBus.getMagnitude (0, 0, n), masterBus.getMagnitude (1, 0, n));
        loudness.process (masterBus.getReadPointer (0), masterBus.getReadPointer (1), n);
        obsSender.write (masterBus.getReadPointer (0), masterBus.getReadPointer (1), n);
        addToOutputs (routedOutputs, routedCount, outputFirst (master.outputFirst.load (std::memory_order_relaxed)), masterBus, routedOffset, n);
        if (split && monitorOutput != nullptr)
            monitorOutput->push (monitorStage.getReadPointer (0), monitorStage.getReadPointer (1), n);
    }
    transport.advance (numSamples); // once for the full device callback, never per chain or chunk
}

//==============================================================================
void MixEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                  float* const* outputChannelData, int numOutputChannels,
                                                  int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    const auto t0 = juce::Time::getHighResolutionTicks();
    renderBlock (inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);
    const double seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
    const double blockSeconds = numSamples / juce::jmax (1.0, sampleRate.load (std::memory_order_relaxed));
    const double load = seconds / juce::jmax (1.0e-6, blockSeconds);
    dspLoad.store (dspLoad.load (std::memory_order_relaxed) * 0.9 + load * 0.1, std::memory_order_relaxed);
}

void MixEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double sr = device->getCurrentSampleRate();
    const int bs = device->getCurrentBufferSizeSamples();
    const MonitorAccess monitorAccess (*this);
    if (isSplitMonitor())
    {
        if (auto* output = monitorAccess.output)
        {
            if (output->getInputSampleRate() != sr || output->getInputPeriod() != bs)
                output->requestRestart();
        }
        else
            monitorRestartRequested.store (true, std::memory_order_release);
    }
    prepare (sr > 0.0 ? sr : 48000.0, bs > 0 ? bs : 256);
    numDeviceInputs.store (device->getActiveInputChannels().countNumberOfSetBits(), std::memory_order_relaxed);
    numDeviceOutputs.store (isSplitMonitor() ? 2 : device->getActiveOutputChannels().countNumberOfSetBits(), std::memory_order_relaxed);
    inputLatencyMs.store (1000.0 * device->getInputLatencyInSamples() / juce::jmax (1.0, sr), std::memory_order_relaxed);
    outputLatencyMs.store (1000.0 * device->getOutputLatencyInSamples() / juce::jmax (1.0, sr), std::memory_order_relaxed);
    deviceRunning.store (device->isOpen(), std::memory_order_release);   // JUCE's ASIO reset starts the callback even when the reopen failed
}

void MixEngine::audioDeviceStopped()
{
    deviceRunning.store (false, std::memory_order_release);
    const MonitorAccess monitorAccess (*this);
    if (isSplitMonitor())
    {
        if (auto* output = monitorAccess.output) output->requestRestart();
        else monitorRestartRequested.store (true, std::memory_order_release);
    }
    dspLoad.store (0.0, std::memory_order_relaxed);
}

void MixEngine::audioDeviceError (const juce::String&)
{
    deviceRunning.store (false, std::memory_order_release);   // the status line says "오디오 멈춤" instead of pretending
    const MonitorAccess monitorAccess (*this);
    if (isSplitMonitor())
    {
        if (auto* output = monitorAccess.output) output->requestRestart();
        else monitorRestartRequested.store (true, std::memory_order_release);
    }
}

//==============================================================================
MixEngine::ChannelNode* MixEngine::findChannel (const juce::Uuid& id) const noexcept
{
    for (auto& c : channels)
        if (c->id == id)
            return c.get();

    return nullptr;
}

MixEngine::FxNode* MixEngine::findFx (const juce::Uuid& id) const noexcept
{
    for (auto& f : fxNodes)
        if (f->id == id)
            return f.get();

    return nullptr;
}

int MixEngine::outputFirst (int requested) const noexcept
{
    return stereoOutputsOnly.load (std::memory_order_relaxed) ? 0 : juce::jlimit (0, maxDeviceChannels - 2, requested);
}

void MixEngine::applyOutput (const MixOutput& output, std::atomic<bool>& toMaster, std::atomic<bool>& direct, std::atomic<int>& directFirst)
{
    directFirst.store (juce::jlimit (0, maxDeviceChannels - 2, output.directFirst), std::memory_order_relaxed);
    toMaster.store (output.master, std::memory_order_relaxed);
    direct.store (output.direct, std::memory_order_relaxed);
}

void MixEngine::applySession (const MixSession& session, juce::StringArray* errors, bool restoreChains)
{
    if (deviceManager.getCurrentAudioDevice() == nullptr)
        stereoOutputsOnly.store (! session.device.isAsio(), std::memory_order_relaxed);
    const double sr = getSampleRate();
    const int bs = getBlockSize();
    const auto factory = pluginHost.makeFactory (sr, bs);
    std::vector<juce::Uuid> oldFxOrder;   // the send ramps are indexed by FX position: a changed order snaps them

    for (const auto& old : fxNodes)
        if (old != nullptr)
            oldFxOrder.push_back (old->id);

    auto restore = [&] (PluginChain& chain, const std::vector<PluginSlotState>& states)
    {
        chain.setTimingHook (&transport);
        chain.prepare (sr, bs);
        const auto chainErrors = chain.restore (states, factory);

        if (errors != nullptr)
            errors->addArray (chainErrors);
    };

    // Everything the callback will see is built here, outside the lock. With 'restoreChains' (a file opened) every
    // node and the master chain are new and their plugins are restored before the graph knows them: the callback runs
    // the old graph until the one swap below, never a mix of the two, and no plugin restore happens under the graph
    // lock. A structural edit keeps the existing nodes (their live chains untouched) and builds only what was added.
    std::vector<std::unique_ptr<FxNode>> freshFx;
    std::vector<std::unique_ptr<ChannelNode>> freshChannels;
    std::unique_ptr<PluginChain> freshMaster;

    for (const auto& f : session.fx)
    {
        if (! restoreChains && findFx (f.id) != nullptr)
            continue;

        auto node = std::make_unique<FxNode>();
        node->id = f.id;
        restore (*node->chain, f.chain);
        freshFx.push_back (std::move (node));
    }

    for (const auto& c : session.channels)
    {
        if (! restoreChains && findChannel (c.id) != nullptr)
            continue;

        auto node = std::make_unique<ChannelNode>();
        node->id = c.id;
        node->onGain = c.on ? 1.0f : 0.0f;
        node->panCurrent = node->panTarget = channelPanGains (clampedPan (c.pan), c.stereo);
        restore (*node->chain, c.chain);
        freshChannels.push_back (std::move (node));
    }

    if (restoreChains)
    {
        freshMaster = std::make_unique<PluginChain>();
        restore (*freshMaster, session.master.chain);
    }

    // the lists the graph switches to, sized now: the swap allocates nothing under the lock
    std::vector<std::unique_ptr<FxNode>> newFx, retiredFx;
    std::vector<std::unique_ptr<ChannelNode>> newChannels, retiredChannels;
    std::unique_ptr<PluginChain> retiredMaster;
    newFx.reserve (session.fx.size());
    newChannels.reserve (session.channels.size());
    retiredFx.reserve (fxNodes.size() + freshFx.size());
    retiredChannels.reserve (channels.size() + freshChannels.size());

    {
        const juce::ScopedLock sl (lock);

        // FX first: the channels' sends are indexed by FX position
        for (const auto& f : session.fx)
        {
            std::unique_ptr<FxNode> node;

            for (auto& fresh : freshFx)
                if (fresh != nullptr && fresh->id == f.id)
                    node = std::move (fresh);

            if (node == nullptr)
                for (auto& old : fxNodes)
                    if (old != nullptr && old->id == f.id)
                        node = std::move (old);

            if (node == nullptr)
                continue;   // beyond the limit (the session is sanitised, so this does not happen)

            node->returnAmount.store ((float) juce::jlimit (0.0, 1.0, f.returnAmount), std::memory_order_relaxed);

            if (restoreChains)
                node->returnCurrent = (float) juce::jlimit (0.0, 1.0, f.returnAmount);   // fresh: starts at its level

            node->mono.store (f.mono, std::memory_order_relaxed);
            applyOutput (f.output, node->toMaster, node->direct, node->directFirst);
            newFx.push_back (std::move (node));

            if ((int) newFx.size() >= maxFx)
                break;
        }

        bool fxOrderChanged = oldFxOrder.size() != newFx.size();

        for (size_t i = 0; ! fxOrderChanged && i < newFx.size(); ++i)
            fxOrderChanged = newFx[i]->id != oldFxOrder[i];

        for (const auto& c : session.channels)
        {
            std::unique_ptr<ChannelNode> node;

            for (auto& fresh : freshChannels)
                if (fresh != nullptr && fresh->id == c.id)
                    node = std::move (fresh);

            if (node == nullptr)
                for (auto& old : channels)
                    if (old != nullptr && old->id == c.id)
                        node = std::move (old);

            if (node == nullptr)
                continue;

            node->on.store (c.on, std::memory_order_relaxed);
            node->inputFirst.store (juce::jlimit (0, maxDeviceChannels - 1, c.inputFirst), std::memory_order_relaxed);
            node->stereo.store (c.stereo, std::memory_order_relaxed);
            node->pan.store (clampedPan (c.pan), std::memory_order_relaxed);
            applyOutput (c.output, node->toMaster, node->direct, node->directFirst);

            for (int f = 0; f < maxFx; ++f)
            {
                float amount = 0.0f;
                bool pre = false;

                if (f < (int) newFx.size())
                    for (const auto& s : c.sends)
                        if (s.fx == newFx[(size_t) f]->id)
                        {
                            amount = (float) juce::jlimit (0.0, 1.0, s.amount);
                            pre = s.pre;
                        }

                node->sends[(size_t) f].amount.store (amount, std::memory_order_relaxed);
                node->sends[(size_t) f].pre.store (pre, std::memory_order_relaxed);

                if (restoreChains || fxOrderChanged)
                    node->sends[(size_t) f].current = amount;   // a fresh node starts at its level; a reordered FX list must not ramp from another FX's level
            }

            newChannels.push_back (std::move (node));

            if ((int) newChannels.size() >= maxChannels)
                break;
        }

        master.outputFirst.store (juce::jlimit (0, maxDeviceChannels - 2, session.master.outputFirst), std::memory_order_relaxed);

        if (freshMaster != nullptr)
        {
            retiredMaster = std::move (master.chain);
            master.chain = std::move (freshMaster);
        }

        // whatever was not moved over is retired (destroyed after the lock: plugin teardown is slow)
        for (auto& old : fxNodes)
            if (old != nullptr)
                retiredFx.push_back (std::move (old));

        for (auto& fresh : freshFx)
            if (fresh != nullptr)
                retiredFx.push_back (std::move (fresh));

        for (auto& old : channels)
            if (old != nullptr)
                retiredChannels.push_back (std::move (old));

        for (auto& fresh : freshChannels)
            if (fresh != nullptr)
                retiredChannels.push_back (std::move (fresh));

        fxNodes.swap (newFx);
        channels.swap (newChannels);
    }

    // the retired nodes, their chains and the old master chain are destroyed here, outside the lock
}

bool MixEngine::captureLivePluginStates (MixSession& session) const
{
    bool complete = true;

    for (auto& c : session.channels)
        if (auto* node = findChannel (c.id))
            c.chain = node->chain->getStates (&complete);

    for (auto& f : session.fx)
        if (auto* node = findFx (f.id))
            f.chain = node->chain->getStates (&complete);

    session.master.chain = master.chain->getStates (&complete);
    return complete;
}

//==============================================================================
void MixEngine::setChannelOn (const juce::Uuid& id, bool on)
{
    if (auto* node = findChannel (id))
        node->on.store (on, std::memory_order_relaxed);
}

void MixEngine::setChannelMuted (const juce::Uuid& id, bool muted)
{
    if (auto* node = findChannel (id))
        node->muted.store (muted, std::memory_order_relaxed);
}

void MixEngine::setChannelInput (const juce::Uuid& id, int first, bool stereo)
{
    if (auto* node = findChannel (id))
    {
        node->inputFirst.store (juce::jlimit (0, maxDeviceChannels - 1, first), std::memory_order_relaxed);
        node->stereo.store (stereo, std::memory_order_relaxed);
    }
}

void MixEngine::setChannelOutput (const juce::Uuid& id, const MixOutput& output)
{
    if (auto* node = findChannel (id))
        applyOutput (output, node->toMaster, node->direct, node->directFirst);
}

void MixEngine::setChannelPan (const juce::Uuid& id, double pan)
{
    if (auto* node = findChannel (id))
        node->pan.store (clampedPan (pan), std::memory_order_relaxed);
}

void MixEngine::setSend (const juce::Uuid& channelId, const juce::Uuid& fxId, double amount, bool pre)
{
    auto* node = findChannel (channelId);

    if (node == nullptr)
        return;

    for (size_t f = 0; f < fxNodes.size() && f < (size_t) maxFx; ++f)
    {
        if (fxNodes[f]->id == fxId)
        {
            node->sends[f].pre.store (pre, std::memory_order_relaxed);
            node->sends[f].amount.store ((float) juce::jlimit (0.0, 1.0, amount), std::memory_order_relaxed);
        }
    }
}

void MixEngine::setFxReturn (const juce::Uuid& fxId, double amount)
{
    if (auto* node = findFx (fxId))
        node->returnAmount.store ((float) juce::jlimit (0.0, 1.0, amount), std::memory_order_relaxed);
}

void MixEngine::setFxMono (const juce::Uuid& fxId, bool mono)
{
    if (auto* node = findFx (fxId))
        node->mono.store (mono, std::memory_order_relaxed);
}

void MixEngine::setFxMuted (const juce::Uuid& fxId, bool muted)
{
    if (auto* node = findFx (fxId))
        node->muted.store (muted, std::memory_order_relaxed);
}

void MixEngine::setFxOutput (const juce::Uuid& fxId, const MixOutput& output)
{
    if (auto* node = findFx (fxId))
        applyOutput (output, node->toMaster, node->direct, node->directFirst);
}

void MixEngine::setMasterOutput (int first)
{
    master.outputFirst.store (juce::jlimit (0, maxDeviceChannels - 2, first), std::memory_order_relaxed);
}

PluginChain* MixEngine::getChannelChain (const juce::Uuid& id) const noexcept
{
    auto* node = findChannel (id);
    return node != nullptr ? node->chain.get() : nullptr;
}

PluginChain* MixEngine::getFxChain (const juce::Uuid& id) const noexcept
{
    auto* node = findFx (id);
    return node != nullptr ? node->chain.get() : nullptr;
}

void MixEngine::forEachChain (const std::function<void (PluginChain&)>& fn) const
{
    for (auto& c : channels)
        fn (*c->chain);

    for (auto& f : fxNodes)
        fn (*f->chain);

    fn (*master.chain);
}

MixEngine::Meter MixEngine::readChannelMeter (const juce::Uuid& id)
{
    auto* node = findChannel (id);
    return node != nullptr ? node->meter.take() : Meter();
}

MixEngine::Meter MixEngine::readFxMeter (const juce::Uuid& id)
{
    auto* node = findFx (id);
    return node != nullptr ? node->meter.take() : Meter();
}

MixEngine::Meter MixEngine::readMasterMeter()
{
    return master.meter.take();
}

} // namespace gocue::livemix
