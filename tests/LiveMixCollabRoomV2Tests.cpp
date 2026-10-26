#include "CollabRoom.h"
#include <juce_core/juce_core.h>

namespace
{
using namespace gocue::livemix::collab;
juce::var roomFixture()
{
    return juce::JSON::parse (R"json({
      "apiVersion":2,"code":"0842","session":"fedcba9876543210","roomMode":"v2",
      "wireRate":44100,"revision":7,"capabilities":["packet_v2","roster_ack","star_3","pcm16_mono","web_guest"],
      "nativeState":"punch",
      "members":[
        {"participantId":1,"kind":"native","nick":"Host","appVersion":"0.17.0","protocols":[1,2],
         "capabilities":["packet_v2","roster_ack","star_3","pcm16_mono","web_guest"],"connectionGeneration":1,"streamEpoch":2,"state":"connected"},
        {"participantId":2,"kind":"native","nick":"Guest","appVersion":"0.17.0","protocols":[1,2],
         "capabilities":["packet_v2","roster_ack","star_3","pcm16_mono","web_guest"],"connectionGeneration":2,"streamEpoch":1,"state":"connecting"},
        {"participantId":4,"kind":"web","nick":"Browser","appVersion":"","protocols":[2],
         "capabilities":["packet_v2","roster_ack","star_3","pcm16_mono","web_guest"],"connectionGeneration":4,"streamEpoch":1,"state":"connected"}
      ],"reservations":[{"participantId":2,"kind":"native","connectionGeneration":2,"expiresAtMs":1800000015000}]
    })json");
}
CollabRoom::Response reply (const juce::var& json) { return { 200, juce::JSON::toString (json, true) }; }

