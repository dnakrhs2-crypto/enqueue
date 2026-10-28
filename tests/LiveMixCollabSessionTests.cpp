#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include "CollabSession.h"
#include "LiveMixSettings.h"
#include "MixEngine.h"
#include "LiveMixCollabTestHelpers.h"
#include "lm_obs_metadata.h"
#include "ui/UiUtils.h"
#include "ui/CollabWindow.h"
#include <atomic>
#include <mutex>
#include <deque>
#include <map>
#include <condition_variable>
#include <thread>
#include <chrono>

namespace gocue::livemix::collab
{
struct CollabSessionTestAccess
{
    static CollabTransport& transport (CollabSession& s) { return s.transport; }
    static const RosterSnapshot& roster (const CollabSession& s) { return s.roster; }
    static PeerInfo localInfo (const CollabSession& s) { return s.localInfo (s.snapshot.myNick); }
    static void heartbeat (CollabSession& s) { s.sendHeartbeat(); }
    static bool heartbeatPending (CollabSession& s) { return s.heartbeatPending; }
    static bool heartbeatStopped (CollabSession& s) { return s.heartbeatStopped; }
    static bool loopbackOnly (CollabSession& s) { return s.options.loopbackOnly; }
    static bool punching (CollabSession& s) { return s.punching; }
    static void renewMappings (CollabSession& s)
    {
        s.lastRenew = juce::Time::getMillisecondCounterHiRes() - s.options.renewIntervalMs;
        s.timerCallback();
    }
    static bool legacyReady (CollabSession& s) { return ! s.switchingTransport && ! s.transportV2 && s.transport.getState() == CollabTransport::State::waiting; }
};
}

namespace gocue::tests
{
using namespace livemix;
using namespace livemix::collab;
namespace
{
using State = CollabSession::State;
bool until (const std::function<bool()>& done, int timeout = 2500)
{
    const double deadline = juce::Time::getMillisecondCounterHiRes() + timeout;
    do
    {
        if (done()) return true;
        MSG message;
        for (int n = 0; n < 32 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++n)
        {
            TranslateMessage (&message); DispatchMessageW (&message);
        }
        juce::Thread::sleep (2);
    } while (juce::Time::getMillisecondCounterHiRes() < deadline);
    return done();
}
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* value = new juce::DynamicObject();
    for (const auto& field : fields) value->setProperty (field.first, field.second);
    return juce::var (value);
}
CollabRoom::Response response (const juce::var& value) { return { 200, juce::JSON::toString (value, true) }; }
const juce::String sessionHex = "fedcba9876543210", nonceHex = "0123456789abcdef0123456789abcdef";
constexpr uint64_t sessionNumber = 0xfedcba9876543210ULL;
int freePort()
{
    CollabTransport socket;
    socket.open (0, true);
    return socket.getLocalPort();
}
void probe (int port)
{
    Nonce nonce {};
    for (size_t n = 0; n < nonce.size(); ++n) nonce[n] = (uint8_t) nonceHex.substring ((int) n * 2, (int) n * 2 + 2).getHexValue32();
    Header h; h.kind = Kind::probe; h.payloadBytes = 16;
    std::array<uint8_t, maxDatagram> packet;
    const auto bytes = writePacket (packet.data(), packet.size(), h, nonce.data());
    juce::DatagramSocket udp;
    udp.write ("127.0.0.1", port, packet.data(), (int) bytes);
}

struct UpnpState
{
    CollabUpnp::Result result { true, true, "203.0.113.8", "192.168.1.9" };
    std::atomic<int> discovers { 0 }, renewals { 0 }, removals { 0 }, port { 0 };
    std::atomic<bool> ranOnMessageThread { false };
};
class FakeUpnp : public CollabUpnp
{
public:
    explicit FakeUpnp (std::shared_ptr<UpnpState> s) : state (std::move (s)) {}
    Result discover (int port) override
    {
        checkThread(); state->port.store (port); ++state->discovers; return state->result;
    }
    bool renew() override { checkThread(); ++state->renewals; return true; }
    void removeMapping() override { checkThread(); ++state->removals; }
private:
    void checkThread()
    {
        if (juce::MessageManager::getInstance()->isThisTheMessageThread()) state->ranOnMessageThread.store (true);
    }
    std::shared_ptr<UpnpState> state;
};

class FakeService
{
public:
    CollabRoom::Response request (const CollabRoom::Request& request)
    {
        if (juce::MessageManager::getInstance()->isThisTheMessageThread()) ranOnMessageThread.store (true);
        const auto action = request.url.fromLastOccurrenceOf ("/", false, false);
        const auto body = juce::JSON::parse (request.body);
        const std::lock_guard<std::mutex> lock (mutex);
        calls.push_back ({ action, body });
        if (action == "create")
        {
            port = (int) body["port"]; hostNick = body["nick"].toString(); present = true; confirmed = connected = false;
            hostStun = body.hasProperty ("stun"); guestCandidates.clear();
            if (sendProbe)
            {
                probe (port);
                juce::Thread::sleep (100); // server probe arrives before a delayed HTTP response
            }
            return response (object ({ { "code", roomCode }, { "session", sessionHex }, { "hostToken", "host-token" },
                                      { "publicIp", "203.0.113.8" }, { "nonce", nonceHex } }));
        }
        if (action == "confirm")
        {
            confirmed = (bool) body["reachable"];
            // like the room server: a closed port keeps the room only for hole punching with a known public address
            if (! confirmed && acceptPunch && (bool) body["punch"] && hostStun)
            {
                confirmed = true;
                return response (object ({ { "ok", true }, { "punch", true } }));
            }
            if (! confirmed) present = false;
        }
        if (action == "heartbeat")
        {
            connected = (bool) body["connected"];
            if (! heartbeatStatuses.empty())
            {
                const int status = heartbeatStatuses.front(); heartbeatStatuses.pop_front();
                if (status != 200) return { status, "{\"error\":\"room\"}" };
            }
            if (! guestCandidates.isEmpty())
                return response (object ({ { "ok", true }, { "guest", object ({ { "candidates", guestCandidates } }) } }));
        }
        if (action == "close") present = false;
        if (action == "join")
        {
            int status = joinStatus.load();
            if (status == 200 && ! present) status = 404;
            if (status == 200 && ! confirmed) status = 425;
            if (status == 200 && connected && body["inst"].toString() != guestInst) status = 409;
            if (status != 200) return { status, "{\"error\":\"room\"}" };
            guestInst = body["inst"].toString();
            guestCandidates.clear();
            if (body.hasProperty ("stun")) guestCandidates.add (object ({ { "ip", body["stun"]["ip"] }, { "port", body["stun"]["port"] } }));
            juce::Array<juce::var> candidates;
            candidates.add (object ({ { "ip", "127.0.0.1" }, { "port", unreachableHost.load() > 0 ? unreachableHost.load() : port } }));
            return response (object ({ { "session", sessionHex }, { "hostNick", hostNick },
                                      { "candidates", candidates }, { "sameNetwork", true } }));
        }
        return response (object ({ { "ok", true } }));
    }
    void openAt (int hostPort)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        port = hostPort; present = confirmed = true; connected = false; hostNick = "host";
    }
    int count (const juce::String& action)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return (int) std::count_if (calls.begin(), calls.end(), [&] (const auto& call) { return call.first == action; });
    }
    juce::var last (const juce::String& action)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        for (auto it = calls.rbegin(); it != calls.rend(); ++it) if (it->first == action) return it->second;
        return {};
    }
    bool sendProbe = true;
    void heartbeats (std::initializer_list<int> statuses, const juce::String& nextCode = "0842")
    {
        const std::lock_guard<std::mutex> lock (mutex);
        heartbeatStatuses = statuses; roomCode = nextCode;
    }
    std::atomic<int> joinStatus { 200 };
    std::atomic<bool> ranOnMessageThread { false };
    std::atomic<bool> acceptPunch { false };
    std::atomic<int> unreachableHost { 0 };   // > 0: the join hands the guest a dead host address, so only a punch can connect
private:
    std::mutex mutex;
    std::vector<std::pair<juce::String, juce::var>> calls;
    juce::Array<juce::var> guestCandidates;
    int port = 0;
    bool present = false, confirmed = false, connected = false, hostStun = false;
    juce::String hostNick, guestInst;
    juce::String roomCode = "0842";
    std::deque<int> heartbeatStatuses;
};

// Answers each STUN binding request with the asker's own address: on loopback no router changes it.
struct StunEcho
{
    StunEcho() { socket.bindToPort (0, "127.0.0.1"); port = socket.getBoundPort(); worker = std::thread ([this] { run(); }); }
    ~StunEcho() { stop.store (true); worker.join(); }
    void run()
    {
        while (! stop.load())
        {
            if (socket.waitUntilReady (true, 50) != 1) continue;
            std::array<uint8_t, 64> request; juce::String from; int fromPort = 0;
            const int n = socket.read (request.data(), (int) request.size(), false, from, fromPort);
            if (n < 20 || request[0] != 0x00 || request[1] != 0x01) continue;
            std::array<uint8_t, 32> reply { 0x01, 0x01, 0x00, 0x0c, 0x21, 0x12, 0xA4, 0x42 };
            std::copy (request.begin() + 8, request.begin() + 20, reply.begin() + 8);
            const int x = fromPort ^ 0x2112;
            const uint8_t attribute[12] { 0x00, 0x20, 0x00, 0x08, 0x00, 0x01, (uint8_t) (x >> 8), (uint8_t) x,
                                          127 ^ 0x21, 0 ^ 0x12, 0 ^ 0xA4, 1 ^ 0x42 };
            std::copy (std::begin (attribute), std::end (attribute), reply.begin() + 20);
            socket.write (from, fromPort, reply.data(), (int) reply.size());
        }
    }
    juce::DatagramSocket socket { false };
    int port = 0;
    std::atomic<bool> stop { false };
    std::thread worker;
};

