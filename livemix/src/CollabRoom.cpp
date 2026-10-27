#include "CollabRoom.h"
#include "ui/UiUtils.h"
#include <algorithm>
#include <chrono>
#include <set>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <wininet.h>
#endif

namespace gocue::livemix::collab
{
namespace
{
// juce::WebInputStream is WinINet, which opens only 2 connections at once to an HTTP/1.1 server by default. A host
// keeps two long polls on the room server (its events and the web guest's), so the offer, candidates and heartbeats
// waited behind them for up to 20 s and the web guest gave up after 10 s (10/27: the production server answers
// HTTP/1.1 through its proxy; the HTTP/1.0 test server allowed 4 and hid it). Process-wide, before the first request.
void allowParallelRequests()
{
   #if JUCE_WINDOWS
    static const bool raised = []
    {
        DWORD connections = 16;
        const bool perServer = InternetSetOptionW (nullptr, INTERNET_OPTION_MAX_CONNS_PER_SERVER, &connections, sizeof (connections)) != FALSE;
        const bool perOldServer = InternetSetOptionW (nullptr, INTERNET_OPTION_MAX_CONNS_PER_1_0_SERVER, &connections, sizeof (connections)) != FALSE;
        return perServer && perOldServer;
    }();
    juce::ignoreUnused (raised);
   #endif
}
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* value = new juce::DynamicObject();
    for (const auto& field : fields) value->setProperty (field.first, field.second);
    return juce::var (value);
}
bool hex (const juce::String& text, int length)
{
    return text.length() == length && text.containsOnly ("0123456789abcdefABCDEF");
}
// [{"ip": "a.b.c.d", "port": n}, ...] -> valid IPv4 addresses only
std::vector<CollabTransport::Address> addresses (const juce::var& list)
{
    std::vector<CollabTransport::Address> result;
    if (auto* items = list.getArray())
        for (const auto& item : *items)
        {
            const auto ip = item["ip"].toString();
            const int port = item["port"];
            if (ip.containsOnly ("0123456789.") && juce::IPAddress (ip).toString() == ip && port > 0 && port <= 65535)
                result.push_back ({ ip, port });
        }
    return result;
}
}

CollabRoom::CollabRoom (RequestFunction fn, juce::String base, std::shared_ptr<Cancellation> cancellation)
    : request (fn ? std::move (fn) : httpRequest), defaultCancellation (std::move (cancellation))
{
    if (base.isEmpty()) base = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_COLLAB_SERVER", {});
    server = base.trim().trimCharactersAtEnd ("/");
    if (server.isEmpty()) server = "https://xn--2j1bo5s81gjmag88b.com";
}

void CollabRoom::Cancellation::cancel()
{
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> guard (mutex);
        cancelled.store (true);
        fn = handler;
    }
    condition.notify_all();
    if (fn) fn();
}

bool CollabRoom::Cancellation::wait (int milliseconds)
{
    std::unique_lock<std::mutex> guard (mutex);
    return condition.wait_for (guard, std::chrono::milliseconds (milliseconds), [this] { return isCancelled(); });
}

void CollabRoom::Cancellation::setHandler (std::function<void()> fn)
{
    std::function<void()> invoke;
    {
        std::lock_guard<std::mutex> guard (mutex);
        handler = std::move (fn);
        if (isCancelled()) invoke = handler;
    }
    if (invoke) invoke();
}

CollabRoom::Response CollabRoom::httpRequest (const Request& request)
{
    allowParallelRequests();
    Response response;
    if (request.cancellation != nullptr)
    {
        auto stream = std::make_shared<juce::WebInputStream> (juce::URL (request.url).withPOSTData (request.body), false);
        stream->withCustomRequestCommand ("POST").withExtraHeaders (request.headers).withConnectionTimeout (request.timeoutMs);
        request.cancellation->setHandler ([stream] { stream->cancel(); });
        if (! request.cancellation->isCancelled() && stream->connect (nullptr))
        {
            response.statusCode = stream->getStatusCode();
            response.body = stream->readEntireStreamAsString();
            if (stream->isError()) response.statusCode = 0;
        }
        request.cancellation->setHandler ({});
        return response;
    }
    auto stream = juce::URL (request.url).withPOSTData (request.body).createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withHttpRequestCmd ("POST").withExtraHeaders (request.headers)
            .withConnectionTimeoutMs (request.timeoutMs).withStatusCode (&response.statusCode));
    if (stream != nullptr) response.body = stream->readEntireStreamAsString();
    return response;
}

