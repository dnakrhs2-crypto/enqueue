#include "ControlServer.h"
#include "ControlDiscovery.h"
#include "MixDocument.h"
#include "MuteGroups.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <deque>
#include <thread>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace gocue::tests
{
using namespace gocue::livemix;

namespace
{
    using P = ControlProtocol;
    double now() { return juce::Time::getMillisecondCounterHiRes(); }

    // TestMain runs UnitTests synchronously. Pump only these socket tests, on the real JUCE message thread,
    // with both an overall deadline and a per-pass message bound. JUCE_MODAL_LOOPS_PERMITTED remains zero.
    template <typename Predicate>
    bool until (Predicate done, int timeoutMs = 1500, bool pump = true)
    {
        jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
        const auto deadline = now() + timeoutMs;
        do
        {
            if (done()) return true;
           #if JUCE_WINDOWS
            if (pump)
            {
                MSG message;
                for (int i = 0; i < 32 && now() < deadline && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++i)
                {
                    if (message.message == WM_QUIT) return false;
                    TranslateMessage (&message);
                    DispatchMessageW (&message);
                }
            }
           #else
            juce::ignoreUnused (pump);
           #endif
            juce::Thread::sleep (2);
        } while (now() < deadline);
        return done();
    }
    void pumpFor (int ms) { until ([] { return false; }, ms); }
    juce::var object() { return juce::var (new juce::DynamicObject()); }
    void put (juce::var& value, const char* key, const juce::var& item) { value.getDynamicObject()->setProperty (key, item); }
    std::string wire (const juce::var& value) { return juce::JSON::toString (value, true).toStdString() + "\n"; }
    juce::var readDiscovery (const juce::File& directory) { return juce::JSON::parse (directory.getChildFile ("discovery.json").loadFileAsString()); }

    struct Client
    {
        explicit Client (int receiveBytes = 65536) : socket (juce::SocketOptions().withReceiveBufferSize (receiveBytes)) {}
        juce::StreamingSocket socket;
        P::Decoder decoder;
        std::deque<juce::var> incoming;
        juce::String instance, session;
        juce::var initial;
        bool eof = false, malformed = false;
        int readSize = 4096;

        bool open (int port) { return port > 0 && socket.connect ("127.0.0.1", port, 500); }
        bool send (const std::string& text, size_t chunk = 4096)
        {
            size_t sent = 0;
            const auto deadline = now() + 1500;
            while (sent < text.size() && now() < deadline)
            {
                const auto ready = socket.waitUntilReady (false, 20);
                if (ready < 0) return false;
                if (ready == 0) continue;
                const auto count = socket.write (text.data() + sent, (int) std::min (chunk, text.size() - sent));
                if (count <= 0) return false;
                sent += (size_t) count;
            }
            return sent == text.size();
        }
        bool send (const juce::var& value) { return send (wire (value)); }
        void collect()
        {
            char bytes[4096];
            for (int i = 0; i < 64 && ! eof; ++i)
            {
                const auto ready = socket.waitUntilReady (true, 0);
                if (ready == 0) return;
                if (ready < 0) { eof = true; return; }
                const auto count = socket.read (bytes, readSize, false);
                if (count <= 0) { eof = true; return; }
                for (const auto& line : decoder.push (bytes, (size_t) count))
                    if (const auto* value = std::get_if<juce::var> (&line)) incoming.push_back (*value);
                    else malformed = true;
            }
        }
        juce::var take (const juce::String& type, const juce::String& id = {}, int timeout = 1500, bool pump = true)
        {
            juce::var result;
            until ([&]
            {
                collect();
                const auto found = std::find_if (incoming.begin(), incoming.end(), [&] (const auto& message)
                { return message["type"].toString() == type && (id.isEmpty() || message["id"].toString() == id); });
                if (found == incoming.end()) return false;
                result = *found; incoming.erase (found); return true;
            }, timeout, pump);
            return result;
        }
        bool closed (int timeout = 1500) { return until ([&] { collect(); return eof; }, timeout); }
        juce::var hello (juce::String token, juce::Array<juce::var> versions = { 1 })
        {
            auto result = object(), client = object();
            put (result, "v", 1); put (result, "type", "hello"); put (result, "id", "1");
            put (result, "token", token); put (result, "supportedVersions", versions);
            put (client, "name", juce::String::fromUTF8 ("곰 Stream Deck")); put (client, "version", "test");
            put (result, "client", client);
            return result;
        }
        juce::var command (int id, const juce::String& name, juce::var args = object())
        {
            auto result = object();
            put (result, "v", 1); put (result, "type", "command"); put (result, "id", juce::String (id));
            put (result, "instanceId", instance); put (result, "sessionId", session);
            put (result, "command", name); put (result, "args", args);
            return result;
        }
        juce::var toggle (int id, juce::Uuid channel)
        {
            auto args = object(); put (args, "channelId", channel.toString());
            return command (id, "toggleChannel", args);
        }
        juce::var ping (int id)
        {
            auto result = object(); put (result, "v", 1); put (result, "type", "ping");
            put (result, "id", juce::String (id)); put (result, "instanceId", instance); return result;
        }
    };

    struct Fixture
    {
        juce::File directory = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("livemix-control-" + juce::Uuid().toString());
        MixEngine engine;
        MixDocument document { engine };
        MuteGroups groups { document };
        std::unique_ptr<ControlServer> server;
        ControlServer::Options options;
        juce::Uuid generation = document.getSessionGeneration();
        bool audio = false, allOnMessageThread = true;
        int values = 0, structures = 0, groupChanges = 0;

        Fixture()
        {
            engine.prepare (48000.0, 256); document.applyToEngine();
            document.setSessionName ("Control test"); // dirty already, to isolate empty deltas from double toggles
            document.onValueChanged = [this]
            {
                ++values; groups.apply(); checkThread();
                if (server) server->documentChanged (ControlServer::ChangeKind::values);
            };
            document.onStructureChanged = [this]
            {
                ++structures; checkThread();
                if (generation != document.getSessionGeneration()) { generation = document.getSessionGeneration(); groups.reset(); }
                else groups.apply();
                if (server) server->documentChanged (ControlServer::ChangeKind::structure);
            };
            groups.onChanged = [this]
            {
                ++groupChanges; checkThread();
                if (server) server->muteGroupsChanged();
            };
            options.enabled = true; options.preferredPort = 0; options.appVersion = "0.5.3"; options.discoveryDirectory = directory;
            makeServer();
        }
        ~Fixture()
        {
            server.reset();
            document.onValueChanged = {}; document.onStructureChanged = {}; groups.onChanged = {};
            directory.deleteRecursively();
        }
        void checkThread() { allOnMessageThread = allOnMessageThread && juce::MessageManager::getInstance()->isThisTheMessageThread(); }
        void makeServer() { server = std::make_unique<ControlServer> (document, groups, [this] { checkThread(); return audio; }); }
        juce::Uuid channel() const { return document.getSession().channels[0].id; }
        bool on() const { return document.getSession().channels[0].on; }
        bool ready()
        {
            return until ([&] { return server->getStatus().port > 0 && readDiscovery (directory)["state"].toString() == "ready"; });
        }
        void start() { server->start (options); }
    };
}

class ControlServerTests : public juce::UnitTest
{
public:
    ControlServerTests() : juce::UnitTest ("LiveMix control server", "LiveMix") {}
    bool start (Fixture& f)
    {
        f.start();
        const auto ready = f.ready();
        expect (ready, "Loopback/discovery startup failed: " + f.server->getStatus().error);
        return ready;
    }
    bool authenticate (Fixture& f, Client& c, size_t chunk = 4096)
    {
        const auto connected = c.open (f.server->getStatus().port);
        expect (connected, "127.0.0.1 connection failed");
        if (! connected) return false;
        expect (c.send (wire (c.hello (readDiscovery (f.directory)["token"].toString(), { 2, 1 })), chunk));
        const auto hello = c.take ("helloAck", "1");
        expectEquals (hello["type"].toString(), juce::String ("helloAck"));
        c.initial = c.take ("state");
        expectEquals (c.initial["reason"].toString(), juce::String ("initial"));
        c.instance = hello["instanceId"].toString(); c.session = c.initial["sessionId"].toString();
        expectEquals (c.instance, readDiscovery (f.directory)["instanceId"].toString());
        expect (! c.malformed);
        return hello.isObject() && c.initial.isObject();
    }
    void code (const juce::var& error, const char* expected)
    {
        expectEquals (error["type"].toString(), juce::String ("error"));
        expectEquals (error["code"].toString(), juce::String (expected));
    }
    void runTest() override
    {
        handshake();
        orderingAndState();
        duplicates();
        clientLimits();
        queueLimits();
        rateLimits();
        framingAndSlowReader();
        discoveryAndPorts();
        lifecycle();
    }

    void handshake()
    {
        beginTest ("real loopback hello negotiates v1, authenticates a CSPRNG token, and sends an immediate offline snapshot");
        Fixture f;
        if (! start (f)) return;
        const auto discovery = readDiscovery (f.directory);
        const auto token = discovery["token"].toString();
        expectEquals (token.length(), 43); expect (token.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"));
        juce::MemoryOutputStream decoded;
        expect (juce::Base64::convertFromBase64 (decoded, token.replaceCharacter ('-', '+').replaceCharacter ('_', '/') + "="));
        expectEquals ((int) decoded.getDataSize(), 32);
        expect (ControlDiscovery::tokensEqual (token, token));
        expect (! ControlDiscovery::tokensEqual (token, token + "x"));
        expect (! ControlDiscovery::tokensEqual (token, "x" + token.substring (1)) || token.startsWithChar ('x'));
        expect (ControlDiscovery::createToken() != token);
        Client good;
        if (! authenticate (f, good, 1)) return; // every byte, including UTF-8 characters, may be split
        expectEquals ((int) good.initial["v"], 1);
        expect (! (bool) good.initial["state"]["audio"]["running"]);
        expectEquals (good.initial["state"]["channels"].size(), 1);
        expectEquals (f.server->getStatus().connectedCount, 1);

        beginTest ("bad token, missing hello, unsupported negotiation and post-hello major fail without unauthenticated state leakage");
        for (int scenario = 0; scenario < 4; ++scenario)
        {
            Client c;
            expect (c.open (f.server->getStatus().port));
            auto hello = c.hello (scenario == 0 ? token + "x" : token, scenario == 2 ? juce::Array<juce::var> { 2 } : juce::Array<juce::var> { 1 });
            if (scenario == 1) { c.instance = good.instance; hello = c.ping (1); }
            expect (c.send (hello));
            if (scenario == 3)
            {
                c.instance = c.take ("helloAck")["instanceId"].toString();
                c.take ("state");
                auto ping = c.ping (2); put (ping, "v", 2); expect (c.send (ping));
            }
            const auto failure = c.take ("error");
            code (failure, scenario == 0 ? "AUTH_FAILED" : scenario == 1 ? "AUTH_REQUIRED" : "UNSUPPORTED_VERSION");
            if (scenario != 3) expect (! failure.hasProperty ("instanceId") && ! failure.hasProperty ("sessionId"));
            if (scenario >= 2) expectEquals (failure["supportedVersions"].size(), 1);
            expect (c.closed()); expect (! c.malformed);
            pumpFor (25); // accept worker reaps the closed unauthenticated slot
        }

        beginTest ("hello has a three-second deadline, and pong must pass through the message thread");
        Client idle;
        expect (idle.open (f.server->getStatus().port));
        const auto began = now();
        code (idle.take ("error", {}, 3500), "HANDSHAKE_TIMEOUT");
        expect (now() - began >= 2900 && now() - began < 3500);
        expect (idle.closed());
        expect (good.send (good.ping (2)));
        juce::Thread::sleep (100); good.collect();
        expect (good.incoming.empty(), "I/O must not manufacture pong without the message thread");
        expectEquals (good.take ("pong", "2")["instanceId"].toString(), good.instance);
        expect (f.allOnMessageThread);
    }

    void orderingAndState()
    {
        beginTest ("split/coalesced lines dispatch in order; two toggles advance revision and publish an empty delta to each client");
        Fixture f;
        if (! start (f)) return;
        Client a, b;
        if (! authenticate (f, a) || ! authenticate (f, b)) return;
        const auto initialRevision = (juce::int64) a.initial["revision"];
        const auto commands = wire (a.toggle (2, f.channel())) + wire (a.toggle (3, f.channel()));
        expect (a.send (commands.substr (0, 17))); juce::Thread::sleep (25);
        expectEquals (f.values, 0);
        expect (a.send (commands.substr (17)));
        const auto first = a.take ("ack", "2"), second = a.take ("ack", "3");
        expect (! (bool) first["result"]["on"] && (bool) second["result"]["on"]);
        expectEquals ((juce::int64) first["revision"], initialRevision + 1);
        expectEquals ((juce::int64) second["revision"], initialRevision + 2);
        expect (f.on()); expectEquals (f.values, 2);
        for (auto* client : { &a, &b })
        {
            const auto delta = client->take ("stateDelta");
            expectEquals ((juce::int64) delta["baseRevision"], initialRevision);
            expectEquals ((juce::int64) delta["revision"], initialRevision + 2);
            expect (delta["changes"].getDynamicObject() != nullptr);
            if (const auto* changes = delta["changes"].getDynamicObject()) expectEquals (changes->getProperties().size(), 0);
        }

        beginTest ("requestState returns ack then full state; no-op edits produce no event and UI edits invalidate CAS before publish");
        auto noopArgs = object(); put (noopArgs, "on", true);
        expect (a.send (a.command (4, "setAllChannelsOn", noopArgs)));
        const auto noop = a.take ("ack", "4");
        expect (! (bool) noop["changed"]); expectEquals ((juce::int64) noop["revision"], initialRevision + 2);
        pumpFor (150); a.collect(); expect (a.incoming.empty());
        auto cas = a.toggle (5, f.channel()); put (cas, "ifRevision", (juce::int64) noop["revision"]);
        f.document.renameChannel (f.channel(), "UI edit");
        expect (a.send (cas)); code (a.take ("error", "5"), "REVISION_CONFLICT");
        expect (f.on());
        auto request = a.command (6, "requestState"); request.getDynamicObject()->removeProperty ("sessionId");
        expect (a.send (request));
        const auto ack = a.take ("ack", "6");
        const auto snapshot = a.take ("state");
        expect (! (bool) ack["changed"]);
        expectEquals (snapshot["requestId"].toString(), juce::String ("6"));
        expectEquals ((juce::int64) snapshot["revision"], (juce::int64) ack["result"]["snapshotRevision"]);
        expectEquals (snapshot["state"]["channels"][0]["name"].toString(), juce::String ("UI edit"));

        beginTest ("public hooks publish structure/session states after group reset, and poll callback-free audio/dirty changes");
        // Flush earlier events so subsequent assertions identify the new publication.
        pumpFor (150); a.collect(); b.collect(); a.incoming.clear(); b.incoming.clear();
        f.document.addPluginGroup (f.channel()); // value callback, but the projection shape changed
        auto structure = a.take ("state");
        expectEquals (structure["reason"].toString(), juce::String ("structureChanged"));
        expectEquals (structure["state"]["channels"][0]["pluginGroups"].size(), 1);
        f.groups.set (MuteGroups::Group::mic, true);
        const auto group = a.take ("stateDelta");
        expect ((bool) group["changes"]["muteGroups"]["mic"]);
        f.audio = true; f.document.discardUnsavedChanges();
        const auto audio = a.take ("stateDelta");
        expect ((bool) audio["changes"]["audio"]["running"]);
        expect (audio["changes"]["session"]["dirty"].isBool() && ! (bool) audio["changes"]["session"]["dirty"]);
        const auto oldSession = a.session;
        f.document.newSession();
        auto session = a.take ("state");
        expectEquals (session["reason"].toString(), juce::String ("sessionChanged"));
        expect (session["sessionId"].toString() != oldSession);
        expect (! (bool) session["state"]["muteGroups"]["mic"]);
        expect (f.allOnMessageThread);
    }

    void duplicates();
    void clientLimits();
    void queueLimits();
    void rateLimits();
    void framingAndSlowReader();
    void discoveryAndPorts();
    void lifecycle();
};

void ControlServerTests::duplicates()
{
    beginTest ("in-flight and completed duplicates execute once; changed content and evicted old ids never execute");
    Fixture f;
    if (! start (f)) return;
    Client c;
    if (! authenticate (f, c)) return;
    const auto toggle = c.toggle (2, f.channel());
    expect (c.send (wire (toggle) + wire (toggle)));
    const auto original = c.take ("ack", "2");
    expectEquals (f.values, 1); expect (! f.on());
    // Reverse object property order and add whitespace: same JSON content must share the response.
    auto reordered = object();
    const auto& properties = toggle.getDynamicObject()->getProperties();
    for (int i = properties.size() - 1; i >= 0; --i)
        reordered.getDynamicObject()->setProperty (properties.getName (i), properties.getValueAt (i));
    expect (c.send (std::string ("  ") + wire (reordered)));
    expect (wire (c.take ("ack", "2")) == wire (original), "Cached completion must match the original");
    expectEquals (f.values, 1);
    auto different = c.command (2, "requestState");
    expect (c.send (different)); code (c.take ("error", "2"), "DUPLICATE_ID");
    expectEquals (f.values, 1);

    auto args = object(); put (args, "on", false);
    for (int batch = 0; batch < 5; ++batch)
    {
        pumpFor (300);
        std::string commands;
        for (int i = 0; i < 7; ++i) commands += wire (c.command (3 + batch * 7 + i, "setAllChannelsOn", args));
        expect (c.send (commands));
        for (int i = 0; i < 7; ++i)
            expectEquals (c.take ("ack", juce::String (3 + batch * 7 + i))["type"].toString(), juce::String ("ack"));
    }
    expect (c.send (toggle)); code (c.take ("error", "2"), "DUPLICATE_ID");
    expectEquals (f.values, 1);
    // The latest request still has its exact result, including its original revision.
    expect (c.send (c.command (37, "setAllChannelsOn", args)));
    const auto cached = c.take ("ack", "37");
    expectEquals ((juce::int64) cached["revision"], (juce::int64) original["revision"]);

    beginTest ("replayed requestState keeps its original result and resets that client's delta baseline coherently");
    pumpFor (600); c.collect(); c.incoming.clear();
    const auto request = c.command (38, "requestState");
    expect (c.send (request));
    const auto ack = c.take ("ack", "38"), snapshot = c.take ("state");
    f.document.renameChannel (f.channel(), "after snapshot");
    const auto delta = c.take ("stateDelta");
    expect ((juce::int64) delta["revision"] > (juce::int64) snapshot["revision"]);
    expect (c.send (request));
    expect (wire (c.take ("ack", "38")) == wire (ack), "Cached requestState ack must match the original");
    expect (wire (c.take ("state")) == wire (snapshot), "Cached requested snapshot must match the original");
    const auto restored = c.take ("stateDelta");
    expectEquals ((juce::int64) restored["baseRevision"], (juce::int64) snapshot["revision"]);
    expectEquals ((juce::int64) restored["revision"], (juce::int64) delta["revision"]);
    expect (c.send (request));
    code (c.take ("error", "38"), "RATE_LIMITED"); // cached snapshots consume the same 2/s query budget
}

void ControlServerTests::clientLimits()
{
    beginTest ("only two unauthenticated connections are retained; closing one releases its slot");
    {
        Fixture f;
        if (! start (f)) return;
        Client a, b, excess;
        expect (a.open (f.server->getStatus().port)); expect (b.open (f.server->getStatus().port));
        pumpFor (60);
        expect (excess.open (f.server->getStatus().port)); expect (excess.closed (500));
        expectEquals (f.server->getStatus().connectedCount, 0);
        a.socket.close(); pumpFor (60);
        Client replacement;
        expect (authenticate (f, replacement));
        expectEquals (f.server->getStatus().connectedCount, 1);
    }
    beginTest ("eight authenticated clients operate independently and a ninth connection is closed");
    Fixture f;
    if (! start (f)) return;
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < 8; ++i)
    {
        clients.push_back (std::make_unique<Client>());
        if (! authenticate (f, *clients.back())) return;
    }
    expectEquals (f.server->getStatus().connectedCount, 8);
    Client excess;
    expect (excess.open (f.server->getStatus().port)); expect (excess.closed (500));
    for (auto& client : clients) expect (client->send (client->ping (2)));
    for (auto& client : clients) expectEquals (client->take ("pong", "2")["type"].toString(), juce::String ("pong"));
    clients[0]->socket.close();
    expect (until ([&] { return f.server->getStatus().connectedCount == 7; }));
    Client replacement; expect (authenticate (f, replacement));
    expectEquals (f.server->getStatus().connectedCount, 8);
}

void ControlServerTests::queueLimits()
{
    beginTest ("per-client pending limit is 16; excess gets SERVER_BUSY on I/O while the message thread is held");
    {
        Fixture f;
        if (! start (f)) return;
        Client c;
        if (! authenticate (f, c)) return;
        std::string batch;
        for (int id = 2; id <= 11; ++id) batch += wire (c.toggle (id, f.channel()));
        expect (c.send (batch));
        juce::Thread::sleep (280); // replenish the burst without draining callAsync
        batch.clear();
        for (int id = 12; id <= 18; ++id) batch += wire (c.toggle (id, f.channel()));
        expect (c.send (batch));
        code (c.take ("error", "18", 400, false), "SERVER_BUSY");
        expectEquals (f.values, 0);
        for (int id = 2; id <= 17; ++id)
            expectEquals (c.take ("ack", juce::String (id))["type"].toString(), juce::String ("ack"));
        expectEquals (f.values, 16); expect (f.on());
    }

    beginTest ("the total pending limit is 64 across clients, and round-robin dispatch preserves each client's order");
    {
        Fixture f;
        if (! start (f)) return;
        std::vector<std::unique_ptr<Client>> clients;
        for (int i = 0; i < 8; ++i)
        {
            clients.push_back (std::make_unique<Client>());
            if (! authenticate (f, *clients.back())) return;
        }
        for (int i = 0; i < 8; ++i)
        {
            if (i == 4) juce::Thread::sleep (300);
            std::string batch;
            for (int id = 2; id <= 9; ++id) batch += wire (clients[(size_t) i]->toggle (id, f.channel()));
            expect (clients[(size_t) i]->send (batch));
        }
        juce::Thread::sleep (60);
        expect (clients[0]->send (clients[0]->toggle (10, f.channel())));
        code (clients[0]->take ("error", "10", 300, false), "SERVER_BUSY");
        expectEquals (f.values, 0);
        for (auto& client : clients)
        {
            juce::int64 previous = 0;
            for (int id = 2; id <= 9; ++id)
            {
                const auto ack = client->take ("ack", juce::String (id));
                expectEquals (ack["type"].toString(), juce::String ("ack"));
                expect ((juce::int64) ack["revision"] > previous); previous = (juce::int64) ack["revision"];
            }
        }
        expectEquals (f.values, 64); expect (f.on());
    }

    beginTest ("COMMAND_EXPIRED rejects a command held over one second before dispatch, then fresh commands still work");
    Fixture f;
    if (! start (f)) return;
    Client c;
    if (! authenticate (f, c)) return;
    expect (c.send (c.toggle (2, f.channel())));
    juce::Thread::sleep (1200); // deliberately no Windows pump
    expectEquals (f.values, 0); expect (f.on());
    code (c.take ("error", "2"), "COMMAND_EXPIRED");
    expectEquals (f.values, 0); expect (f.on());
    expect (c.send (c.toggle (3, f.channel())));
    expectEquals (c.take ("ack", "3")["type"].toString(), juce::String ("ack"));
    expectEquals (f.values, 1); expect (! f.on());
}

void ControlServerTests::rateLimits()
{
    beginTest ("command token bucket permits burst ten then limits; requestState permits two per second");
    {
        Fixture f;
        if (! start (f)) return;
        Client c;
        if (! authenticate (f, c)) return;
        std::string batch;
        for (int id = 2; id <= 12; ++id) batch += wire (c.toggle (id, f.channel()));
        expect (c.send (batch)); code (c.take ("error", "12"), "RATE_LIMITED");
        for (int id = 2; id <= 11; ++id) expectEquals (c.take ("ack", juce::String (id))["type"].toString(), juce::String ("ack"));
        expectEquals (f.values, 10);
        pumpFor (150);
        batch.clear();
        for (int id = 13; id <= 15; ++id) batch += wire (c.command (id, "requestState"));
        expect (c.send (batch)); code (c.take ("error", "15"), "RATE_LIMITED");
        for (int id = 13; id <= 14; ++id)
        {
            expectEquals (c.take ("ack", juce::String (id))["type"].toString(), juce::String ("ack"));
            expectEquals (c.take ("state")["requestId"].toString(), juce::String (id));
        }
    }

    beginTest ("global command 120/s and requestState 8/s buckets constrain simultaneous clients before dispatch");
    Fixture f;
    if (! start (f)) return;
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < 8; ++i)
    {
        clients.push_back (std::make_unique<Client>());
        if (! authenticate (f, *clients.back())) return;
    }
    const auto began = now();
    for (auto& client : clients)
    {
        std::string batch;
        for (int id = 2; id <= 11; ++id) batch += wire (client->toggle (id, f.channel()));
        expect (client->send (batch));
    }
    juce::Thread::sleep (100);
    const auto inputWindow = now() - began;
    int limited = 0, accepted = 0;
    expect (until ([&]
    {
        int total = 0;
        for (auto& client : clients)
        {
            client->collect();
            for (const auto& message : client->incoming)
                if (message["type"].toString() == "ack" || message["type"].toString() == "error") ++total;
        }
        return total == 80;
    }));
    for (auto& client : clients)
    {
        for (const auto& message : client->incoming)
        {
            if (message["type"].toString() == "ack") ++accepted;
            if (message["code"].toString() == "RATE_LIMITED") ++limited;
        }
        client->incoming.clear();
    }
    expect (accepted >= 40 && accepted <= 40 + (int) (120 * inputWindow / 1000));
    expect (limited > 0); expectEquals (accepted + limited, 80);
    expectEquals (f.values, accepted);
    pumpFor (400);
    for (auto& client : clients)
        expect (client->send (wire (client->command (12, "requestState")) + wire (client->command (13, "requestState"))));
    int snapshots = 0, requestLimited = 0;
    expect (until ([&]
    {
        snapshots = requestLimited = 0;
        for (auto& client : clients)
        {
            client->collect();
            for (const auto& message : client->incoming)
            {
                if (message["type"].toString() == "state" && message["reason"].toString() == "requested") ++snapshots;
                if (message["code"].toString() == "RATE_LIMITED") ++requestLimited;
            }
        }
        return snapshots + requestLimited == 16;
    }));
    expectEquals (snapshots, 8); expectEquals (requestLimited, 8);
}