struct TempDirectory
{
    TempDirectory() { file.createDirectory(); }
    ~TempDirectory() { file.deleteRecursively(); }
    juce::File file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LiveMixCollab-" + juce::Uuid().toString());
};
struct Fixture
{
    explicit Fixture (const std::shared_ptr<FakeService>& service, CollabUpnp::Result upnpResult = { true, true, "203.0.113.8", "192.168.1.9" },
                      int heartbeatMs = 200, bool loopbackOnly = true, std::vector<CollabTransport::Address> stun = {})
        : settings (directory.file), audio ("Local\\LiveMix.Collab.Test." + juce::Uuid().toString())
    {
        audio.prepare (48000, 120);
        settings.setCollabPort (freePort()); upnp->result = upnpResult;
        CollabSession::Options options;
        options.loopbackOnly = loopbackOnly; options.probeTimeoutMs = 250; options.connectTimeoutMs = 350;
        options.heartbeatIntervalMs = heartbeatMs; options.renewIntervalMs = 250;
        options.webUpnpFactory = [state = webUpnp] { return std::make_unique<FakeUpnp> (state); };
        if (! stun.empty())
        {
            options.stunServers = stun; options.stunTimeoutMs = 500; options.punchHeartbeatMs = 100;
            options.connectTimeoutMs = 3000;   // the guest waits for the host's knock instead of a reachable address
        }
        session = std::make_unique<CollabSession> (audio, settings,
            std::make_unique<CollabRoom> ([service] (const auto& request) { return service->request (request); }, "http://127.0.0.1:8899"),
            std::make_unique<FakeUpnp> (upnp), options);
    }
    bool reaches (State state, int timeout = 2500) { return until ([&] { return session->getSnapshot().state == state; }, timeout); }
    int port() { return CollabSessionTestAccess::transport (*session).getLocalPort(); }
    TempDirectory directory;
    LiveMixSettings settings;
    CollabAudio audio;
    std::shared_ptr<UpnpState> upnp = std::make_shared<UpnpState>();
    std::shared_ptr<UpnpState> webUpnp = std::make_shared<UpnpState>();
    std::unique_ptr<CollabSession> session;
};
struct Listener : juce::ChangeListener
{
    void changeListenerCallback (juce::ChangeBroadcaster*) override { ++changes; }
    int changes = 0;
};
}

