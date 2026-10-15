#pragma once
#include "CollabPlayout.h"
#include "CollabSender.h"
#include "ObsSender.h"

namespace gocue::livemix::collab
{
class CollabTransport;
class CollabAudio
{
public:
    explicit CollabAudio (const juce::String& peerMapping = "Local\\LiveMix.ObsAudio.v1.Peer");
    ~CollabAudio();
    void prepare (double deviceRate, int maxBlock);
    void attach (CollabTransport&); // lifecycle thread; transport and audio detach each other on destruction
    void detach();
    void pushLocal (const float* left, const float* right, int frames) noexcept;
    void pushLocalSilence (int frames) noexcept { pushLocal (nullptr, nullptr, frames); }
    void renderRemote (float* const* outputs, int numOutputs, int masterFirst, int frames) noexcept;
    void setMonitor (float volume01, bool on) noexcept;
    void setObsEnabled (bool enabled) { peerObs.setEnabled (enabled); }
    ObsSender& getPeerObs() noexcept { return peerObs; }
    CollabPlayout::Stats getStats() const noexcept
    {
        auto stats = playout.getStats();
        if (! connected.load()) stats.state = CollabPlayout::State::off;
        return stats;
    }
    void rebuffer() noexcept { playout.rebuffer(); }
    double getDeviceRate() const noexcept { return publishedRate.load(); }

    // Transport receiver/sender entry points. The same entry points support offline packet tests.
    void receive (const Header&, const uint8_t*, uint64_t arrivalNs) noexcept;
    void drain (const CollabSender::Sink&);
    void setConnected (bool value) noexcept;
    void* getSendEvent() const noexcept { return sender.getEvent(); }

private:
    friend class CollabTransport;
    struct Access;
    void suspend();
    void startSession();
    void transportStopped() noexcept;
    CollabTransport* transport = nullptr; // lifecycle only
    std::atomic<bool> ready { false }, attached { false }, connected { false };
    std::atomic<unsigned> readers { 0 };
    juce::CriticalSection lifecycle;
    CollabSender sender;
    CollabPlayout playout;
    ObsSender peerObs;
    std::vector<float> left, right;
    std::atomic<float> volume { 1.0f };
    std::atomic<bool> monitorOn { true };
    std::atomic<double> publishedRate { wireRate };
    float gain = 1.0f, gainTarget = 1.0f, gainStep = 0.0f;
    int gainRemaining = 0, rampFrames = 240;
    bool wasConnected = false;
    float tailL = 0, tailR = 0;
    int tailRemaining = 0;
};
}
