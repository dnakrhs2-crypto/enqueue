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
namespace
{
bool waitFor (const std::function<bool()>& predicate, int ms = 2000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds (ms);
    do { if (predicate()) return true; std::this_thread::sleep_for (std::chrono::milliseconds (10)); } while (std::chrono::steady_clock::now() < end);
    return predicate();
}
struct RawPeer
{
    RawPeer()
    {
        socket = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in local {}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        bind (socket, reinterpret_cast<sockaddr*> (&local), sizeof (local));
        int bytes = sizeof (local); getsockname (socket, reinterpret_cast<sockaddr*> (&local), &bytes); port = ntohs (local.sin_port);
        const DWORD timeout = 100; setsockopt (socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*> (&timeout), sizeof (timeout));
    }
    ~RawPeer() { closesocket (socket); }
    void send (int portTo, Header h, const uint8_t* payload)
    {
        std::array<uint8_t, maxDatagram> data; const auto bytes = writePacket (data.data(), data.size(), h, payload);
        sockaddr_in to {}; to.sin_family = AF_INET; to.sin_addr.s_addr = htonl (INADDR_LOOPBACK); to.sin_port = htons ((u_short) portTo);
        sendto (socket, reinterpret_cast<const char*> (data.data()), (int) bytes, 0, reinterpret_cast<sockaddr*> (&to), sizeof (to));
    }
    bool receiveKind (Kind kind)
    {
        std::array<uint8_t, maxDatagram> data;
        const int bytes = recv (socket, reinterpret_cast<char*> (data.data()), (int) data.size(), 0);
        Header h; const uint8_t* payload = nullptr;
        return bytes > 0 && readPacket (data.data(), (size_t) bytes, h, payload) && h.kind == kind;
    }
    SOCKET socket;
    int port = 0;
};
// Answers every RFC 5389 binding request with a fixed XOR-MAPPED-ADDRESS (the address a router would show).
struct FakeStun
{
    FakeStun (std::array<uint8_t, 4> ip, int mappedPort) : address (ip), mapped (mappedPort)
    {
        worker = std::thread ([this]
        {
            while (! stop.load())
            {
                std::array<uint8_t, 64> request; sockaddr_in from {}; int bytes = sizeof (from);
                const int n = recvfrom (raw.socket, reinterpret_cast<char*> (request.data()), (int) request.size(), 0,
                                        reinterpret_cast<sockaddr*> (&from), &bytes);
                if (n < 20 || request[0] != 0x00 || request[1] != 0x01) continue;
                ++requests;
                std::array<uint8_t, 32> reply { 0x01, 0x01, 0x00, 0x0c, 0x21, 0x12, 0xA4, 0x42 };
                std::copy (request.begin() + 8, request.begin() + 20, reply.begin() + 8);
                const uint8_t cookie[4] { 0x21, 0x12, 0xA4, 0x42 };
                const int port = mapped ^ 0x2112;
                const uint8_t attribute[12] { 0x00, 0x20, 0x00, 0x08, 0x00, 0x01, (uint8_t) (port >> 8), (uint8_t) port,
                                              (uint8_t) (address[0] ^ cookie[0]), (uint8_t) (address[1] ^ cookie[1]),
                                              (uint8_t) (address[2] ^ cookie[2]), (uint8_t) (address[3] ^ cookie[3]) };
                std::copy (std::begin (attribute), std::end (attribute), reply.begin() + 20);
                sendto (raw.socket, reinterpret_cast<const char*> (reply.data()), 32, 0, reinterpret_cast<sockaddr*> (&from), sizeof (from));
            }
        });
    }
    ~FakeStun() { stop.store (true); worker.join(); }
    RawPeer raw;
    std::array<uint8_t, 4> address;
    int mapped, port = raw.port;
    std::atomic<int> requests { 0 };
    std::atomic<bool> stop { false };
    std::thread worker;
};
}
class LiveMixCollabTransportTests : public juce::UnitTest
{
public:
    LiveMixCollabTransportTests() : UnitTest ("LiveMix Collab UDP", "LiveMix") {}
    void runTest() override
    {
        beginTest ("loopback hello/welcome, two candidates, session filter, RTT, bye and Korean bind error");
        {
            CollabTransport host, guest, wrong, occupied;
            expect (host.open (0, true).isEmpty()); expect (guest.open (0, true).isEmpty()); expect (wrong.open (0, true).isEmpty());
            expect (occupied.open (host.getLocalPort(), true).contains (juce::String::fromUTF8 ("열 수 없습니다")));
            host.startHost (42, { "host", "0.15.0", 2, 3 });
            wrong.startGuest (43, { { "127.0.0.1", host.getLocalPort() } }, { "wrong" });
            std::this_thread::sleep_for (std::chrono::milliseconds (150));
            expect (host.getState() == CollabTransport::State::waiting); wrong.stop();
            int closedPort;
            { RawPeer closed; closedPort = closed.port; }
            guest.startGuest (42, { { "127.0.0.1", closedPort }, { "127.0.0.1", host.getLocalPort() } }, { "guest", "0.15.0", 4, 5 });
            expect (waitFor ([&] { return host.getState() == CollabTransport::State::connected && guest.getState() == CollabTransport::State::connected; }));
            expectEquals (guest.getStats().address.port, host.getLocalPort());
            expectEquals (host.getStats().peer.nickname, juce::String ("guest"));
            expectEquals (guest.getStats().peer.nickname, juce::String ("host"));
            expect (waitFor ([&] { return host.getStats().rttMs > 0 && guest.getStats().rttMs > 0; }));
            expectGreaterThan (guest.estimatedOneWayMs(), 2.0 + 1.25 + 5.0);
            expect (wrong.open (0, true).isEmpty());
            wrong.startGuest (42, { { "127.0.0.1", host.getLocalPort() } }, { "another guest" });
            expect (waitFor ([&] { return wrong.getState() == CollabTransport::State::peerLeft; }));
            expectEquals (host.getStats().peer.nickname, juce::String ("guest"));
            guest.stop();
            expect (waitFor ([&] { return host.getState() == CollabTransport::State::peerLeft; }));
            host.stop(); expect (host.getState() == CollabTransport::State::off);
        }

        beginTest ("probe requires exact nonce with session zero");
        {
            CollabTransport host; expect (host.open (0, true).isEmpty());
            RawPeer raw; Nonce nonce {}; nonce[0] = 23; nonce[15] = 71;
            host.expectProbe (nonce);
            Header h; h.kind = Kind::probe; h.payloadBytes = 16;
            Nonce wrong = nonce; wrong[2] = 1; raw.send (host.getLocalPort(), h, wrong.data());
            std::this_thread::sleep_for (std::chrono::milliseconds (50)); expect (! host.hasProbe());
            raw.send (host.getLocalPort(), h, nonce.data()); expect (waitFor ([&] { return host.hasProbe(); }));
        }

        beginTest ("host replaces mapped address for same sender, rejects another, times out and reconnects");
        {
            CollabTransport host; expect (host.open (0, true).isEmpty()); host.startHost (66, { "host" });
            RawPeer first, remapped, outsider;
            std::array<uint8_t, 128> payload;
            Header h; h.kind = Kind::hello; h.session = 66; h.sender = 100;
            h.payloadBytes = (uint16_t) writePeerInfo (payload.data(), { "guest" });
            first.send (host.getLocalPort(), h, payload.data()); expect (waitFor ([&] { return host.getState() == CollabTransport::State::connected; }));
            remapped.send (host.getLocalPort(), h, payload.data()); expect (waitFor ([&] { return host.getStats().address.port == remapped.port; }));
            auto other = h; other.sender = 101; outsider.send (host.getLocalPort(), other, payload.data());
            expect (waitFor ([&] { return outsider.receiveKind (Kind::bye); }));
            expectEquals (host.getStats().address.port, remapped.port);
            expect (waitFor ([&] { return host.getState() == CollabTransport::State::disconnected; }, 4500));
            remapped.send (host.getLocalPort(), h, payload.data()); expect (waitFor ([&] { return host.getState() == CollabTransport::State::connected; }));
        }

        beginTest ("probe received before the HTTP response is remembered by expectProbe");
        {
            CollabTransport host; expect (host.open (0, true).isEmpty());
            RawPeer raw; Nonce nonce {}; nonce[0] = 87; nonce[15] = 35;
            Header h; h.kind = Kind::probe; h.payloadBytes = 16;
            raw.send (host.getLocalPort(), h, nonce.data());
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
            host.expectProbe (nonce); expect (host.hasProbe());
            auto other = nonce; other[2] = 42;
            host.expectProbe (other); expect (! host.hasProbe());
        }

        beginTest ("new sender replaces a guest whose socket closed without bye");
        {
            CollabTransport host; CollabAudio audio;
            audio.prepare (48000, 120); audio.attach (host);
            expect (host.open (0, true).isEmpty()); host.startHost (77, { "host" });
            std::array<uint8_t, 128> payload;
            Header hello; hello.kind = Kind::hello; hello.session = 77; hello.sender = 201;
            {
                RawPeer first;
                hello.payloadBytes = (uint16_t) writePeerInfo (payload.data(), { "guest A" });
                first.send (host.getLocalPort(), hello, payload.data());
                expect (waitFor ([&] { return host.getState() == CollabTransport::State::connected; }));
            }
            expect (waitFor ([&] { return host.getState() == CollabTransport::State::disconnected; }, 4500));
            RawPeer second;
            hello.sender = 202; hello.payloadBytes = (uint16_t) writePeerInfo (payload.data(), { "guest B" });
            second.send (host.getLocalPort(), hello, payload.data());
            expect (waitFor ([&] { return host.getState() == CollabTransport::State::connected; }));
            expectEquals (host.getStats().peer.nickname, juce::String ("guest B"));
            expectEquals (host.getStats().address.port, second.port);
        }

        beginTest ("connected audio survives device-rate restart and attachment destruction");
        {
            CollabTransport host, guest; CollabAudio a, b;
            a.prepare (48000, 120); b.prepare (48000, 120); a.attach (host); b.attach (guest);
            expect (host.open (0, true).isEmpty()); expect (guest.open (0, true).isEmpty());
            host.startHost (99, { "a" }); guest.startGuest (99, { { "127.0.0.1", host.getLocalPort() } }, { "b" });
            expect (waitFor ([&] { return host.getState() == CollabTransport::State::connected && guest.getState() == CollabTransport::State::connected; }));
            std::array<float, 240> input; input.fill (0.25f); std::array<float, 240> left {}, right {}; float* out[] { left.data(), right.data() };
            uint64_t before = 0;
            for (int block = 0; block < 100; ++block)
            {
                if (block == 50) { before = b.getStats().received; b.prepare (96000, 240); }
                a.pushLocal (input.data(), input.data(), 120); b.renderRemote (out, 2, 0, block < 50 ? 120 : 240);
                std::this_thread::sleep_for (std::chrono::milliseconds (3));
            }
            expect (before > 10 && b.getStats().received > 10);
            expectGreaterThan (left.back(), 0.2f);
            expect (host.getState() == CollabTransport::State::connected && guest.getState() == CollabTransport::State::connected);
            expect (waitFor ([&] { return host.getStats().peer.deviceRate == 96000; }));
            b.detach(); a.detach();
        }

        beginTest ("guest keeps sending hello at 500 ms after three seconds without peer packets");
        {
            CollabTransport guest; expect (guest.open (0, true).isEmpty());
            RawPeer host;
            guest.startGuest (123, { { "127.0.0.1", host.port } }, { "guest" });
            expect (waitFor ([&] { return host.receiveKind (Kind::hello); }));
            std::array<uint8_t, 128> payload;
            Header welcome; welcome.kind = Kind::welcome; welcome.session = 123; welcome.sender = 678;
            welcome.payloadBytes = (uint16_t) writePeerInfo (payload.data(), { "host" });
            host.send (guest.getLocalPort(), welcome, payload.data());
            expect (waitFor ([&] { return guest.getState() == CollabTransport::State::connected; }));
            expect (waitFor ([&] { return guest.getState() == CollabTransport::State::disconnected; }, 4500));
            expect (waitFor ([&] { return host.receiveKind (Kind::hello); }));
            expect (waitFor ([&] { return host.receiveKind (Kind::hello); }, 1000));
            host.send (guest.getLocalPort(), welcome, payload.data());
            expect (waitFor ([&] { return guest.getState() == CollabTransport::State::connected; }));
        }

        beginTest ("STUN binding answers give the socket's public address; servers that disagree give no usable mapping");
        {
            // 10/16 two-PC test: both routers kept 49761 -> 49761 for two Google STUN servers (hole punching works)
            CollabTransport socket; expect (socket.open (0, true).isEmpty());   // first: it starts Winsock for the raw sockets
            FakeStun first ({ 203, 0, 113, 7 }, 40000), second ({ 203, 0, 113, 7 }, 40000), other ({ 203, 0, 113, 7 }, 40001);
            auto mapping = socket.discoverPublic ({ { "127.0.0.1", first.port }, { "127.0.0.1", second.port } }, 1500);
            expectEquals (mapping.responses, 2); expect (mapping.consistent && mapping.usable());
            expectEquals (mapping.address.host, juce::String ("203.0.113.7")); expectEquals (mapping.address.port, 40000);
            mapping = socket.discoverPublic ({ { "127.0.0.1", first.port }, { "127.0.0.1", other.port } }, 1500);
            expectEquals (mapping.responses, 2); expect (! mapping.consistent && ! mapping.usable());   // a symmetric NAT
            int silent;
            { RawPeer closed; silent = closed.port; }
            mapping = socket.discoverPublic ({ { "127.0.0.1", first.port }, { "127.0.0.1", silent } }, 400);
            expectEquals (mapping.responses, 1); expect (mapping.usable());
            // two names of one server are asked once (10/16: stun.l.google.com and stun1.l.google.com were one address)
            const int before = first.requests.load();
            mapping = socket.discoverPublic ({ { "127.0.0.1", first.port }, { "127.0.0.1", first.port } }, 1500);
            expectEquals (mapping.responses, 1); expectEquals (first.requests.load() - before, 1);
            expect (first.requests.load() >= 3);
            // a socket bound to 127.0.0.1 does not wait for servers it cannot reach
            const auto started = juce::Time::getMillisecondCounterHiRes();
            mapping = socket.discoverPublic ({ { "203.0.113.200", 3478 } }, 1500);
            expectEquals (mapping.responses, 0); expectLessThan (juce::Time::getMillisecondCounterHiRes() - started, 200.0);
        }

        beginTest ("a host punches the guest's public addresses; a guest answers a punch of its session with hello");
        {
            CollabTransport host; expect (host.open (0, true).isEmpty()); host.startHost (55, { "host" });
            RawPeer guestRouter;
            host.punch ({ { "127.0.0.1", guestRouter.port } }, 1000);
            expect (waitFor ([&] { return guestRouter.receiveKind (Kind::punch); }));
            expectGreaterThan ((int) host.getStats().punched, 0);
            CollabTransport guest; expect (guest.open (0, true).isEmpty());
            int closedPort;
            { RawPeer closed; closedPort = closed.port; }
            guest.startGuest (56, { { "127.0.0.1", closedPort } }, { "guest" });
            RawPeer stranger, hostRouter;
            Header punch; punch.kind = Kind::punch; punch.session = 999; punch.sender = 4;
            stranger.send (guest.getLocalPort(), punch, nullptr);                      // another room's knock
            std::this_thread::sleep_for (std::chrono::milliseconds (300));
            expect (! stranger.receiveKind (Kind::hello));
            punch.session = 56; hostRouter.send (guest.getLocalPort(), punch, nullptr);
            expect (waitFor ([&] { return hostRouter.receiveKind (Kind::hello); }));   // and then every 100 ms
            expect (waitFor ([&] { return hostRouter.receiveKind (Kind::hello); }, 500));
        }
    }
};
static LiveMixCollabTransportTests liveMixCollabTransportTests;
}