class LiveMixCollabSessionTests : public juce::UnitTest
{
public:
    LiveMixCollabSessionTests() : UnitTest ("LiveMix Collab Session", "LiveMix") {}
    void runTest() override
    {
        beginTest ("host check table and exact failure text");
        {
            using Cause = CollabHostCheck::Cause;
            struct Row { bool received, found, mapped; const char* external; const char* publicIp; Cause cause; };
            const Row rows[] {
                { true, false, false, "", "203.0.113.8", Cause::none },
                { true, true, false, "10.0.0.1", "203.0.113.8", Cause::none },
                { false, false, false, "", "203.0.113.8", Cause::upnpDisabled },
                { false, true, false, "203.0.113.8", "203.0.113.8", Cause::upnpDisabled },
                { false, true, true, "10.1.2.3", "10.1.2.3", Cause::sharedAddress },
                { false, true, true, "172.16.0.1", "172.16.0.1", Cause::sharedAddress },
                { false, true, true, "172.31.255.254", "172.31.255.254", Cause::sharedAddress },
                { false, true, true, "192.168.0.1", "192.168.0.1", Cause::sharedAddress },
                { false, true, true, "100.64.0.1", "100.64.0.1", Cause::sharedAddress },
                { false, true, true, "100.127.255.254", "100.127.255.254", Cause::sharedAddress },
                { false, true, true, "203.0.113.9", "203.0.113.8", Cause::sharedAddress },
                { false, true, true, "203.0.113.8", "203.0.113.8", Cause::firewall },
                { false, true, true, "172.32.0.1", "172.32.0.1", Cause::firewall },
                { false, true, true, "100.128.0.1", "100.128.0.1", Cause::firewall }
            };
            for (const auto& row : rows)
            {
                const auto result = CollabHostCheck::evaluate (row.received, { row.found, row.mapped, row.external, {} }, row.publicIp);
                expect (result.cause == row.cause); expect (result.reachable == row.received);
                if (! result.reachable)
                    expect (result.message().startsWith (ko ("이 PC는 공유기 포트가 열리지 않아 방장을 할 수 없습니다. 상대방이 방을 만들어 주세요.\n")));
            }
            expectEquals (CollabHostCheck::evaluate (false, {}, "203.0.113.8").reason, ko ("공유기 자동 포트 열기(UPnP)가 꺼져 있습니다"));
            expectEquals (CollabHostCheck::evaluate (false, { true, true, "10.0.0.1", {} }, "203.0.113.8").reason,
                          ko ("공유기가 두 대 연결돼 있거나 통신사 공유 IP라 바깥에서 들어올 수 없습니다"));
            expectEquals (CollabHostCheck::evaluate (false, { true, true, "203.0.113.8", {} }, "203.0.113.8").reason,
                          ko ("윈도우 방화벽이나 보안 프로그램이 막고 있을 수 있습니다"));
        }

        beginTest ("injected HTTP requests match server JSON, hex values, headers and timeout");
        {
            std::vector<CollabRoom::Request> calls;
            CollabRoom room ([&] (const auto& request)
            {
                calls.push_back (request);
                if (request.url.endsWith ("/create"))
                    return response (object ({ { "code", "0842" }, { "session", sessionHex }, { "hostToken", "token" },
                                              { "publicIp", "203.0.113.8" }, { "nonce", nonceHex } }));
                if (request.url.endsWith ("/join"))
                {
                    juce::Array<juce::var> candidates;
                    candidates.add (object ({ { "ip", "192.168.1.9" }, { "port", 49731 } }));
                    candidates.add (object ({ { "ip", "203.0.113.8" }, { "port", 49731 } }));
                    return response (object ({ { "session", sessionHex }, { "hostNick", ko ("방장") }, { "candidates", candidates }, { "sameNetwork", true } }));
                }
                return response (object ({ { "ok", true } }));
            }, "http://127.0.0.1:8899/");
            const auto created = room.create (ko ("내 이름"), "install-id", 49731, { "192.168.1.9" }, { true, true, "203.0.113.8", "192.168.1.9" });
            expect (created.ok()); expect (created.session == sessionNumber); expectEquals ((int) created.nonce[15], 0xef);
            expect (room.confirm (created.code, created.hostToken, true, {}).ok());
            expect (room.heartbeat (created.code, created.hostToken, true).ok());
            const auto joined = room.join (created.code, ko ("참가자"), "guest-id");
            expect (joined.ok()); expect (joined.session == sessionNumber); expect (joined.sameNetwork);
            expectEquals ((int) joined.candidates.size(), 2);
            if (joined.candidates.size() == 2) expectEquals (joined.candidates[0].host, juce::String ("192.168.1.9"));
            expect (room.close (created.code, created.hostToken).ok());
            expectEquals ((int) calls.size(), 5);
            for (const auto& call : calls)
            {
                expect (call.url.startsWith ("http://127.0.0.1:8899/api/livemix/collab/"));
                expectEquals (call.headers, juce::String ("Content-Type: application/json\r\n")); expectEquals (call.timeoutMs, 8000);
            }
            auto body = juce::JSON::parse (calls[0].body);
            expectEquals (body["nick"].toString(), ko ("내 이름")); expectEquals (body["inst"].toString(), juce::String ("install-id"));
            expectEquals ((int) body["port"], 49731); expectEquals (body["lan"][0].toString(), juce::String ("192.168.1.9"));
            expect ((bool) body["upnp"]["found"] && (bool) body["upnp"]["mapped"]);
            expectEquals (body["upnp"]["externalIp"].toString(), juce::String ("203.0.113.8"));
            body = juce::JSON::parse (calls[1].body);
            expect ((bool) body["reachable"]); expect (body.hasProperty ("reason")); expectEquals (body["hostToken"].toString(), created.hostToken);
            expect ((bool) juce::JSON::parse (calls[2].body)["connected"]);
            body = juce::JSON::parse (calls[3].body);
            expectEquals (body["code"].toString(), juce::String ("0842")); expectEquals (body["inst"].toString(), juce::String ("guest-id"));
            expectEquals (juce::JSON::parse (calls[4].body)["hostToken"].toString(), created.hostToken);
        }

        beginTest ("create, early probe, heartbeat, mapping renewal, join, guest leave, rejoin and host leave");
        {
            auto service = std::make_shared<FakeService>(); Listener listener;
            Fixture host (service), guest (service);
            host.session->addChangeListener (&listener);
            host.session->createRoom (ko ("방장"));
            expect (host.reaches (State::hostWaiting));
            expectEquals (host.session->getSnapshot().code, juce::String ("0842"));
            expect ((bool) service->last ("confirm")["reachable"]);
            expectEquals (host.upnp->port.load(), host.port());
            expectEquals (host.webUpnp->port.load(), host.port() + 1);
            expectEquals (host.webUpnp->discovers.load(), 1);
            expect (until ([&] { return service->count ("heartbeat") > 0 && host.upnp->renewals.load() > 0
                && host.webUpnp->renewals.load() > 0; }));
            guest.session->joinRoom ("0842", ko ("참가자"));
            expect (guest.reaches (State::connected)); expect (host.reaches (State::connected));
            expectEquals (host.session->getSnapshot().peerNick, ko ("참가자"));
            expectEquals (guest.session->getSnapshot().peerNick, ko ("방장"));
            expect (until ([&] { return (bool) service->last ("heartbeat")["connected"]; }));
            guest.session->leave(); expect (host.reaches (State::hostWaiting));
            expect (host.session->getSnapshot().peerNick.isEmpty());
            expect (until ([&] { return guest.port() == 0; }));
            guest.session->joinRoom ("0842", ko ("다시 참가"));
            expect (guest.reaches (State::connected)); expect (host.reaches (State::connected));
            expectEquals (host.session->getSnapshot().peerNick, ko ("다시 참가"));
            expect (until ([&] { return guest.session->getSnapshot().rttMs > 0; }));
            expectGreaterThan (guest.session->getSnapshot().estimatedOneWayMs, 1.0);
            host.session->leave(); expect (guest.reaches (State::off));
            expectEquals (guest.session->getSnapshot().message, ko ("방장이 방을 닫았습니다"));
            expect (until ([&] { return service->count ("close") == 1 && host.upnp->removals.load() == 1
                && host.webUpnp->removals.load() == 1 && host.port() == 0; }));
            expect (! service->ranOnMessageThread.load()); expect (! host.upnp->ranOnMessageThread.load());
            expect (! host.webUpnp->ranOnMessageThread.load()); expectEquals (guest.webUpnp->discovers.load(), 0);
            expectGreaterThan (listener.changes, 0); host.session->removeChangeListener (&listener);
        }

        for (int roomRate : { 44100, 48000 })
        for (const auto devices : { std::pair<int, int> { 48000, 44100 }, { 44100, 48000 }, { 44100, 44100 } })
        {
            beginTest ("welcome chooses room " + juce::String (roomRate) + " before audio attach, devices "
                       + juce::String (devices.first) + "/" + juce::String (devices.second));
            auto service = std::make_shared<FakeService>();
            Fixture host (service), guest (service);
            const int guestPreference = roomRate == 44100 ? 48000 : 44100;
            host.settings.setCollabRate (roomRate); guest.settings.setCollabRate (guestPreference);
            host.audio.prepare (devices.first, devices.first / 100);
            guest.audio.prepare (devices.second, devices.second / 100);
            guest.audio.setWireRate (guestPreference);
            host.session->createRoom ("host");
            expect (host.reaches (State::hostWaiting));
            expectEquals ((int) host.session->getSnapshot().wireRate, roomRate);
            expectEquals ((int) host.audio.getWireRate(), roomRate);
            guest.session->joinRoom ("0842", "guest");
            expect (guest.reaches (State::connected)); expect (host.reaches (State::connected));
            expectEquals ((int) guest.audio.getWireRate(), roomRate);
            expectEquals ((int) guest.session->getSnapshot().wireRate, roomRate);
            expectEquals ((int) host.session->getSnapshot().wireRate, roomRate);
            expectEquals (guest.settings.getCollabRate(), guestPreference);
            expectEquals ((int) host.audio.getDeviceRate(), devices.first);
            expectEquals ((int) guest.audio.getDeviceRate(), devices.second);
            const auto& transport = CollabSessionTestAccess::transport (*guest.session);
            expectEquals ((int) transport.getStats().peer.wireRate, roomRate);
            const auto stats = transport.getStats();
            const double expectedDelay = packetDurationMs (roomRate) * 0.5 + stats.rttMs * 0.5
                + guest.audio.getStats().targetMs + 16000.0 / roomRate
                + (devices.first != roomRate ? 16000.0 / devices.first : 0.0);
            expectWithinAbsoluteError (transport.estimatedOneWayMs(), expectedDelay, 0.01);
            // A normal device restart must keep the room rate selected by welcome.
            guest.audio.prepare (96000, 960);
            expectEquals (guest.audio.getWireRate(), (uint32_t) roomRate);
            expectEquals (guest.audio.getDeviceRate(), 96000.0);
            expect (until ([&] { return CollabSessionTestAccess::transport (*host.session).getStats().peer.deviceRate == 96000; }));
            expectEquals ((int) CollabSessionTestAccess::transport (*host.session).getStats().peer.wireRate, roomRate);
            guest.session->leave(); expect (host.reaches (State::hostWaiting));
            expectEquals ((int) host.session->getSnapshot().wireRate, roomRate);
        }

        beginTest ("connected audio survives heartbeat failures and stops heartbeats after a missing room");
        {
            auto service = std::make_shared<FakeService>();
            Fixture host (service, {}, 60000), guest (service);
            host.session->createRoom ("host"); expect (host.reaches (State::hostWaiting));
            guest.session->joinRoom ("0842", "guest"); expect (guest.reaches (State::connected)); expect (host.reaches (State::connected));
            {
                CollabWindow window (*host.session, host.settings);
                window.open(); window.closeButtonPressed();
                expect (! window.isVisible()); expect (host.session->getSnapshot().state == State::connected);
                window.open(); window.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
                expect (! window.isVisible()); expect (host.session->getSnapshot().state == State::connected);
            }
            expect (until ([&] { return ! CollabSessionTestAccess::heartbeatPending (*host.session); }));
            const int port = host.port();
            service->heartbeats ({ 0, 503, 404 });
            for (int n = 0; n < 3; ++n)
            {
                CollabSessionTestAccess::heartbeat (*host.session);
                expect (until ([&] { return ! CollabSessionTestAccess::heartbeatPending (*host.session); }));
                expect (host.session->getSnapshot().state == State::connected);
                expectEquals (host.port(), port);
                auto& receiver = CollabSessionTestAccess::transport (*guest.session);
                const auto received = receiver.getStats().received;
                std::array<float, 120> pcm; pcm.fill (0.25f);
                host.audio.pushLocal (pcm.data(), pcm.data(), 120);
                expect (until ([&] { return receiver.getStats().received > received; }));
            }
            expect (CollabSessionTestAccess::heartbeatStopped (*host.session));
            const int calls = service->count ("heartbeat");
            CollabSessionTestAccess::heartbeat (*host.session);
            expectEquals (service->count ("heartbeat"), calls);
            expectEquals (service->count ("close"), 0);
        }

        beginTest ("a waiting host recreates a missing room with a fresh port check and new code");
        for (int status : { 404, 410 })
        {
            auto service = std::make_shared<FakeService>(); Fixture host (service, {}, 60000);
            host.session->createRoom ("host"); expect (host.reaches (State::hostWaiting));
            service->heartbeats ({ status }, "4827");
            CollabSessionTestAccess::heartbeat (*host.session);
            expect (host.reaches (State::checkingHost));
            expect (host.reaches (State::hostWaiting));
            expectEquals (host.session->getSnapshot().code, juce::String ("4827"));
            expectEquals (host.session->getSnapshot().myNick, juce::String ("host"));
            expectEquals (service->count ("create"), 2); expectEquals (service->count ("confirm"), 2);
            expectEquals (host.upnp->discovers.load(), 2);
        }

        beginTest ("waiting host retries temporary heartbeat errors twice, resets on success, fails on the third");
        for (int status : { 0, 503 })
        {
            auto service = std::make_shared<FakeService>(); Fixture host (service, {}, 60000);
            host.session->createRoom ("host"); expect (host.reaches (State::hostWaiting));
            service->heartbeats ({ status, status, 200, status, status, status });
            for (int i = 0; i < 6; ++i)
            {
                CollabSessionTestAccess::heartbeat (*host.session);
                expect (until ([&] { return ! CollabSessionTestAccess::heartbeatPending (*host.session); }));
                expect (host.session->getSnapshot().state == (i < 5 ? State::hostWaiting : State::error));
            }
            expectEquals (host.session->getSnapshot().message, CollabRoom::errorMessage (status));
            expect (until ([&] { return host.port() == 0; }));
        }

        beginTest ("a reconnecting host keeps its socket when the room server loses the room");
        {
            auto service = std::make_shared<FakeService>(); Fixture host (service, {}, 60000);
            host.session->createRoom ("host"); expect (host.reaches (State::hostWaiting));
            juce::DatagramSocket guest;
            expect (guest.bindToPort (0, "127.0.0.1"));
            std::array<uint8_t, 128> payload;
            Header h; h.kind = Kind::hello; h.session = sessionNumber; h.sender = 987;
            h.payloadBytes = (uint16_t) writePeerInfo (payload.data(), { "guest" });
            std::array<uint8_t, maxDatagram> packet;
            const auto bytes = writePacket (packet.data(), packet.size(), h, payload.data());
            guest.write ("127.0.0.1", host.port(), packet.data(), (int) bytes);
            expect (host.reaches (State::connected));
            expect (until ([&] { return host.session->getSnapshot().state == State::disconnected; }, 4500));
            expect (until ([&] { return ! CollabSessionTestAccess::heartbeatPending (*host.session); }));
            const int port = host.port();
            service->heartbeats ({ 404 }); CollabSessionTestAccess::heartbeat (*host.session);
            expect (until ([&] { return ! CollabSessionTestAccess::heartbeatPending (*host.session); }));
            expect (host.session->getSnapshot().state == State::disconnected); expectEquals (host.port(), port);
            expect (CollabSessionTestAccess::heartbeatStopped (*host.session));
        }

        beginTest ("loopback environment enables the same transport option used by local endpoint tests");
        {
            const auto previous = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_COLLAB_LOOPBACK", {});
            SetEnvironmentVariableW (L"LIVEMIX_COLLAB_LOOPBACK", L"1");
            auto service = std::make_shared<FakeService>(); Fixture host (service, {}, 60000, false);
            SetEnvironmentVariableW (L"LIVEMIX_COLLAB_LOOPBACK", previous.isEmpty() ? nullptr : previous.toWideCharPointer());
            expect (CollabSessionTestAccess::loopbackOnly (*host.session));
            host.session->createRoom ("host"); expect (host.reaches (State::hostWaiting));
        }

        beginTest ("missing probe confirms false, hides code, closes transport and removes mapping");
        {
            auto service = std::make_shared<FakeService>(); service->sendProbe = false;
            Fixture host (service, { false, false, {}, {} });
            host.session->createRoom ("host"); expect (host.reaches (State::hostUnavailable));
            expectEquals (service->count ("confirm"), 1); expect (! (bool) service->last ("confirm")["reachable"]);
            expectEquals (host.session->getSnapshot().reason, ko ("공유기 자동 포트 열기(UPnP)가 꺼져 있습니다"));
            expect (host.session->getSnapshot().code.isEmpty()); expectEquals (host.port(), 0);
            expectEquals (host.upnp->removals.load(), 1);
        }

        beginTest ("a closed port keeps the room by hole punching: the guest's public address comes back with the heartbeat and is punched");
        {
            // 10/16 two-PC test: doksan's router has UPnP off, so it could not host; both routers allow hole punching
            StunEcho stun;
            auto service = std::make_shared<FakeService>(); service->sendProbe = false; service->acceptPunch = true;
            Fixture host (service, { false, false, {}, {} }, 200, true, { { "127.0.0.1", stun.port } });
            host.session->createRoom ("host"); expect (host.reaches (State::hostWaiting));
            const auto create = service->last ("create"), confirm = service->last ("confirm");
            expectEquals (create["stun"]["ip"].toString(), juce::String ("127.0.0.1")); expectEquals ((int) create["stun"]["port"], host.port());
            expect (! (bool) confirm["reachable"] && (bool) confirm["punch"]);
            expect (CollabSessionTestAccess::punching (*host.session));
            int dead;
            { CollabTransport closed; closed.open (0, true); dead = closed.getLocalPort(); }
            service->unreachableHost = dead;   // only the host's knock can reach the guest now
            Fixture guest (service, {}, 200, true, { { "127.0.0.1", stun.port } });
            guest.session->joinRoom ("0842", "guest");
            expect (guest.reaches (State::connected, 5000)); expect (host.reaches (State::connected, 5000));
            const auto join = service->last ("join");
            expectEquals ((int) join["stun"]["port"], guest.port()); expectEquals ((int) join["port"], guest.port());
            expectGreaterThan ((int) CollabSessionTestAccess::transport (*host.session).getStats().punched, 0);
        }

        beginTest ("an older room server that closes the room on a failed check still ends hosting as before");
        {
            StunEcho stun;
            auto service = std::make_shared<FakeService>(); service->sendProbe = false;   // acceptPunch false = 0.15.2 server
            Fixture host (service, { false, false, {}, {} }, 200, true, { { "127.0.0.1", stun.port } });
            host.session->createRoom ("host"); expect (host.reaches (State::hostUnavailable));
            expect ((bool) service->last ("confirm")["punch"]); expect (! CollabSessionTestAccess::punching (*host.session));
            expect (host.session->getSnapshot().code.isEmpty()); expectEquals (host.port(), 0);
        }

        beginTest ("hole punching fields: create and join carry the public address, confirm asks to keep the room, heartbeat returns the guest");
        {
            std::vector<CollabRoom::Request> calls;
            CollabRoom room ([&] (const auto& request)
            {
                calls.push_back (request);
                if (request.url.endsWith ("/confirm")) return response (object ({ { "ok", true }, { "punch", true } }));
                if (request.url.endsWith ("/heartbeat"))
                {
                    juce::Array<juce::var> list;
                    list.add (object ({ { "ip", "198.51.100.4" }, { "port", 50123 } }));
                    list.add (object ({ { "ip", "not an ip" }, { "port", 1 } }));
                    list.add (object ({ { "ip", "198.51.100.4" }, { "port", 70000 } }));
                    return response (object ({ { "ok", true }, { "guest", object ({ { "candidates", list } }) } }));
                }
                if (request.url.endsWith ("/join"))
                {
                    juce::Array<juce::var> candidates;
                    candidates.add (object ({ { "ip", "203.0.113.8" }, { "port", 49731 } }));
                    return response (object ({ { "session", sessionHex }, { "hostNick", "host" }, { "candidates", candidates }, { "sameNetwork", false } }));
                }
                return response (object ({ { "code", "0842" }, { "session", sessionHex }, { "hostToken", "token" },
                                          { "publicIp", "203.0.113.8" }, { "nonce", nonceHex } }));
            }, "http://127.0.0.1:8899/");
            const CollabTransport::Mapping mapping { { "203.0.113.8", 61000 }, 2, true };
            expect (room.create ("host", "install-id", 49731, {}, {}, mapping).ok());
            auto body = juce::JSON::parse (calls.back().body);
            expectEquals (body["stun"]["ip"].toString(), juce::String ("203.0.113.8")); expectEquals ((int) body["stun"]["port"], 61000);
            expect (room.create ("host", "install-id", 49731, {}, {}, { { "203.0.113.8", 61000 }, 2, false }).ok());
            expect (! juce::JSON::parse (calls.back().body).hasProperty ("stun"));   // a symmetric NAT cannot be punched
            const auto confirmed = room.confirm ("0842", "token", false, "reason", true);
            expect (confirmed.ok() && confirmed.punch); expect ((bool) juce::JSON::parse (calls.back().body)["punch"]);
            expect (room.confirm ("0842", "token", true, {}).ok());
            expect (! juce::JSON::parse (calls.back().body).hasProperty ("punch"));
            const auto beat = room.heartbeat ("0842", "token", false);
            expect (beat.ok()); expectEquals ((int) beat.guest.size(), 1);
            if (! beat.guest.empty())
            {
                expectEquals (beat.guest[0].host, juce::String ("198.51.100.4")); expectEquals (beat.guest[0].port, 50123);
            }
            expect (room.join ("0842", "guest", "guest-id", mapping, 50999).ok());
            body = juce::JSON::parse (calls.back().body);
            expectEquals ((int) body["stun"]["port"], 61000); expectEquals ((int) body["port"], 50999);
        }

        beginTest ("the partner's OBS ring runs while a room is open, with or without the master's OBS switch");
        {
            auto service = std::make_shared<FakeService>();
            Fixture host (service);
            expect (! host.audio.getPeerObs().isEnabled());
            host.session->createRoom ("host"); expect (host.reaches (State::hostWaiting));
            expect (host.audio.getPeerObs().isEnabled());
            host.session->leave(); expect (! host.audio.getPeerObs().isEnabled());
            host.audio.setObsEnabled (true); expect (host.audio.getPeerObs().isEnabled());
            host.session->createRoom ("host"); expect (host.reaches (State::hostWaiting));
            host.session->leave(); expect (host.audio.getPeerObs().isEnabled());   // the master switch keeps it on
            host.audio.setObsEnabled (false); expect (! host.audio.getPeerObs().isEnabled());
        }

        beginTest ("manual port forwarding permits hosting when UPnP is absent");
        {
            auto service = std::make_shared<FakeService>();
            Fixture host (service, {});
            host.session->createRoom ("host"); expect (host.reaches (State::hostWaiting));
            expect ((bool) service->last ("confirm")["reachable"]);
            host.session.reset();
            expectEquals (service->count ("close"), 1); expectEquals (host.upnp->removals.load(), 1);
        }

        beginTest ("join HTTP error messages");
        {
            const std::pair<int, juce::String> rows[] {
                { 0, ko ("합방 서버에 연결하지 못했습니다") }, { 404, ko ("없는 방 코드입니다") }, { 410, ko ("닫힌 방입니다") },
                { 425, ko ("방장이 아직 확인 중입니다. 잠시 뒤 다시 참가하세요") }, { 409, ko ("이미 다른 사람이 들어와 있습니다") },
                { 403, ko ("합방 서버 오류 (403)") }, { 400, ko ("합방 서버 오류 (400)") }, { 503, ko ("합방 서버 오류 (503)") }
            };
            auto service = std::make_shared<FakeService>(); Fixture guest (service);
            for (const auto& row : rows)
            {
                service->joinStatus.store (row.first);
                guest.session->joinRoom ("0842", "guest"); expect (guest.reaches (State::error));
                expectEquals (guest.session->getSnapshot().message, row.second); expectEquals (guest.port(), 0);
            }
        }

        beginTest ("join timeout closes the socket and permits another join");
        {
            auto service = std::make_shared<FakeService>(); service->openAt (freePort());
            Fixture guest (service);
            guest.session->joinRoom ("0842", "guest"); expect (guest.reaches (State::error));
            expectEquals (guest.session->getSnapshot().message, ko ("방장 PC에 연결하지 못했습니다"));
            expect (until ([&] { return guest.port() == 0; }));
            CollabTransport host; expect (host.open (0, true).isEmpty()); host.startHost (sessionNumber, { "host" });
            service->openAt (host.getLocalPort());
            guest.session->joinRoom ("0842", "guest"); expect (guest.reaches (State::connected));
            guest.session->leave(); expect (until ([&] { return guest.port() == 0; }));
            expectEquals (service->count ("close"), 0); expectEquals (guest.upnp->discovers.load(), 0);
        }

        beginTest ("occupied host port reports the transport error before UPnP or create");
        {
            auto service = std::make_shared<FakeService>(); Fixture host (service);
            CollabTransport occupied; expect (occupied.open (0, true).isEmpty()); host.settings.setCollabPort (occupied.getLocalPort());
            host.session->createRoom ("host"); expect (host.reaches (State::error));
            expect (host.session->getSnapshot().message.contains (ko ("열 수 없습니다")));
            expectEquals (service->count ("create"), 0); expectEquals (host.upnp->discovers.load(), 0);
        }

        beginTest ("connected guest keeps its transport during disconnection and resumes on welcome");
        {
            auto service = std::make_shared<FakeService>(); Fixture guest (service);
            juce::DatagramSocket host;
            expect (host.bindToPort (0, "127.0.0.1")); service->openAt (host.getBoundPort());
            guest.session->joinRoom ("0842", "guest");
            expect (until ([&] { return CollabSessionTestAccess::transport (*guest.session).getState() == CollabTransport::State::connecting; }));
            auto welcome = [&]
            {
                std::array<uint8_t, 128> payload;
                Header h; h.kind = Kind::welcome; h.session = sessionNumber; h.sender = 987;
                h.payloadBytes = (uint16_t) writePeerInfo (payload.data(), { "host" });
                std::array<uint8_t, maxDatagram> packet;
                const auto bytes = writePacket (packet.data(), packet.size(), h, payload.data());
                host.write ("127.0.0.1", guest.port(), packet.data(), (int) bytes);
            };
            welcome(); expect (guest.reaches (State::connected));
            const int boundPort = guest.port();
            expect (until ([&] { return guest.session->getSnapshot().state == State::disconnected; }, 4500));
            expectEquals (guest.port(), boundPort);
            welcome(); expect (guest.reaches (State::connected)); expectEquals (guest.port(), boundPort);
        }

        beginTest ("app preferences persist independently, and LAN addresses exclude loopback and link-local");
        {
            TempDirectory directory;
            juce::String id;
            {
                LiveMixSettings settings (directory.file);
                expectEquals (settings.getCollabPort(), 49731); expectEquals (settings.getCollabMonitorVolume(), 1.0f); expect (settings.getCollabMonitorOn());
                id = settings.getCollabInstallId(); expect (id.isNotEmpty()); expect (id.containsOnly ("0123456789abcdefABCDEF-"));
                settings.setCollabNick (ko ("저장할 이름")); settings.setCollabPort (51234); settings.saveIfNeeded();
            }
            LiveMixSettings settings (directory.file);
            expectEquals (settings.getCollabInstallId(), id); expectEquals (settings.getCollabPort(), 51234);
            expectEquals (settings.getCollabNick(), ko ("저장할 이름"));
            auto service = std::make_shared<FakeService>(); Fixture fixture (service);
            fixture.session->setMonitor (0.4f, false); fixture.session->rebuffer();
            expectEquals (fixture.session->getSnapshot().monitorVolume, 0.4f); expect (! fixture.session->getSnapshot().monitorOn);
            LiveMixSettings reloaded (fixture.directory.file);
            expectEquals (reloaded.getCollabMonitorVolume(), 0.4f); expect (! reloaded.getCollabMonitorOn());
            for (const auto& ip : CollabSession::lanAddresses())
                expect (! ip.startsWith ("127.") && ! ip.startsWith ("169.254.") && ! ip.containsChar (':'));
        }
    }
};
static LiveMixCollabSessionTests liveMixCollabSessionTests;

