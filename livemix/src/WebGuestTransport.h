#pragma once

#include "CollabContracts.h"
#include <juce_core/juce_core.h>
#include <array>
#include <functional>
#include <memory>

namespace gocue::livemix::collab
{
class WebGuestTransport
{
public:
    enum class State { off, connecting, open, closed, failed };
    struct Options { bool loopbackOnly = false; };
    struct Callbacks
    {
        std::function<void(uint32_t, const juce::String& type, const juce::String& sdp)> onLocalDescription;
        std::function<void(uint32_t, const juce::String& candidate, const juce::String& mid, int mLineIndex)> onLocalCandidate;
        std::function<void(uint32_t)> onGatheringComplete;
        std::function<void(uint32_t, State)> onState;
        std::function<void(uint32_t, const uint8_t*, size_t, uint64_t)> onPacket;
    };
    struct Snapshot
    {
        State state = State::off;
        uint32_t generation = 0;
        uint64_t sent = 0, received = 0, dropped = 0, bufferedBytes = 0;
        bool ordered = false;
        int maxRetransmits = 0;
    };

    WebGuestTransport();
    ~WebGuestTransport();
    void startAcceptedGuest(const contracts::WebConnection&, Options, Callbacks);
    void applyRemoteDescription(uint32_t generation, const juce::String& type, const juce::String& sdp);
    void addRemoteCandidate(uint32_t generation, const juce::String& candidate, const juce::String& mid, int mLineIndex);
    void endRemoteCandidates(uint32_t generation);
    bool sendAudio(uint32_t generation, const void* packet, size_t bytes);
    bool sendControl(uint32_t generation, const void* packet, size_t bytes);
    void close();
    Snapshot snapshot() const;

    // Lifecycle/signaling: one control thread, never a library callback.
    // send*: network producer only, NOT the audio callback. Copies into bounded,
    // preallocated storage; true means queued, not delivered. A private network
    // thread calls libdatachannel::send (including allocations). R4/R8 own the
    // audio-callback -> network-producer SPSC boundary.
    // Callbacks must only enqueue work, never wait for control/HTTP/playout.
    // onPacket bytes live only during the callback; timestamp is monotonic ns.
    // close joins the sender and drains callback invocations before returning.
    // snapshot counters are per accepted connection; sent counts messages handed
    // to SCTP (false from its send means buffered, NOT a reason to retransmit).

private:
    struct Packet
    {
        std::array<uint8_t, contracts::maxPacketBytes> data {};
        size_t bytes = 0;
        uint64_t queuedAt = 0;
    };
    // This policy is also tested with a synthetic clock/buffer amount, so the
    // congestion test does not depend on saturating a localhost SCTP socket.
    struct AudioQueue
    {
        // A host device block arrives here at once: up to 2048 frames (18 packets, 46.4 ms at 44.1 kHz) per downlink
        // stream, plus the 20 ms congestion allowance (8 packets), for each of up to two streams: the host and one
        // forwarded native participant (contract 4, per-person downlink). 10/27: 8 packets / 20 ms per stream dropped
        // the tail of every 1024-frame block (6%, 228 dropouts in 12 s on the web) and more over Wi-Fi, where SCTP
        // still holds the previous burst for a round trip.
        static constexpr size_t blockPackets = (2048 + contracts::packetFrames - 1) / contracts::packetFrames;
        static constexpr size_t capacity = (blockPackets + 8) * contracts::remoteSlots;
        static constexpr size_t packetBytes = contracts::v2HeaderBytes + contracts::stereoPcm24Bytes;
        static constexpr uint64_t maxAgeNs = 70'000'000;   // a whole 2048-frame block (46.4 ms) plus 20 ms
        std::array<Packet, capacity> packets {};
        size_t first = 0, count = 0;
        uint64_t push(const void*, size_t, uint64_t now);
        uint64_t trim(size_t buffered, uint64_t now);
        Packet pop();
    };
    friend class WebGuestTransportTests;
    struct Impl;
    std::unique_ptr<Impl> impl;
    WebGuestTransport(const WebGuestTransport&) = delete;
    WebGuestTransport& operator=(const WebGuestTransport&) = delete;
};
}