void ControlServerTests::framingAndSlowReader()
{
    beginTest ("oversize lines without LF, malformed JSON/UTF-8/depth and oversized states terminate only the offending connection");
    {
        Fixture f;
        if (! start (f)) return;
        Client healthy;
        if (! authenticate (f, healthy)) return;
        const std::pair<std::string, const char*> invalid[] {
            { std::string (P::maxLineBytes + 1, 'x'), "MESSAGE_TOO_LARGE" },
            { "{invalid}\n", "INVALID_JSON" },
            { std::string ("{\"x\":\"") + "\xff" + "\"}\n", "INVALID_UTF8" },
            { "{\"x\":" + std::string (16, '[') + "0" + std::string (16, ']') + "}\n", "INVALID_ARGUMENT" }
        };
        for (const auto& input : invalid)
        {
            Client bad;
            expect (bad.open (f.server->getStatus().port)); expect (bad.send (input.first));
            const auto error = bad.take ("error"); code (error, input.second);
            expect (! error.hasProperty ("instanceId")); expect (bad.closed());
            pumpFor (25);
        }
        expect (healthy.send (healthy.ping (2)));
        expectEquals (healthy.take ("pong", "2")["type"].toString(), juce::String ("pong"));
        f.document.renameChannel (f.channel(), juce::String::repeatedString ("x", (int) P::maxLineBytes));
        const auto tooLarge = healthy.take ("error");
        code (tooLarge, "STATE_TOO_LARGE");
        expectEquals (tooLarge["instanceId"].toString(), healthy.instance);
        expectEquals (tooLarge["sessionId"].toString(), healthy.session);
        expect (healthy.closed());
        expect (f.server->getStatus().enabled);
    }

    beginTest ("large frames span many socket writes/reads without losing FIFO order or UTF-8 bytes");
    {
        Fixture f;
        const auto channelName = juce::String::repeatedString (juce::String::fromUTF8 ("곰\"\\"), 6000);
        f.document.renameChannel (f.channel(), channelName);
        if (! start (f)) return;
        Client c;
        c.readSize = 137;
        if (! authenticate (f, c, 7)) return;
        expectEquals (c.initial["state"]["channels"][0]["name"].toString(), channelName);
        expect (c.send (wire (c.command (2, "requestState")) + wire (c.ping (3))));
        expect (until ([&] { c.collect(); return c.incoming.size() >= 3; }));
        if (c.incoming.size() >= 3)
        {
            expectEquals (c.incoming[0]["type"].toString(), juce::String ("ack"));
            expectEquals (c.incoming[1]["type"].toString(), juce::String ("state"));
            expectEquals (c.incoming[2]["type"].toString(), juce::String ("pong"));
            expectEquals (c.incoming[1]["state"]["channels"][0]["name"].toString(), channelName);
        }
        expect (! c.malformed);
    }

    beginTest ("a non-reading client is cut off after two seconds of no send progress while a healthy peer continues");
    {
        Fixture f;
        if (! start (f)) return;
        Client slow (512), healthy;
        if (! authenticate (f, slow) || ! authenticate (f, healthy)) return;
        const auto began = now();
        // Fill the initial Windows TCP receive window as well as the sender's kernel buffer. A single 60 KiB
        // frame can be accepted entirely by TCP even with SO_RCVBUF=512; that is real send progress, not a stall.
        // Four frames remain below the application's 256 KiB FIFO limit, isolating the two-second timeout.
        for (const auto* letter : { "s", "t", "u", "v" })
        {
            const auto channelName = juce::String::repeatedString (letter, 60000);
            f.document.renameChannel (f.channel(), channelName);
            expectEquals (healthy.take ("stateDelta")["changes"]["channels"][0]["name"].toString(), channelName);
        }
        for (int id = 2; id <= 6; ++id)
        {
            expect (healthy.send (healthy.ping (id)));
            expectEquals (healthy.take ("pong", juce::String (id))["type"].toString(), juce::String ("pong"));
            pumpFor (500);
        }
        expect (until ([&] { return f.server->getStatus().connectedCount == 1; }, 1000), "Slow reader must be isolated");
        expect (now() - began >= 2000 && now() - began < 4000);
        expect (f.server->getStatus().enabled);
        expect (healthy.send (healthy.toggle (7, f.channel())));
        expectEquals (healthy.take ("ack", "7")["type"].toString(), juce::String ("ack"));
        expect (! f.on());
    }

    beginTest ("the 256 KiB output budget includes immutable DTOs and closes an overflowing FIFO without affecting peers");
    {
        ControlSocket transport;
        std::mutex mutex;
        std::vector<ControlSocket::Connection::Ptr> accepted;
        const auto port = transport.start (0, [&] (auto connection)
        {
            connection->helloReceived(); connection->authenticated();
            { std::lock_guard<std::mutex> lock (mutex); accepted.push_back (connection); }
            return ControlSocket::Handler {};
        });
        expect (port > 0); transport.allowConnections();
        Client a, b;
        expect (a.open (port)); expect (b.open (port));
        expect (until ([&] { std::lock_guard<std::mutex> lock (mutex); return accepted.size() == 2; }));
        std::vector<ControlSocket::Connection::Ptr> streams;
        { std::lock_guard<std::mutex> lock (mutex); streams = accepted; }
        if (streams.size() == 2)
        {
            ControlSocket::Messages overflow;
            P::Projection p; p.session.name = juce::String::repeatedString ("x", 60000);
            const P::Context context { juce::Uuid(), juce::Uuid(), 1 };
            for (int i = 0; i < 5; ++i) overflow.push_back (P::State { context, P::StateReason::resync, {}, p });
            expect (! streams[0]->send (overflow));
            expect (a.closed());
            expect (streams[1]->send (P::Pong { "2", context }));
            expectEquals (b.take ("pong", "2")["type"].toString(), juce::String ("pong"));
        }
        const auto began = now(); transport.stop(); expect (now() - began < 1000);
    }
}

