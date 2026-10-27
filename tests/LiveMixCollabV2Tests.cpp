#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include "CollabTransport.h"
#include "CollabAudio.h"
#include <chrono>
#include <thread>

namespace gocue::tests
{
using namespace livemix::collab;
namespace c = livemix::collab::contracts;
namespace
{
uint64_t stamp()
{
    return (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool until (const std::function<bool()>& condition, int timeout = 3000)
{
    const auto end = stamp() + (uint64_t) timeout * 1000000;
    do { if (condition()) return true; std::this_thread::sleep_for (std::chrono::milliseconds (10)); } while (stamp() < end);
    return condition();
}
RosterSnapshot room (std::initializer_list<uint32_t> ids, bool web = false)
{
    RosterSnapshot r; r.session = 0x1020304050607080ULL; r.revision = 1;
    for (auto id : ids)
    {
        auto& m = r.members[r.memberCount++]; m.participantId = id; m.capabilities = 31;
        m.nickname = "person " + juce::String (id); m.appVersion = "0.17.0"; m.state = c::ConnectionState::connected;
    }
    if (web) { r.members[r.memberCount - 1].kind = c::ParticipantKind::web; r.members[r.memberCount - 1].appVersion.clear(); }
    return r;
}
struct Record
{
    c::HeaderV2 h;
    std::array<uint8_t, 720> payload {};
};
struct Trace
{
    explicit Trace (uint32_t id) : self (id) {}
    uint32_t self;
    std::array<std::atomic<unsigned>, 16> origins {};
    std::array<std::atomic<int>, 16> states {};
    std::atomic<unsigned> errors { 0 }, rosterCalls { 0 }, revision { 0 }, members { 0 };
    std::array<Record, 512> records;
    std::atomic<size_t> size { 0 };
    void audio (const c::HeaderV2& h, const uint8_t* p)
    {
        if (h.origin == self || h.origin >= origins.size() || h.frames != 120) { ++errors; return; }
        std::array<float, 120> left, right;
        left.fill ((float) h.origin / 16); right.fill (-(float) h.origin / 32);
        std::array<uint8_t, 720> expected {};
        if (h.flags == c::Flags::monoPcm16) packPcm16 (expected.data(), left.data(), 120);
        else packPcm24 (expected.data(), left.data(), right.data(), 120);
        if (! std::equal (expected.begin(), expected.begin() + h.payloadBytes, p)) ++errors;
        const auto n = size.load (std::memory_order_relaxed);
        if (n < records.size())
        {
            records[n].h = h; std::copy_n (p, h.payloadBytes, records[n].payload.data());
            size.store (n + 1, std::memory_order_release);
        }
        ++origins[h.origin];
    }
    CollabTransport::V2Callbacks callbacks()
    {
        return {
            [this] (const c::HeaderV2& h, const uint8_t* p, uint64_t) { audio (h, p); },
            [this] (const RosterSnapshot& r) { revision = r.revision; members = (unsigned) r.memberCount; ++rosterCalls; },
            [this] (uint32_t id, CollabTransport::State s) { if (id < states.size()) states[id] = (int) s; }
        };
    }
    const Record* find (uint32_t origin, uint32_t seq, uint32_t epoch = 1) const
    {
        const auto end = size.load (std::memory_order_acquire);
        for (size_t n = 0; n < end; ++n)
            if (records[n].h.origin == origin && records[n].h.seq == seq && records[n].h.streamEpoch == epoch) return &records[n];
        return nullptr;
    }
};
struct Node
{
    explicit Node (uint32_t who) : id (who), trace (std::make_unique<Trace> (who)), audio ("Local\\LiveMix.R3." + juce::String (who))
    {
        audio.prepare (48000, 120); audio.attach (transport);
        left.fill ((float) id / 16); right.fill (-(float) id / 32);
    }
    ~Node() { transport.stop(); audio.detach(); }
    uint32_t id;
    std::unique_ptr<Trace> trace;
    CollabAudio audio;
    CollabTransport transport;
    std::array<float, 120> left, right;
    CollabTransport::Address address() const { return { "127.0.0.1", transport.getLocalPort() }; }
    void start (const RosterSnapshot& r, const std::vector<CollabTransport::V2Peer>& peers)
    {
        transport.configureV2 (id, r, peers, trace->callbacks());
        if (id == 1) transport.startHost (r.session, { "host", "0.17.0" });
        else transport.startGuest (r.session, peers[0].candidates, { "guest", "0.17.0" });
    }
    void push() { audio.pushLocal (left.data(), right.data(), 120); }
};
void pump (std::initializer_list<Node*> nodes, int blocks = 40)
{
    for (int n = 0; n < blocks; ++n)
    {
        for (auto* node : nodes) node->push();
        std::this_thread::sleep_for (std::chrono::milliseconds (3));
    }
}
struct Raw
{
    Raw()
    {
        socket = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a {}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        bind (socket, (const sockaddr*) &a, sizeof (a)); int length = sizeof (a);
        getsockname (socket, (sockaddr*) &a, &length); port = ntohs (a.sin_port);
        u_long nonblocking = 1; ioctlsocket (socket, FIONBIO, &nonblocking);
    }
    ~Raw() { close(); }
    SOCKET socket = INVALID_SOCKET;
    int port = 0;
    std::array<uint8_t, 1200> data {};
    void close() { if (socket != INVALID_SOCKET) { closesocket (socket); socket = INVALID_SOCKET; } }
    void send (int toPort, const uint8_t* p, size_t bytes)
    {
        sockaddr_in a {}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl (INADDR_LOOPBACK); a.sin_port = htons ((u_short) toPort);
        sendto (socket, (const char*) p, (int) bytes, 0, (const sockaddr*) &a, sizeof (a));
    }
    void send (int toPort, const Header& h, const uint8_t* p = nullptr)
    {
        std::array<uint8_t, 1200> d; send (toPort, d.data(), writePacket (d.data(), d.size(), h, p));
    }
    void send (int toPort, const c::HeaderV2& h, const uint8_t* p = nullptr)
    {
        std::array<uint8_t, 1200> d; send (toPort, d.data(), writePacketV2 (d.data(), d.size(), h, p));
    }
    int receive() { return recv (socket, (char*) data.data(), (int) data.size(), 0); }
    bool v2 (c::Kind kind, c::HeaderV2& h, int timeout = 2000, uint32_t revision = 0)
    {
        return until ([&]
        {
            const auto bytes = receive(); const uint8_t* p = nullptr;
            return bytes > 0 && readPacketV2 (data.data(), (size_t) bytes, h, p) && h.kind == kind
                && (revision == 0 || h.rosterRevision == revision);
        }, timeout);
    }
};
c::HeaderV2 control (c::Kind kind, const RosterSnapshot& r, uint32_t id)
{
    c::HeaderV2 h; h.kind = kind; h.session = r.session; h.origin = h.linkSender = id; h.rosterRevision = r.revision; return h;
}
c::HeaderV2 audioHeader (const RosterSnapshot& r, uint32_t id, uint32_t seq, bool mono = false)
{
    auto h = control (c::Kind::audio, r, id); h.flags = mono ? c::Flags::monoPcm16 : c::Flags::stereoPcm24;
    h.streamEpoch = 1; h.seq = seq; h.firstFrame = (uint64_t) seq * 120; h.frames = 120; h.payloadBytes = mono ? 240 : 720; return h;
}
std::array<uint8_t, 720> samples (uint32_t id, bool mono = false)
{
    std::array<float, 120> l, r; l.fill ((float) id / 16); r.fill (-(float) id / 32);
    std::array<uint8_t, 720> pcm {};
    if (mono) packPcm16 (pcm.data(), l.data(), 120); else packPcm24 (pcm.data(), l.data(), r.data(), 120);
    return pcm;
}
// Separate producers: the transport writes controls; the test writes injections.
// Only the fake web thread calls receiveWebPacket, like a real PeerConnection.
template <size_t N> struct Mailbox
{
    struct Message { std::array<uint8_t, 1200> data {}; size_t bytes = 0; };
    std::array<Message, N> messages;
    std::atomic<size_t> read { 0 }, write { 0 };
    bool push (const uint8_t* data, size_t bytes)
    {
        auto w = write.load(); if (w - read.load() == N) return false;
        auto& m = messages[w % N]; std::copy_n (data, bytes, m.data.data()); m.bytes = bytes; write.store (w + 1); return true;
    }
    bool pop (Message& m)
    {
        auto r = read.load(); if (r == write.load()) return false;
        m = messages[r % N]; read.store (r + 1); return true;
    }
};
struct FakeWeb
{
    FakeWeb (CollabTransport& t, uint32_t who) : host (t), id (who), trace (std::make_unique<Trace> (who))
    {
        worker = std::thread ([this]
        {
            Mailbox<64>::Message m;
            while (! stopping.load())
            {
                while (controls.pop (m))
                {
                    c::HeaderV2 h; const uint8_t* payload = nullptr;
                    if (! readPacketV2 (m.data.data(), m.bytes, h, payload)) { ++trace->errors; continue; }
                    if (h.kind == c::Kind::roster)
                    {
                        RosterSnapshot r;
                        if (! readRoster (payload, h.payloadBytes, h.session, h.rosterRevision, id, r)) { ++trace->errors; continue; }
                        revision = h.rosterRevision; ++rosters;
                        if (holdAck.load()) continue;
                        h.kind = c::Kind::rosterAck; h.payloadBytes = 0;
                    }
                    else if (h.kind == c::Kind::ping) h.kind = c::Kind::pong;
                    else continue;
                    h.origin = h.linkSender = id; host.receiveWebPacket (h, payload, stamp());
                }
                while (injections.pop (m))
                {
                    c::HeaderV2 h; const uint8_t* payload = nullptr;
                    if (readPacketV2 (m.data.data(), m.bytes, h, payload)) host.receiveWebPacket (h, payload, stamp());
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
        });
    }
    ~FakeWeb() { stopping = true; worker.join(); }
    CollabTransport& host;
    uint32_t id;
    std::unique_ptr<Trace> trace;
    Mailbox<64> controls, injections;
    std::atomic<bool> stopping { false }, holdAck { false }, congested { false };
    std::atomic<unsigned> rosters { 0 }, revision { 0 };
    std::thread worker;
    CollabTransport::WebLinkSend senders()
    {
        return {
            [this] (const uint8_t* p, size_t n)
            {
                if (congested.load()) return false;
                c::HeaderV2 h; const uint8_t* payload = nullptr;
                if (readPacketV2 (p, n, h, payload)) trace->audio (h, payload); else ++trace->errors;
                return true;
            },
            [this] (const uint8_t* p, size_t n) { return controls.push (p, n); }
        };
    }
    void inject (const c::HeaderV2& h, const uint8_t* payload = nullptr)
    {
        std::array<uint8_t, 1200> d; const auto bytes = writePacketV2 (d.data(), d.size(), h, payload);
        injections.push (d.data(), bytes);
    }
};
}

class LiveMixCollabV2Tests : public juce::UnitTest
{
public:
    LiveMixCollabV2Tests() : UnitTest ("LiveMix Collab v2", "LiveMix") {}
    void runTest() override
    {
        beginTest ("three loopback natives receive each origin, never self; bye and seat reuse keep the other link alive");
        {
            Node host (1), b (2), d (3);
            expect (host.transport.open (0, true).isEmpty()); expect (b.transport.open (0, true).isEmpty()); expect (d.transport.open (0, true).isEmpty());
            auto r = room ({ 1, 2, 3 });
            host.start (r, { { 2, 22, { b.address() } }, { 3, 33, { d.address() } } });
            b.start (r, { { 1, 22, { host.address() } } }); d.start (r, { { 1, 33, { host.address() } } });
            expect (until ([&] { return host.transport.getStats (2).v2 && host.transport.getStats (3).v2
                && b.transport.getStats (1).v2 && d.transport.getStats (1).v2; }));
            pump ({ &host, &b, &d });
            expect (until ([&] { return host.trace->origins[2] >= 30 && host.trace->origins[3] >= 30
                && b.trace->origins[1] >= 30 && b.trace->origins[3] >= 30 && d.trace->origins[1] >= 30 && d.trace->origins[2] >= 30; }));
            for (auto* n : { &host, &b, &d })
            {
                expectEquals (n->trace->errors.load(), 0u); expectEquals (n->trace->origins[n->id].load(), 0u);
                const auto count = n->trace->size.load();
                bool headersValid = true;
                for (size_t k = 0; k < count; ++k)
                {
                    const auto& h = n->trace->records[k].h;
                    headersValid &= h.firstFrame == (uint64_t) h.seq * 120 && h.streamEpoch == 1 && h.flags == c::Flags::stereoPcm24
                        && h.linkSender == (n->id == 1 ? h.origin : 1u);
                }
                expect (headersValid);
            }
            const auto* source = host.trace->find (2, 10); const auto* relayed = d.trace->find (2, 10);
            expect (source != nullptr && relayed != nullptr);
            if (source && relayed) expect (source->payload == relayed->payload && source->h.firstFrame == relayed->h.firstFrame);
            const auto before = b.trace->origins[1].load(); d.transport.stop();
            expect (until ([&] { return host.trace->members == 2 && b.trace->members == 2; }));
            expect (host.transport.getState() == CollabTransport::State::connected && b.transport.getState() == CollabTransport::State::connected);
            pump ({ &host, &b }); expectGreaterThan (b.trace->origins[1].load(), before + 20);
            Node e (4); expect (e.transport.open (0, true).isEmpty());
            r.members[2].participantId = 4; r.members[2].nickname = "replacement"; r.revision = host.trace->revision.load() + 1;
            host.transport.updateRoster (r); host.transport.updatePeers ({ { 2, 22, { b.address() } }, { 4, 44, { e.address() } } });
            e.start (r, { { 1, 44, { host.address() } } });
            expect (until ([&] { return host.transport.getStats (4).v2 && b.trace->revision == r.revision && e.transport.getStats (1).v2; }));
            const auto departed = b.trace->origins[3].load(); pump ({ &host, &b, &e });
            expect (until ([&] { return b.trace->origins[4] >= 30 && e.trace->origins[2] >= 30 && host.trace->origins[4] >= 30; }));
            expectEquals (b.trace->origins[3].load(), departed);
            expectEquals (b.trace->errors.load() + e.trace->errors.load() + host.trace->errors.load(), 0u);
        }

        beginTest ("web roster retries/ACK, individual stereo downlinks, original mono relay, congestion isolation and web bye");
        {
            Node host (1), b (2);
            expect (host.transport.open (0, true).isEmpty()); expect (b.transport.open (0, true).isEmpty());
            auto r = room ({ 1, 2, 3 }, true);
            host.start (r, { { 2, 22, { b.address() } } }); b.start (r, { { 1, 22, { host.address() } } });
            auto web = std::make_unique<FakeWeb> (host.transport, 3); web->holdAck = true;
            host.transport.setWebLink (3, 33, web->senders());
            expect (until ([&] { return host.transport.getStats (2).v2 && web->rosters >= 3; }));
            pump ({ &host, &b });
            expectEquals (web->trace->origins[1].load() + web->trace->origins[2].load(), 0u);
            web->holdAck = false;
            expect (until ([&] { return host.transport.getStats (3).v2; }));
            pump ({ &host, &b });
            expect (until ([&] { return web->trace->origins[1] >= 30 && web->trace->origins[2] >= 30; }));
            const auto* source = host.trace->find (2, 50); const auto* down = web->trace->find (2, 50);
            expect (source != nullptr && down != nullptr);
            if (source && down) expect (source->payload == down->payload && down->h.origin == 2 && down->h.linkSender == 1
                && source->h.seq == down->h.seq && source->h.streamEpoch == down->h.streamEpoch && source->h.firstFrame == down->h.firstFrame);
            const auto* hostNative = b.trace->find (1, 50); const auto* hostWeb = web->trace->find (1, 50);
            expect (hostNative != nullptr && hostWeb != nullptr);
            if (hostNative && hostWeb) expect (hostNative->payload == hostWeb->payload && hostNative->h.firstFrame == hostWeb->h.firstFrame);
            auto mono = audioHeader (r, 3, 73, true); const auto pcm = samples (3, true);
            auto forgedWeb = mono; forgedWeb.origin = 2; web->inject (forgedWeb, pcm.data());
            forgedWeb = mono; ++forgedWeb.rosterRevision; web->inject (forgedWeb, pcm.data());
            forgedWeb = mono; forgedWeb.flags = c::Flags::stereoPcm24; forgedWeb.payloadBytes = 720;
            web->inject (forgedWeb, pcm.data()); web->inject (mono, pcm.data());
            expect (until ([&] { return host.trace->origins[3] == 1 && b.trace->origins[3] == 1; }));
            const auto* relay = b.trace->find (3, 73); expect (relay != nullptr);
            if (relay) expect (relay->h.flags == c::Flags::monoPcm16 && relay->h.linkSender == 1 && relay->h.streamEpoch == 1
                && relay->h.firstFrame == mono.firstFrame && std::equal (pcm.begin(), pcm.begin() + 240, relay->payload.begin()));
            expectEquals (web->trace->origins[3].load(), 0u);
            // A slow DataChannel does not stall the UDP link; the web output ring
            // drops its oldest packets and sends recent audio on recovery.
            web->congested = true; const auto before = b.trace->origins[1].load();
            pump ({ &host, &b }, 100);
            expectGreaterThan (b.trace->origins[1].load(), before + 80);
            expect (host.transport.getStats (3).droppedAudio > 0);
            expectEquals ((int) host.transport.getStats (2).droppedAudio, 0);
            const auto webBefore = web->trace->origins[1].load(); web->congested = false; pump ({ &host, &b });
            expectGreaterThan (web->trace->origins[1].load(), webBefore + 20);
            expectEquals (web->trace->errors.load() + host.trace->errors.load() + b.trace->errors.load(), 0u);
            web->inject (control (c::Kind::bye, r, 3));
            expect (until ([&] { return host.trace->members == 2 && b.trace->members == 2; }));
            expect (host.transport.getState() == CollabTransport::State::connected);
            host.transport.stop(); web.reset(); // callbacks are quiescent before the fake is destroyed
        }

        beginTest ("a whole host device block reaches the web link: 1024 and 2048 frames, nothing dropped");
        {
            // 10/27 gom (44.1 kHz, 1024-frame ASIO): the web link kept 8 packets / 20 ms per stream, so the transport
            // dropped the tail of every device block - 6% of the host's audio at 1024 frames, 53% at 2048.
            for (const int block : { 1024, 2048 })
            {
                Node host (1);
                host.audio.prepare (48000, block);
                expect (host.transport.open (0, true).isEmpty());
                auto r = room ({ 1, 3 }, true);
                host.start (r, {});
                auto web = std::make_unique<FakeWeb> (host.transport, 3);
                host.transport.setWebLink (3, 33, web->senders());
                expect (until ([&] { return host.transport.getStats (3).v2; }));
                std::vector<float> left ((size_t) block, 1.0f / 16), right ((size_t) block, -1.0f / 32);
                constexpr int blocks = 6;
                for (int n = 0; n < blocks; ++n)   // device pace: one whole block per period
                {
                    host.audio.pushLocal (left.data(), right.data(), block);
                    std::this_thread::sleep_for (std::chrono::milliseconds (block * 1000 / 48000));
                }
                const auto expected = (unsigned) (blocks * block / 120);
                const auto label = "block " + juce::String (block);
                expect (until ([&] { return web->trace->origins[1].load() >= expected; }), label);
                expectEquals ((int) host.transport.getStats (3).droppedAudio, 0, label);
                expectEquals (web->trace->errors.load(), 0u, label);
                host.transport.stop(); web.reset();
            }
        }

        beginTest ("native handshake retries, address/origin/revision validation, restart ACK and independent disconnect");
        {
            Node host (1), b (2);
            expect (host.transport.open (0, true).isEmpty()); expect (b.transport.open (0, true).isEmpty());
            Raw raw, stranger; auto r = room ({ 1, 2, 3 });
            host.start (r, { { 2, 22, { b.address() } }, { 3, 33, { { "127.0.0.1", raw.port } } } });
            b.start (r, { { 1, 22, { host.address() } } });
            std::array<uint8_t, 128> info {};
            Header hello; hello.kind = Kind::hello; hello.session = r.session; hello.sender = 3;
            hello.payloadBytes = (uint16_t) writePeerInfo (info.data(), { "raw native", "0.17.0" });
            raw.send (host.transport.getLocalPort(), hello, info.data());
            expect (until ([&]
            {
                const auto bytes = raw.receive(); Header h; const uint8_t* p = nullptr;
                return bytes > 0 && readPacket (raw.data.data(), (size_t) bytes, h, p) && h.kind == Kind::welcome && h.sender == 1;
            }));
            c::HeaderV2 offerHeader;
            expect (raw.v2 (c::Kind::offer, offerHeader));
            OfferV2 offer; expect (readOfferV2 (raw.data.data() + 48, 16, offer));
            expect (offer.peerParticipantId == 3 && offer.connectionGeneration == 33 && offer.capabilities == 31);
            c::HeaderV2 repeated; expect (raw.v2 (c::Kind::offer, repeated));
            expect (repeated.seq == offerHeader.seq && repeated.rosterRevision == offerHeader.rosterRevision);
            const auto initialPcm = samples (3);
            Header beforeUpgrade; beforeUpgrade.session = r.session; beforeUpgrade.sender = 3;
            beforeUpgrade.frames = 120; beforeUpgrade.payloadBytes = 720;
            raw.send (host.transport.getLocalPort(), beforeUpgrade, initialPcm.data());
            expect (until ([&] { return host.trace->origins[3] == 1; }));
            expect (host.transport.getStats (3).state == CollabTransport::State::connected && ! host.transport.getStats (3).v2);
            auto ack = control (c::Kind::offerAck, r, 3); ack.seq = offerHeader.seq; ack.payloadBytes = 16;
            offer.peerParticipantId = 1; std::array<uint8_t, 16> encoded {}; writeOfferV2 (encoded.data(), encoded.size(), offer);
            // Wrong reservation generation cannot advance the negotiation.
            auto wrong = offer; ++wrong.connectionGeneration; std::array<uint8_t, 16> wrongBytes {};
            writeOfferV2 (wrongBytes.data(), wrongBytes.size(), wrong); raw.send (host.transport.getLocalPort(), ack, wrongBytes.data());
            expect (raw.v2 (c::Kind::offer, repeated));
            raw.send (host.transport.getLocalPort(), ack, encoded.data());
            c::HeaderV2 rosterHeader; expect (raw.v2 (c::Kind::roster, rosterHeader));
            expect (raw.v2 (c::Kind::roster, repeated));
            expect (repeated.seq == rosterHeader.seq && repeated.rosterRevision == rosterHeader.rosterRevision);
            auto rosterAck = control (c::Kind::rosterAck, r, 3); rosterAck.seq = rosterHeader.seq;
            raw.send (host.transport.getLocalPort(), rosterAck);
            expect (until ([&] { return host.transport.getStats (3).v2 && host.transport.getStats (2).v2; }));
            auto pcm = samples (3); auto sound = audioHeader (r, 3, 60);
            // A participant cannot claim another origin or use a new source port.
            auto forged = sound; forged.origin = 2; raw.send (host.transport.getLocalPort(), forged, pcm.data());
            stranger.send (host.transport.getLocalPort(), sound, pcm.data());
            forged = sound; ++forged.session; raw.send (host.transport.getLocalPort(), forged, pcm.data());
            forged = sound; forged.rosterRevision = 2; raw.send (host.transport.getLocalPort(), forged, pcm.data());
            forged = sound; forged.streamEpoch = 2; raw.send (host.transport.getLocalPort(), forged, pcm.data());
            forged = sound; forged.flags = c::Flags::monoPcm16; forged.payloadBytes = 240;
            raw.send (host.transport.getLocalPort(), forged, pcm.data());
            // Participants also reject relays from anywhere except their bound host.
            forged = sound; forged.linkSender = 1; stranger.send (b.transport.getLocalPort(), forged, pcm.data());
            raw.send (host.transport.getLocalPort(), sound, pcm.data());
            expect (until ([&] { return host.trace->origins[3] == 2 && b.trace->origins[3] == 1; }));
            expectEquals (host.trace->origins[2].load(), 0u);
            Header old; old.session = r.session; old.sender = 3; old.seq = 61; old.firstFrame = 7320; old.frames = 120; old.payloadBytes = 720;
            raw.send (host.transport.getLocalPort(), old, pcm.data()); // no v1 after initial v2 activation

            auto restart = control (c::Kind::streamRestart, r, 3); restart.streamEpoch = 2; restart.seq = 900; restart.payloadBytes = 12;
            StreamRestartV2 settings { 44100, 2000, 4000 }; std::array<uint8_t, 12> settingBytes {};
            writeStreamRestartV2 (settingBytes.data(), settingBytes.size(), settings);
            raw.send (host.transport.getLocalPort(), restart, settingBytes.data());
            expect (raw.v2 (c::Kind::roster, rosterHeader, 2000, 2)); expectEquals (rosterHeader.rosterRevision, 2u);
            expect (until ([&] { return b.trace->revision == 2; }));
            raw.send (host.transport.getLocalPort(), restart, settingBytes.data());
            expect (raw.v2 (c::Kind::roster, repeated, 2000, 2)); expectEquals (repeated.rosterRevision, 2u); expectEquals (repeated.seq, rosterHeader.seq);
            sound.streamEpoch = 2; sound.seq = 0; sound.firstFrame = 0; sound.rosterRevision = 2;
            raw.send (host.transport.getLocalPort(), sound, pcm.data()); // new epoch before ACK is dropped
            const auto before = b.trace->origins[1].load(); pump ({ &host, &b });
            expect (until ([&] { return b.trace->origins[1] >= before + 30; }));
            // The unchanged host stream keeps the RAW receiver's old ACK revision.
            expect (raw.v2 (c::Kind::audio, repeated)); expectEquals (repeated.rosterRevision, 1u);
            expectEquals (host.trace->origins[3].load(), 2u); expectEquals (b.trace->origins[3].load(), 1u);
            rosterAck.seq = rosterHeader.seq; rosterAck.rosterRevision = rosterHeader.rosterRevision;
            raw.send (host.transport.getLocalPort(), rosterAck);
            expect (until ([&] { return host.transport.getStats (3).rosterRevision == 2; }));
            raw.send (host.transport.getLocalPort(), sound, pcm.data());
            expect (until ([&] { return host.trace->origins[3] == 3 && b.trace->origins[3] == 2; }));
            expect (b.trace->find (3, 0, 2) != nullptr);
            auto stale = audioHeader (r, 3, 62); raw.send (host.transport.getLocalPort(), stale, pcm.data());
            // Earlier revision remains valid for an unchanged origin/epoch.
            auto fromHost = audioHeader (r, 1, 1000); auto hostPcm = samples (1);
            // Stranger's correctly shaped old-revision packet still has the wrong address.
            stranger.send (b.transport.getLocalPort(), fromHost, hostPcm.data());
            raw.close(); // loss of one physical link, no bye
            const auto stillPlaying = b.trace->origins[1].load(); pump ({ &host, &b }, 1150);
            expect (until ([&] { return host.trace->members == 2 && b.trace->members == 2; }));
            expect (host.transport.getState() == CollabTransport::State::connected && b.transport.getState() == CollabTransport::State::connected);
            expectGreaterThan (b.trace->origins[1].load(), stillPlaying + 500);
            expectEquals (host.trace->origins[3].load(), 3u); expectEquals (b.trace->origins[3].load(), 2u);
            expectEquals (host.trace->errors.load() + b.trace->errors.load(), 0u);
        }

        beginTest ("legacy2 with no capabilities retains v1 hello/welcome and 756-byte audio");
        {
            Node host (1); expect (host.transport.open (0, true).isEmpty()); Raw old;
            auto r = room ({ 1, 2 }); r.mode = c::RoomMode::legacy2;
            for (auto& m : r.members) m.capabilities = 0;
            host.transport.configureV2 (1, r, {}, host.trace->callbacks());
            host.transport.startHost (r.session, { "host", "0.17.0" });
            Header hello; hello.kind = Kind::hello; hello.session = r.session; hello.sender = 0x31415926;
            std::array<uint8_t, 128> data {}; hello.payloadBytes = (uint16_t) writePeerInfo (data.data(), { "old", "0.16.0" });
            old.send (host.transport.getLocalPort(), hello, data.data());
            expect (until ([&] { return host.transport.getState() == CollabTransport::State::connected; }));
            bool gotWelcome = false, gotAudio = false, v2 = false, exactAudio = true;
            for (int n = 0; n < 60; ++n)
            {
                host.push(); std::this_thread::sleep_for (std::chrono::milliseconds (3));
                for (int bytes = old.receive(); bytes > 0; bytes = old.receive())
                {
                    v2 |= old.data[4] == 2;
                    Header h; const uint8_t* p = nullptr;
                    if (! readPacket (old.data.data(), (size_t) bytes, h, p)) continue;
                    if (h.kind == Kind::welcome) gotWelcome = true;
                    if (h.kind == Kind::audio) { gotAudio = true; exactAudio &= bytes == 756 && h.flags == 0; }
                }
            }
            expect (gotWelcome && gotAudio && exactAudio && ! v2); expect (! host.transport.getStats().v2);
            const auto pcm = samples (2); Header sound; sound.session = r.session; sound.sender = hello.sender;
            sound.frames = 120; sound.payloadBytes = 720;
            old.send (host.transport.getLocalPort(), sound, pcm.data());
            expect (until ([&] { return host.audio.getStats().received > 0; }));
        }
    }
};
static LiveMixCollabV2Tests liveMixCollabV2Tests;
}
