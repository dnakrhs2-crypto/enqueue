#include "WebGuestSignaling.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <future>
#include <thread>

namespace
{
using namespace gocue::livemix::collab;
using namespace std::chrono_literals;
const juce::String rid = "11111111111111111111111111111111";
const juce::String secondRid = "22222222222222222222222222222222";
const juce::String hostConnection = juce::String::repeatedString ("a", 64);
juce::var obj (std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* value = new juce::DynamicObject();
    for (const auto& field : fields) value->setProperty (field.first, field.second);
    return juce::var (value);
}
juce::var signalingRoom()
{
    return juce::JSON::parse (R"({"apiVersion":2,"code":"0842","session":"fedcba9876543210","roomMode":"v2",
        "wireRate":48000,"revision":19,"capabilities":["packet_v2","roster_ack","star_3","pcm16_mono","web_guest"],"nativeState":"unavailable",
        "members":[{"participantId":1,"kind":"native","nick":"Host","appVersion":"0.17.0","protocols":[1,2],"capabilities":["packet_v2","roster_ack","star_3","pcm16_mono","web_guest"],"connectionGeneration":1,"streamEpoch":3,"state":"connected"},
        {"participantId":4,"kind":"web","nick":"Web","appVersion":"","protocols":[2],"capabilities":["packet_v2","roster_ack","star_3","pcm16_mono","web_guest"],"connectionGeneration":7,"streamEpoch":1,"state":"connecting"}],
        "reservations":[{"participantId":4,"kind":"web","connectionGeneration":7,"expiresAtMs":1800000015000}]})");
}
juce::var pendingRequest (const juce::String& id)
{
    return obj ({ { "requestId", id }, { "nick", "Web" }, { "expiresAtMs", (juce::int64) 1800000060000LL } });
}
juce::var event (const char* id, const char* type, const juce::var& data)
{
    return obj ({ { "id", id }, { "type", type }, { "data", data } });
}
CollabRoom::Response jsonResponse (const juce::var& value) { return { 200, juce::JSON::toString (value, true) }; }
CollabRoom::Response eventResponse (const char* cursor, std::initializer_list<juce::var> events)
{
    juce::Array<juce::var> list;
    for (const auto& e : events) list.add (e);
    return jsonResponse (obj ({ { "apiVersion", 2 }, { "cursor", cursor }, { "events", list } }));
}

struct FakeSignalingServer
{
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<CollabRoom::Response> hostEvents, connectionEvents;
    std::vector<CollabRoom::Request> calls;
    int inviteGeneration = 0, acceptAttempts = 0, signalAttempts = 0;
    bool retryAccept = false, retrySignal = false;