juce::String CollabRoom::errorMessage (int status)
{
    if (status <= 0) return ko ("합방 서버에 연결하지 못했습니다");
    switch (status)
    {
        case 404: return ko ("없는 방 코드입니다");
        case 410: return ko ("닫힌 방입니다");
        case 425: return ko ("방장이 아직 확인 중입니다. 잠시 뒤 다시 참가하세요");
        case 409: return ko ("이미 다른 사람이 들어와 있습니다");
        default: return ko ("합방 서버 오류 (") + juce::String (status) + ")";
    }
}

CollabRoom::Reply CollabRoom::post (const char* action, const juce::var& body, juce::var& json, int timeoutMs,
                                  std::shared_ptr<Cancellation> cancellation)
{
    const auto response = request ({ server + "/api/livemix/collab/" + action, juce::JSON::toString (body, true),
        "Content-Type: application/json\r\n", timeoutMs, cancellation ? std::move (cancellation) : defaultCancellation });
    json = juce::JSON::parse (response.body);
    const bool ok = response.statusCode == 200 && json.getDynamicObject() != nullptr && ! json.hasProperty ("error");
    return { response.statusCode, ok ? juce::String() : errorMessage (response.statusCode), json["error"].toString() };
}

CollabRoom::Reply CollabRoom::command (const char* action, const juce::var& body)
{
    juce::var json;
    auto reply = post (action, body, json);
    if (reply.ok() && ! (bool) json.getProperty ("ok", false)) reply.error = errorMessage (reply.statusCode);
    return reply;
}

CollabRoom::Created CollabRoom::create (const juce::String& nick, const juce::String& inst, int port,
                                      const juce::StringArray& lan, const CollabUpnp::Result& upnp, const CollabTransport::Mapping& stun)
{
    juce::Array<juce::var> lanAddresses;
    for (const auto& ip : lan) if (lanAddresses.size() < 8) lanAddresses.add (ip);
    juce::var json;
    Created result;
    auto body = object ({ { "nick", nick }, { "inst", inst }, { "port", port }, { "lan", lanAddresses },
        { "upnp", object ({ { "found", upnp.found }, { "mapped", upnp.mapped }, { "externalIp", upnp.externalIp } }) } });
    if (stun.usable()) body.getDynamicObject()->setProperty ("stun", object ({ { "ip", stun.address.host }, { "port", stun.address.port } }));
    static_cast<Reply&> (result) = post ("create", body, json);
    if (! result.ok()) return result;
    result.code = json["code"].toString(); result.hostToken = json["hostToken"].toString(); result.publicIp = json["publicIp"].toString();
    const auto session = json["session"].toString(), nonce = json["nonce"].toString();
    if (! hex (session, 16) || ! hex (nonce, 32) || result.code.length() != 4 || ! result.code.containsOnly ("0123456789")
        || result.hostToken.isEmpty() || result.publicIp.isEmpty())
    {
        result.error = errorMessage (result.statusCode); return result;
    }
    result.session = (uint64_t) session.getHexValue64();
    for (size_t n = 0; n < result.nonce.size(); ++n)
        result.nonce[n] = (uint8_t) nonce.substring ((int) n * 2, (int) n * 2 + 2).getHexValue32();
    return result;
}

