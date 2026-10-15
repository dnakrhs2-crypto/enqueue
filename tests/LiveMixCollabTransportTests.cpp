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
    }
};
static LiveMixCollabTransportTests liveMixCollabTransportTests;
}