void ControlServerTests::discoveryAndPorts()
{
    beginTest ("occupied preferred port 49721 falls back to a real ephemeral loopback port advertised in discovery");
    {
        juce::StreamingSocket occupied;
        const auto held = occupied.createListener (ControlSocket::preferredPort, "127.0.0.1");
        // If another process already owns it, the same collision case is still exercised without disturbing it.
        Fixture f; f.options.preferredPort = ControlSocket::preferredPort;
        if (! start (f)) return;
        const auto status = f.server->getStatus();
        const auto discovery = readDiscovery (f.directory);
        expect (status.port > 0 && status.port != ControlSocket::preferredPort);
        expectEquals ((int) discovery["port"], status.port);
        expectEquals (discovery["host"].toString(), juce::String ("127.0.0.1"));
        expectEquals ((int) discovery["schemaVersion"], 1); expectEquals ((int) discovery["leaseMs"], 15000);
        expectEquals (discovery["app"].toString(), juce::String ("LiveMix"));
        expectEquals (discovery["appVersion"].toString(), juce::String ("0.5.3"));
        expect ((juce::int64) discovery["pid"] > 0); expect (discovery["updatedAt"].toString().endsWithChar ('Z'));
        logMessage ("Verified 127.0.0.1:49721 collision -> 127.0.0.1:" + juce::String (status.port)
                    + "; discovery port matches (preferred port held by " + (held ? juce::String ("test") : juce::String ("existing listener")) + ")");
        Client c; expect (authenticate (f, c));
        const auto beforeHeartbeat = (juce::int64) readDiscovery (f.directory)["heartbeat"];
        expect (until ([&] { return (juce::int64) readDiscovery (f.directory)["heartbeat"] > beforeHeartbeat; }, 5500, false));
        expectEquals (readDiscovery (f.directory)["state"].toString(), juce::String ("ready"));
        f.server->stop();
        auto stopped = readDiscovery (f.directory);
        expectEquals (stopped["state"].toString(), juce::String ("stopped"));
        expect (! stopped.hasProperty ("token") && ! stopped.hasProperty ("port"));
        expect ((juce::int64) stopped["heartbeat"] > (juce::int64) discovery["heartbeat"]);
        if (held)
        {
            occupied.close(); pumpFor (30);
            f.start(); expect (f.ready());
            expectEquals (f.server->getStatus().port, ControlSocket::preferredPort);
            expectEquals ((int) readDiscovery (f.directory)["port"], ControlSocket::preferredPort);
            logMessage ("Verified available preferred port -> 127.0.0.1:49721");
        }
    }

    beginTest ("disabled discovery refreshes its five-second heartbeat without a message pump, then clean stop writes a tombstone");
    {
        Fixture f; f.options.enabled = false; f.start();
        expect (until ([&] { return readDiscovery (f.directory)["state"].toString() == "disabled"; }));
        const auto first = readDiscovery (f.directory);
        expect (! f.server->getStatus().enabled); expectEquals (f.server->getStatus().port, 0);
        expect (! first.hasProperty ("token") && ! first.hasProperty ("port"));
        juce::Thread::sleep (5250);
        const auto heartbeat = readDiscovery (f.directory);
        expectEquals (heartbeat["state"].toString(), juce::String ("disabled"));
        expect ((juce::int64) heartbeat["heartbeat"] > (juce::int64) first["heartbeat"]);
        expect (heartbeat["updatedAt"].toString() != first["updatedAt"].toString());
        f.server->stop();
        auto tombstone = readDiscovery (f.directory);
        expectEquals (tombstone["state"].toString(), juce::String ("stopped"));
        expect (! tombstone.hasProperty ("token") && ! tombstone.hasProperty ("port"));
        logMessage ("Verified disabled heartbeat refresh without message pumping and stopped tombstone with no token/port");
    }

    beginTest ("discovery replacements are complete JSON under concurrent reads and file failure prevents listener publication");
    {
        Fixture f;
        ControlDiscovery::Status status; status.appVersion = "test"; status.state = ControlDiscovery::State::disabled;
        std::atomic<int> writes { 0 }, failures { 0 }, invalidReads { 0 }, reads { 0 };
        std::atomic<bool> done { false };
        ControlDiscovery publisher (f.directory, status, [&] (const auto&, bool ok) { if (ok) ++writes; else ++failures; });
        expect (until ([&] { return writes.load() > 0; }));
        std::thread reader ([&]
        {
            while (! done)
            {
                const auto json = readDiscovery (f.directory);
                if (! json.isObject() || (int) json["schemaVersion"] != 1 || json["state"].toString() != "disabled") ++invalidReads;
                ++reads;
                juce::Thread::sleep (1);
            }
        });
        for (int i = 0; i < 80; ++i)
        {
            status.appVersion = juce::String (i); publisher.publish (status); juce::Thread::sleep (3);
        }
        expect (until ([&] { return readDiscovery (f.directory)["appVersion"].toString() == "79"; }));
        done = true; reader.join(); publisher.stop (status);
        expect (reads.load() > 0 && writes.load() > 2); expectEquals (invalidReads.load(), 0); expectEquals (failures.load(), 0);
    }
    {
        Fixture f;
        expect (f.directory.replaceWithText ("a file cannot be a discovery directory"));
        f.start();
        expect (until ([&] { return f.server->getStatus().error == "DISCOVERY_FAILED"; }));
        expectEquals (f.server->getStatus().port, 0); expectEquals (f.server->getStatus().connectedCount, 0);
    }
}

