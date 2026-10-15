#pragma once
#include "CollabProtocol.h"
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
        uint64_t sent = 0, received = 0;
        uint32_t wireRate = defaultWireRate;
    };
    CollabTransport();
    ~CollabTransport();
    juce::String open (int port, bool loopbackOnly = false);
    int getLocalPort() const noexcept;
    void startHost (uint64_t session, const PeerInfo& me);
    void startGuest (uint64_t session, const std::vector<Address>& candidates, const PeerInfo& me);
    void expectProbe (const Nonce&);
    bool hasProbe() const noexcept;
    void stop();
    Stats getStats() const;
    State getState() const noexcept;
    double estimatedOneWayMs() const;
    void attachAudio (CollabAudio&); // called by CollabAudio's lifecycle API
    void detachAudio (CollabAudio&);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
