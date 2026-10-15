#include "CollabRoom.h"
#include "ui/UiUtils.h"

namespace gocue::livemix::collab
{
namespace
{
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
}

CollabRoom::CollabRoom (RequestFunction fn, juce::String base) : request (fn ? std::move (fn) : httpRequest)
{
    if (base.isEmpty()) base = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_COLLAB_SERVER", {});
    server = base.trim().trimCharactersAtEnd ("/");
    if (server.isEmpty()) server = "https://xn--2j1bo5s81gjmag88b.com";
}

CollabRoom::Response CollabRoom::httpRequest (const Request& request)
{
    Response response;
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

CollabRoom::Reply CollabRoom::post (const char* action, const juce::var& body, juce::var& json)
{
    const auto response = request ({ server + "/api/livemix/collab/" + action, juce::JSON::toString (body, true) });
    json = juce::JSON::parse (response.body);
    const bool ok = response.statusCode == 200 && json.getDynamicObject() != nullptr && ! json.hasProperty ("error");
    return { response.statusCode, ok ? juce::String() : errorMessage (response.statusCode) };
}

CollabRoom::Reply CollabRoom::command (const char* action, const juce::var& body)
{
    juce::var json;
    auto reply = post (action, body, json);
    if (reply.ok() && ! (bool) json.getProperty ("ok", false)) reply.error = errorMessage (reply.statusCode);
    return reply;
}

CollabRoom::Created CollabRoom::create (const juce::String& nick, const juce::String& inst, int port,
                                      const juce::StringArray& lan, const CollabUpnp::Result& upnp)
{
    juce::Array<juce::var> addresses;
    for (const auto& ip : lan) if (addresses.size() < 8) addresses.add (ip);
    juce::var json;
    Created result;
    static_cast<Reply&> (result) = post ("create", object ({ { "nick", nick }, { "inst", inst }, { "port", port },
        { "lan", addresses }, { "upnp", object ({ { "found", upnp.found }, { "mapped", upnp.mapped }, { "externalIp", upnp.externalIp } }) } }), json);
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

CollabRoom::Reply CollabRoom::confirm (const juce::String& code, const juce::String& token, bool reachable, const juce::String& reason)
{
    return command ("confirm", object ({ { "code", code }, { "hostToken", token }, { "reachable", reachable }, { "reason", reason } }));
}
CollabRoom::Reply CollabRoom::heartbeat (const juce::String& code, const juce::String& token, bool connected)
{
    return command ("heartbeat", object ({ { "code", code }, { "hostToken", token }, { "connected", connected } }));
}
CollabRoom::Reply CollabRoom::close (const juce::String& code, const juce::String& token)
{
    return command ("close", object ({ { "code", code }, { "hostToken", token } }));
}
CollabRoom::Joined CollabRoom::join (const juce::String& code, const juce::String& nick, const juce::String& inst)
{
    juce::var json;
    Joined result;
    static_cast<Reply&> (result) = post ("join", object ({ { "code", code }, { "nick", nick }, { "inst", inst } }), json);
    if (! result.ok()) return result;
    const auto session = json["session"].toString();
    result.hostNick = json["hostNick"].toString(); result.sameNetwork = (bool) json["sameNetwork"];
    if (auto* candidates = json["candidates"].getArray())
        for (const auto& candidate : *candidates)
        {
            const auto ip = candidate["ip"].toString();
            const int port = candidate["port"];
            if (ip.containsOnly ("0123456789.") && juce::IPAddress (ip).toString() == ip && port > 0 && port <= 65535)
                result.candidates.push_back ({ ip, port });
        }
    if (! hex (session, 16) || result.candidates.empty() || result.hostNick.isEmpty())
        result.error = errorMessage (result.statusCode);
    else result.session = (uint64_t) session.getHexValue64();
    return result;
}
}