namespace
{
namespace c = livemix::collab::contracts;
class V2Service
{
public:
    CollabRoom::RequestFunction requests() { return [this] (const auto& request) { return call (request); }; }
    juce::String pending (const juce::String& nick)
    {
        std::lock_guard<std::mutex> lock (mutex);
        const auto id = juce::Uuid().toString().removeCharacters ("-");
        auto value = object ({ { "requestId", id }, { "nick", nick }, { "expiresAtMs", juce::Time::currentTimeMillis() + (juce::int64) 60000 } });
        requestsById[id] = value; event (false, "request", value); return id;
    }
    void expire (const juce::String& id)
    {
        std::lock_guard<std::mutex> lock (mutex);
        requestsById.erase (id); event (false, "requestState", object ({ { "requestId", id }, { "state", "expired" } }));
    }
    void remoteSignal (uint32_t generation, const juce::var& value)
    {
        std::lock_guard<std::mutex> lock (mutex);
        event (true, "signal", object ({ { "connectionGeneration", (int) generation }, { "signalId", 1 }, { "from", "web" }, { "signal", value } }));
    }
    void reserveLegacy (int port)
    {
        std::lock_guard<std::mutex> lock (mutex);
        members.push_back ({ nextId++, "Old", "native", port, "connected", 1, true }); ++revision;
    }
    // A room a 0.16 host created through the old create API: it sent no wireRate, so the server reports its 48 kHz default.
    void legacyHost (int port)
    {
        std::lock_guard<std::mutex> lock (mutex);
        members = { { 1, "OldHost", "native", port, "connected", 1, true } }; wireRate = 48000; ++revision;
    }
    int count (const char* suffix)
    {
        std::lock_guard<std::mutex> lock (mutex);
        return (int) std::count_if (calls.begin(), calls.end(), [&] (const auto& item) { return item.first.endsWith (suffix); });
    }
    juce::var last (const char* suffix)
    {
        std::lock_guard<std::mutex> lock (mutex);
        for (auto i = calls.rbegin(); i != calls.rend(); ++i) if (i->first.endsWith (suffix)) return i->second;
        return {};
    }
    bool available = true;
    std::atomic<bool> onMessageThread { false };
private:
    struct Member { int id; juce::String nick, kind; int port; juce::String state; int epoch = 1; bool legacy = false; };
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Member> members;
    std::vector<std::pair<juce::String, juce::var>> calls;
    std::vector<std::pair<bool, juce::var>> events;
    std::map<juce::String, juce::var> requestsById;
    int nextId = 2, revision = 1, inviteGeneration = 0, eventId = 0, wireRate = 48000;
    juce::var room()
    {
        juce::Array<juce::var> list, reservations, names;
        for (const auto& cap : c::capabilityNames) names.add (cap.name);
        bool legacy = false;
        for (const auto& m : members)
        {
            legacy |= m.legacy;
            juce::Array<juce::var> protocols; protocols.add (m.kind == "web" ? 2 : 1);
            if (m.kind != "web" && ! m.legacy) protocols.add (2);
            list.add (object ({ { "participantId", m.id }, { "kind", m.kind }, { "nick", m.nick },
                { "appVersion", m.legacy ? "0.16.0" : "0.17.0" }, { "protocols", protocols },
                { "capabilities", m.legacy ? juce::Array<juce::var>() : names }, { "connectionGeneration", m.id },
                { "streamEpoch", m.epoch }, { "state", m.state } }));
        }
        return object ({ { "apiVersion", 2 }, { "code", "0842" }, { "session", sessionHex },
            { "roomMode", legacy ? "legacy2" : "v2" }, { "wireRate", wireRate }, { "revision", revision },
            { "capabilities", legacy ? juce::Array<juce::var>() : names }, { "nativeState", available ? "reachable" : "unavailable" },
            { "members", list }, { "reservations", reservations } });
    }
    void event (bool connection, const char* type, const juce::var& value)
    {
        events.push_back ({ connection, object ({ { "id", juce::String (++eventId) }, { "type", type }, { "data", value } }) });
        wake.notify_all();
    }
    CollabRoom::Response call (const CollabRoom::Request& request)
    {
        if (juce::MessageManager::getInstance()->isThisTheMessageThread()) onMessageThread.store (true);
        const auto action = request.url.fromLastOccurrenceOf ("/", false, false);
        const auto body = juce::JSON::parse (request.body);
        if (request.cancellation) request.cancellation->setHandler ([this] { std::lock_guard<std::mutex> lock (mutex); wake.notify_all(); });
        std::unique_lock<std::mutex> lock (mutex);
        calls.push_back ({ request.url, body });
        if (action == "events")
        {
            const bool connection = body.hasProperty ("connectionToken");
            const auto cursor = body["cursor"].toString().getLargeIntValue();
            const auto ready = [&]
            { return std::any_of (events.begin(), events.end(), [&] (const auto& e) { return e.first == connection && e.second["id"].toString().getLargeIntValue() > cursor; }); };
            wake.wait (lock, [&] { return ready() || (request.cancellation && request.cancellation->isCancelled()); });
            if (request.cancellation && request.cancellation->isCancelled()) return {};
            juce::Array<juce::var> result;
            for (const auto& e : events) if (e.first == connection && e.second["id"].toString().getLargeIntValue() > cursor) result.add (e.second);
            return response (object ({ { "apiVersion", 2 }, { "cursor", juce::String (eventId) }, { "events", result } }));
        }
        if (action == "create")
        {
            wireRate = (int) body["wireRate"];
            members = { { 1, body["nick"].toString(), "native", (int) body["port"], "connected" } };
            if (available) probe (members[0].port);
            auto value = room(); auto* o = value.getDynamicObject();
            o->setProperty ("hostToken", nonceHex); o->setProperty ("publicIp", "203.0.113.8"); o->setProperty ("nonce", nonceHex);
            o->setProperty ("participantId", 1); o->setProperty ("connectionGeneration", 1); o->setProperty ("streamEpoch", 1);
            return response (value);
        }
        if (action == "join")
        {
            if (members.size() >= 3) return { 409, "{\"error\":\"full\"}" };
            const int id = nextId++;
            members.push_back ({ id, body["nick"].toString(), "native", (int) body["port"], "connecting" }); ++revision;
            auto value = room(); auto* o = value.getDynamicObject();
            o->setProperty ("hostNick", members[0].nick); o->setProperty ("sameNetwork", true);
            o->setProperty ("candidates", juce::Array<juce::var> { object ({ { "ip", "127.0.0.1" }, { "port", members[0].port } }) });
            o->setProperty ("participantId", id); o->setProperty ("connectionGeneration", id); o->setProperty ("streamEpoch", 1);
            return response (value);
        }
        if (action == "heartbeat")
        {
            if (const auto* states = body["memberStates"].getArray())
                for (const auto& state : *states)
                {
                    const int id = (int) state["participantId"];
                    auto m = std::find_if (members.begin(), members.end(), [&] (const auto& p) { return p.id == id; });
                    if (m == members.end()) continue;
                    if (state["state"].toString() == "left") { members.erase (m); ++revision; }
                    else if (m->state != state["state"].toString() || m->epoch != (int) state["streamEpoch"])
                    { m->state = state["state"].toString(); m->epoch = (int) state["streamEpoch"]; ++revision; }
                }
        }
        if (action == "heartbeat" || action == "confirm")
        {
            auto value = room(); auto* o = value.getDynamicObject(); o->setProperty ("ok", true); o->setProperty ("punch", false);
            juce::Array<juce::var> peers;
            for (const auto& m : members) if (m.id != 1 && m.kind == "native")
                peers.add (object ({ { "participantId", m.id }, { "connectionId", juce::String::repeatedString ("1", 32) },
                    { "candidates", juce::Array<juce::var> { object ({ { "ip", "127.0.0.1" }, { "port", m.port } }) } } }));
            o->setProperty ("peers", peers); return response (value);
        }
        if (action == "invite")
        {
            ++inviteGeneration;
            for (const auto& pending : requestsById) event (false, "requestState", object ({ { "requestId", pending.first }, { "state", "invalidated" } }));
            requestsById.clear();
            return response (object ({ { "apiVersion", 2 }, { "inviteGeneration", inviteGeneration },
                { "inviteToken", juce::String::repeatedString (inviteGeneration == 1 ? "a" : "b", 64) }, { "hostNick", members[0].nick } }));
        }
        if (action == "accept")
        {
            const auto rid = body["requestId"].toString(); const auto pending = requestsById.find (rid);
            if (pending == requestsById.end()) return { 410, "{\"error\":\"expired\"}" };
            if (members.size() >= 3) return { 409, "{\"error\":\"full\"}" };
            const int id = nextId++; members.push_back ({ id, pending->second["nick"].toString(), "web", 0, "connecting" }); ++revision;
            requestsById.erase (pending);
            return response (object ({ { "apiVersion", 2 }, { "requestId", rid }, { "state", "accepted" }, { "participantId", id },
                { "streamEpoch", 1 }, { "connectionGeneration", id }, { "connectionToken", juce::String::repeatedString ("c", 64) },
                { "reservationExpiresAtMs", juce::Time::currentTimeMillis() + (juce::int64) 15000 }, { "room", room() } }));
        }
        if (action == "reject") requestsById.erase (body["requestId"].toString());
        if (action == "kick" || action == "leave")
        {
            const int generation = (int) body["connectionGeneration"];
            members.erase (std::remove_if (members.begin(), members.end(), [&] (const auto& m) { return m.id == generation; }), members.end()); ++revision;
            event (false, "ended", object ({ { "participantId", generation }, { "connectionGeneration", generation },
                                             { "reason", action == "kick" ? "kicked" : "left" } }));
        }
        return response (object ({ { "apiVersion", 2 }, { "ok", true }, { "signalId", body["signalId"] } }));
    }
};

class FakeWebEndpoint : public CollabSession::WebEndpoint
{
public:
    FakeWebEndpoint() : worker ([this] { run(); }) {}
    ~FakeWebEndpoint() override
    {
        { std::lock_guard<std::mutex> lock (mutex); stopping = true; } wake.notify_all(); worker.join();
    }
    void start (const c::WebConnection& connection, WebGuestTransport::Options options, WebGuestTransport::Callbacks value) override
    {
        { std::lock_guard<std::mutex> lock (mutex); current = connection; callbacks = value; lastOptions = options; }
        ++starts;
        value.onLocalDescription (connection.generation, "offer", "fake-sdp");
        value.onLocalCandidate (connection.generation, "candidate:fake", "0", 0);
        value.onGatheringComplete (connection.generation);
        value.onState (connection.generation, WebGuestTransport::State::open);
    }
    void description (uint32_t, const juce::String&, const juce::String&) override { ++descriptions; }
    void candidate (uint32_t, const juce::String&, const juce::String&, int) override { ++candidates; }
    void endCandidates (uint32_t) override { ++ends; }
    void sendTestAudio (uint32_t callbackGeneration)
    {
        std::lock_guard<std::mutex> lock (mutex);
        c::HeaderV2 h; h.session = current.session; h.origin = h.linkSender = current.guestId;
        h.streamEpoch = current.guestEpoch; h.rosterRevision = lastRoster; h.frames = 120; h.payloadBytes = 240;
        h.flags = c::Flags::monoPcm16;
        std::array<float, 120> mono; mono.fill (0.25f); std::array<uint8_t, 240> payload;
        packPcm16 (payload.data(), mono.data(), 120);
        std::vector<uint8_t> packet (288); writePacketV2 (packet.data(), packet.size(), h, payload.data());
        incoming.push_back ({ callbackGeneration, std::move (packet) }); wake.notify_all();
    }
    bool send (uint32_t generation, const uint8_t* data, size_t bytes, bool audio) override
    {
        if (audio) { ++audioPackets; return true; }
        c::HeaderV2 h; const uint8_t* payload = nullptr;
        if (! readPacketV2 (data, bytes, h, payload)) return false;
        std::lock_guard<std::mutex> lock (mutex);
        if (generation != current.generation) return false;
        if (h.kind == c::Kind::roster || h.kind == c::Kind::ping)
        {
            const bool ping = h.kind == c::Kind::ping;
            if (! ping) lastRoster = h.rosterRevision;
            h.kind = ping ? c::Kind::pong : c::Kind::rosterAck; h.origin = h.linkSender = current.guestId; h.payloadBytes = ping ? 8 : 0;
            std::vector<uint8_t> packet (1200);
            packet.resize (writePacketV2 (packet.data(), packet.size(), h, ping ? payload : nullptr));
            incoming.push_back ({ generation, std::move (packet) }); wake.notify_all();
        }
        return true;
    }
    void close() override
    {
        std::unique_lock<std::mutex> lock (mutex); current = {}; callbacks = {}; incoming.clear();
        wake.wait (lock, [&] { return ! delivering; }); ++closes;
    }
    std::atomic<int> starts { 0 }, closes { 0 }, descriptions { 0 }, candidates { 0 }, ends { 0 }, audioPackets { 0 };
    WebGuestTransport::Options getOptions()
    {
        std::lock_guard<std::mutex> lock (mutex); return lastOptions;
    }
private:
    void run()
    {
        std::unique_lock<std::mutex> lock (mutex);
        while (! stopping)
        {
            wake.wait (lock, [&] { return stopping || ! incoming.empty(); });
            if (stopping) break;
            auto packet = std::move (incoming.front()); incoming.pop_front(); auto cb = callbacks.onPacket; delivering = true;
            lock.unlock();
            if (cb) cb (packet.first, packet.second.data(), packet.second.size(),
                (uint64_t) (juce::Time::getMillisecondCounterHiRes() * 1000000.0));
            lock.lock(); delivering = false; wake.notify_all();
        }
    }
    std::mutex mutex;
    std::condition_variable wake;
    c::WebConnection current;
    uint32_t lastRoster = 0;
    WebGuestTransport::Callbacks callbacks;
    WebGuestTransport::Options lastOptions;
    std::deque<std::pair<uint32_t, std::vector<uint8_t>>> incoming;
    bool stopping = false, delivering = false;
    std::thread worker;
};

struct V2Fixture
{
    explicit V2Fixture (V2Service& service, int heartbeatMs = 100)
        : settings (directory.file), engine ("Local\\LiveMix.R8.Session." + juce::Uuid().toString())
    {
        engine.prepare (48000, 120); settings.setCollabPort (freePort());
        CollabSession::Options options; options.loopbackOnly = true; options.stunServers.clear();
        options.probeTimeoutMs = 250; options.heartbeatIntervalMs = heartbeatMs; options.punchHeartbeatMs = 100;
        options.webRequest = service.requests(); options.webServer = "http://127.0.0.1:8899"; options.webEndpoint = web;
        options.webUpnpFactory = [state = webUpnp] { return std::make_unique<FakeUpnp> (state); };
        session = std::make_unique<CollabSession> (engine, settings, std::make_unique<CollabRoom> (service.requests(), options.webServer),
            std::make_unique<FakeUpnp> (upnp), options);
    }
    const CollabSession::PersonSnapshot* person (const CollabSession::Snapshot& snapshot, uint32_t id)
    {
        for (const auto& p : snapshot.people) if (p.participantId == id) return &p;
        return nullptr;
    }
    bool people (int n) { return until ([&] { return (int) session->getSnapshot().personCount == n; }); }
    TempDirectory directory;
    LiveMixSettings settings;
    MixEngine engine;
    std::shared_ptr<FakeWebEndpoint> web = std::make_shared<FakeWebEndpoint>();
    std::shared_ptr<UpnpState> upnp = std::make_shared<UpnpState>(), webUpnp = std::make_shared<UpnpState>();
    std::unique_ptr<CollabSession> session;
};

// A silent device whose driver latency follows its buffer size. Opening it uses
// the real AudioDeviceManager -> MixEngine::audioDeviceAboutToStart path.
class RestartLatencyDevice : public juce::AudioIODevice
{
public:
    RestartLatencyDevice (const juce::String& type, const juce::String& input, const juce::String& output)
        : AudioIODevice (output.isNotEmpty() ? output : input, type), hasInput (input.isNotEmpty()), hasOutput (output.isNotEmpty()) {}
    ~RestartLatencyDevice() override { close(); }
    juce::StringArray getInputChannelNames() override { return hasInput ? juce::StringArray { "In L", "In R" } : juce::StringArray {}; }
    juce::StringArray getOutputChannelNames() override { return hasOutput ? juce::StringArray { "Out L", "Out R" } : juce::StringArray {}; }
    juce::Array<double> getAvailableSampleRates() override { return { 48000.0, 44100.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 256, 512 }; }
    int getDefaultBufferSize() override { return 256; }
    juce::String open (const juce::BigInteger& ins, const juce::BigInteger& outs, double sr, int bs) override
    {
        close(); rate = sr; block = bs; inputs = ins; outputs = outs;
        inputs.setRange (hasInput ? 2 : 0, 256, false); outputs.setRange (hasOutput ? 2 : 0, 256, false);
        opened = true; return {};
    }
    void close() override { stop(); opened = false; }
    bool isOpen() override { return opened; }
    void start (juce::AudioIODeviceCallback* cb) override { callback = cb; if (cb) cb->audioDeviceAboutToStart (this); }
    void stop() override { if (callback) callback->audioDeviceStopped(); callback = nullptr; }
    bool isPlaying() override { return callback != nullptr; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return block; }
    double getCurrentSampleRate() override { return rate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveInputChannels() const override { return inputs; }
    juce::BigInteger getActiveOutputChannels() const override { return outputs; }
    int getInputLatencyInSamples() override { return hasInput ? block : 0; }
    int getOutputLatencyInSamples() override { return hasOutput ? block * 2 : 0; }
private:
    const bool hasInput, hasOutput;
    bool opened = false;
    double rate = 48000;
    int block = 256;
    juce::BigInteger inputs, outputs;
    juce::AudioIODeviceCallback* callback = nullptr;
};
class RestartLatencyDeviceType : public juce::AudioIODeviceType
{
public:
    explicit RestartLatencyDeviceType (bool split) : AudioIODeviceType (split ? "Windows Audio" : "ASIO") {}
    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool input) const override { return { input || getTypeName() == "ASIO" ? "Capture" : "Monitor" }; }
    int getDefaultDeviceIndex (bool) const override { return 0; }
    bool hasSeparateInputsAndOutputs() const override { return getTypeName() != "ASIO"; }
    int getIndexOfDevice (juce::AudioIODevice*, bool) const override { return 0; }
    juce::AudioIODevice* createDevice (const juce::String& output, const juce::String& input) override
    { return new RestartLatencyDevice (getTypeName(), input, output); }
};
struct SessionMetadataView
{
    explicit SessionMetadataView (const juce::String& ring)
    {
        handle = OpenFileMappingW (FILE_MAP_READ, FALSE, (ring + ".Metadata").toWideCharPointer());
        if (handle) data = (const lm_obs_metadata*) MapViewOfFile (handle, FILE_MAP_READ, 0, 0, 256);
    }
    ~SessionMetadataView() { if (data) UnmapViewOfFile (data); if (handle) CloseHandle (handle); }
    HANDLE handle = nullptr; const lm_obs_metadata* data = nullptr;
};
}

