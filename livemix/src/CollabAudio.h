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
    bool setWireRate (uint32_t); // message thread, before attach; keeps the device format
    uint32_t getWireRate() const noexcept { return wireRate.load(); }
    void attach (CollabTransport&); // lifecycle thread; transport and audio detach each other on destruction
    void detach();
    void pushLocal (const float* left, const float* right, int frames) noexcept;
    void pushLocalSilence (int frames) noexcept { pushLocal (nullptr, nullptr, frames); }
    void renderRemote (float* const* outputs, int numOutputs, int masterFirst, int frames) noexcept;
    void setMonitor (float volume01, bool on) noexcept;
    /** The partner's OBS ring runs while 'OBS로 보내기' is on or a collab is open, so either person can add the
        'LiveMix 합방 상대' source without the master switch. Message thread. */
    void setObsEnabled (bool enabled) { obsWanted = enabled; peerObs.setEnabled (obsWanted || collabOpen); }
    void setCollabOpen (bool open) { if (collabOpen == open) return; collabOpen = open; peerObs.setEnabled (obsWanted || collabOpen); }
    /** Where the partner plays: on the master's pair after the master (never inside the master bus, which the partner
        and the 'LiveMix' OBS source receive), on a pair of its own, or nowhere when it is heard through OBS monitoring.
        The peer OBS ring is written in every route. Any thread. */
    enum class Route { master = 0, direct = 1, obs = 2 };
    void setRoute (Route r, int directFirst) noexcept { route.store ((int) r); routeFirst.store (directFirst < 0 ? 0 : directFirst); }
    Route getRoute() const noexcept { return (Route) route.load(); }
    int getRouteFirst() const noexcept { return routeFirst.load(); }
    ObsSender& getPeerObs() noexcept { return peerObs; }
    CollabPlayout::Stats getStats() const noexcept
    {
        auto stats = playout.getStats();
        if (! connected.load()) stats.state = CollabPlayout::State::off;
        return stats;
    }
    void rebuffer() noexcept { playout.rebuffer(); }
    double getDeviceRate() const noexcept { return publishedRate.load(); }
    struct Meter { float left = 0, right = 0; };
    // Pre-headphone stereo maxima since the UI's previous read, like MixEngine::MeterCell.
    Meter readRemoteMeter() noexcept
    {
        return { peakLeft.exchange (0.0f, std::memory_order_relaxed), peakRight.exchange (0.0f, std::memory_order_relaxed) };
    }

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
    bool obsWanted = false, collabOpen = false;   // message thread
    std::vector<float> left, right;
    std::atomic<float> volume { 1.0f };
    std::atomic<float> peakLeft { 0.0f }, peakRight { 0.0f };
    std::atomic<bool> monitorOn { true };
    std::atomic<int> route { 0 }, routeFirst { 0 };
    std::atomic<double> publishedRate { defaultWireRate };
    std::atomic<uint32_t> wireRate { defaultWireRate };
    float gain = 1.0f, gainTarget = 1.0f, gainStep = 0.0f;
    int gainRemaining = 0, rampFrames = 240;
    bool wasConnected = false;
    float tailL = 0, tailR = 0;
    int tailRemaining = 0;
};
}