CollabRoom::Confirmed CollabRoom::confirm (const juce::String& code, const juce::String& token, bool reachable, const juce::String& reason, bool punch)
{
    auto body = object ({ { "code", code }, { "hostToken", token }, { "reachable", reachable }, { "reason", reason } });
    if (punch) body.getDynamicObject()->setProperty ("punch", true);   // keep the room: the guest is reached by hole punching
    juce::var json;
    Confirmed result;
    static_cast<Reply&> (result) = post ("confirm", body, json);
    if (result.ok() && ! (bool) json.getProperty ("ok", false)) result.error = errorMessage (result.statusCode);
    result.punch = result.ok() && (bool) json.getProperty ("punch", false);
    return result;
}
CollabRoom::Beat CollabRoom::heartbeat (const juce::String& code, const juce::String& token, bool connected)
{
    juce::var json;
    Beat result;
    static_cast<Reply&> (result) = post ("heartbeat", object ({ { "code", code }, { "hostToken", token }, { "connected", connected } }), json);
    if (result.ok() && ! (bool) json.getProperty ("ok", false)) result.error = errorMessage (result.statusCode);
    if (result.ok()) result.guest = addresses (json["guest"]["candidates"]);
    return result;
}
CollabRoom::Reply CollabRoom::close (const juce::String& code, const juce::String& token)
{
    return command ("close", object ({ { "code", code }, { "hostToken", token } }));
}
CollabRoom::Joined CollabRoom::join (const juce::String& code, const juce::String& nick, const juce::String& inst,
                                    const CollabTransport::Mapping& stun, int localPort)
{
    juce::var json;
    Joined result;
    auto body = object ({ { "code", code }, { "nick", nick }, { "inst", inst } });
    // where the host must knock when its own port is closed
    if (stun.usable()) body.getDynamicObject()->setProperty ("stun", object ({ { "ip", stun.address.host }, { "port", stun.address.port } }));
    if (localPort > 0) body.getDynamicObject()->setProperty ("port", localPort);
    static_cast<Reply&> (result) = post ("join", body, json);
    if (! result.ok()) return result;
    const auto session = json["session"].toString();
    result.hostNick = json["hostNick"].toString(); result.sameNetwork = (bool) json["sameNetwork"];
    result.candidates = addresses (json["candidates"]);
    if (! hex (session, 16) || result.candidates.empty() || result.hostNick.isEmpty())
        result.error = errorMessage (result.statusCode);
    else result.session = (uint64_t) session.getHexValue64();
    return result;
}

namespace
{
bool u32 (const juce::var& value, uint32_t& result)
{
    if (! value.isInt() && ! value.isInt64()) return false;
    const auto number = (juce::int64) value;
    if (number <= 0 || number > 0xffffffffLL) return false;
    result = (uint32_t) number;
    return true;
}
bool strings (const juce::var& value, juce::StringArray& result)
{
    const auto* items = value.getArray();
    if (items == nullptr || items->size() > 5) return false;
    result.clear();
    for (const auto& item : *items)
    {
        if (! item.isString() || result.contains (item.toString())) return false;
        result.add (item.toString());
    }
    return true;
}
bool timestamp (const juce::var& value, int64_t& result)
{
    if (! value.isInt() && ! value.isInt64()) return false;
    result = (juce::int64) value;
    return result > 0 && result <= 9007199254740991LL;
}
juce::var clientBody (juce::var body, const CollabRoom::ClientInfo& info)
{
    auto* obj = body.getDynamicObject();
    obj->setProperty ("apiVersion", 2);
    obj->setProperty ("appVersion", info.appVersion);
    obj->setProperty ("connectionId", info.connectionId);
    obj->setProperty ("wireRate", (juce::int64) info.wireRate);
    juce::Array<juce::var> protocols, capabilities;
    for (auto p : info.protocols) protocols.add (p);
    for (const auto& c : info.capabilities) capabilities.add (c);
    obj->setProperty ("protocols", protocols);
    obj->setProperty ("capabilities", capabilities);
    return body;
}
juce::var hostBody (const juce::String& code, const juce::String& token)
{
    return object ({ { "apiVersion", 2 }, { "code", code }, { "hostToken", token } });
}
juce::var connectionBody (const juce::String& code, const juce::String& token, uint32_t generation)
{
    return object ({ { "apiVersion", 2 }, { "code", code }, { "connectionToken", token },
                    { "connectionGeneration", (juce::int64) generation } });
}
template <typename Result>
void readRoom (Result& result, const juce::var& json)
{
    if (result.ok() && ! CollabRoom::parseRoomSnapshot (json, result.room))
        result.error = CollabRoom::errorMessage (result.statusCode);
}
template <typename Result>
void readIdentity (Result& result, const juce::var& json)
{
    readRoom (result, json);
    if (! result.ok() || result.room.apiVersion == 0) return;
    if (! u32 (json["participantId"], result.participantId)
        || ! u32 (json["connectionGeneration"], result.connectionGeneration)
        || ! u32 (json["streamEpoch"], result.streamEpoch))
    {
        result.error = CollabRoom::errorMessage (result.statusCode);
        return;
    }
    for (const auto& member : result.room.members)
        if (member.participantId == result.participantId && member.connectionGeneration == result.connectionGeneration
            && member.streamEpoch == result.streamEpoch) return;
    result.error = CollabRoom::errorMessage (result.statusCode);
}
}

