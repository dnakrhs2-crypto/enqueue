#pragma once
#include "CollabPlayout.h"
#include "CollabSender.h"
#include "CollabRoster.h"
#include "ObsSender.h"

namespace gocue::livemix::collab
{
class CollabTransport;
class CollabAudio
{
public:
    explicit CollabAudio (const juce::String& peerMapping = "Local\\LiveMix.ObsAudio.v1.Peer");
    CollabAudio (const juce::String& peer1Mapping, const juce::String& peer2Mapping);
    ~CollabAudio();
    void prepare (double deviceRate, int maxBlock);
    bool setWireRate (uint32_t); // message thread, before attach; keeps the device format
    uint32_t getWireRate() const noexcept { return wireRate.load(); }
    void attach (CollabTransport&); // lifecycle thread; transport and audio detach each other on destruction
    void detach();
    void pushLocal (const float* left, const float* right, int frames) noexcept;
    void pushLocalSilence (int frames) noexcept { pushLocal (nullptr, nullptr, frames); }
    // Legacy wrapper: outputs/first are already routed by the caller, as in MixEngine::finishOutput.
    void renderRemote (float* const* outputs, int numOutputs, int masterFirst, int frames) noexcept;
    // Once per callback, frames <= prepared maxBlock. Views remain valid until renderSources/prepare.
    contracts::RenderedSources renderSources (int frames) noexcept;
    // Receives the output array and master's first channel; resolves the shared master/direct/OBS route.
    void renderMonitor (const contracts::RenderedSources&, float* const* outputs, int numOutputs, int masterFirst) noexcept;
    void setSlotMonitorVolume (unsigned slotIndex, float volume01) noexcept;
    void setMonitor (float volume01, bool on) noexcept;
    /** The partner's OBS ring runs while 'OBS로 보내기' is on or a collab is open, so either person can add the
        'LiveMix 합방 상대' source without the master switch. Message thread. */
    void setObsEnabled (bool enabled);
    void setCollabOpen (bool open);
    /** Where the partner plays: on the master's pair after the master (never inside the master bus, which the partner
        and the 'LiveMix' OBS source receive), on a pair of its own, or nowhere when it is heard through OBS monitoring.
        The peer OBS ring is written in every route. Any thread. */
    enum class Route { master = 0, direct = 1, obs = 2 };
    void setRoute (Route r, int directFirst) noexcept { route.store ((int) r); routeFirst.store (directFirst < 0 ? 0 : directFirst); }
    Route getRoute() const noexcept { return (Route) route.load(); }
    int getRouteFirst() const noexcept { return routeFirst.load(); }
    ObsSender& getPeerObs() noexcept { return getPeerObs (0); }
    ObsSender& getPeerObs (unsigned slotIndex) noexcept;
    CollabPlayout::Stats getStats() const noexcept { return getStats (0); }
    CollabPlayout::Stats getStats (unsigned slotIndex) const noexcept;
    // Cumulative valid-session packets rejected before their roster was installed.
    uint64_t getFutureRevisionDrops() const noexcept { return futureRevisionDrops.load(); }
    void rebuffer() noexcept;
    double getDeviceRate() const noexcept { return publishedRate.load(); }
    struct Meter { float left = 0, right = 0; };
    // Pre-headphone stereo maxima since the UI's previous read, like MixEngine::MeterCell.
    Meter readRemoteMeter() noexcept
    {
        return { peakLeft.exchange (0.0f, std::memory_order_relaxed), peakRight.exchange (0.0f, std::memory_order_relaxed) };
    }

    // Transport receiver/sender entry points. The same entry points support offline packet tests.
    void receive (const Header&, const uint8_t*, uint64_t arrivalNs) noexcept;
    void receiveV2 (const contracts::HeaderV2&, const uint8_t*, uint64_t arrivalNs) noexcept;
    void applyRoster (const RosterSnapshot&, contracts::ParticipantId selfId); // control thread; already validated by R3
    bool hasInstalledRoster (uint64_t session, contracts::Revision) const noexcept;
    void drain (const CollabSender::Sink&);
    void setConnected (bool value) noexcept;
    void* getSendEvent() const noexcept { return sender.getEvent(); }

private:
    friend class CollabTransport;
    friend struct CollabSourcesTestAccess;
    struct Access;
    struct SlotTable;
    void suspend();
    void startSession();
    void transportStopped() noexcept;
    // The legacy engine supplies an already routed output pair; the new API resolves the route itself.
    void renderMonitor (const contracts::RenderedSources&, float* const*, int count, int first,
                        bool alreadyRouted, int offset) noexcept;
    CollabTransport* transport = nullptr; // lifecycle only
    std::atomic<bool> ready { false }, attached { false }, connected { false };
    std::atomic<uint64_t> futureRevisionDrops { 0 };
    mutable std::atomic<unsigned> readers { 0 };
    juce::CriticalSection lifecycle;
    CollabSender sender;
    // Immutable identity table. Only the control thread copies/releases shared ownership of playouts.
    // Readers borrow the published pointer under Access; only the control thread waits to retire it.
    std::unique_ptr<SlotTable> ownedSlots;
    std::atomic<const SlotTable*> publishedSlots { nullptr };
    ObsSender peerObs, peerObs2;
    const bool secondObs;
    bool obsWanted = false, collabOpen = false;   // message thread
    struct MonitorSlot
    {
        std::vector<float> left, right; // stable across roster changes, until the next renderSources/prepare
        std::atomic<float> volume { 1.0f };
        float gain = 1.0f, gainTarget = 1.0f, gainStep = 0.0f;
        int gainRemaining = 0;
    };
    std::array<MonitorSlot, contracts::remoteSlots> monitor;
    std::atomic<float> peakLeft { 0.0f }, peakRight { 0.0f };
    std::atomic<bool> monitorOn { true };
    std::atomic<int> route { 0 }, routeFirst { 0 };
    std::atomic<double> publishedRate { defaultWireRate };
    std::atomic<uint32_t> wireRate { defaultWireRate };
    int rampFrames = 240;
    bool wasConnected = false;
    float tailL = 0, tailR = 0;
    int tailRemaining = 0;
};
}
