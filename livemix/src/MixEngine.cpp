#include "MixEngine.h"

#include <algorithm>

namespace gocue::livemix
{

MixEngine::MixEngine()
{
    prepare (48000.0, 256);
}

MixEngine::~MixEngine()
{
    shutdown();
}

//==============================================================================
juce::String MixEngine::initialise (const juce::XmlElement* savedDeviceState)
{
    // ASIO only: the other types are never listed, never opened
    juce::AudioIODeviceType* asio = nullptr;

    for (auto* type : deviceManager.getAvailableDeviceTypes())
        if (type->getTypeName().containsIgnoreCase ("ASIO"))
            asio = type;

    if (asio == nullptr)
        return juce::String::fromUTF8 ("ASIO 장치 타입을 쓸 수 없습니다 (ASIO 드라이버가 설치된 오디오 인터페이스가 필요합니다).");

    deviceManager.setCurrentAudioDeviceType (asio->getTypeName(), false);

    juce::String error = deviceManager.initialise (maxDeviceChannels, maxDeviceChannels, savedDeviceState, false, asio->getTypeName());

    if (deviceManager.getCurrentAudioDevice() == nullptr || deviceManager.getCurrentAudioDeviceType() != asio->getTypeName())
    {
        // nothing (or the wrong type) opened: the first ASIO device
        asio->scanForDevices();
        const auto names = asio->getDeviceNames (false);

        if (names.isEmpty())
            return error.isNotEmpty() ? error : juce::String::fromUTF8 ("ASIO 장치가 없습니다. 오디오 인터페이스를 연결하고 드라이버를 설치하세요.");

        deviceManager.setCurrentAudioDeviceType (asio->getTypeName(), false);
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.outputDeviceName = names[0];
        setup.inputDeviceName = names[0];
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = true;
        error = deviceManager.setAudioDeviceSetup (setup, true);
    }

    if (deviceManager.getCurrentAudioDevice() == nullptr)
        return error.isNotEmpty() ? error : juce::String::fromUTF8 ("ASIO 장치를 열지 못했습니다.");

    const auto widened = openAllChannels();

    if (! callbackAdded)
    {
        deviceManager.addAudioCallback (this);
        callbackAdded = true;
    }

    return widened;
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
    allIn.setRange (0, juce::jmin (ins, maxDeviceChannels), true);
    allOut.setRange (0, juce::jmin (outs, maxDeviceChannels), true);

    if (setup.inputChannels == allIn && setup.outputChannels == allOut && ! setup.useDefaultInputChannels && ! setup.useDefaultOutputChannels)
        return {};

    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels = allIn;
    setup.outputChannels = allOut;
    return deviceManager.setAudioDeviceSetup (setup, true);
}

void MixEngine::shutdown()
{
    if (callbackAdded)
    {
        deviceManager.removeAudioCallback (this);
        callbackAdded = false;
    }

    deviceManager.closeAudioDevice();
}

double MixEngine::getLatencyMs() const
{
    return inputLatencyMs.load (std::memory_order_relaxed) + outputLatencyMs.load (std::memory_order_relaxed);
}

int MixEngine::getXRunCount() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return juce::jmax (0, device->getXRunCount());

    return 0;
}