bool CollabRoom::parseRoomSnapshot (const juce::var& json, RoomSnapshot& output)
{
    RoomSnapshot room;
    if (! json.hasProperty ("apiVersion")) { output = room; return true; }
    if (! json["apiVersion"].isInt() || (int) json["apiVersion"] != 2) return false;
    room.apiVersion = 2;
    room.code = json["code"].toString();
    room.roomMode = json["roomMode"].toString();
    room.nativeState = json["nativeState"].toString();
    const auto session = json["session"].toString();
    if (! json["code"].isString() || ! json["session"].isString()
        || room.code.length() != 4 || ! room.code.containsOnly ("0123456789") || ! hex (session, 16)
        || ! session.containsOnly ("0123456789abcdef")
        || (room.roomMode != "v2" && room.roomMode != "legacy2")
        || ! juce::StringArray { "checking", "reachable", "punch", "unavailable" }.contains (room.nativeState)
        || ! u32 (json["wireRate"], room.wireRate) || (room.wireRate != 44100 && room.wireRate != 48000)
        || ! u32 (json["revision"], room.revision) || ! strings (json["capabilities"], room.capabilities)) return false;
    room.session = (uint64_t) session.getHexValue64();
    const auto* members = json["members"].getArray();
    const auto* reservations = json["reservations"].getArray();
    if (members == nullptr || members->isEmpty() || members->size() > 3 || reservations == nullptr || reservations->size() > 2) return false;
    std::set<uint32_t> ids;
    int webCount = 0;
    for (const auto& item : *members)
    {
        Member member;
        member.kind = item["kind"].toString(); member.nick = item["nick"].toString();
        member.appVersion = item["appVersion"].toString(); member.state = item["state"].toString();
        if (! u32 (item["participantId"], member.participantId) || ! u32 (item["connectionGeneration"], member.connectionGeneration)
            || ! u32 (item["streamEpoch"], member.streamEpoch) || ! ids.insert (member.participantId).second
            || (member.kind != "native" && member.kind != "web") || ! item["nick"].isString() || member.nick.isEmpty()
            || member.nick.length() > 40 || member.nick.getNumBytesAsUTF8() > 160
            || ! item["appVersion"].isString() || member.appVersion.getNumBytesAsUTF8() > 32
            || (member.state != "connecting" && member.state != "connected")
            || ! strings (item["capabilities"], member.capabilities)) return false;
        if (member.kind == "web") ++webCount;
        if (member.participantId == 1 && member.kind != "native") return false;
        const auto* protocols = item["protocols"].getArray();
        if (protocols == nullptr || protocols->isEmpty() || protocols->size() > 2) return false;
        for (const auto& p : *protocols)
        {
            if (! p.isInt() || ((int) p != 1 && (int) p != 2)) return false;
            if (std::find (member.protocols.begin(), member.protocols.end(), (int) p) != member.protocols.end()) return false;
            member.protocols.push_back ((int) p);
        }
        room.members.push_back (std::move (member));
    }
    if (! ids.count (1) || webCount > 1 || (room.roomMode == "legacy2" && (members->size() > 2 || webCount != 0))) return false;
    std::set<uint32_t> reserved;
    for (const auto& item : *reservations)
    {
        Reservation r;
        r.kind = item["kind"].toString();
        if (! u32 (item["participantId"], r.participantId) || r.participantId == 1
            || ! u32 (item["connectionGeneration"], r.connectionGeneration)
            || ! timestamp (item["expiresAtMs"], r.expiresAtMs) || ! reserved.insert (r.participantId).second) return false;
        const auto member = std::find_if (room.members.begin(), room.members.end(), [&] (const Member& m) { return m.participantId == r.participantId; });
        if (member == room.members.end() || member->kind != r.kind || member->connectionGeneration != r.connectionGeneration
            || member->state != "connecting") return false;
        room.reservations.push_back (std::move (r));
    }
    output = std::move (room);
    return true;
}

