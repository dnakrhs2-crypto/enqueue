#pragma once
#include "CollabAudio.h"
#include "CollabHostCheck.h"
#include "CollabRoom.h"
#include "WebGuestSignaling.h"
#include "WebGuestTransport.h"
#include "ObsMetadata.h"
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
    struct PersonSnapshot
    {
        contracts::ParticipantId participantId = 0;
        int slot = 0;
        bool isSelf = false, isHost = false;
        contracts::ParticipantKind kind = contracts::ParticipantKind::none;
        contracts::ConnectionState state = contracts::ConnectionState::empty;
        contracts::StreamEpoch streamEpoch = 0;
        juce::String nickname, appVersion;
        double rttMs = 0, estimatedOneWayMs = 0, bufferMs = 0, lossPercent = 0;
        float peakLeft = 0, peakRight = 0, monitorVolume = 1.0f;
    };
    struct WebRequestSnapshot
    {
        juce::String requestId, nickname;
        int64_t expiresAtMs = 0;
        bool canAccept = false, canReject = false;
    };
    struct Snapshot
    {
        State state = State::off;
        bool isHost = false;
        juce::String code, myNick, peerNick;
        double rttMs = 0, estimatedOneWayMs = 0, bufferMs = 0, lossPercent = 0;
        juce::String reason, message;
        float monitorVolume = 1.0f;
        bool monitorOn = true;
        CollabAudio::Route route = CollabAudio::Route::master;
        int routeFirst = 0;
        uint32_t wireRate = defaultWireRate;
        uint64_t session = 0;
        contracts::ParticipantId selfId = 0;
        uint32_t capabilities = 0;
        contracts::RoomMode roomMode = contracts::RoomMode::legacy2;
        contracts::Revision roomRevision = 0, rosterRevision = 0;
        bool nativeAvailable = false, signalingConnected = false;
        std::array<PersonSnapshot, 3> people {};
        size_t personCount = 0;
        std::array<WebRequestSnapshot, 8> webRequests {};
        size_t webRequestCount = 0;
        juce::String webInviteUrl, webInviteQrUrl;
        uint32_t inviteGeneration = 0;
        bool canCreateWebInvite = false;
        contracts::ParticipantId kickableWebId = 0;
    };
    // R8-owned seam: production delegates directly to R1, tests supply a packet endpoint.
    struct WebEndpoint
    {
        virtual ~WebEndpoint() = default;
        virtual void start (const contracts::WebConnection&, WebGuestTransport::Options, WebGuestTransport::Callbacks) = 0;
        virtual void description (uint32_t, const juce::String&, const juce::String&) = 0;
        virtual void candidate (uint32_t, const juce::String&, const juce::String&, int) = 0;
        virtual void endCandidates (uint32_t) = 0;
        virtual bool send (uint32_t, const uint8_t*, size_t, bool audio) = 0;
        virtual void close() = 0;
    };
    struct Options
    {
        int probeTimeoutMs = 2500, connectTimeoutMs = 10000;
        int heartbeatIntervalMs = 10000, renewIntervalMs = 30 * 60 * 1000;
        bool loopbackOnly = false;
        // UDP hole punching (10/16 two-PC test: a host behind a router without UPnP): this socket's public address
        // from STUN servers; while punching, a waiting host asks the room server for the guest every second.
        // Two providers: a symmetric NAT shows only to different server addresses (stun1.l.google.com is the
        // same server as stun.l.google.com).
        std::vector<CollabTransport::Address> stunServers { { "stun.l.google.com", 19302 }, { "stun.cloudflare.com", 3478 } };
        int stunTimeoutMs = 1500, punchHeartbeatMs = 1000, punchMs = 10000;
        int punchHelloTtl = 3, punchHelloMs = 3000;
        CollabRoom::RequestFunction webRequest;
        juce::String webServer;
        std::shared_ptr<WebEndpoint> webEndpoint;
        std::function<std::unique_ptr<CollabUpnp>()> webUpnpFactory = CollabUpnp::makeMiniUpnp;
        std::array<juce::String, 2> peerRingNames {}; // empty in the one-ring legacy test constructor
    };
    CollabSession (MixEngine&, LiveMixSettings&);
    CollabSession (MixEngine&, LiveMixSettings&, std::unique_ptr<CollabRoom>, std::unique_ptr<CollabUpnp>, Options);
    // Injectable service, UPnP and deadlines for loopback tests.
    CollabSession (CollabAudio&, LiveMixSettings&, std::unique_ptr<CollabRoom>, std::unique_ptr<CollabUpnp>, Options);
    ~CollabSession() override; // finishes leave on the worker before the engine is destroyed

    void createRoom (const juce::String& nick);
    void joinRoom (const juce::String& code, const juce::String& nick);
    void leave(); // immediately off; network cleanup completes on the worker
    void rebuffer();
    void setMonitor (float volume01, bool on);
    void setRoute (CollabAudio::Route, int directFirst);
    void createWebInvite();
    void acceptWebRequest (const juce::String& requestId);
    void rejectWebRequest (const juce::String& requestId);
    void kickWebGuest (contracts::ParticipantId);
    void setPersonMonitorVolume (contracts::ParticipantId, float volume01);
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
    void applyRoom (const CollabRoom::RoomSnapshot&, const std::vector<CollabRoom::Peer>& = {});
    void applyRoster (const RosterSnapshot&);
    void publishPeople();
    void refreshWebActions();
    void startTransport (const PeerInfo&, const std::vector<CollabTransport::Address>& = {}, bool punchGuest = false);
    void switchHostTransport();
    void peerChanged (contracts::ParticipantId, CollabTransport::State);
    void finishGuestLink (CollabTransport::State);
    void startSignaling();
    void startWeb (contracts::WebConnection);
    void endWeb (bool notifyServer);
    void sendWebSignal (uint32_t, const juce::var&);
    void removeWebRequest (const juce::String&);

    CollabAudio& audio;
    LiveMixSettings& settings;
    Options options;
    CollabTransport transport;
    MixEngine* engine = nullptr;
    std::array<ObsMetadata, 2> metadata;
    CollabRoom::RoomSnapshot roomSnapshot;
    RosterSnapshot roster;
    std::vector<CollabTransport::V2Peer> nativePeers;
    std::vector<CollabRoom::MemberState> leftMembers;
    contracts::WebConnection webConnection;
    std::atomic<uint32_t> webGeneration { 0 };
    bool transportV2 = false, switchingTransport = false, webAcceptPending = false;
    unsigned transportSerial = 0;
    uint32_t guestGeneration = 0;
    juce::String acceptingNick;
    std::unique_ptr<Impl> impl;
    std::function<PeerInfo()> peerInfo;
    Snapshot snapshot;
    bool started = false, audioAttached = false, mapped = false, heartbeatPending = false, renewPending = false;
    bool webMapped = false;
    bool punching = false; // hosting with a closed port: the guest is reached by hole punching
    bool hostEverConnected = false;
    bool heartbeatConnected = false, heartbeatStopped = false;
    bool heartbeatDirty = false;
    int heartbeatFailures = 0;
    double joinDeadline = 0, lastHeartbeat = 0, lastRenew = 0, lastNotification = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CollabSession)
};
}
}