class LiveMixCollabRoomV2Tests : public juce::UnitTest
{
public:
    LiveMixCollabRoomV2Tests() : UnitTest ("LiveMix Collab room v2", "LiveMix") {}
    void runTest() override
    {
        beginTest ("extended native requests and room identity, reservations, peers");
        std::vector<CollabRoom::Request> calls;
        auto service = [&] (const CollabRoom::Request& request)
        {
            calls.push_back (request);
            auto json = roomFixture();
            auto* obj = json.getDynamicObject();
            obj->setProperty ("ok", true);
            if (request.url.endsWith ("/create"))
            {
                obj->setProperty ("hostToken", "0123456789abcdef0123456789abcdef");
                obj->setProperty ("publicIp", "203.0.113.8");
                obj->setProperty ("nonce", "0123456789abcdef0123456789abcdef");
                obj->setProperty ("participantId", 1); obj->setProperty ("connectionGeneration", 1); obj->setProperty ("streamEpoch", 2);
            }
            if (request.url.endsWith ("/join"))
            {
                obj->setProperty ("hostNick", "Host"); obj->setProperty ("sameNetwork", true);
                obj->setProperty ("candidates", juce::JSON::parse (R"([{"ip":"192.168.1.9","port":12000},{"ip":"203.0.113.8","port":40000}])"));
                obj->setProperty ("participantId", 2); obj->setProperty ("connectionGeneration", 2); obj->setProperty ("streamEpoch", 1);
            }
            obj->setProperty ("punch", true);
            obj->setProperty ("peers", juce::JSON::parse (R"([{"participantId":2,"connectionId":"11111111111111111111111111111111","candidates":[{"ip":"198.51.100.2","port":15000}]}])"));
            return reply (json);
        };
        CollabRoom room (service, "http://127.0.0.1:8899/");
        CollabRoom::ClientInfo info;
        info.connectionId = "11111111111111111111111111111111";
        const auto created = room.createV2 ("Host", "same-profile", 12000, { "192.168.1.9" }, {}, info);
        expect (created.ok()); expect (created.session == UINT64_C (0xfedcba9876543210));
        expectEquals ((int) created.participantId, 1); expectEquals ((int) created.streamEpoch, 2);
        expectEquals (created.room.apiVersion, 2); expectEquals ((int) created.room.wireRate, 44100);
        expectEquals ((int) created.room.members.size(), 3); expectEquals ((int) created.room.reservations.size(), 1);
        expectEquals ((int) created.room.revision, 7); expectEquals (created.room.nativeState, juce::String ("punch"));
        if (! created.room.reservations.empty()) expect (created.room.reservations[0].expiresAtMs == INT64_C (1800000015000));
        const auto confirmed = room.confirmV2 (created.code, created.hostToken, false, "probe", true);
        expect (confirmed.ok() && confirmed.punch);
        const auto joined = room.joinV2 (created.code, "Guest", "same-profile", info, {}, 13000);
        expect (joined.ok() && joined.sameNetwork); expectEquals ((int) joined.candidates.size(), 2);
        expectEquals ((int) joined.participantId, 2); expectEquals ((int) joined.connectionGeneration, 2);
        const auto beat = room.heartbeatV2 (created.code, created.hostToken, true, { { 2, 2, "connected", 3 }, { 4, 4, "left", 1 } });
        expect (beat.ok()); expectEquals ((int) beat.peers.size(), 1);
        if (! beat.peers.empty())
        {
            expectEquals (beat.peers[0].connectionId, info.connectionId);
            expectEquals ((int) beat.peers[0].candidates.size(), 1);
        }
        expect (room.closeV2 (created.code, created.hostToken).ok());
        expectEquals ((int) calls.size(), 5);
        for (const auto& call : calls)
        {
            expectEquals ((int) juce::JSON::parse (call.body)["apiVersion"], 2);
            expectEquals (call.timeoutMs, 8000);
        }
        auto body = juce::JSON::parse (calls[0].body);
        expectEquals (body["connectionId"].toString(), info.connectionId);
        expectEquals (body["protocols"].size(), 2); expectEquals (body["capabilities"].size(), 5);
        expectEquals ((int) body["wireRate"], 48000);
        body = juce::JSON::parse (calls[3].body);
        expectEquals (body["memberStates"].size(), 2);
        expectEquals ((int) body["memberStates"][0]["streamEpoch"], 3);
        expectEquals (body["memberStates"][1]["state"].toString(), juce::String ("left"));

        beginTest ("new client reads legacy server without assigning v2 identity");
        CollabRoom old ([&] (const CollabRoom::Request& request)
        {
            if (request.url.endsWith ("/create")) return CollabRoom::Response { 200,
                R"({"code":"0842","session":"fedcba9876543210","hostToken":"old","publicIp":"203.0.113.8","nonce":"0123456789abcdef0123456789abcdef"})" };
            if (request.url.endsWith ("/join")) return CollabRoom::Response { 200,
                R"({"session":"fedcba9876543210","hostNick":"Old host","candidates":[{"ip":"203.0.113.8","port":12000}],"sameNetwork":false})" };
            return CollabRoom::Response { 200, R"({"ok":true,"guest":{"candidates":[{"ip":"198.51.100.2","port":13000}]}})" };
        });
        const auto legacyCreated = old.createV2 ("Host", "id", 12000, {}, {}, info);
        expect (legacyCreated.ok()); expectEquals (legacyCreated.room.apiVersion, 0);
        expectEquals (legacyCreated.room.roomMode, juce::String ("legacy2")); expectEquals ((int) legacyCreated.participantId, 0);
        const auto legacyJoined = old.joinV2 ("0842", "Guest", "id", info);
        expect (legacyJoined.ok()); expectEquals ((int) legacyJoined.connectionGeneration, 0);
        expect (old.confirmV2 ("0842", "old", true, {}).ok());
        const auto legacyBeat = old.heartbeatV2 ("0842", "old", false, {});
        expect (legacyBeat.ok()); expectEquals ((int) legacyBeat.guest.size(), 1);
        expect (old.closeV2 ("0842", "old").ok());

        beginTest ("malformed extended room cannot silently become legacy");
        const std::vector<std::function<void(juce::var&)>> invalid {
            [] (auto& j) { j.getDynamicObject()->setProperty ("session", 42); },
            [] (auto& j) { j.getDynamicObject()->setProperty ("revision", true); },
            [] (auto& j) { j.getDynamicObject()->setProperty ("apiVersion", 3); },
            [] (auto& j) { j.getDynamicObject()->setProperty ("roomMode", "legacy2"); },
            [] (auto& j) { j.getDynamicObject()->setProperty ("wireRate", 96000); },
            [] (auto& j) { j["members"][1].getDynamicObject()->setProperty ("participantId", 1); },
            [] (auto& j) { j["members"][0].getDynamicObject()->setProperty ("participantId", 7); },
            [] (auto& j) { j["reservations"][0].getDynamicObject()->setProperty ("connectionGeneration", 99); }
        };
        for (const auto& corrupt : invalid)
        {
            auto json = roomFixture(); corrupt (json);
            CollabRoom::RoomSnapshot snapshot;
            expect (! CollabRoom::parseRoomSnapshot (json, snapshot));
        }

        beginTest ("web endpoints use role tokens and the separate event timeout");
        calls.clear();
        CollabRoom web ([&] (const auto& call)
        {
            calls.push_back (call);
            return CollabRoom::Response { 200, R"({"apiVersion":2,"ok":true,"cursor":"18446744073709551615","events":[]})" };
        });
        auto cancel = std::make_shared<CollabRoom::Cancellation>();
        expect (web.webInvite ("0842", "host").ok());
        expect (web.webAccept ("0842", "host", "request").ok());
        expect (web.webReject ("0842", "host", "request").ok());
        expect (web.webKick ("0842", "host", 4, 4).ok());
        expect (web.webSignal ("0842", "connection", 4, 1, juce::JSON::parse (R"({"type":"offer","sdp":"v=0\r\n"})")).ok());
        expect (web.webEvents ("0842", "host", "0", 0, cancel).ok());
        expect (web.webEvents ("0842", "connection", "123", 4, cancel).ok());
        expect (web.webLeave ("0842", "connection", 4).ok());
        for (size_t i = 0; i < calls.size(); ++i)
        {
            const auto json = juce::JSON::parse (calls[i].body);
            expectEquals ((int) json["apiVersion"], 2);
            expectEquals (calls[i].timeoutMs, i == 5 || i == 6 ? 30000 : 8000);
            if (i == 5 || i == 6) expect (calls[i].cancellation == cancel);
            expect (json.hasProperty ("hostToken") == (i <= 3 || i == 5));
            expect (! calls[i].url.contains ("Token="));
        }
        expectEquals ((int) juce::JSON::parse (calls[4].body)["signalId"], 1);
        expectEquals (juce::JSON::parse (calls[6].body)["cursor"].toString(), juce::String ("123"));
        CollabRoom failure ([] (const auto&) { return CollabRoom::Response { 409, R"({"error":"generation"})" }; });
        const auto failed = failure.webKick ("0842", "host", 4, 4);
        expect (! failed.ok()); expectEquals (failed.errorCode, juce::String ("generation"));
    }
};
LiveMixCollabRoomV2Tests liveMixCollabRoomV2Tests;
}