CollabRoom::CreatedV2 CollabRoom::createV2 (const juce::String& nick, const juce::String& inst, int port,
    const juce::StringArray& lan, const CollabUpnp::Result& upnp, const ClientInfo& info, const CollabTransport::Mapping& stun)
{
    // Reuse the legacy serializer/parser exactly; only the outgoing JSON is extended.
    juce::var json;
    CollabRoom legacy ([&] (const Request& incoming)
    {
        auto outgoing = incoming;
        outgoing.body = juce::JSON::toString (clientBody (juce::JSON::parse (incoming.body), info), true);
        const auto response = request (outgoing);
        json = juce::JSON::parse (response.body);
        return response;
    }, server, defaultCancellation);
    CreatedV2 result;
    static_cast<Created&> (result) = legacy.create (nick, inst, port, lan, upnp, stun);
    readIdentity (result, json);
    return result;
}

CollabRoom::JoinedV2 CollabRoom::joinV2 (const juce::String& code, const juce::String& nick, const juce::String& inst,
    const ClientInfo& info, const CollabTransport::Mapping& stun, int localPort)
{
    juce::var json;
    CollabRoom legacy ([&] (const Request& incoming)
    {
        auto outgoing = incoming;
        outgoing.body = juce::JSON::toString (clientBody (juce::JSON::parse (incoming.body), info), true);
        const auto response = request (outgoing);
        json = juce::JSON::parse (response.body);
        return response;
    }, server, defaultCancellation);
    JoinedV2 result;
    static_cast<Joined&> (result) = legacy.join (code, nick, inst, stun, localPort);
    readIdentity (result, json);
    return result;
}

CollabRoom::ConfirmedV2 CollabRoom::confirmV2 (const juce::String& code, const juce::String& token, bool reachable,
                                              const juce::String& reason, bool punch)
{
    auto body = hostBody (code, token);
    body.getDynamicObject()->setProperty ("reachable", reachable);
    body.getDynamicObject()->setProperty ("reason", reason);
    body.getDynamicObject()->setProperty ("punch", punch);
    juce::var json;
    ConfirmedV2 result;
    static_cast<Reply&> (result) = post ("confirm", body, json);
    if (result.ok() && ! (bool) json["ok"]) result.error = errorMessage (result.statusCode);
    result.punch = result.ok() && (bool) json["punch"];
    readRoom (result, json);
    return result;
}