class LiveMixCollabRestartLatencyTests : public juce::UnitTest
{
public:
    LiveMixCollabRestartLatencyTests() : UnitTest ("LiveMix Collab restart latency", "LiveMix") {}
    void runTest() override
    {
        for (const bool split : { false, true })
        for (const bool hostRestarts : { false, true })
        {
            beginTest (juce::String (hostRestarts ? "host" : "guest") + (split ? " split monitor" : " ASIO")
                       + " buffer reopen updates roster and third person's estimatedOneWayMs across heartbeat");
            // A full room's normal heartbeat is deferred so it cannot conceal a
            // stale host restart. Explicit heartbeats below exercise reconciliation.
            V2Service service; V2Fixture host (service, 60000), first (service, 60000), observer (service, 60000);
            auto& changed = hostRestarts ? host : first;
            const uint32_t pid = hostRestarts ? 1u : 2u;
            auto& manager = changed.engine.getDeviceManager();
            while (! manager.getAvailableDeviceTypes().isEmpty()) manager.removeAudioDeviceType (manager.getAvailableDeviceTypes().getLast());
            manager.addAudioDeviceType (std::make_unique<RestartLatencyDeviceType> (split));
            const auto open = [&] (int block, double rate)
            {
                const auto error = changed.engine.openDevice ({ split ? "Windows Audio" : "ASIO", "Capture",
                                                               split ? "Monitor" : "Capture", block, rate });
                expect (error.isEmpty(), error);
            };
            open (256, 48000);
            expect (changed.engine.isSplitMonitor() == split);
            host.session->createRoom ("Host");
            expect (until ([&] { return host.session->getSnapshot().canCreateWebInvite; }));
            first.session->joinRoom ("0842", "First"); expect (host.people (2));
            expect (until ([&] { return first.session->getSnapshot().state == State::connected; }));
            observer.session->joinRoom ("0842", "Observer");
            expect (host.people (3)); expect (first.people (3)); expect (observer.people (3));
            expect (until ([&] { return CollabSessionTestAccess::transport (*host.session).getStats (3).v2; }));
            const auto settle = [&]
            {
                const auto end = juce::Time::getMillisecondCounterHiRes() + 1200;
                until ([&]
                {
                    for (auto* f : { &host, &first, &observer }) f->engine.getCollab().pushLocalSilence (120);
                    return juce::Time::getMillisecondCounterHiRes() >= end;
                }, 2000);
            };
            const auto check = [&] (const char* stage, bool initial = false)
            {
                const auto local = CollabSessionTestAccess::localInfo (*changed.session);
                // The initial guest hello rounds to us; session/v2 restarts truncate.
                const auto micros = [&] (double ms) { return (uint32_t) (initial && ! hostRestarts ? std::lround (ms * 1000) : ms * 1000); };
                const auto inputUs = micros (local.inputLatencyMs), outputUs = micros (local.outputLatencyMs);
                const auto* member = findMember (CollabSessionTestAccess::roster (*host.session), pid);
                expect (member != nullptr);
                if (member)
                {
                    logMessage (juce::String (stage) + ": roster input/output us=" + juce::String (member->inputLatencyUs)
                                + "/" + juce::String (member->outputLatencyUs) + ", expected=" + juce::String (inputUs) + "/" + juce::String (outputUs));
                    expectEquals ((int) member->inputLatencyUs, (int) inputUs); expectEquals ((int) member->outputLatencyUs, (int) outputUs);
                    expectEquals ((int) member->deviceRate, (int) local.deviceRate);
                }
                const auto snapshot = observer.session->getSnapshot();
                const auto* person = observer.person (snapshot, pid);
                expect (person != nullptr);
                if (person)
                {
                    const double otherDelay = person->rttMs * 0.5 + person->bufferMs + 0.5 * packetDurationMs (snapshot.wireRate)
                        + 16000.0 / snapshot.wireRate + CollabSessionTestAccess::localInfo (*observer.session).outputLatencyMs
                        + (local.deviceRate != snapshot.wireRate ? 16000.0 / local.deviceRate : 0.0);
                    logMessage (juce::String (stage) + ": estimatedOneWayMs input contribution="
                                + juce::String (person->estimatedOneWayMs - otherDelay, 3) + ", expected=" + juce::String (inputUs / 1000.0, 3));
                    expectWithinAbsoluteError (person->estimatedOneWayMs, otherDelay + inputUs / 1000.0, 0.00001);
                }
            };
            settle(); check ("initial", true);
            for (const auto rate : { 48000.0, 44100.0 })
            {
                if (rate == 44100.0)
                    beginTest (juce::String (hostRestarts ? "host" : "guest") + (split ? " split monitor" : " ASIO")
                               + " sample rate reopen updates roster and estimatedOneWayMs across heartbeat");
                const auto before = changed.session->getSnapshot();
                const auto* self = changed.person (before, pid);
                expect (self != nullptr);
                const auto epoch = self ? self->streamEpoch : 0;
                open (512, rate);
                settle();
                const auto after = observer.session->getSnapshot();
                const auto* person = observer.person (after, pid);
                expect (person != nullptr && person->streamEpoch == epoch + 1);
                check ("reopened");
                CollabSessionTestAccess::heartbeat (*host.session);
                expect (until ([&] { return ! CollabSessionTestAccess::heartbeatPending (*host.session); }));
                settle(); check ("after heartbeat");
            }
        }
    }
};
static LiveMixCollabRestartLatencyTests liveMixCollabRestartLatencyTests;

