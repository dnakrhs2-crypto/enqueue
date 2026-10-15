#pragma once
#include "CollabAudio.h"
#include "CollabHostCheck.h"
#include "CollabRoom.h"
#include <juce_events/juce_events.h>

namespace gocue::livemix
{
class LiveMixSettings;
class MixEngine;
namespace collab
{
// App lifetime, independent of MixDocument. Public methods and notifications use the message thread.
class CollabSession : public juce::ChangeBroadcaster, private juce::Timer
{
public:
    enum class State { off, checkingHost, hostWaiting, hostUnavailable, joining, connected, disconnected, error };
    struct Snapshot
    {
        State state = State::off;
        bool isHost = false;
        juce::String code, myNick, peerNick;
        double rttMs = 0, estimatedOneWayMs = 0, bufferMs = 0, lossPercent = 0;
        juce::String reason, message;
        float monitorVolume = 1.0f;
        bool monitorOn = true;
        uint32_t wireRate = defaultWireRate;
    };
    struct Options
    {
        int probeTimeoutMs = 2500, connectTimeoutMs = 10000;
        int heartbeatIntervalMs = 10000, renewIntervalMs = 30 * 60 * 1000;
        bool loopbackOnly = false;
    };
    CollabSession (MixEngine&, LiveMixSettings&);
    // Injectable service, UPnP and deadlines for loopback tests.
    CollabSession (CollabAudio&, LiveMixSettings&, std::unique_ptr<CollabRoom>, std::unique_ptr<CollabUpnp>, Options);
    ~CollabSession() override; // finishes leave on the worker before the engine is destroyed

    void createRoom (const juce::String& nick);
    void joinRoom (const juce::String& code, const juce::String& nick);
    void leave(); // immediately off; network cleanup completes on the worker
    void rebuffer();
    void setMonitor (float volume01, bool on);
    Snapshot getSnapshot() const;
    static juce::StringArray lanAddresses();

private:
    friend struct CollabSessionTestAccess;
    struct Impl;
    void timerCallback() override;
    void setState (State, const juce::String& message = {}, const juce::String& reason = {});
    void fail (const juce::String& message);
    PeerInfo localInfo (const juce::String& nick) const;
    void sendHeartbeat();

    CollabAudio& audio;
    LiveMixSettings& settings;
    Options options;
    CollabTransport transport;
    std::unique_ptr<Impl> impl;
    std::function<PeerInfo()> peerInfo;
    Snapshot snapshot;
    bool started = false, audioAttached = false, mapped = false, heartbeatPending = false, renewPending = false;
    bool heartbeatConnected = false, heartbeatStopped = false;
    int heartbeatFailures = 0;
    double joinDeadline = 0, lastHeartbeat = 0, lastRenew = 0, lastNotification = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CollabSession)
};
}
}