    void push (bool connection, CollabRoom::Response response)
    {
        { std::lock_guard<std::mutex> guard (mutex); (connection ? connectionEvents : hostEvents).push_back (std::move (response)); }
        wake.notify_all();
    }
    void snapshot (bool withRequest = true)
    {
        juce::Array<juce::var> list;
        if (withRequest) list.add (pendingRequest (rid));
        push (false, eventResponse ("1", { event ("1", "snapshot", obj ({ { "room", signalingRoom() }, { "pendingWebRequests", list } })) }));
    }
    bool waitCalls (const char* suffix, int count)
    {
        std::unique_lock<std::mutex> guard (mutex);
        return wake.wait_for (guard, 8s, [&]
        {
            return std::count_if (calls.begin(), calls.end(), [&] (const auto& r) { return r.url.endsWith (suffix); }) >= count;
        });
    }
    std::vector<CollabRoom::Request> requests()
    {
        std::lock_guard<std::mutex> guard (mutex); return calls;
    }
    CollabRoom::Response request (const CollabRoom::Request& r)
    {
        const auto body = juce::JSON::parse (r.body);
        if (r.cancellation) r.cancellation->setHandler ([this]
        {
            std::lock_guard<std::mutex> guard (mutex);
            wake.notify_all();
        });
        std::unique_lock<std::mutex> guard (mutex);
        calls.push_back (r); wake.notify_all();
        if (r.url.endsWith ("/events"))
        {
            auto& events = body.hasProperty ("hostToken") ? hostEvents : connectionEvents;
            wake.wait (guard, [&] { return ! events.empty() || (r.cancellation && r.cancellation->isCancelled()); });
            if (r.cancellation && r.cancellation->isCancelled()) return {};
            auto response = events.front(); events.pop_front(); return response;
        }
        if (r.url.endsWith ("/invite"))
            return jsonResponse (obj ({ { "apiVersion", 2 }, { "inviteToken", juce::String::repeatedString (++inviteGeneration == 1 ? "b" : "c", 64) },
                                       { "inviteGeneration", inviteGeneration }, { "hostNick", "Host" } }));
        if (r.url.endsWith ("/accept"))
        {
            if (++acceptAttempts == 1 && retryAccept) return {};
            return jsonResponse (obj ({ { "apiVersion", 2 }, { "requestId", rid }, { "state", "accepted" },
                { "participantId", 4 }, { "streamEpoch", 1 }, { "connectionGeneration", 7 }, { "connectionToken", hostConnection },
                { "reservationExpiresAtMs", (juce::int64) 1800000015000LL }, { "room", signalingRoom() } }));
        }
        if (r.url.endsWith ("/signal"))
        {
            if (++signalAttempts == 1 && retrySignal) return { 503, R"({"error":"busy"})" };
            return jsonResponse (obj ({ { "apiVersion", 2 }, { "ok", true }, { "signalId", body["signalId"] } }));
        }
        return { 200, R"({"apiVersion":2,"ok":true})" };
    }
};

struct Observations
{
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<WebGuestSignaling::Invite> invites;
    std::vector<WebGuestSignaling::Accepted> accepted;
    std::vector<juce::String> requests, removed, reasons;
    std::vector<uint32_t> signalGenerations, endedGenerations;
    std::vector<juce::var> signals;
    int errors = 0;
    std::thread::id callbackThread;
    WebGuestSignaling::Callbacks callbacks()
    {
        WebGuestSignaling::Callbacks cb;
        cb.onInvite = [this] (const auto& value) { record ([&] { invites.push_back (value); }); };
        cb.onRequest = [this] (const auto& value) { record ([&] { requests.push_back (value.requestId); }); };
        cb.onRequestEnded = [this] (const auto& value) { record ([&] { removed.push_back (value); }); };
        cb.onAccepted = [this] (const auto& value) { record ([&] { accepted.push_back (value); }); };
        cb.onSignal = [this] (auto generation, const auto& value) { record ([&] { signalGenerations.push_back (generation); signals.push_back (value); }); };
        cb.onEnded = [this] (auto generation, const auto& reason) { record ([&] { endedGenerations.push_back (generation); reasons.push_back (reason); }); };
        cb.onError = [this] (int, const auto&) { record ([&] { ++errors; }); };
        return cb;
    }
    template <typename Fn> void record (Fn fn)
    {
        { std::lock_guard<std::mutex> guard (mutex); callbackThread = std::this_thread::get_id(); fn(); }
        wake.notify_all();
    }
    template <typename Fn> bool until (Fn predicate)
    {
        std::unique_lock<std::mutex> guard (mutex); return wake.wait_for (guard, 8s, predicate);
    }
};