class LiveMixCollabRosterAudioInstallationTests : public juce::UnitTest
{
public:
    LiveMixCollabRosterAudioInstallationTests() : UnitTest ("LiveMix Collab roster audio installation", "LiveMix") {}
    void runTest() override
    {
        beginTest ("normal third-person leave and rejoin never sends audio ahead of the installed roster");
        V2Service service; V2Fixture host (service), first (service), second (service);
        host.session->createRoom ("Host");
        expect (until ([&] { return host.session->getSnapshot().canCreateWebInvite; }));
        first.session->joinRoom ("0842", "First"); expect (host.people (2));
        expect (until ([&] { return first.session->getSnapshot().state == State::connected; }));
        second.session->joinRoom ("0842", "Second");
        expect (host.people (3)); expect (first.people (3)); expect (second.people (3));
        expect (until ([&] { return CollabSessionTestAccess::transport (*host.session).getStats (3).v2; }));

        std::atomic<bool> running { true };
        std::thread device ([&]
        {
            std::array<float, 120> pcm; pcm.fill (0.125f);
            auto deadline = std::chrono::steady_clock::now();
            while (running.load())
            {
                for (auto* fixture : { &host, &first, &second })
                {
                    auto& audio = fixture->engine.getCollab();
                    audio.pushLocal (pcm.data(), pcm.data(), 120); audio.renderSources (120);
                }
                deadline += std::chrono::microseconds (2500); std::this_thread::sleep_until (deadline);
            }
        });
        const auto settle = []
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + 3000;
            until ([&] { return juce::Time::getMillisecondCounterHiRes() >= end; }, 3500);
        };
        // The message loop runs normally throughout; actions are seconds apart, just as in R9 nnn.
        settle();
        auto& h = host.engine.getCollab(); auto& f = first.engine.getCollab();
        const auto hostDrops = h.getFutureRevisionDrops(), firstDrops = f.getFutureRevisionDrops();
        const auto hostReceived = h.getStats (0).received, firstReceived = f.getStats (0).received;
        second.session->leave(); expect (host.people (2)); expect (first.people (2));
        settle();
        second.session->joinRoom ("0842", "Second");
        expect (host.people (3)); expect (first.people (3)); expect (second.people (3));
        settle();
        running.store (false); device.join();
        const auto hostRejected = h.getFutureRevisionDrops() - hostDrops;
        const auto firstRejected = f.getFutureRevisionDrops() - firstDrops;
        logMessage ("future-revision rejects during leave/rejoin: host=" + juce::String (hostRejected)
                    + ", first=" + juce::String (firstRejected));
        expectEquals (hostRejected, uint64_t (0)); expectEquals (firstRejected, uint64_t (0));
        expectGreaterThan (h.getStats (0).received, hostReceived);
        expectGreaterThan (f.getStats (0).received, firstReceived);
        expect (host.session->getSnapshot().state == State::connected);
        expect (first.session->getSnapshot().state == State::connected);
    }
};
static LiveMixCollabRosterAudioInstallationTests liveMixCollabRosterAudioInstallationTests;