CollabRoom::BeatV2 CollabRoom::heartbeatV2 (const juce::String& code, const juce::String& token, bool connected,
                                         const std::vector<MemberState>& states)
{
    auto body = hostBody (code, token);
    body.getDynamicObject()->setProperty ("connected", connected);
    juce::Array<juce::var> members;
    for (const auto& state : states)
        members.add (object ({ { "participantId", (juce::int64) state.participantId },
            { "connectionGeneration", (juce::int64) state.connectionGeneration },
            { "state", state.state }, { "streamEpoch", (juce::int64) state.streamEpoch } }));
    body.getDynamicObject()->setProperty ("memberStates", members);
    juce::var json;
    BeatV2 result;
    static_cast<Reply&> (result) = post ("heartbeat", body, json);
    if (result.ok() && ! (bool) json["ok"]) result.error = errorMessage (result.statusCode);
    readRoom (result, json);
    if (! result.ok()) return result;
    result.guest = addresses (json["guest"]["candidates"]);
    if (result.room.apiVersion == 0) return result;
    const auto* peers = json["peers"].getArray();
    if (peers == nullptr || peers->size() > 2) { result.error = errorMessage (result.statusCode); return result; }
    for (const auto& peer : *peers)
    {
        Peer p;
        p.connectionId = peer["connectionId"].toString();
        if (! u32 (peer["participantId"], p.participantId) || (! p.connectionId.isEmpty() && ! hex (p.connectionId, 32)))
        { result.error = errorMessage (result.statusCode); return result; }
        p.candidates = addresses (peer["candidates"]);
        result.peers.push_back (std::move (p));
    }
    return result;
}

CollabRoom::Reply CollabRoom::closeV2 (const juce::String& code, const juce::String& token)
{
    return command ("close", hostBody (code, token));
}

CollabRoom::WebReply CollabRoom::webPost (const char* action, const juce::var& body, int timeoutMs,
                                        std::shared_ptr<Cancellation> cancellation)
{
    WebReply result;
    static_cast<Reply&> (result) = post (action, body, result.json, timeoutMs, std::move (cancellation));
    if (result.ok() && (! result.json["apiVersion"].isInt() || (int) result.json["apiVersion"] != 2))
        result.error = errorMessage (result.statusCode);
    if (result.ok() && result.json.hasProperty ("ok") && ! (bool) result.json["ok"])
        result.error = errorMessage (result.statusCode);
    return result;
}
CollabRoom::WebReply CollabRoom::webInvite (const juce::String& code, const juce::String& token)
{
    return webPost ("web/invite", hostBody (code, token));
}
CollabRoom::WebReply CollabRoom::webAccept (const juce::String& code, const juce::String& token, const juce::String& requestId)
{
    auto body = hostBody (code, token); body.getDynamicObject()->setProperty ("requestId", requestId);
    return webPost ("web/accept", body);
}
CollabRoom::WebReply CollabRoom::webReject (const juce::String& code, const juce::String& token, const juce::String& requestId)
{
    auto body = hostBody (code, token); body.getDynamicObject()->setProperty ("requestId", requestId);
    return webPost ("web/reject", body);
}
CollabRoom::WebReply CollabRoom::webKick (const juce::String& code, const juce::String& token, uint32_t pid, uint32_t generation)
{
    auto body = hostBody (code, token);
    body.getDynamicObject()->setProperty ("participantId", (juce::int64) pid);
    body.getDynamicObject()->setProperty ("connectionGeneration", (juce::int64) generation);
    return webPost ("web/kick", body);
}
CollabRoom::WebReply CollabRoom::webSignal (const juce::String& code, const juce::String& token, uint32_t generation,
                                          uint32_t signalId, const juce::var& signal)
{
    auto body = connectionBody (code, token, generation);
    body.getDynamicObject()->setProperty ("signalId", (juce::int64) signalId);
    body.getDynamicObject()->setProperty ("signal", signal);
    return webPost ("web/signal", body);
}
CollabRoom::WebReply CollabRoom::webEvents (const juce::String& code, const juce::String& token, const juce::String& cursor,
                                          uint32_t generation, std::shared_ptr<Cancellation> cancellation)
{
    auto body = generation == 0 ? hostBody (code, token) : connectionBody (code, token, generation);
    body.getDynamicObject()->setProperty ("cursor", cursor);
    return webPost ("web/events", body, (int) contracts::eventTimeoutMs, std::move (cancellation));
}
CollabRoom::WebReply CollabRoom::webLeave (const juce::String& code, const juce::String& token, uint32_t generation)
{
    return webPost ("web/leave", connectionBody (code, token, generation));
}
}