class LiveMixWebGuestSignalingTests : public juce::UnitTest
{
public:
    LiveMixWebGuestSignalingTests() : UnitTest ("LiveMix WebGuest signaling", "LiveMix") {}
    void runTest() override
    {
        beginTest ("poll retries keep cursor; pending snapshot, reissue, accept, answer and signal retry");
        {
            FakeSignalingServer service;
            Observations seen;
            service.retryAccept = service.retrySignal = true;
            service.push (false, { 503, R"({"error":"busy"})" });
            service.snapshot();
            WebGuestSignaling signaling ([&] (const auto& r) { return service.request (r); });
            signaling.startHost ("0842", "host", seen.callbacks());
            expect (seen.until ([&] { return seen.requests.size() == 1; }));
            expect (service.waitCalls ("/events", 3)); // host poll is now waiting
            signaling.createInvite();
            expect (seen.until ([&] { return seen.invites.size() == 1; }));
            signaling.createInvite();
            expect (seen.until ([&] { return seen.invites.size() == 2; }));
            signaling.accept (rid);
            expect (seen.until ([&] { return seen.accepted.size() == 1; }));
            expect (service.waitCalls ("/events", 4)); // connection poll has its own wait
            const auto answer = obj ({ { "type", "answer" }, { "sdp", "v=0\r\n" } });
            service.push (true, eventResponse ("5", { event ("5", "signal", obj ({ { "connectionGeneration", 7 },
                { "signalId", 1 }, { "from", "web" }, { "signal", answer } })) }));
            expect (seen.until ([&] { return seen.signals.size() == 1; }));
            // Duplicate already processed event, plus a stale generation, must not reach R1.
            service.push (true, eventResponse ("6", {
                event ("5", "signal", obj ({ { "connectionGeneration", 7 }, { "from", "web" }, { "signal", answer } })),
                event ("6", "signal", obj ({ { "connectionGeneration", 6 }, { "from", "web" }, { "signal", answer } })) }));
            signaling.sendSignal (7, obj ({ { "type", "offer" }, { "sdp", "v=0\r\n" } }));
            expect (service.waitCalls ("/signal", 2));
            signaling.sendSignal (6, obj ({ { "type", "endOfCandidates" } }));
            signaling.sendSignal (7, obj ({ { "type", "endOfCandidates" } }));
            expect (service.waitCalls ("/signal", 3));
            signaling.accept (rid); // an ordinary HTTP retry must not start a second transport
            expect (service.waitCalls ("/accept", 3));
            signaling.leave (7);
            expect (seen.until ([&] { return ! seen.reasons.empty(); }));
            signaling.stop();
            expectEquals ((int) seen.accepted.size(), 1);
            expectEquals ((int) seen.signals.size(), 1);
            expectEquals ((int) seen.removed.size(), 1);
            expectEquals ((int) seen.endedGenerations[0], 7);
            expectEquals (seen.reasons[0], juce::String ("left"));
            expect (seen.callbackThread != std::this_thread::get_id());
            expectEquals (seen.errors, 1); // transient host poll failure
            expect (seen.invites[0].inviteToken != seen.invites[1].inviteToken);
            expectEquals ((int) seen.invites[1].inviteGeneration, 2);
            const auto& connection = seen.accepted[0].connection;
            expect (connection.session == UINT64_C (0xfedcba9876543210));
            expectEquals ((int) connection.guestId, 4); expectEquals ((int) connection.generation, 7);
            expectEquals ((int) connection.hostEpoch, 3); expectEquals ((int) connection.rosterRevision, 0);
            const auto& acceptedRoom = seen.accepted[0].room;
            expectEquals ((int) acceptedRoom.revision, 19);
            expect (acceptedRoom.session == connection.session);
            expectEquals ((int) acceptedRoom.members.size(), 2);
            expect (std::any_of (acceptedRoom.members.begin(), acceptedRoom.members.end(), [&] (const auto& member)
            {
                return member.participantId == connection.guestId && member.connectionGeneration == connection.generation
                    && member.kind == "web" && member.state == "connecting";
            }));
            std::vector<juce::var> signals;
            std::vector<juce::String> hostCursors;
            for (const auto& r : service.requests())
            {
                const auto body = juce::JSON::parse (r.body);
                if (r.url.endsWith ("/signal")) signals.push_back (body);
                if (r.url.endsWith ("/events"))
                {
                    expectEquals (r.timeoutMs, 30000); expect (r.cancellation != nullptr);
                    if (body.hasProperty ("hostToken")) hostCursors.push_back (body["cursor"].toString());
                    else expectEquals (body["connectionToken"].toString(), hostConnection);
                }
            }
            expectEquals ((int) signals.size(), 3);
            if (signals.size() == 3)
            {
                expectEquals (juce::JSON::toString (signals[0]), juce::JSON::toString (signals[1]));
                expectEquals ((int) signals[2]["signalId"], 2);
            }
            expectEquals (hostCursors[0], juce::String ("0")); expectEquals (hostCursors[1], juce::String ("0"));
            expectEquals (hostCursors[2], juce::String ("1"));
        }

        beginTest ("pending request events, reject, kick, and clean cancellation of both polls");
        {
            FakeSignalingServer service;
            Observations seen;
            service.snapshot (false);
            WebGuestSignaling signaling ([&] (const auto& r) { return service.request (r); });
            signaling.startHost ("0842", "host", seen.callbacks());
            service.push (false, eventResponse ("3", { event ("2", "request", pendingRequest (rid)), event ("3", "request", pendingRequest (secondRid)) }));
            expect (seen.until ([&] { return seen.requests.size() == 2; }));
            signaling.reject (secondRid);
            expect (seen.until ([&] { return seen.removed.size() == 1; }));
            signaling.accept (rid);
            expect (seen.until ([&] { return seen.accepted.size() == 1; }));
            expect (service.waitCalls ("/events", 4));
            signaling.kick (4, 7);
            expect (seen.until ([&] { return seen.reasons.size() == 1; }));
            // Host events repeat the same lifecycle change; callbacks remain once only.
            service.push (false, eventResponse ("4", { event ("4", "ended", obj ({ { "requestId", rid },
                { "participantId", 4 }, { "connectionGeneration", 7 }, { "reason", "kicked" } })) }));
            expect (service.waitCalls ("/events", 5));
            auto stopped = std::async (std::launch::async, [&] { signaling.stop(); });
            expect (stopped.wait_for (5s) == std::future_status::ready);
            stopped.get();
            expectEquals ((int) seen.removed.size(), 2); expectEquals ((int) seen.reasons.size(), 1);
            expectEquals (seen.reasons[0], juce::String ("kicked"));
            const auto count = service.requests().size();
            signaling.createInvite(); signaling.sendSignal (7, obj ({ { "type", "connected" } }));
            expect (service.requests().size() == count);
        }

        beginTest ("room closure stops polling, cancels the connection and permits a new host run");
        {
            FakeSignalingServer service;
            Observations seen;
            service.snapshot();
            WebGuestSignaling signaling ([&] (const auto& r) { return service.request (r); });
            signaling.startHost ("0842", "host", seen.callbacks());
            expect (seen.until ([&] { return ! seen.requests.empty(); }));
            signaling.accept (rid);
            expect (seen.until ([&] { return ! seen.accepted.empty(); }));
            service.push (false, eventResponse ("2", { event ("2", "ended", obj ({ { "reason", "closed" } })) }));
            expect (seen.until ([&] { return ! seen.reasons.empty(); }));
            signaling.stop();
            expectEquals ((int) seen.endedGenerations[0], 7); expectEquals (seen.reasons[0], juce::String ("closed"));
            service.snapshot (false);
            Observations next;
            signaling.startHost ("0842", "new-host", next.callbacks());
            signaling.createInvite();
            expect (next.until ([&] { return ! next.invites.empty(); }));
            signaling.stop();
            expect (next.accepted.empty());
        }

        beginTest ("stop actively cancels outstanding host and connection requests");
        {
            FakeSignalingServer service;
            Observations seen;
            service.snapshot();
            WebGuestSignaling signaling ([&] (const auto& r) { return service.request (r); });
            signaling.startHost ("0842", "host", seen.callbacks());
            expect (seen.until ([&] { return ! seen.requests.empty(); }));
            signaling.accept (rid);
            expect (seen.until ([&] { return ! seen.accepted.empty(); }));
            expect (service.waitCalls ("/events", 3));
            auto stop = std::async (std::launch::async, [&] { signaling.stop(); });
            expect (stop.wait_for (5s) == std::future_status::ready);
            stop.get();
            int cancelled = 0;
            for (const auto& r : service.requests())
                if (r.url.endsWith ("/events") && r.cancellation && r.cancellation->isCancelled()) ++cancelled;
            expect (cancelled >= 2);
            expect (seen.reasons.empty());
        }
    }
};
LiveMixWebGuestSignalingTests liveMixWebGuestSignalingTests;
}