//==============================================================================
void MixEngine::prepare (double newSampleRate, int newBlockSize)
{
    newBlockSize = juce::jmax (16, newBlockSize);
    const juce::ScopedLock sl (lock);
    sampleRate.store (newSampleRate, std::memory_order_relaxed);
    blockSize.store (newBlockSize, std::memory_order_relaxed);
    chBuf.setSize (2, newBlockSize, false, true, true);
    preBuf.setSize (2, newBlockSize, false, true, true);
    masterBus.setSize (2, newBlockSize, false, true, true);

    for (auto& b : fxBus)
        b.setSize (2, newBlockSize, false, true, true);

    for (auto& c : channels)
        c->chain->prepare (newSampleRate, newBlockSize);

    for (auto& f : fxNodes)
        f->chain->prepare (newSampleRate, newBlockSize);

    master.chain->prepare (newSampleRate, newBlockSize);
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
    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputs[ch], numSamples);

    if (numSamples <= 0)
        return;

    const juce::ScopedLock sl (lock);
    const int chunkSize = juce::jmax (1, masterBus.getNumSamples());   // a driver may deliver more than announced: chunk, never grow
    const double sr = sampleRate.load (std::memory_order_relaxed);
    const float rampStepPerSample = (float) (1.0 / juce::jmax (1.0, onOffRampSeconds * sr));

    for (int offset = 0; offset < numSamples; offset += chunkSize)
    {
        const int n = juce::jmin (chunkSize, numSamples - offset);
        masterBus.clear (0, n);
        const int numFx = (int) fxNodes.size();

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

            bool anyPre = false;

            for (int f = 0; f < numFx; ++f)
                if (node->sends[(size_t) f].pre.load (std::memory_order_relaxed) && node->sends[(size_t) f].amount.load (std::memory_order_relaxed) > 0.0f)
                    anyPre = true;

            if (anyPre)
            {
                preBuf.copyFrom (0, 0, chBuf, 0, 0, n);
                preBuf.copyFrom (1, 0, chBuf, 1, 0, n);
            }

            node->chain->process (chBuf, n);
            node->meter.push (chBuf.getMagnitude (0, 0, n), chBuf.getMagnitude (1, 0, n));   // what the chain delivers, before the switch

            // mic ON/OFF: a short linear ramp, applied to the sends as well
            const float target = node->on.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
            const float start = node->onGain;
            float end = start;

            if (! juce::approximatelyEqual (start, target))
            {
                const float step = rampStepPerSample * (float) n;
                end = start < target ? juce::jmin (target, start + step) : juce::jmax (target, start - step);
            }

            node->onGain = end;

            if (start <= 0.0f && end <= 0.0f)
                continue;   // off: nothing reaches the buses, the sends included

            if (! juce::approximatelyEqual (start, 1.0f) || ! juce::approximatelyEqual (end, 1.0f))
            {
                chBuf.applyGainRamp (0, n, start, end);

                if (anyPre)
                    preBuf.applyGainRamp (0, n, start, end);
            }

            for (int f = 0; f < numFx; ++f)
            {
                const auto& send = node->sends[(size_t) f];
                const float amount = send.amount.load (std::memory_order_relaxed);

                if (amount <= 0.0f)
                    continue;

                const auto& source = send.pre.load (std::memory_order_relaxed) ? preBuf : chBuf;
                fxBus[(size_t) f].addFrom (0, 0, source, 0, 0, n, amount);
                fxBus[(size_t) f].addFrom (1, 0, source, 1, 0, n, amount);
            }

            if (node->toMaster.load (std::memory_order_relaxed))
            {
                masterBus.addFrom (0, 0, chBuf, 0, 0, n);
                masterBus.addFrom (1, 0, chBuf, 1, 0, n);
            }

            if (node->direct.load (std::memory_order_relaxed))
                addToOutputs (outputs, numOutputs, node->directFirst.load (std::memory_order_relaxed), chBuf, offset, n);
        }

        for (int f = 0; f < numFx; ++f)
        {
            auto& fx = *fxNodes[(size_t) f];
            auto& bus = fxBus[(size_t) f];
            fx.chain->process (bus, n);
            const float ret = fx.returnAmount.load (std::memory_order_relaxed);

            if (! juce::approximatelyEqual (ret, 1.0f))
                bus.applyGain (0, n, ret);

            fx.meter.push (bus.getMagnitude (0, 0, n), bus.getMagnitude (1, 0, n));

            if (fx.toMaster.load (std::memory_order_relaxed))
            {
                masterBus.addFrom (0, 0, bus, 0, 0, n);
                masterBus.addFrom (1, 0, bus, 1, 0, n);
            }

            if (fx.direct.load (std::memory_order_relaxed))
                addToOutputs (outputs, numOutputs, fx.directFirst.load (std::memory_order_relaxed), bus, offset, n);
        }

        master.chain->process (masterBus, n);
        master.meter.push (masterBus.getMagnitude (0, 0, n), masterBus.getMagnitude (1, 0, n));
        addToOutputs (outputs, numOutputs, master.outputFirst.load (std::memory_order_relaxed), masterBus, offset, n);
    }
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
    prepare (sr > 0.0 ? sr : 48000.0, bs > 0 ? bs : 256);
    numDeviceInputs.store (device->getActiveInputChannels().countNumberOfSetBits(), std::memory_order_relaxed);
    numDeviceOutputs.store (device->getActiveOutputChannels().countNumberOfSetBits(), std::memory_order_relaxed);
    inputLatencyMs.store (1000.0 * device->getInputLatencyInSamples() / juce::jmax (1.0, sr), std::memory_order_relaxed);
    outputLatencyMs.store (1000.0 * device->getOutputLatencyInSamples() / juce::jmax (1.0, sr), std::memory_order_relaxed);
    deviceRunning.store (true, std::memory_order_release);
}

void MixEngine::audioDeviceStopped()
{
    deviceRunning.store (false, std::memory_order_release);
    dspLoad.store (0.0, std::memory_order_relaxed);
}