void ControlServerTests::lifecycle()
{
    beginTest ("disable invalidates pending callAsync before edits and publishes serverStatus; re-enable rotates token and instance");
    {
        Fixture f;
        if (! start (f)) return;
        Client old;
        if (! authenticate (f, old)) return;
        const auto original = readDiscovery (f.directory);
        expect (old.send (old.toggle (2, f.channel()))); juce::Thread::sleep (60);
        const auto began = now(); f.server->setEnabled (false);
        expect (now() - began < 200, "OFF must not join connection threads on the message thread");
        expect (! f.server->getStatus().enabled); expectEquals (f.server->getStatus().port, 0);
        const auto disabled = old.take ("serverStatus");
        expectEquals (disabled["status"].toString(), juce::String ("disabled"));
        expectEquals (disabled["reason"].toString(), juce::String ("controlDisabled"));
        code (old.take ("error", "2"), "CONTROL_DISABLED");
        expect (old.closed()); expectEquals (f.values, 0); expect (f.on());
        expect (until ([&] { return readDiscovery (f.directory)["state"].toString() == "disabled"; }));
        expect (! readDiscovery (f.directory).hasProperty ("token"));
        f.server->setEnabled (true); expect (f.ready());
        const auto current = readDiscovery (f.directory);
        expect (current["token"].toString() != original["token"].toString());
        expect (current["instanceId"].toString() != original["instanceId"].toString());
        Client stale;
        expect (stale.open (f.server->getStatus().port)); expect (stale.send (stale.hello (original["token"].toString())));
        code (stale.take ("error"), "AUTH_FAILED"); expect (stale.closed());
        Client fresh; expect (authenticate (f, fresh));
        expectEquals ((juce::int64) fresh.initial["revision"], (juce::int64) 1);
        expect (f.on()); expectEquals (f.values, 0);
    }

    beginTest ("beginShutdown precedes final save, and stop/restart with pending commands is safe twice in the same process");
    for (int round = 0; round < 2; ++round)
    {
        Fixture f;
        if (! start (f)) return;
        Client c;
        if (! authenticate (f, c)) return;
        expect (c.send (c.toggle (2, f.channel()))); juce::Thread::sleep (60);
        f.server->beginShutdown();
        f.document.discardUnsavedChanges(); // final-save boundary must not acquire new remote dirty edits
        const auto status = c.take ("serverStatus");
        expectEquals (status["status"].toString(), juce::String ("stopping"));
        expectEquals (status["reason"].toString(), juce::String ("shutdown"));
        code (c.take ("error", "2"), "SERVER_STOPPING");
        const auto began = now(); f.server->stop(); expect (now() - began < 1000);
        expect (! f.document.isDirty()); expect (f.on()); expectEquals (f.values, 0);

        // Restart the same server object while callbacks from the preceding generation are still possible.
        f.start(); expect (f.ready());
        Client second; if (! authenticate (f, second)) return;
        expect (second.send (second.toggle (2, f.channel()))); juce::Thread::sleep (60);
        // Destroy the owner before pumping that queued callback, then create another owner in the same process.
        f.server.reset(); f.makeServer(); f.start(); expect (f.ready());
        pumpFor (100);
        expectEquals (f.values, 0); expect (f.on()); expect (! f.document.isDirty());
        Client third; if (! authenticate (f, third)) return;
        expect (third.send (third.toggle (2, f.channel())));
        expectEquals (third.take ("ack", "2")["type"].toString(), juce::String ("ack"));
        expectEquals (f.values, 1); expect (! f.on()); expect (f.allOnMessageThread);
    }
}

static ControlServerTests controlServerTests;

} // namespace gocue::tests
