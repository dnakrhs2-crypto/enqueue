#pragma once
#include "CollabProtocol.h"
#include "CollabRoster.h"
#include <functional>
#include <memory>
#include <vector>

namespace gocue::livemix::collab
{
class CollabAudio;
class CollabTransport
{
public:
    enum class State { off, waiting, connecting, connected, disconnected, peerLeft };
    struct Address
    {
        juce::String host;
        int port = 0;
        bool operator== (const Address& other) const { return host == other.host && port == other.port; }
    };
    struct Stats
    {
        State state = State::off;
        PeerInfo peer;
        Address address;
        double rttMs = 0;
        uint64_t sent = 0, received = 0, punched = 0;
        uint32_t wireRate = defaultWireRate;
        uint64_t droppedAudio = 0;
        contracts::Revision rosterRevision = 0;
        bool v2 = false;
    };
    /** This socket's public address as STUN servers (RFC 5389 binding) see it: what a partner behind another
        router must send to. 'consistent' = every answering server saw the same address (not a symmetric NAT). */
    struct Mapping
    {
        Address address;
        int responses = 0;
        bool consistent = false;
        bool usable() const { return responses > 0 && consistent; }
    };
    CollabTransport();
    ~CollabTransport();
    juce::String open (int port, bool loopbackOnly = false);
    int getLocalPort() const noexcept;
    Mapping discoverPublic (const std::vector<Address>& stunServers, int timeoutMs); // blocking; worker thread
    void startHost (uint64_t session, const PeerInfo& me);
    void startGuest (uint64_t session, const std::vector<Address>& candidates, const PeerInfo& me);
    /** Host whose port is closed: knock on the guest's public addresses until the guest's hello comes back
        through the opened path (UDP hole punching), for 'milliseconds' from now. */
    void punch (const std::vector<Address>& targets, int milliseconds);
    void expectProbe (const Nonce&);
    bool hasProbe() const noexcept;
    void stop();
    Stats getStats() const;
    State getState() const noexcept;
    double estimatedOneWayMs() const;
    void attachAudio (CollabAudio&); // called by CollabAudio's lifecycle API
    void detachAudio (CollabAudio&);

    struct V2Peer
    {
        contracts::ParticipantId participantId = 0;
        // The guest reservation's generation on both ends of a native link.
        contracts::ConnectionGeneration generation = 0;
        std::vector<Address> candidates;
    };
    // Called on the transport worker, without its control lock. Do not wait in
    // these callbacks. Views/payloads are valid only for the duration of the call.
    struct V2Callbacks
    {
        std::function<void(const contracts::HeaderV2&, const uint8_t*, uint64_t)> onAudio;
        std::function<void(const RosterSnapshot&)> onRoster;
        std::function<void(contracts::ParticipantId, State)> onPeerState;
    };
    void configureV2 (contracts::ParticipantId self, const RosterSnapshot&,
                      const std::vector<V2Peer>&, V2Callbacks); // before startHost/startGuest
    void updateRoster (const RosterSnapshot&); // host control thread
    void updatePeers (const std::vector<V2Peer>&); // control thread, complete native link list
    // Single web producer. R8 checks the accepted PeerConnection/generation
    // before calling; the queued copy is also tagged with the current generation.
    void receiveWebPacket (const contracts::HeaderV2&, const uint8_t*, uint64_t arrivalNs);
    struct WebLinkSend
    {
        // Nonblocking; false means congested. Control is always serviced first.
        std::function<bool(const uint8_t*, size_t)> audio, control;
    };
    void setWebLink (contracts::ParticipantId webId, contracts::ConnectionGeneration, WebLinkSend);
    Stats getStats (contracts::ParticipantId linkedPeer) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
