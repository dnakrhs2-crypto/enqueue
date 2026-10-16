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

namespace gocue::livemix::collab
{
namespace
{
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
    int bytes = sizeof (local); getsockname (i.socket, reinterpret_cast<sockaddr*> (&local), &bytes);
    i.loopback = loopbackOnly;
    i.port.store (ntohs (local.sin_port)); i.running.store (true);
    i.receiver = std::thread ([&i] { i.receiveLoop(); }); i.transmitter = std::thread ([&i] { i.sendLoop(); });
    return {};
}

void CollabTransport::startHost (uint64_t session, const PeerInfo& me)
{
    { const Impl::AudioAccess access (*impl); if (access.audio != nullptr) access.audio->startSession(); }
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
    const juce::ScopedLock lock (impl->control);
    return { impl->state.load(), impl->peer, impl->havePeer ? describe (impl->peerAddress) : Address {}, impl->rttMs,
             impl->sent.load(), impl->received.load(), impl->punched.load(), impl->me.wireRate };
}
double CollabTransport::estimatedOneWayMs() const
{
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
        if (i.socket != INVALID_SOCKET && i.havePeer)
            for (int n = 0; n < 3; ++n) i.sendControl (Kind::bye, i.peerAddress);
        i.running.store (false); SetEvent (i.wake);
    }
    if (i.receiver.joinable()) i.receiver.join();
    if (i.transmitter.joinable()) i.transmitter.join();
    if (i.socket != INVALID_SOCKET) { closesocket (i.socket); i.socket = INVALID_SOCKET; }
    const juce::ScopedLock lock (i.control);
    i.port.store (0); i.havePeer = i.roleStarted = false; i.changeState (State::off);
    i.probeExpected = false; i.probeSeen.store (false); i.recentProbes = {}; i.nextProbe = 0;
    i.peer = {}; i.peerSender = 0; i.candidates.clear();
    i.punchTargets.clear(); i.punchUntil = i.lastPunch = 0; i.stunQueries.clear();
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
}
