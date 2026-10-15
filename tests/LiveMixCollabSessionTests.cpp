#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include "CollabSession.h"
#include "LiveMixSettings.h"
#include "ui/UiUtils.h"
#include <atomic>
#include <mutex>

namespace gocue::livemix::collab
{
struct CollabSessionTestAccess
{
    static CollabTransport& transport (CollabSession& s) { return s.transport; }
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
            if (sendProbe)
            {
                probe (port);
                juce::Thread::sleep (100); // server probe arrives before a delayed HTTP response
            }
            return response (object ({ { "code", "0842" }, { "session", sessionHex }, { "hostToken", "host-token" },
                                      { "publicIp", "203.0.113.8" }, { "nonce", nonceHex } }));
        }
        if (action == "confirm")
        {
            confirmed = (bool) body["reachable"];
            if (! confirmed) present = false;
        }
        if (action == "heartbeat") connected = (bool) body["connected"];
        if (action == "close") present = false;
        if (action == "join")
        {
            int status = joinStatus.load();
            if (status == 200 && ! present) status = 404;
            if (status == 200 && ! confirmed) status = 425;
            if (status == 200 && connected && body["inst"].toString() != guestInst) status = 409;
            if (status != 200) return { status, "{\"error\":\"room\"}" };
            guestInst = body["inst"].toString();
            juce::Array<juce::var> candidates;
            candidates.add (object ({ { "ip", "127.0.0.1" }, { "port", port } }));
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
    std::atomic<int> joinStatus { 200 };
    std::atomic<bool> ranOnMessageThread { false };
private:
    std::mutex mutex;
    std::vector<std::pair<juce::String, juce::var>> calls;
    int port = 0;
    bool present = false, confirmed = false, connected = false;
    juce::String hostNick, guestInst;
};

struct TempDirectory
{
    TempDirectory() { file.createDirectory(); }
    ~TempDirectory() { file.deleteRecursively(); }
    juce::File file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LiveMixCollab-" + juce::Uuid().toString());
};
struct Fixture
{
    explicit Fixture (const std::shared_ptr<FakeService>& service, CollabUpnp::Result upnpResult = { true, true, "203.0.113.8", "192.168.1.9" })
        : settings (directory.file), audio ("Local\\LiveMix.Collab.Test." + juce::Uuid().toString())
    {
        audio.prepare (48000, 120);
        settings.setCollabPort (freePort()); upnp->result = upnpResult;
        CollabSession::Options options;
        options.loopbackOnly = true; options.probeTimeoutMs = 250; options.connectTimeoutMs = 350;
        options.heartbeatIntervalMs = 200; options.renewIntervalMs = 250;
        session = std::make_unique<CollabSession> (audio, settings,
            std::make_unique<CollabRoom> ([service] (const auto& request) { return service->request (request); }, "http://127.0.0.1:8899"),
            std::make_unique<FakeUpnp> (upnp), options);
    }
    bool reaches (State state) { return until ([&] { return session->getSnapshot().state == state; }); }
    int port() { return CollabSessionTestAccess::transport (*session).getLocalPort(); }
    TempDirectory directory;
    LiveMixSettings settings;
    CollabAudio audio;
    std::shared_ptr<UpnpState> upnp = std::make_shared<UpnpState>();
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
            expect (until ([&] { return service->count ("heartbeat") > 0 && host.upnp->renewals.load() > 0; }));
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
            expect (until ([&] { return service->count ("close") == 1 && host.upnp->removals.load() == 1 && host.port() == 0; }));
            expect (! service->ranOnMessageThread.load()); expect (! host.upnp->ranOnMessageThread.load());
            expectGreaterThan (listener.changes, 0); host.session->removeChangeListener (&listener);
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
}