class LiveMixCollabR8SessionTests : public juce::UnitTest
{
public:
    LiveMixCollabR8SessionTests() : UnitTest ("LiveMix Collab R8 session integration", "LiveMix") {}
    void runTest() override
    {
        beginTest ("old server keeps the v1 connection and exposes the same people and volume commands");
        {
            auto service = std::make_shared<FakeService>(); Fixture host (service), guest (service);
            host.session->setMonitor (0.3f, true); host.session->createRoom ("Host");
            expect (host.reaches (State::hostWaiting));
            guest.session->joinRoom ("0842", "Guest");
            expect (host.reaches (State::connected)); expect (guest.reaches (State::connected));
            auto snapshot = host.session->getSnapshot();
            expect (snapshot.roomMode == c::RoomMode::legacy2 && snapshot.nativeAvailable);
            expect (guest.session->getSnapshot().nativeAvailable);
            expect (! snapshot.canCreateWebInvite && ! CollabSessionTestAccess::transport (*host.session).getStats().v2);
            expectEquals ((int) snapshot.personCount, 2);
            expectEquals (snapshot.people[1].slot, 1); expectEquals (snapshot.people[1].nickname, juce::String ("Guest"));
            expectEquals (snapshot.people[1].monitorVolume, 0.3f);
            host.session->setPersonMonitorVolume (2, 0.6f);
            expect (until ([&] { return host.session->getSnapshot().people[1].monitorVolume == 0.6f; }));
            expectEquals (host.settings.getCollabPersonVolume ("Guest"), 0.6f);
        }
        beginTest ("host plus two native guests: roster, persistent seats, person volumes and metadata");
        {
            V2Service service; V2Fixture host (service), first (service), second (service);
            host.session->setMonitor (0.4f, true); host.session->createRoom ("Host");
            expect (until ([&] { return host.session->getSnapshot().canCreateWebInvite; }));
            const auto takes = host.directory.file.getChildFile ("takes");
            expect (host.engine.startRecording ({ recording::Format::wav, 320, takes.getFullPathName().toStdString() }));
            expect (until ([&] { return host.engine.getRecorder().snapshot().state == recording::State::recording; }));
            expect (host.engine.getRecorder().snapshot().collabAtStart && host.engine.getRecorder().snapshot().fileCount == 4);
            first.session->joinRoom ("0842", "First"); expect (host.people (2));
            expect (until ([&] { return first.session->getSnapshot().state == State::connected; }));
            second.session->joinRoom ("0842", "Second"); expect (host.people (3)); expect (first.people (3)); expect (second.people (3));
            expect (until ([&] { return CollabSessionTestAccess::transport (*host.session).getStats (3).v2; }));
            auto hs = host.session->getSnapshot(); auto fs = first.session->getSnapshot(); auto ss = second.session->getSnapshot();
            expect (host.person (hs, 2) && host.person (hs, 3) && first.person (fs, 1) && second.person (ss, 1));
            if (host.person (hs, 2) && host.person (hs, 3))
            {
                expectEquals (host.person (hs, 2)->slot, 1); expectEquals (host.person (hs, 3)->slot, 2);
                expectEquals (host.person (hs, 2)->monitorVolume, 0.4f);
                expectEquals (host.person (hs, 3)->monitorVolume, 0.4f);
            }
            if (first.person (fs, 1)) expectEquals (first.person (fs, 1)->slot, 1);
            if (second.person (ss, 1)) expectEquals (second.person (ss, 1)->slot, 1);
            SessionMetadataView one (host.engine.getPeerRingNames()[0]), two (host.engine.getPeerRingNames()[1]);
            expect (one.data && two.data);
            if (one.data && two.data)
            {
                expectEquals ((int) one.data->participant_id, 2); expectEquals ((int) two.data->participant_id, 3);
                expectEquals (juce::String::fromUTF8 (two.data->nickname_utf8), juce::String ("Second"));
            }
            host.session->setPersonMonitorVolume (2, 0.2f); host.session->setPersonMonitorVolume (3, 0.7f);
            expect (until ([&] { return host.settings.getCollabPersonVolume ("Second") == 0.7f; }));
            hs = host.session->getSnapshot();
            juce::AudioBuffer<float> monitor (2, 120);
            for (uint64_t n = 0; n < 100; ++n)
            {
                collabtest::receiveTone (host.engine.getCollab(), 2, n, 0.25f, false, hs.rosterRevision);
                collabtest::receiveTone (host.engine.getCollab(), 3, n, 0.125f, false, hs.rosterRevision);
                host.engine.renderBlock (nullptr, 0, monitor.getArrayOfWritePointers(), 2, 120);
            }
            expectWithinAbsoluteError (monitor.getSample (0, 119), 0.1375f, 0.00001f);
            host.engine.getRecorder().finish();
            auto files = takes.findChildFiles (juce::File::findFiles, true, "*.wav");
            expect (std::any_of (files.begin(), files.end(), [] (const auto& f) { return f.getFileName() == "B_First.wav"; }));
            expect (std::any_of (files.begin(), files.end(), [] (const auto& f) { return f.getFileName() == "C_Second.wav"; }));
            host.session->setMonitor (0.9f, false); hs = host.session->getSnapshot();
            if (host.person (hs, 2)) expectEquals (host.person (hs, 2)->monitorVolume, 0.2f);
            expect (! hs.monitorOn); expect (! hs.canCreateWebInvite);
            first.session->leave(); expect (host.people (2));
            hs = host.session->getSnapshot();
            if (host.person (hs, 3)) { expectEquals (host.person (hs, 3)->slot, 2); expectEquals (host.person (hs, 3)->monitorVolume, 0.7f); }
            expect (hs.state == State::connected);
            if (one.data) expectEquals ((int) one.data->participant_id, 0);
            first.session->joinRoom ("0842", "First"); expect (host.people (3)); hs = host.session->getSnapshot();
            const auto* returning = host.person (hs, 4); expect (returning != nullptr);
            if (returning) { expectEquals (returning->slot, 1); expectEquals (returning->monitorVolume, 0.2f); }
            host.session->leave();
            if (one.data && two.data) expect (one.data->participant_id == 0 && two.data->participant_id == 0);
            expect (! service.onMessageThread.load());
        }
        beginTest ("web mapping is independent, renewed, removed and passed only with a public address");
        for (int scenario = 0; scenario < 4; ++scenario)
        {
            V2Service service; V2Fixture host (service);
            host.webUpnp->result.externalIp = scenario == 1 ? "192.168.1.1" : "1.1.1.1";
            host.webUpnp->result.mapped = scenario != 2;
            host.upnp->result.mapped = scenario != 3;
            host.session->createRoom ("Host");
            expect (until ([&] { return host.session->getSnapshot().canCreateWebInvite; }));
            expectEquals (host.webUpnp->discovers.load(), 1);
            expectEquals (host.webUpnp->port.load(), host.settings.getCollabPort() + 1);
            CollabSessionTestAccess::renewMappings (*host.session);
            expect (until ([&] { return scenario == 2 ? host.upnp->renewals.load() == 1 : host.webUpnp->renewals.load() == 1; }));
            expectEquals (host.upnp->renewals.load(), scenario == 3 ? 0 : 1);
            expectEquals (host.webUpnp->renewals.load(), scenario == 2 ? 0 : 1);
            host.session->createWebInvite();
            expect (until ([&] { return host.session->getSnapshot().inviteGeneration == 1; }));
            const auto request = service.pending ("Browser");
            expect (until ([&] { return host.session->getSnapshot().webRequestCount == 1; }));
            host.session->acceptWebRequest (request);
            expect (until ([&] { return host.web->starts.load() == 1; }));
            const auto options = host.web->getOptions();
            expectEquals ((int) options.localPort, scenario == 2 ? 0 : host.settings.getCollabPort() + 1);
            expectEquals (options.externalIp, scenario == 1 || scenario == 2 ? juce::String() : juce::String ("1.1.1.1"));
            if (options.externalIp.isNotEmpty()) expectEquals (options.lanIp, host.webUpnp->result.lanIp);
            host.session->leave();
            expect (until ([&] { return host.upnp->removals.load() == 1 && host.webUpnp->removals.load() == 1; }));
            expect (! host.webUpnp->ranOnMessageThread.load());
        }

        beginTest ("no router found for the room's port: the web port is not discovered a second time");
        {
            V2Service service; V2Fixture host (service);
            host.upnp->result = {};   // found = false, mapped = false
            host.session->createRoom ("Host");
            expect (until ([&] { return host.session->getSnapshot().canCreateWebInvite; }));
            expectEquals (host.webUpnp->discovers.load(), 0);
            host.session->createWebInvite();
            expect (until ([&] { return host.session->getSnapshot().inviteGeneration == 1; }));
            const auto request = service.pending ("Browser");
            expect (until ([&] { return host.session->getSnapshot().webRequestCount == 1; }));
            host.session->acceptWebRequest (request);
            expect (until ([&] { return host.web->starts.load() == 1; }));
            expectEquals ((int) host.web->getOptions().localPort, 0);
            host.session->leave();
        }

        beginTest ("host plus native plus web: invite, reject, expiry, accept, signals and kick");
        {
            V2Service service; V2Fixture host (service), native (service);
            host.session->createRoom ("Host"); expect (until ([&] { return host.session->getSnapshot().canCreateWebInvite; }));
            native.session->joinRoom ("0842", "Native"); expect (host.people (2));
            host.session->createWebInvite(); expect (until ([&] { return host.session->getSnapshot().inviteGeneration == 1; }));
            auto hs = host.session->getSnapshot();
            expectEquals (hs.webInviteUrl, juce::String (c::webOrigin) + c::joinPath + "#v=2&code=0842&invite=" + juce::String::repeatedString ("a", 64) + "&generation=1");
            expectEquals (hs.webInviteUrl, hs.webInviteQrUrl);
            auto request = service.pending ("Rejected"); expect (until ([&] { return host.session->getSnapshot().webRequestCount == 1; }));
            host.session->rejectWebRequest (request); expect (until ([&] { return host.session->getSnapshot().webRequestCount == 0; }));
            request = service.pending ("Expired"); expect (until ([&] { return host.session->getSnapshot().webRequestCount == 1; }));
            service.expire (request); expect (until ([&] { return host.session->getSnapshot().webRequestCount == 0; }));
            request = service.pending ("Old invite"); expect (until ([&] { return host.session->getSnapshot().webRequestCount == 1; }));
            host.session->createWebInvite(); expect (until ([&] { return host.session->getSnapshot().inviteGeneration == 2 && host.session->getSnapshot().webRequestCount == 0; }));
            request = service.pending ("Browser"); expect (until ([&] { return host.session->getSnapshot().webRequestCount == 1; }));
            expect (host.session->getSnapshot().webRequests[0].canAccept);
            host.session->acceptWebRequest (request); expect (host.people (3)); expect (native.people (3));
            expect (until ([&] { return CollabSessionTestAccess::transport (*host.session).getStats (3).v2; }));
            expect (until ([&] { return service.last ("/signal")["signal"]["type"].toString() == "connected"; }));
            hs = host.session->getSnapshot(); expectEquals ((int) hs.kickableWebId, 3); expect (! hs.canCreateWebInvite);
            expectEquals ((int) hs.webRequestCount, 0); expectEquals (host.web->starts.load(), 1);
            const auto* browser = host.person (hs, 3); expect (browser && browser->kind == c::ParticipantKind::web && browser->slot == 2);
            juce::AudioBuffer<float> output (2, 120);
            for (int n = 0; n < 4; ++n) host.engine.renderBlock (nullptr, 0, output.getArrayOfWritePointers(), 2, 120);
            expect (until ([&] { return host.web->audioPackets > 0; }));
            service.remoteSignal (3, object ({ { "type", "answer" }, { "sdp", "fake-answer" } }));
            service.remoteSignal (3, object ({ { "type", "candidate" }, { "candidate", "fake-candidate" }, { "sdpMid", "0" }, { "sdpMLineIndex", 0 } }));
            service.remoteSignal (3, object ({ { "type", "endOfCandidates" } }));
            expect (until ([&] { return host.web->descriptions == 1 && host.web->candidates == 1 && host.web->ends == 1; }));
            const auto extra = service.pending ("Waiting"); expect (until ([&] { return host.session->getSnapshot().webRequestCount == 1; }));
            expect (! host.session->getSnapshot().webRequests[0].canAccept);
            host.session->kickWebGuest (3); expect (host.people (2)); expect (native.people (2));
            expect (until ([&] { return host.session->getSnapshot().canCreateWebInvite; }));
            expect (host.session->getSnapshot().state == State::connected); expectEquals ((int) host.session->getSnapshot().kickableWebId, 0);
            host.session->acceptWebRequest (extra); expect (host.people (3));
            expect (until ([&] { return CollabSessionTestAccess::transport (*host.session).getStats (4).v2; }));
            host.web->sendTestAudio (3); // an earlier PeerConnection's generation is ignored even with the new participant's packet
            host.web->sendTestAudio (4);
            expect (until ([&] { return host.engine.getCollab().getStats (1).received > 0; }));
            expectEquals ((int) host.engine.getCollab().getStats (1).received, 1);
            host.session.reset(); expectEquals ((int) service.last ("/close")["apiVersion"], 2);
            expectEquals (host.webUpnp->removals.load(), 1);
            expect (! service.onMessageThread.load());
        }
        beginTest ("v2 web-only host remains open when native UDP is unavailable");
        {
            V2Service service; service.available = false; V2Fixture host (service);
            host.session->createRoom ("Host"); expect (until ([&] { return host.session->getSnapshot().canCreateWebInvite; }));
            expect (! host.session->getSnapshot().nativeAvailable); expect (host.session->getSnapshot().state == State::hostWaiting);
        }
        beginTest ("v2 native negotiation keeps the host's 44.1k room rate across a guest device restart");
        {
            V2Service service; V2Fixture host (service), guest (service);
            host.settings.setCollabRate (44100); host.session->createRoom ("Host");
            expect (until ([&] { return host.session->getSnapshot().canCreateWebInvite; }));
            guest.session->joinRoom ("0842", "Guest");
            expect (until ([&] { return CollabSessionTestAccess::transport (*guest.session).getStats (1).v2; }));
            expectEquals ((int) guest.engine.getCollab().getWireRate(), 44100);
            guest.engine.prepare (96000, 120);
            expect (until ([&]
            {
                const auto snapshot = host.session->getSnapshot(); const auto* p = host.person (snapshot, 2);
                return p && p->streamEpoch == 2;
            }));
            expectEquals ((int) guest.engine.getCollab().getWireRate(), 44100);
            expectEquals (guest.settings.getCollabRate(), 48000);
        }
        beginTest ("a legacy guest selects v1, then leaving restores the host's v2 invitation");
        {
            V2Service service; V2Fixture host (service); CollabTransport old;
            host.session->createRoom ("Host"); expect (until ([&] { return host.session->getSnapshot().canCreateWebInvite; }));
            expect (old.open (0, true).isEmpty()); service.reserveLegacy (old.getLocalPort());
            expect (until ([&] { return host.session->getSnapshot().roomMode == c::RoomMode::legacy2; }));
            expect (until ([&] { return CollabSessionTestAccess::legacyReady (*host.session); }));
            old.startGuest (sessionNumber, { { "127.0.0.1", CollabSessionTestAccess::transport (*host.session).getLocalPort() } }, { "Old", "0.16.0" });
            expect (until ([&] { return host.session->getSnapshot().state == State::connected; }));
            expect (! old.getStats().v2); expect (! host.session->getSnapshot().canCreateWebInvite);
            old.stop(); expect (until ([&] { return host.session->getSnapshot().canCreateWebInvite; }));
        }
        beginTest ("a 0.17 guest in a 0.16 host's 44.1k room shows the negotiated rate, not the server default");
        {
            V2Service service; V2Fixture guest (service); CollabTransport old;
            expect (old.open (0, true).isEmpty()); service.legacyHost (old.getLocalPort());
            old.startHost (sessionNumber, { "OldHost", "0.16.0", 0.0, 0.0, 44100, 44100 });
            guest.session->joinRoom ("0842", "Guest");
            expect (until ([&] { return guest.session->getSnapshot().state == State::connected; }));
            expect (guest.session->getSnapshot().roomMode == c::RoomMode::legacy2);
            expectEquals ((int) guest.session->getSnapshot().wireRate, 44100);
            expectEquals ((int) guest.engine.getCollab().getWireRate(), 44100);
            old.stop();
        }
    }
};
static LiveMixCollabR8SessionTests liveMixCollabR8SessionTests;
}