void MixEngine::audioDeviceError (const juce::String& errorMessage)
{
    juce::Logger::writeToLog ("LiveMix audio device error: " + errorMessage);
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

void MixEngine::applyOutput (const MixOutput& output, std::atomic<bool>& toMaster, std::atomic<bool>& direct, std::atomic<int>& directFirst)
{
    directFirst.store (juce::jlimit (0, maxDeviceChannels - 2, output.directFirst), std::memory_order_relaxed);
    toMaster.store (output.master, std::memory_order_relaxed);
    direct.store (output.direct, std::memory_order_relaxed);
}

void MixEngine::applySession (const MixSession& session, juce::StringArray* errors, bool restoreChains)
{
    const double sr = getSampleRate();
    const int bs = getBlockSize();
    const auto factory = pluginHost.makeFactory (sr, bs);

    auto restore = [&] (PluginChain& chain, const std::vector<PluginSlotState>& states)
    {
        chain.prepare (sr, bs);
        const auto chainErrors = chain.restore (states, factory);

        if (errors != nullptr)
            errors->addArray (chainErrors);
    };

    // new nodes are built (plugins and all) outside the lock; existing ones are reused by id
    std::vector<std::unique_ptr<FxNode>> freshFx;
    std::vector<std::unique_ptr<ChannelNode>> freshChannels;

    for (const auto& f : session.fx)
    {
        if (auto* existing = findFx (f.id))
        {
            if (restoreChains)
                restore (*existing->chain, f.chain);

            continue;
        }

        auto node = std::make_unique<FxNode>();
        node->id = f.id;
        restore (*node->chain, f.chain);
        freshFx.push_back (std::move (node));
    }

    for (const auto& c : session.channels)
    {
        if (auto* existing = findChannel (c.id))
        {
            if (restoreChains)
                restore (*existing->chain, c.chain);

            continue;
        }

        auto node = std::make_unique<ChannelNode>();
        node->id = c.id;
        node->onGain = c.on ? 1.0f : 0.0f;
        restore (*node->chain, c.chain);
        freshChannels.push_back (std::move (node));
    }

    if (restoreChains || master.chain->getNumSlots() == 0)
        restore (*master.chain, session.master.chain);

    std::vector<std::unique_ptr<FxNode>> retiredFx;
    std::vector<std::unique_ptr<ChannelNode>> retiredChannels;

    {
        const juce::ScopedLock sl (lock);

        // FX first: the channels' sends are indexed by FX position
        std::vector<std::unique_ptr<FxNode>> newFx;

        for (const auto& f : session.fx)
        {
            std::unique_ptr<FxNode> node;

            for (auto& old : fxNodes)
                if (old != nullptr && old->id == f.id)
                    node = std::move (old);

            if (node == nullptr)
                for (auto& fresh : freshFx)
                    if (fresh != nullptr && fresh->id == f.id)
                        node = std::move (fresh);

            if (node == nullptr)
                continue;   // beyond the limit (the session is sanitised, so this does not happen)

            node->returnAmount.store ((float) juce::jlimit (0.0, 1.0, f.returnAmount), std::memory_order_relaxed);
            applyOutput (f.output, node->toMaster, node->direct, node->directFirst);
            newFx.push_back (std::move (node));

            if ((int) newFx.size() >= maxFx)
                break;
        }

        std::vector<std::unique_ptr<ChannelNode>> newChannels;

        for (const auto& c : session.channels)
        {
            std::unique_ptr<ChannelNode> node;

            for (auto& old : channels)
                if (old != nullptr && old->id == c.id)
                    node = std::move (old);

            if (node == nullptr)
                for (auto& fresh : freshChannels)
                    if (fresh != nullptr && fresh->id == c.id)
                        node = std::move (fresh);

            if (node == nullptr)
                continue;

            node->on.store (c.on, std::memory_order_relaxed);
            node->inputFirst.store (juce::jlimit (0, maxDeviceChannels - 1, c.inputFirst), std::memory_order_relaxed);
            node->stereo.store (c.stereo, std::memory_order_relaxed);
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
            }

            newChannels.push_back (std::move (node));

            if ((int) newChannels.size() >= maxChannels)
                break;
        }

        master.outputFirst.store (juce::jlimit (0, maxDeviceChannels - 2, session.master.outputFirst), std::memory_order_relaxed);

        // whatever was not moved over is retired (destroyed after the lock: plugin teardown is slow)
        for (auto& old : fxNodes)
            if (old != nullptr)
                retiredFx.push_back (std::move (old));

        for (auto& old : channels)
            if (old != nullptr)
                retiredChannels.push_back (std::move (old));

        fxNodes.swap (newFx);
        channels.swap (newChannels);
    }

    retiredFx.clear();
    retiredChannels.clear();
    freshFx.clear();
    freshChannels.clear();
}

void MixEngine::captureLivePluginStates (MixSession& session) const
{
    for (auto& c : session.channels)
        if (auto* node = findChannel (c.id))
            c.chain = node->chain->getStates();

    for (auto& f : session.fx)
        if (auto* node = findFx (f.id))
            f.chain = node->chain->getStates();

    session.master.chain = master.chain->getStates();
}

//==============================================================================
void MixEngine::setChannelOn (const juce::Uuid& id, bool on)
{
    if (auto* node = findChannel (id))
        node->on.store (on, std::memory_order_relaxed);
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
