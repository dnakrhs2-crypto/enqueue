#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <avrt.h>
#include "CollabTransport.h"
#include "CollabAudio.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>

namespace gocue::livemix::collab
{
namespace c = contracts;
namespace
{
// One producer per input (UDP and WebRTC never share a queue). The consumer
// alone advances read; full inputs drop the arriving datagram, never overwrite
// storage while it is being read. Per-destination output queues below drop oldest.
template <typename T, size_t N> struct InputQueue
{
    std::array<T, N> items {};
    std::atomic<uint64_t> read { 0 }, write { 0 };
    bool push (const T& item) noexcept
    {
        const auto w = write.load (std::memory_order_relaxed);
        if (w - read.load (std::memory_order_acquire) == N) return false;
        items[w % N] = item; write.store (w + 1, std::memory_order_release); return true;
    }
    bool pop (T& item) noexcept
    {
        const auto r = read.load (std::memory_order_relaxed);
        if (r == write.load (std::memory_order_acquire)) return false;
        item = items[r % N]; read.store (r + 1, std::memory_order_release); return true;
    }
    void reset() noexcept { read.store (0); write.store (0); } // joined producers only
};
uint64_t nowNs() noexcept
{
    return (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct ProAudio
{
    ProAudio()
    {
        DWORD index = 0; handle = AvSetMmThreadCharacteristicsW (L"Pro Audio", &index);
        if (handle != nullptr) AvSetMmThreadPriority (handle, AVRT_PRIORITY_HIGH);
        else SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
    ~ProAudio() { if (handle != nullptr) AvRevertMmThreadCharacteristics (handle); }
    HANDLE handle = nullptr;
};
bool resolve (const CollabTransport::Address& address, sockaddr_in& result)
{
    result = {}; result.sin_family = AF_INET; result.sin_port = htons ((u_short) address.port);
    return address.port > 0 && address.port <= 65535 && InetPtonA (AF_INET, address.host.toRawUTF8(), &result.sin_addr) == 1;
}
CollabTransport::Address describe (const sockaddr_in& addr)
{
    char host[INET_ADDRSTRLEN] {};
    InetNtopA (AF_INET, const_cast<in_addr*> (&addr.sin_addr), host, sizeof (host));
    return { host, ntohs (addr.sin_port) };
}
bool sameAddress (const sockaddr_in& a, const sockaddr_in& b)
{
    return a.sin_port == b.sin_port && a.sin_addr.s_addr == b.sin_addr.s_addr;
}
// A STUN server is named (stun.l.google.com) or numeric; IPv4 only, like the rest of the transport.
bool resolveName (const CollabTransport::Address& address, sockaddr_in& result)
{
    if (resolve (address, result)) return true;
    if (address.port <= 0 || address.port > 65535 || address.host.isEmpty()) return false;
    addrinfo hints {}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (getaddrinfo (address.host.toRawUTF8(), nullptr, &hints, &found) != 0 || found == nullptr) return false;
    result = *reinterpret_cast<const sockaddr_in*> (found->ai_addr); result.sin_port = htons ((u_short) address.port);
    freeaddrinfo (found);
    return true;
}
constexpr uint8_t stunCookie[4] { 0x21, 0x12, 0xA4, 0x42 };
bool isStunSuccess (const uint8_t* d, size_t bytes)
{
    return bytes >= 20 && d[0] == 0x01 && d[1] == 0x01 && std::equal (d + 4, d + 8, stunCookie);
}
}

struct CollabTransport::Impl
{
    Impl()
    {
        WSADATA data {}; winsock = WSAStartup (MAKEWORD (2, 2), &data) == 0;
        wake = CreateEventW (nullptr, FALSE, FALSE, nullptr);
    }
    ~Impl() { if (wake != nullptr) CloseHandle (wake); if (winsock) WSACleanup(); }
    struct AudioAccess
    {
        explicit AudioAccess (const Impl& i) : impl (i)
        {
            impl.audioReaders.fetch_add (1); audio = impl.audio.load();
            if (audio == nullptr) impl.audioReaders.fetch_sub (1);
        }
        ~AudioAccess() { if (audio != nullptr) impl.audioReaders.fetch_sub (1); }
        const Impl& impl;
        CollabAudio* audio;
    };

    bool winsock = false;
    SOCKET socket = INVALID_SOCKET;
    HANDLE wake = nullptr;
    std::atomic<bool> running { false }, probeSeen { false };
    std::atomic<int> port { 0 };
    std::atomic<State> state { State::off };
    std::atomic<uint64_t> sent { 0 }, received { 0 };
    std::atomic<uint32_t> sequence { 0 };
    std::atomic<CollabAudio*> audio { nullptr };
    mutable std::atomic<unsigned> audioReaders { 0 };
    std::thread receiver, transmitter;
    mutable juce::CriticalSection control; // network/control threads only
    bool host = false, roleStarted = false, havePeer = false, probeExpected = false;
    uint64_t session = 0, lastReceived = 0, lastHello = 0, lastPing = 0;
    uint32_t sender = 1, peerSender = 0;
    PeerInfo me, peer;
    Nonce nonce {};
    struct RecentProbe { Nonce nonce {}; uint64_t receivedAt = 0; };
    std::array<RecentProbe, 8> recentProbes {};
    size_t nextProbe = 0;
    sockaddr_in peerAddress {};
    std::vector<sockaddr_in> candidates;
    std::array<double, 5> rtts {};
    size_t rttCount = 0, rttIndex = 0;
    double rttMs = 0;
    bool loopback = false;
    std::vector<sockaddr_in> punchTargets;
    uint64_t punchUntil = 0, lastPunch = 0;
    std::atomic<uint64_t> punched { 0 };
    struct StunQuery { std::array<uint8_t, 12> id {}; bool answered = false; sockaddr_in mapped {}; };
    std::vector<StunQuery> stunQueries;

    struct Datagram
    {
        std::array<uint8_t, maxDatagram> data {};
        size_t bytes = 0;
        sockaddr_in address {};
        uint64_t arrival = 0;
        c::ConnectionGeneration generation = 0;
    };
    InputQueue<Datagram, 128> udpInput, webInput;
    std::atomic<bool> useV2 { false };
    std::atomic<c::ConnectionGeneration> webGeneration { 0 };
    std::atomic<unsigned> webReaders { 0 };
    std::vector<std::function<void()>> commands;
    std::atomic<bool> commandsPending { false };
    // All fields below, including output rings, are owned by the single sender.
    // Lifecycle changes are commands, not locks around packet validation/relay.
    struct Link
    {
        c::ParticipantId id = 0;
        c::ConnectionGeneration generation = 0;
        bool enabled = false, web = false, bound = false, offered = false, offerAccepted = false;
        bool txV2 = false, rxV2 = false;
        bool rosterAckPending = false;
        State state = State::off;
        sockaddr_in address {};
        std::vector<sockaddr_in> candidates;
        PeerInfo info;
        uint64_t started = 0, lastReceived = 0, lastHello = 0, lastOffer = 0, lastRoster = 0, lastPing = 0;
        uint32_t offerSeq = 0, rosterSeq = 0;
        c::Revision offerRevision = 0;
        OfferV2 offer;
        RosterSnapshot acknowledged, pendingRoster;
        std::array<Datagram, 64> output {};
        size_t outputRead = 0, outputCount = 0;
        WebLinkSend webSend;
    };
    std::array<Link, 3> links; // two native sockets and one optional web link
    std::array<Stats, 3> publishedLinks; // metadata, under control, never read by the audio path
    std::array<c::ParticipantId, 3> publishedIds {};
    std::array<std::atomic<uint64_t>, 3> linkSent {}, linkReceived {}, linkDropped {}, linkPunched {};
    std::array<std::atomic<double>, 3> linkRtt {};
    std::atomic<double> v2OutputLatencyMs { 0 };
    RosterSnapshot roster;
    c::ParticipantId selfId = 0;
    V2Callbacks callbacks;
    bool starStarted = false, starHost = false, rosterInstalled = false;
    uint32_t controlSeq = 0;
    PeerInfo starMe;
    uint64_t localBase = 0, lastLocalFrame = 0;
    bool localBasePending = true, localFrameSeen = false;
    c::StreamEpoch localEpoch = 1, restartEpoch = 0;
    StreamRestartV2 restart;
    uint32_t restartSeq = 0;
    uint64_t lastRestart = 0;
    bool sourceRestart = false;
    CollabSender::Sink localSink;

    void post (std::function<void()> command)
    {
        const juce::ScopedLock lock (control);
        commands.push_back (std::move (command)); commandsPending.store (true); SetEvent (wake);
    }
    void runCommands()
    {
        if (! commandsPending.load()) return;
        std::vector<std::function<void()>> work;
        {
            const juce::ScopedLock lock (control);
            work.swap (commands); commandsPending.store (false);
        }
        for (auto& command : work) command();
    }
    size_t indexOf (const Link& link) const noexcept { return (size_t) (&link - links.data()); }
    Link* linkFor (c::ParticipantId id) noexcept
    {
        for (auto& link : links) if (link.enabled && link.id == id) return &link;
        return nullptr;
    }
    void publishLink (Link& link)
    {
        const auto n = indexOf (link);
        const juce::ScopedLock lock (control);
        publishedIds[n] = link.id;
        auto& s = publishedLinks[n]; s.state = link.state; s.peer = link.info;
        s.address = link.bound && ! link.web ? describe (link.address) : Address {};
        s.wireRate = roster.wireRate; s.v2 = link.txV2; s.rosterRevision = link.acknowledged.revision;
    }
    void overallState()
    {
        bool connected = false, connecting = false;
        for (const auto& l : links)
        {
            connected |= l.enabled && l.state == State::connected;
            connecting |= l.enabled && l.state == State::connecting;
        }
        const auto next = connected ? State::connected : connecting ? State::connecting : starHost ? State::waiting : State::disconnected;
        if (state.load() != next) changeState (next);
    }
    void linkState (Link& l, State value)
    {
        if (l.state == value) return;
        l.state = value; publishLink (l); overallState();
        if (callbacks.onPeerState) callbacks.onPeerState (l.id, value);
    }
    void setPeers (const std::vector<V2Peer>& peers)
    {
        for (size_t n = 0; n < 2; ++n)
        {
            auto& l = links[n];
            if (l.enabled && std::none_of (peers.begin(), peers.end(), [&] (const auto& p)
                { return p.participantId == l.id && p.generation == l.generation; }))
            {
                l.enabled = false; l.outputCount = 0; linkState (l, State::peerLeft);
            }
        }
        for (const auto& p : peers)
        {
            const auto* member = findMember (roster, p.participantId);
            if (p.participantId == selfId || p.generation == 0 || member == nullptr || member->kind != c::ParticipantKind::native
                || (selfId != c::hostParticipant && p.participantId != c::hostParticipant)) continue;
            auto* existing = linkFor (p.participantId);
            if (existing == nullptr)
            {
                for (size_t n = 0; n < 2; ++n) if (! links[n].enabled) { existing = &links[n]; break; }
                if (existing == nullptr) continue;
                *existing = Link {};
                existing->id = p.participantId; existing->generation = p.generation; existing->enabled = true;
                existing->started = nowNs();
                const auto n = indexOf (*existing);
                linkSent[n] = 0; linkReceived[n] = 0; linkDropped[n] = 0; linkPunched[n] = 0; linkRtt[n] = 0;
                existing->info = { member->nickname, member->appVersion, member->inputLatencyUs / 1000.0,
                                   member->outputLatencyUs / 1000.0, member->deviceRate, roster.wireRate };
                linkState (*existing, State::connecting);
            }
            existing->candidates.clear();
            for (const auto& a : p.candidates)
            {
                sockaddr_in address {};
                if (resolve (a, address) && existing->candidates.size() < 16) existing->candidates.push_back (address);
            }
        }
    }
    c::HeaderV2 controlHeader (c::Kind kind) const noexcept
    {
        c::HeaderV2 h; h.kind = kind; h.session = roster.session; h.origin = h.linkSender = selfId;
        h.rosterRevision = roster.revision; return h;
    }
    bool sendBytes (Link& l, const Datagram& d, bool audioPacket)
    {
        bool ok = false;
        if (l.web)
        {
            const auto& fn = audioPacket ? l.webSend.audio : l.webSend.control;
            ok = fn && fn (d.data.data(), d.bytes);
        }
        else if (l.bound)
            ok = sendto (socket, (const char*) d.data.data(), (int) d.bytes, 0,
                         (const sockaddr*) &l.address, sizeof (l.address)) == (int) d.bytes;
        if (ok) { ++sent; ++linkSent[indexOf (l)]; }
        return ok;
    }
    void sendV2Control (Link& l, const c::HeaderV2& h, const uint8_t* payload = nullptr)
    {
        Datagram d; d.bytes = writePacketV2 (d.data.data(), d.data.size(), h, payload);
        if (d.bytes != 0) sendBytes (l, d, false);
    }
    void sendV1Control (Link& l, Kind kind, const sockaddr_in& to, uint64_t stamp = 0, uint32_t seq = 0)
    {
        Header h; h.kind = kind; h.session = roster.session; h.sender = selfId; h.seq = seq != 0 ? seq : ++controlSeq;
        std::array<uint8_t, 128> payload {};
        if (kind == Kind::hello || kind == Kind::welcome) h.payloadBytes = (uint16_t) writePeerInfo (payload.data(), starMe);
        if (kind == Kind::ping || kind == Kind::pong) { h.payloadBytes = 8; writeTimestamp (payload.data(), stamp); }
        send (h, payload.data(), to); ++linkSent[indexOf (l)];
    }
    void sendOffer (Link& l, uint64_t now)
    {
        if (! l.offered)
        {
            const auto* mine = findMember (roster, selfId); const auto* theirs = findMember (roster, l.id);
            if (mine == nullptr || theirs == nullptr) return;
            l.offer = { mine->capabilities & theirs->capabilities & c::allCapabilities, roster.wireRate, l.id, l.generation };
            l.offerSeq = ++controlSeq; l.offerRevision = roster.revision; l.offered = true;
        }
        auto h = controlHeader (c::Kind::offer); h.seq = l.offerSeq; h.rosterRevision = l.offerRevision; h.payloadBytes = 16;
        std::array<uint8_t, 16> data {}; writeOfferV2 (data.data(), data.size(), l.offer);
        sendV2Control (l, h, data.data()); l.lastOffer = now;
    }
    bool rosterReady (const RosterSnapshot& value) const
    {
        return ! callbacks.isRosterInstalled || callbacks.isRosterInstalled (value);
    }
    void acknowledgeInstalledRoster (Link& l)
    {
        if (! l.rosterAckPending || ! rosterReady (l.pendingRoster)) return;
        auto ack = controlHeader (c::Kind::rosterAck);
        ack.seq = l.rosterSeq; ack.rosterRevision = l.pendingRoster.revision;
        sendV2Control (l, ack);
        l.acknowledged = l.pendingRoster; l.rosterAckPending = false;
        l.txV2 = l.rxV2 = true; publishLink (l); linkState (l, State::connected);
    }
    void sendRoster (Link& l, uint64_t now)
    {
        // A guest can start sending at the advertised revision as soon as it ACKs.
        // Keep every existing link on its last ACK until our own audio table is installed too.
        if (! rosterReady (roster)) return;
        if (l.pendingRoster.revision != roster.revision)
        {
            l.pendingRoster = roster; l.rosterSeq = ++controlSeq;
        }
        std::array<uint8_t, 674> data {};
        auto h = controlHeader (c::Kind::roster); h.seq = l.rosterSeq;
        h.payloadBytes = (uint16_t) writeRoster (data.data(), data.size(), l.pendingRoster);
        sendV2Control (l, h, data.data()); l.lastRoster = now;
    }
    bool currentAudio (const c::HeaderV2& h) const noexcept
    {
        if (h.origin == selfId || h.rosterRevision > roster.revision) return false;
        const auto* m = findMember (roster, h.origin);
        return m != nullptr && m->streamEpoch == h.streamEpoch
            && h.flags == (m->kind == c::ParticipantKind::web ? c::Flags::monoPcm16 : c::Flags::stereoPcm24);
    }
    bool canSend (const Link& l, const c::HeaderV2& h) const noexcept
    {
        if (! l.enabled || ! l.txV2 || l.id == h.origin || (l.web && h.flags != c::Flags::stereoPcm24)) return false;
        const auto* current = findMember (roster, h.origin);
        const auto* acked = findMember (l.acknowledged, h.origin);
        return current != nullptr && acked != nullptr && current->streamEpoch == h.streamEpoch
            && acked->streamEpoch == h.streamEpoch && acked->kind == current->kind;
    }
    size_t outputLimit (const Link& l) const noexcept
    {
        if (! l.web) return l.output.size();
        size_t streams = 0;
        for (size_t n = 0; n < roster.memberCount; ++n)
        {
            const auto& m = roster.members[n]; const auto* acked = findMember (l.acknowledged, m.participantId);
            if (m.kind == c::ParticipantKind::native && acked != nullptr && acked->streamEpoch == m.streamEpoch) ++streams;
        }
        // Every downward packet is 768 bytes: a whole device block plus 20 ms per stream (contracts::webBacklogPackets).
        return std::max (size_t (1), streams * c::webBacklogPackets);
    }
    static_assert (c::webBacklogPackets * c::remoteSlots <= std::tuple_size_v<decltype (Link::output)>);
    void queueAudio (Link& l, const c::HeaderV2& original, const uint8_t* payload, uint64_t now)
    {
        if (! canSend (l, original)) return;
        auto h = original; h.linkSender = selfId; h.rosterRevision = l.acknowledged.revision;
        const auto limit = outputLimit (l);
        while (l.outputCount >= limit)
        {
            l.outputRead = (l.outputRead + 1) % l.output.size(); --l.outputCount; ++linkDropped[indexOf (l)];
        }
        auto& d = l.output[(l.outputRead + l.outputCount) % l.output.size()];
        d.bytes = writePacketV2 (d.data.data(), d.data.size(), h, payload); d.arrival = now;
        if (d.bytes != 0) ++l.outputCount;
    }
    void flushAudio (uint64_t now)
    {
        for (auto& l : links)
        {
            while (l.enabled && l.outputCount != 0)
            {
                const auto& d = l.output[l.outputRead];
                c::HeaderV2 h; const uint8_t* payload = nullptr;
                const bool obsolete = ! readPacketV2 (d.data.data(), d.bytes, h, payload) || ! canSend (l, h)
                    || (l.web && now > d.arrival && now - d.arrival > c::webBacklogAgeNs);
                if (! obsolete && ! sendBytes (l, d, true)) break; // only this link is congested
                if (obsolete) ++linkDropped[indexOf (l)];
                l.outputRead = (l.outputRead + 1) % l.output.size(); --l.outputCount;
            }
        }
    }
    void deliver (const c::HeaderV2& h, const uint8_t* payload, uint64_t arrival, const Header* v1 = nullptr)
    {
        if (callbacks.onAudio) callbacks.onAudio (h, payload, arrival);
        else if (v1 != nullptr)
        {
            const AudioAccess access (*this);
            if (access.audio != nullptr) access.audio->receive (*v1, payload, arrival);
        }
    }
    void notifyRoster()
    {
        if (const auto* mine = findMember (roster, selfId)) v2OutputLatencyMs.store (mine->outputLatencyUs / 1000.0);
        if (callbacks.onRoster) callbacks.onRoster (roster);
    }
    void installHostRoster (const RosterSnapshot& next)
    {
        if (! starHost || next.revision <= roster.revision || ! validateRoster (next, selfId, &roster)) return;
        roster = next;
        for (auto& l : links)
        {
            if (l.enabled && findMember (roster, l.id) == nullptr)
            {
                l.enabled = false; l.outputCount = 0;
                if (l.web) webGeneration.store (0);
                linkState (l, State::peerLeft);
            }
            l.lastRoster = 0;
        }
        const auto* mine = findMember (roster, selfId);
        if (mine != nullptr && mine->streamEpoch != localEpoch)
        {
            localEpoch = mine->streamEpoch; localBasePending = true;
        }
        notifyRoster();
    }
    void leaveLink (Link& l, State why)
    {
        l.enabled = false; l.outputCount = 0;
        if (l.web) webGeneration.store (0);
        linkState (l, why);
        if (starHost)
        {
            auto next = roster;
            size_t count = 0;
            for (size_t n = 0; n < roster.memberCount; ++n)
                if (roster.members[n].participantId != l.id) next.members[count++] = roster.members[n];
            if (count != roster.memberCount && next.revision != UINT32_MAX)
            {
                next.memberCount = count; ++next.revision; installHostRoster (next);
            }
        }
        else if (why == State::peerLeft) changeState (State::peerLeft);
    }
    void receiveV1 (const Datagram& d)
    {
        Header h; const uint8_t* payload = nullptr;
        if (! readPacket (d.data.data(), d.bytes, h, payload) || h.session != roster.session || h.sender == selfId) return;
        auto* lp = linkFor (h.sender);
        if (lp == nullptr || lp->web) return;
        auto& l = *lp;
        if (l.bound && ! sameAddress (l.address, d.address)) return;
        if (h.kind == Kind::punch && ! starHost && ! l.bound)
        {
            if (l.candidates.size() < 16 && std::none_of (l.candidates.begin(), l.candidates.end(),
                [&] (const auto& a) { return sameAddress (a, d.address); })) l.candidates.push_back (d.address);
            sendV1Control (l, Kind::hello, d.address); return;
        }
        if ((starHost && h.kind == Kind::hello) || (! starHost && h.kind == Kind::welcome))
        {
            if (! starHost && ! l.bound && std::none_of (l.candidates.begin(), l.candidates.end(),
                [&] (const auto& a) { return sameAddress (a, d.address); })) return;
            PeerInfo info;
            if (! readPeerInfo (payload, h.payloadBytes, info) || (! starHost && info.wireRate != roster.wireRate)) return;
            l.info = info; l.address = d.address; l.bound = true; l.lastReceived = d.arrival;
            ++received; ++linkReceived[indexOf (l)]; publishLink (l);
            // The completed v1 handshake remains playable while v2 control is
            // retried. Negotiation still has its separate ten-second deadline.
            linkState (l, State::connected);
            if (starHost)
            {
                sendV1Control (l, Kind::welcome, l.address);
                if (! l.offerAccepted) sendOffer (l, nowNs());
            }
            return;
        }
        if (! l.bound) return;
        if (h.kind == Kind::audio)
        {
            if (l.rxV2) return;
            const auto* m = findMember (roster, l.id);
            if (m == nullptr) return;
            auto v2 = controlHeader (c::Kind::audio); v2.origin = v2.linkSender = l.id;
            v2.flags = c::Flags::stereoPcm24; v2.seq = h.seq; v2.firstFrame = h.firstFrame;
            v2.streamEpoch = m->streamEpoch; v2.frames = h.frames; v2.payloadBytes = h.payloadBytes;
            l.lastReceived = d.arrival; ++received; ++linkReceived[indexOf (l)];
            deliver (v2, payload, d.arrival, &h); return;
        }
        l.lastReceived = d.arrival; ++received; ++linkReceived[indexOf (l)];
        if (h.kind == Kind::bye) leaveLink (l, State::peerLeft);
        else if (h.kind == Kind::ping) sendV1Control (l, Kind::pong, l.address, readTimestamp (payload), h.seq);
        else if (h.kind == Kind::pong)
        {
            const auto stamp = readTimestamp (payload);
            if (stamp == l.lastPing && stamp <= d.arrival) linkRtt[indexOf (l)] = (d.arrival - stamp) / 1.0e6;
        }
    }
    void receiveV2 (const Datagram& d, bool fromWeb)
    {
        c::HeaderV2 h; const uint8_t* payload = nullptr;
        if (! readPacketV2 (d.data.data(), d.bytes, h, payload) || h.session != roster.session) return;
        auto* lp = linkFor (starHost ? h.linkSender : c::hostParticipant);
        if (lp == nullptr || lp->web != fromWeb) return;
        auto& l = *lp;
        if ((fromWeb && (! starHost || d.generation != l.generation || d.generation == 0))
            || (! fromWeb && (! l.bound || ! sameAddress (l.address, d.address)))
            || h.linkSender != l.id || (starHost && h.origin != l.id)) return;
        if (h.kind == c::Kind::audio)
        {
            if (! l.rxV2 || ! currentAudio (h) || (fromWeb && h.flags != c::Flags::monoPcm16)) return;
            if (starHost)
            {
                const auto* acknowledged = findMember (l.acknowledged, h.origin);
                if (acknowledged == nullptr || acknowledged->streamEpoch != h.streamEpoch) return;
            }
            l.lastReceived = d.arrival; ++received; ++linkReceived[indexOf (l)];
            // Relay the validated source bytes BEFORE entering any playout callback.
            if (starHost)
            {
                for (auto& destination : links) queueAudio (destination, h, payload, d.arrival);
                flushAudio (nowNs());
            }
            deliver (h, payload, d.arrival); return;
        }
        if (h.origin != l.id) return;
        if (h.kind == c::Kind::offer || h.kind == c::Kind::offerAck)
        {
            if (fromWeb) return;
            OfferV2 offer;
            const auto* mine = findMember (roster, selfId); const auto* theirs = findMember (roster, l.id);
            if (! readOfferV2 (payload, h.payloadBytes, offer) || mine == nullptr || theirs == nullptr
                || offer.wireRate != roster.wireRate || offer.peerParticipantId != selfId
                || offer.connectionGeneration != l.generation
                || offer.capabilities != (mine->capabilities & theirs->capabilities & c::allCapabilities)
                || (offer.capabilities & c::requiredNativeV2Capabilities) != c::requiredNativeV2Capabilities) return;
            if (! starHost && h.kind == c::Kind::offer)
            {
                if (l.offered && (h.seq != l.offerSeq || h.rosterRevision != l.offerRevision)) return;
                l.offered = l.offerAccepted = true; l.offerSeq = h.seq; l.offerRevision = h.rosterRevision; l.offer = offer;
                auto ack = controlHeader (c::Kind::offerAck); ack.seq = h.seq; ack.rosterRevision = h.rosterRevision; ack.payloadBytes = 16;
                offer.peerParticipantId = l.id;
                std::array<uint8_t, 16> data {}; writeOfferV2 (data.data(), data.size(), offer);
                sendV2Control (l, ack, data.data());
            }
            else if (starHost && h.kind == c::Kind::offerAck && l.offered && h.seq == l.offerSeq && h.rosterRevision == l.offerRevision)
            {
                l.offerAccepted = true; sendRoster (l, nowNs());
            }
            else return;
        }
        else if (h.kind == c::Kind::roster)
        {
            if (starHost || ! l.offerAccepted || h.rosterRevision < roster.revision) return;
            RosterSnapshot next;
            if (! readRoster (payload, h.payloadBytes, h.session, h.rosterRevision, selfId, next)
                || next.mode != c::RoomMode::v2 || ! validateRoster (next, selfId, &roster)) return;
            if (! rosterInstalled || next.revision > roster.revision)
            {
                roster = std::move (next); rosterInstalled = true;
                const auto epoch = findMember (roster, selfId)->streamEpoch;
                if (epoch != localEpoch) { localEpoch = epoch; localBasePending = true; }
                if (restartEpoch == epoch) restartEpoch = 0;
                notifyRoster();
            }
            l.pendingRoster = roster; l.rosterSeq = h.seq; l.rosterAckPending = true;
            acknowledgeInstalledRoster (l);
        }
        else if (h.kind == c::Kind::rosterAck)
        {
            if (! starHost || ! l.offerAccepted || h.rosterRevision != l.pendingRoster.revision || h.seq != l.rosterSeq) return;
            l.acknowledged = l.pendingRoster; l.txV2 = l.rxV2 = true; publishLink (l); linkState (l, State::connected);
        }
        else if (h.kind == c::Kind::streamRestart)
        {
            if (! starHost || ! l.rxV2) return;
            const auto* m = findMember (roster, l.id); StreamRestartV2 request;
            if (m == nullptr || ! readStreamRestartV2 (payload, h.payloadBytes, request)) return;
            if (h.streamEpoch == m->streamEpoch)
            {
                if (request.deviceRate != m->deviceRate || request.inputLatencyUs != m->inputLatencyUs
                    || request.outputLatencyUs != m->outputLatencyUs) return;
                sendRoster (l, nowNs());
            }
            else
            {
                if (m->streamEpoch == UINT32_MAX || h.streamEpoch != m->streamEpoch + 1 || roster.revision == UINT32_MAX) return;
                auto next = roster; ++next.revision;
                auto& changed = next.members[(size_t) (m - roster.members.data())];
                changed.streamEpoch = h.streamEpoch; changed.deviceRate = request.deviceRate;
                changed.inputLatencyUs = request.inputLatencyUs; changed.outputLatencyUs = request.outputLatencyUs;
                installHostRoster (next); sendRoster (l, nowNs());
            }
        }
        else if (h.kind == c::Kind::bye) { leaveLink (l, State::peerLeft); return; }
        else if (h.kind == c::Kind::ping)
        {
            auto pong = controlHeader (c::Kind::pong); pong.seq = h.seq; pong.payloadBytes = 8;
            pong.rosterRevision = h.rosterRevision; sendV2Control (l, pong, payload);
        }
        else if (h.kind == c::Kind::pong)
        {
            const auto stamp = readTimestamp (payload);
            if (stamp == l.lastPing && stamp <= d.arrival) linkRtt[indexOf (l)] = (d.arrival - stamp) / 1.0e6;
        }
        else return;
        l.lastReceived = d.arrival; ++received; ++linkReceived[indexOf (l)];
    }
    void beginLocalRestart (const CollabAudio& localAudio)
    {
        if (restartEpoch != 0 || localEpoch == UINT32_MAX || roster.revision == UINT32_MAX) return;
        const auto* mine = findMember (roster, selfId);
        if (mine == nullptr) return;
        const auto deviceRate = (uint32_t) localAudio.getDeviceRate();
        restart = { deviceRate, mine->inputLatencyUs, mine->outputLatencyUs };
        localAudio.getDeviceLatencyUs (restart.inputLatencyUs, restart.outputLatencyUs);
        starMe.deviceRate = deviceRate;
        if (starHost)
        {
            auto next = roster; ++next.revision;
            auto& m = next.members[(size_t) (mine - roster.members.data())];
            ++m.streamEpoch; m.deviceRate = deviceRate;
            m.inputLatencyUs = restart.inputLatencyUs; m.outputLatencyUs = restart.outputLatencyUs;
            installHostRoster (next);
        }
        else
        {
            restartEpoch = ++localEpoch; restartSeq = ++controlSeq; lastRestart = 0; localBasePending = true;
        }
        sourceRestart = false;
    }
    void sendLocal (const Header& source, const uint8_t* payload)
    {
        if (localFrameSeen && source.firstFrame < lastLocalFrame) { sourceRestart = true; localBasePending = true; }
        lastLocalFrame = source.firstFrame; localFrameSeen = true;
        if (sourceRestart) return; // the control phase publishes the new epoch first
        if (localBasePending) { localBase = source.firstFrame; localBasePending = false; }
        auto h = controlHeader (c::Kind::audio); h.flags = c::Flags::stereoPcm24;
        h.frames = source.frames; h.payloadBytes = source.payloadBytes; h.streamEpoch = localEpoch;
        h.firstFrame = source.firstFrame - localBase; h.seq = (uint32_t) (h.firstFrame / framesPerPacket);
        if (restartEpoch != 0) return; // clock still advances while awaiting the roster
        const auto now = nowNs();
        for (auto& l : links)
        {
            if (! l.enabled) continue;
            if (l.txV2) queueAudio (l, h, payload, now);
            else if (! l.web && l.bound)
            {
                auto old = source; old.session = roster.session; old.sender = selfId; old.seq = h.seq;
                send (old, payload, l.address); ++linkSent[indexOf (l)];
            }
        }
    }
    void starTick()
    {
        if (! starStarted) return;
        const auto now = nowNs();
        const AudioAccess access (*this);
        if (access.audio != nullptr && (sourceRestart || (uint32_t) access.audio->getDeviceRate() != starMe.deviceRate))
            beginLocalRestart (*access.audio);
        for (auto& l : links)
        {
            if (! l.enabled) continue;
            if (! starHost) acknowledgeInstalledRoster (l);
            if ((! l.txV2 && now - l.started >= 10000000000ULL)
                || (l.txV2 && l.lastReceived != 0 && now - l.lastReceived >= 3000000000ULL))
            { leaveLink (l, State::disconnected); continue; }
            if (! l.web && ! l.bound && now - l.lastHello >= 100000000ULL)
            {
                l.lastHello = now;
                for (const auto& address : l.candidates)
                {
                    sendV1Control (l, starHost ? Kind::punch : Kind::hello, address);
                    if (starHost) { ++punched; ++linkPunched[indexOf (l)]; }
                }
            }
            if (starHost && (l.bound || l.web))
            {
                if (! l.offerAccepted && now - l.lastOffer >= c::controlRetryMs * 1000000ULL) sendOffer (l, now);
                if (l.offerAccepted && l.acknowledged.revision != roster.revision
                    && now - l.lastRoster >= c::controlRetryMs * 1000000ULL) sendRoster (l, now);
            }
            if (! starHost && restartEpoch != 0 && l.txV2 && now - lastRestart >= c::controlRetryMs * 1000000ULL)
            {
                auto h = controlHeader (c::Kind::streamRestart); h.streamEpoch = restartEpoch; h.seq = restartSeq; h.payloadBytes = 12;
                std::array<uint8_t, 12> data {}; writeStreamRestartV2 (data.data(), data.size(), restart);
                sendV2Control (l, h, data.data()); lastRestart = now;
            }
            if ((l.bound || l.web) && now - l.lastPing >= 1000000000ULL)
            {
                l.lastPing = now;
                if (l.txV2 || l.web)
                {
                    auto h = controlHeader (c::Kind::ping); h.seq = ++controlSeq; h.payloadBytes = 8;
                    std::array<uint8_t, 8> data {}; writeTimestamp (data.data(), now); sendV2Control (l, h, data.data());
                }
                else sendV1Control (l, Kind::ping, l.address, now);
            }
        }
        Datagram d;
        // Bounded work per ingress keeps one noisy source from starving the other.
        for (size_t n = 0; n < 128 && udpInput.pop (d); ++n)
        {
            if (d.bytes >= 5 && d.data[4] == 1) receiveV1 (d); else receiveV2 (d, false);
        }
        for (size_t n = 0; n < 128 && webInput.pop (d); ++n) receiveV2 (d, true);
        if (access.audio != nullptr) access.audio->drain (localSink); // one drain, then fan out
        flushAudio (nowNs());
    }
    void stopStar()
    {
        for (auto& l : links)
            if (l.enabled && (l.bound || l.web))
                for (int n = 0; n < 3; ++n)
                {
                    if (l.txV2 || l.web) sendV2Control (l, controlHeader (c::Kind::bye));
                    else sendV1Control (l, Kind::bye, l.address);
                }
        webGeneration.store (0); starStarted = false;
    }

    void takeStun (const uint8_t* d, size_t bytes)
    {
        auto query = std::find_if (stunQueries.begin(), stunQueries.end(), [d] (const auto& q) { return std::equal (q.id.begin(), q.id.end(), d + 8); });
        if (query == stunQueries.end() || query->answered) return;
        const size_t end = std::min (bytes, (size_t) 20 + ((size_t) d[2] << 8 | d[3]));
        for (size_t i = 20; i + 4 <= end;)
        {
            const int type = d[i] << 8 | d[i + 1], length = d[i + 2] << 8 | d[i + 3];
            if (i + 4 + (size_t) length > end) return;
            // XOR-MAPPED-ADDRESS, or the old MAPPED-ADDRESS; IPv4 (family 1) only
            if ((type == 0x0020 || type == 0x0001) && length >= 8 && d[i + 5] == 0x01)
            {
                const bool xored = type == 0x0020;
                sockaddr_in mapped {}; mapped.sin_family = AF_INET;
                const int port = (d[i + 6] << 8 | d[i + 7]) ^ (xored ? 0x2112 : 0);
                mapped.sin_port = htons ((u_short) port);
                auto* ip = reinterpret_cast<uint8_t*> (&mapped.sin_addr);
                for (int n = 0; n < 4; ++n) ip[n] = (uint8_t) (d[i + 8 + n] ^ (xored ? stunCookie[n] : 0));
                query->mapped = mapped; query->answered = true;
                if (type == 0x0020) return;
            }
            i += 4 + (size_t) ((length + 3) & ~3);
        }
    }

    void changeState (State value)
    {
        state.store (value);
        const AudioAccess access (*this);
        if (access.audio != nullptr) access.audio->setConnected (value == State::connected);
    }
    void send (Header h, const uint8_t* payload, const sockaddr_in& address)
    {
        std::array<uint8_t, maxDatagram> packet;
        const auto size = writePacket (packet.data(), packet.size(), h, payload);
        if (size > 0 && sendto (socket, reinterpret_cast<const char*> (packet.data()), (int) size, 0,
                              reinterpret_cast<const sockaddr*> (&address), sizeof (address)) == (int) size) sent.fetch_add (1);
    }
    void sendControl (Kind kind, const sockaddr_in& to, uint64_t stamp = 0)
    {
        Header h; h.kind = kind; h.session = session; h.sender = sender; h.seq = sequence.fetch_add (1);
        std::array<uint8_t, 128> payload {};
        if (kind == Kind::hello || kind == Kind::welcome)
        {
            const AudioAccess access (*this);
            if (access.audio != nullptr) me.deviceRate = (uint32_t) access.audio->getDeviceRate();
            h.payloadBytes = (uint16_t) writePeerInfo (payload.data(), me);
        }
        if (kind == Kind::ping || kind == Kind::pong) { h.payloadBytes = 8; writeTimestamp (payload.data(), stamp); }
        send (h, payload.data(), to);
    }
    void acceptPeer (const Header& h, const PeerInfo& info, const sockaddr_in& from, uint64_t now)
    {
        if (! havePeer || peerSender != h.sender)
        {
            changeState (State::waiting); // resets the attached playout before accepting a replacement
            rttCount = rttIndex = 0; rttMs = 0; lastPing = 0;
        }
        peerSender = h.sender; peer = info; peerAddress = from; havePeer = true; lastReceived = now;
        changeState (State::connected);
    }
    void receiveLoop()
    {
        const ProAudio priority;
        std::array<uint8_t, maxDatagram + 1> data;
        while (running.load())
        {
            fd_set readable; FD_ZERO (&readable); FD_SET (socket, &readable);
            timeval timeout { 0, 50000 };
            if (select (0, &readable, nullptr, nullptr, &timeout) <= 0) continue;
            sockaddr_in from {}; int addressBytes = sizeof (from);
            const int bytes = recvfrom (socket, reinterpret_cast<char*> (data.data()), (int) data.size(), 0,
                                        reinterpret_cast<sockaddr*> (&from), &addressBytes);
            if (bytes <= 0) continue;
            if (isStunSuccess (data.data(), (size_t) bytes))
            {
                const juce::ScopedLock lock (control);
                takeStun (data.data(), (size_t) bytes);
                continue;
            }
            if (useV2.load() && ! (bytes >= 6 && data[4] == 1 && data[5] == (uint8_t) Kind::probe))
            {
                if (bytes <= maxDatagram)
                {
                    Datagram d; d.bytes = (size_t) bytes; d.address = from; d.arrival = nowNs();
                    std::copy_n (data.data(), d.bytes, d.data.data());
                    udpInput.push (d); SetEvent (wake);
                }
                continue;
            }
            Header h; const uint8_t* payload = nullptr;
            if (! readPacket (data.data(), (size_t) bytes, h, payload)) continue;
            const auto now = nowNs();
            const juce::ScopedLock lock (control);
            if (h.kind == Kind::probe)
            {
                auto& recent = recentProbes[nextProbe++ % recentProbes.size()];
                std::copy_n (payload, recent.nonce.size(), recent.nonce.begin()); recent.receivedAt = now;
                if (probeExpected && std::equal (nonce.begin(), nonce.end(), payload)) probeSeen.store (true);
                continue;
            }
            if (! roleStarted || h.session != session || h.sender == sender) continue;
            if (h.kind == Kind::punch)
            {
                // The host knocked through its own router: answer on the very address it came from as well
                // (a router that changes ports maps the host somewhere the room server could not see).
                if (! host && (state.load() == State::connecting || state.load() == State::disconnected))
                {
                    if (candidates.size() < 16
                        && std::none_of (candidates.begin(), candidates.end(), [&from] (const auto& a) { return sameAddress (a, from); }))
                        candidates.push_back (from);
                    sendControl (Kind::hello, from);
                }
                continue;
            }
            if (h.kind == Kind::hello && host)
            {
                PeerInfo info;
                if (! readPeerInfo (payload, h.payloadBytes, info)) continue;
                if (havePeer && h.sender != peerSender && state.load() != State::peerLeft
                    && state.load() != State::disconnected
                    && now - lastReceived < 3000000000ULL) { sendControl (Kind::bye, from); continue; }
                acceptPeer (h, info, from, now); sendControl (Kind::welcome, from); received.fetch_add (1);
                continue;
            }
            if (h.kind == Kind::welcome && ! host)
            {
                if (havePeer && h.sender != peerSender) continue;
                if (state.load() == State::connected && ! sameAddress (peerAddress, from)) continue;
                PeerInfo info;
                if (! readPeerInfo (payload, h.payloadBytes, info)) continue;
                me.wireRate = info.wireRate;
                acceptPeer (h, info, from, now); received.fetch_add (1);
                continue;
            }
            if (h.kind == Kind::bye && ! host && ! havePeer && state.load() == State::connecting
                && std::any_of (candidates.begin(), candidates.end(), [&from] (const auto& a) { return sameAddress (a, from); }))
            {
                received.fetch_add (1); changeState (State::peerLeft); continue;
            }
            if (! havePeer || h.sender != peerSender || ! sameAddress (peerAddress, from)) continue;
            lastReceived = now; received.fetch_add (1);
            if (h.kind == Kind::bye)
            {
                changeState (State::peerLeft);
                if (host) havePeer = false;
            }
            else if (h.kind == Kind::ping) sendControl (Kind::pong, from, readTimestamp (payload));
            else if (h.kind == Kind::pong)
            {
                const auto stamp = readTimestamp (payload);
                if (stamp > now || stamp != lastPing) continue;
                rtts[rttIndex++ % rtts.size()] = (now - stamp) / 1.0e6;
                rttCount = std::min (rtts.size(), rttCount + 1);
                auto sorted = rtts; std::sort (sorted.begin(), sorted.begin() + rttCount);
                rttMs = (sorted[(rttCount - 1) / 2] + sorted[rttCount / 2]) * 0.5;
            }
            else if (h.kind == Kind::audio && state.load() == State::connected)
            {
                const AudioAccess access (*this);
                if (access.audio != nullptr) access.audio->receive (h, payload, now);
            }
        }
    }
    void sendLoop()
    {
        const ProAudio priority;
        while (running.load())
        {
            if (useV2.load())
            {
                runCommands(); starTick();
                const AudioAccess access (*this);
                HANDLE events[2] { wake, access.audio != nullptr ? access.audio->getSendEvent() : nullptr };
                WaitForMultipleObjects (events[1] != nullptr ? 2 : 1, events, FALSE, 20);
                continue;
            }
            {
                const juce::ScopedLock lock (control);
                const auto now = nowNs();
                if (state.load() == State::connected && now - lastReceived >= 3000000000ULL) changeState (State::disconnected);
                if (roleStarted && ! host && (state.load() == State::connecting || state.load() == State::disconnected))
                {
                    const uint64_t interval = state.load() == State::connecting ? 100000000 : 500000000;
                    if (now - lastHello >= interval)
                    {
                        lastHello = now;
                        for (const auto& address : candidates) sendControl (Kind::hello, address);
                        if (havePeer && std::none_of (candidates.begin(), candidates.end(), [this] (const auto& a) { return sameAddress (a, peerAddress); }))
                            sendControl (Kind::hello, peerAddress);
                    }
                }
                if (roleStarted && host && state.load() != State::connected && ! punchTargets.empty() && now < punchUntil
                    && now - lastPunch >= 100000000ULL)
                {
                    lastPunch = now;
                    for (const auto& address : punchTargets) { sendControl (Kind::punch, address); punched.fetch_add (1); }
                }
                if (state.load() == State::connected && now - lastPing >= 1000000000ULL)
                {
                    lastPing = now; sendControl (Kind::ping, peerAddress, now);
                }
                if (state.load() == State::connected)
                {
                    const AudioAccess access (*this);
                    if (access.audio != nullptr && me.deviceRate != (uint32_t) access.audio->getDeviceRate())
                        sendControl (host ? Kind::welcome : Kind::hello, peerAddress);
                }
            }
            const AudioAccess access (*this);
            if (access.audio != nullptr)
                access.audio->drain ([this] (const Header& packet, const uint8_t* payload)
                {
                    const juce::ScopedLock lock (control);
                    if (state.load() != State::connected) return;
                    auto h = packet; h.session = session; h.sender = sender; h.seq = sequence.fetch_add (1);
                    send (h, payload, peerAddress);
                });
            HANDLE events[2] { wake, access.audio != nullptr ? access.audio->getSendEvent() : nullptr };
            // Audio signals the second event. The timer also services hello, timeout and ping without polling audio.
            WaitForMultipleObjects (events[1] != nullptr ? 2 : 1, events, FALSE, state.load() == State::connected ? 1000 : 100);
        }
        if (useV2.load()) stopStar();
    }
};

CollabTransport::CollabTransport() : impl (std::make_unique<Impl>()) {}
CollabTransport::~CollabTransport()
{
    stop();
    if (auto* audio = impl->audio.load()) { detachAudio (*audio); audio->transportStopped(); }
}

juce::String CollabTransport::open (int wantedPort, bool loopbackOnly)
{
    stop();
    auto& i = *impl;
    auto failure = [&] { return juce::String::fromUTF8 ("UDP 포트 ") + juce::String (wantedPort) + juce::String::fromUTF8 ("을 열 수 없습니다"); };
    if (! i.winsock || wantedPort < 0 || wantedPort > 65535) return failure();
    i.socket = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (i.socket == INVALID_SOCKET) return failure();
    const int size = 64 * 1024;
    const DWORD timeout = 50;
    setsockopt (i.socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*> (&size), sizeof (size));
    setsockopt (i.socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*> (&size), sizeof (size));
    setsockopt (i.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*> (&timeout), sizeof (timeout));
    sockaddr_in local {}; local.sin_family = AF_INET; local.sin_port = htons ((u_short) wantedPort);
    local.sin_addr.s_addr = htonl (loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind (i.socket, reinterpret_cast<const sockaddr*> (&local), sizeof (local)) != 0)
    {
        closesocket (i.socket); i.socket = INVALID_SOCKET; return failure();
    }
    // sendto must never wait behind a slow link. select() bounds receive waits.
    u_long nonblocking = 1; ioctlsocket (i.socket, FIONBIO, &nonblocking);
    int bytes = sizeof (local); getsockname (i.socket, reinterpret_cast<sockaddr*> (&local), &bytes);
    i.loopback = loopbackOnly;
    i.port.store (ntohs (local.sin_port)); i.running.store (true);
    i.receiver = std::thread ([&i] { i.receiveLoop(); }); i.transmitter = std::thread ([&i] { i.sendLoop(); });
    return {};
}

void CollabTransport::startHost (uint64_t session, const PeerInfo& me)
{
    { const Impl::AudioAccess access (*impl); if (access.audio != nullptr) access.audio->startSession(); }
    if (impl->useV2.load())
    {
        impl->post ([i = impl.get(), session, me]
        {
            if (i->selfId != c::hostParticipant || i->roster.session != session) return;
            i->starHost = i->starStarted = i->rosterInstalled = true;
            i->starMe = me; i->starMe.wireRate = i->roster.wireRate;
            i->sent = 0; i->received = 0; i->punched = 0;
            for (auto& l : i->links) if (l.enabled) l.started = nowNs();
            i->overallState(); i->notifyRoster();
        });
        return;
    }
    auto& i = *impl; const juce::ScopedLock lock (i.control);
    i.session = session; i.me = me; i.host = true; i.roleStarted = true; i.havePeer = false;
    i.sender = (uint32_t) juce::Random::getSystemRandom().nextInt(); if (i.sender == 0) i.sender = 1;
    i.lastHello = i.lastPing = 0; i.rttCount = i.rttIndex = 0; i.rttMs = 0;
    i.sent.store (0); i.received.store (0); i.sequence.store (0);
    i.punchTargets.clear(); i.punchUntil = i.lastPunch = 0; i.punched.store (0);
    i.changeState (State::waiting); SetEvent (i.wake);
}
void CollabTransport::punch (const std::vector<Address>& targets, int milliseconds)
{
    auto& i = *impl; const juce::ScopedLock lock (i.control);
    i.punchTargets.clear();
    for (const auto& address : targets)
    {
        sockaddr_in addr {};
        if (resolve (address, addr) && i.punchTargets.size() < 8
            && std::none_of (i.punchTargets.begin(), i.punchTargets.end(), [&addr] (const auto& a) { return sameAddress (a, addr); }))
            i.punchTargets.push_back (addr);
    }
    i.punchUntil = nowNs() + (uint64_t) std::max (0, milliseconds) * 1000000ULL; i.lastPunch = 0;
    SetEvent (i.wake);
}
CollabTransport::Mapping CollabTransport::discoverPublic (const std::vector<Address>& servers, int timeoutMs)
{
    auto& i = *impl;
    if (i.socket == INVALID_SOCKET) return {};   // open/stop/this run on the session worker only
    std::vector<sockaddr_in> targets;
    for (const auto& server : servers)   // DNS outside the lock: the receiver keeps running meanwhile
    {
        sockaddr_in to {};
        // a socket bound to 127.0.0.1 (loopback tests) cannot reach a server outside this PC; two names of one
        // server count once (only different servers can tell a symmetric NAT)
        if (resolveName (server, to) && ! (i.loopback && (ntohl (to.sin_addr.s_addr) >> 24) != 127)
            && std::none_of (targets.begin(), targets.end(), [&to] (const auto& t) { return sameAddress (t, to); }))
            targets.push_back (to);
    }
    if (targets.empty()) return {};
    {
        const juce::ScopedLock lock (i.control);
        i.stunQueries.assign (targets.size(), {});
        for (auto& query : i.stunQueries) juce::Random::getSystemRandom().fillBitsRandomly (query.id.data(), query.id.size());
    }
    const auto send = [&i, &targets]
    {
        const juce::ScopedLock lock (i.control);
        for (size_t n = 0; n < targets.size() && n < i.stunQueries.size(); ++n)
        {
            if (i.stunQueries[n].answered) continue;
            std::array<uint8_t, 20> request { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };   // binding request, no attributes
            std::copy (i.stunQueries[n].id.begin(), i.stunQueries[n].id.end(), request.begin() + 8);
            sendto (i.socket, reinterpret_cast<const char*> (request.data()), (int) request.size(), 0,
                    reinterpret_cast<const sockaddr*> (&targets[n]), sizeof (targets[n]));
        }
    };
    const auto allAnswered = [&i]
    {
        const juce::ScopedLock lock (i.control);
        return std::all_of (i.stunQueries.begin(), i.stunQueries.end(), [] (const auto& q) { return q.answered; });
    };
    // UDP may drop a request: send again after 250 and 600 ms, like a STUN client's retransmission timer
    const double start = juce::Time::getMillisecondCounterHiRes();
    int resent = 0;
    send();
    while (! allAnswered() && juce::Time::getMillisecondCounterHiRes() - start < timeoutMs && i.running.load())
    {
        const double elapsed = juce::Time::getMillisecondCounterHiRes() - start;
        if ((resent == 0 && elapsed >= 250) || (resent == 1 && elapsed >= 600)) { ++resent; send(); }
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    Mapping result;
    const juce::ScopedLock lock (i.control);
    for (const auto& query : i.stunQueries)
    {
        if (! query.answered) continue;
        const auto address = describe (query.mapped);
        if (result.responses++ == 0) { result.address = address; result.consistent = true; }
        else if (! (address == result.address)) result.consistent = false;
    }
    i.stunQueries.clear();
    return result;
}
void CollabTransport::startGuest (uint64_t session, const std::vector<Address>& candidates, const PeerInfo& me)
{
    if (impl->useV2.load())
    {
        { const Impl::AudioAccess access (*impl); if (access.audio != nullptr) access.audio->startSession(); }
        impl->post ([i = impl.get(), session, candidates, me]
        {
            if (i->selfId == c::hostParticipant || i->roster.session != session) return;
            i->starHost = false; i->starStarted = true; i->starMe = me; i->starMe.wireRate = i->roster.wireRate;
            i->sent = 0; i->received = 0; i->punched = 0;
            if (auto* l = i->linkFor (c::hostParticipant))
            {
                l->started = nowNs();
                if (l->candidates.empty())
                    for (const auto& a : candidates) { sockaddr_in to {}; if (resolve (a, to)) l->candidates.push_back (to); }
            }
            i->overallState();
        });
        return;
    }
    startHost (session, me);
    auto& i = *impl; const juce::ScopedLock lock (i.control);
    i.host = false; i.candidates.clear();
    for (const auto& address : candidates) { sockaddr_in addr {}; if (resolve (address, addr)) i.candidates.push_back (addr); }
    i.changeState (State::connecting); SetEvent (i.wake);
}
void CollabTransport::expectProbe (const Nonce& nonce)
{
    const juce::ScopedLock lock (impl->control);
    impl->nonce = nonce; impl->probeExpected = true;
    const auto now = nowNs();
    impl->probeSeen.store (std::any_of (impl->recentProbes.begin(), impl->recentProbes.end(), [&] (const auto& probe)
    {
        return probe.receivedAt != 0 && now - probe.receivedAt <= 10000000000ULL && probe.nonce == nonce;
    }));
}
bool CollabTransport::hasProbe() const noexcept { return impl->probeSeen.load(); }
int CollabTransport::getLocalPort() const noexcept { return impl->port.load(); }
CollabTransport::State CollabTransport::getState() const noexcept { return impl->state.load(); }
CollabTransport::Stats CollabTransport::getStats() const
{
    if (impl->useV2.load())
    {
        c::ParticipantId id = 0;
        {
            const juce::ScopedLock lock (impl->control);
            for (size_t n = 0; n < impl->publishedIds.size(); ++n)
            {
                if (impl->publishedIds[n] == 0) continue;
                id = impl->publishedIds[n];
                if (impl->publishedLinks[n].state == State::connected) break;
            }
        }
        auto stats = getStats (id); stats.state = getState(); return stats;
    }
    const juce::ScopedLock lock (impl->control);
    return { impl->state.load(), impl->peer, impl->havePeer ? describe (impl->peerAddress) : Address {}, impl->rttMs,
             impl->sent.load(), impl->received.load(), impl->punched.load(), impl->me.wireRate };
}
double CollabTransport::estimatedOneWayMs() const
{
    if (impl->useV2.load())
    {
        const auto s = getStats();
        const Impl::AudioAccess access (*impl);
        const double target = access.audio != nullptr ? access.audio->getStats().targetMs : 0;
        return s.peer.inputLatencyMs + packetDurationMs (s.wireRate) * 0.5 + s.rttMs * 0.5 + target
            + (s.peer.deviceRate != s.wireRate ? 16000.0 / s.peer.deviceRate : 0) + 16000.0 / s.wireRate + impl->v2OutputLatencyMs.load();
    }
    const juce::ScopedLock lock (impl->control);
    const Impl::AudioAccess access (*impl);
    const double target = access.audio != nullptr ? access.audio->getStats().targetMs : 0;
    const auto wireRate = impl->me.wireRate;
    const double senderAsrc = impl->peer.deviceRate != wireRate ? 16000.0 / impl->peer.deviceRate : 0;
    return impl->peer.inputLatencyMs + packetDurationMs (wireRate) * 0.5 + impl->rttMs * 0.5
        + target + senderAsrc + 16000.0 / wireRate + impl->me.outputLatencyMs;
}
void CollabTransport::stop()
{
    auto& i = *impl;
    {
        const juce::ScopedLock lock (i.control);
        if (! i.useV2.load() && i.socket != INVALID_SOCKET && i.havePeer)
            for (int n = 0; n < 3; ++n) i.sendControl (Kind::bye, i.peerAddress);
        i.running.store (false); SetEvent (i.wake);
    }
    if (i.receiver.joinable()) i.receiver.join();
    if (i.transmitter.joinable()) i.transmitter.join();
    if (i.socket != INVALID_SOCKET) { closesocket (i.socket); i.socket = INVALID_SOCKET; }
    i.useV2.store (false);
    while (i.webReaders.load() != 0) std::this_thread::yield();
    const juce::ScopedLock lock (i.control);
    i.port.store (0); i.havePeer = i.roleStarted = false; i.changeState (State::off);
    i.probeExpected = false; i.probeSeen.store (false); i.recentProbes = {}; i.nextProbe = 0;
    i.peer = {}; i.peerSender = 0; i.candidates.clear();
    i.punchTargets.clear(); i.punchUntil = i.lastPunch = 0; i.stunQueries.clear();
    i.useV2.store (false); i.webGeneration.store (0); i.commands.clear(); i.commandsPending.store (false);
    i.udpInput.reset(); i.webInput.reset(); i.links = {}; i.publishedLinks = {}; i.publishedIds = {};
    i.callbacks = {}; i.localSink = {}; i.roster = {}; i.selfId = 0;
    i.starStarted = i.starHost = i.rosterInstalled = false; i.controlSeq = 0;
    i.localBase = i.lastLocalFrame = 0; i.localBasePending = true; i.localFrameSeen = i.sourceRestart = false;
    i.localEpoch = 1; i.restartEpoch = 0; i.lastRestart = 0;
}
void CollabTransport::attachAudio (CollabAudio& a)
{
    if (auto* old = impl->audio.load()) { detachAudio (*old); old->transportStopped(); }
    impl->audio.store (&a); a.setConnected (getState() == State::connected); SetEvent (impl->wake);
}
void CollabTransport::detachAudio (CollabAudio& a)
{
    if (impl->audio.load() != &a) return;
    impl->audio.store (nullptr); SetEvent (impl->wake);
    while (impl->audioReaders.load() != 0) std::this_thread::yield();
}

void CollabTransport::configureV2 (c::ParticipantId self, const RosterSnapshot& roster,
                                   const std::vector<V2Peer>& peers, V2Callbacks callbacks)
{
    // R8 selects legacy2 when the server/peer has no extension. Its arbitrary
    // legacy sender binding must not be replaced with participant ID 1.
    if (roster.mode != c::RoomMode::v2 || ! validateRoster (roster, self)) return;
    impl->post ([i = impl.get(), self, roster, peers, callbacks = std::move (callbacks)] () mutable
    {
        i->selfId = self; i->roster = roster; i->starHost = self == c::hostParticipant;
        i->localEpoch = findMember (roster, self)->streamEpoch; i->callbacks = std::move (callbacks);
        i->localSink = [i] (const Header& h, const uint8_t* p) { i->sendLocal (h, p); };
        i->setPeers (peers);
    });
    impl->useV2.store (true); SetEvent (impl->wake);
}
void CollabTransport::updateRoster (const RosterSnapshot& roster)
{
    if (impl->useV2.load()) impl->post ([i = impl.get(), roster] { i->installHostRoster (roster); });
}
void CollabTransport::updatePeers (const std::vector<V2Peer>& peers)
{
    if (impl->useV2.load()) impl->post ([i = impl.get(), peers] { i->setPeers (peers); });
}
void CollabTransport::receiveWebPacket (const c::HeaderV2& h, const uint8_t* payload, uint64_t arrivalNs)
{
    struct Access
    {
        explicit Access (std::atomic<unsigned>& value) : readers (value) { readers.fetch_add (1); }
        ~Access() { readers.fetch_sub (1); }
        std::atomic<unsigned>& readers;
    } access (impl->webReaders);
    if (! impl->useV2.load()) return;
    Impl::Datagram d; d.generation = impl->webGeneration.load(); d.arrival = arrivalNs;
    if (d.generation == 0) return;
    d.bytes = writePacketV2 (d.data.data(), d.data.size(), h, payload);
    if (d.bytes != 0) { impl->webInput.push (d); SetEvent (impl->wake); }
}
void CollabTransport::setWebLink (c::ParticipantId webId, c::ConnectionGeneration generation, WebLinkSend send)
{
    if (! impl->useV2.load()) return;
    impl->post ([i = impl.get(), webId, generation, send = std::move (send)] () mutable
    {
        if (! i->starHost) return;
        auto& l = i->links[2];
        if (! send.audio || ! send.control)
        {
            if (l.enabled && l.id == webId && l.generation == generation) i->leaveLink (l, State::peerLeft);
            return;
        }
        const auto* member = findMember (i->roster, webId);
        if (generation == 0 || member == nullptr || member->kind != c::ParticipantKind::web
            || (l.id == webId && generation < l.generation)) return;
        if (l.enabled && l.id == webId && l.generation == generation) { l.webSend = std::move (send); return; }
        l = Impl::Link {}; l.id = webId; l.generation = generation; l.web = l.enabled = l.offerAccepted = true;
        l.webSend = std::move (send); l.started = l.lastReceived = nowNs();
        l.info = { member->nickname, member->appVersion, member->inputLatencyUs / 1000.0,
                   member->outputLatencyUs / 1000.0, member->deviceRate, i->roster.wireRate };
        i->linkSent[2] = 0; i->linkReceived[2] = 0; i->linkDropped[2] = 0; i->linkRtt[2] = 0;
        i->webGeneration.store (generation); i->linkState (l, State::connecting);
    });
}
CollabTransport::Stats CollabTransport::getStats (c::ParticipantId linkedPeer) const
{
    const juce::ScopedLock lock (impl->control);
    for (size_t n = 0; n < impl->publishedIds.size(); ++n)
        if (linkedPeer != 0 && impl->publishedIds[n] == linkedPeer)
        {
            auto stats = impl->publishedLinks[n];
            stats.sent = impl->linkSent[n].load(); stats.received = impl->linkReceived[n].load();
            stats.droppedAudio = impl->linkDropped[n].load(); stats.punched = impl->linkPunched[n].load(); stats.rttMs = impl->linkRtt[n].load();
            return stats;
        }
    return {};
}
}
