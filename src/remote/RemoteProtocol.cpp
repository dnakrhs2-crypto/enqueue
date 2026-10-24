#include "remote/RemoteProtocol.h"
#include <cmath>

namespace gocue::remote
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* value = new juce::DynamicObject();
    for (const auto& field : fields) value->setProperty (field.first, field.second);
    return juce::var (value);
}
juce::String encode (const juce::var& value) { return juce::JSON::toString (value, true); }
juce::var ack (juce::int64 id, const juce::Result& result)
{
    auto value = object ({ { "t", "ack" }, { "id", id }, { "ok", result.wasOk() } });
    if (result.failed()) value.getDynamicObject()->setProperty ("msg", result.getErrorMessage());
    return value;
}
juce::var bye (const juce::String& reason) { return object ({ { "t", "bye" }, { "reason", reason } }); }

Request Request::parse (const juce::String& json)
{
    Request request;
    const auto invalid = juce::Result::fail (juce::String::fromUTF8 ("잘못된 요청"));
    if (juce::JSON::parse (json, request.data).failed() || ! request.data.isObject())
    { request.error = invalid; return request; }
    const auto integer = [&] (const char* field, double minimum = 0.0)
    {
        const auto v = request[field];
        if (! v.isInt() && ! v.isInt64() && ! v.isDouble()) return false;
        const double n = static_cast<double> (v);
        return std::isfinite (n) && n >= minimum && n <= 9007199254740991.0 && n == std::floor (n);
    };
    const auto string = [&] (const char* field) { return request[field].isString() && request[field].toString().isNotEmpty(); };
    const auto nullableId = [&] (const char* field)
    { return request.data.getDynamicObject()->hasProperty (field) && (request[field].isVoid() || string (field)); };
    if (integer ("id", 1.0)) request.id = static_cast<juce::int64> (request["id"]);
    else { request.error = invalid; return request; }
    request.type = request["t"].toString();
    const auto& t = request.type;
    bool valid = false;
    if (t == "go") valid = request["down"].isBool() && integer ("touch");
    else if (t == "cmd") valid = juce::StringArray { "pause", "fade", "panic" }.contains (request["cmd"].toString());
    else if (t == "cue") valid = string ("cue") && integer ("order")
        && juce::StringArray { "pause", "resume", "stop" }.contains (request["op"].toString());
    else if (t == "wait") valid = string ("cue") && integer ("startId")
        && static_cast<juce::int64> (request["startId"]) <= std::numeric_limits<int>::max()
        && juce::StringArray { "pre", "post", "wait" }.contains (request["kind"].toString());
    else if (t == "fire" || t == "stopcue") valid = string ("cue");
    else if (t == "tab") valid = string ("tab");
    else if (t == "playhead") valid = string ("tab") && string ("cue");
    else if (t == "move") valid = string ("tab") && string ("cue") && nullableId ("before") && nullableId ("parent");
    else if (t == "mode") valid = request["show"].isBool();
    else if (t == "ping") valid = true;
    else if (t == "hi") valid = string ("device");
    if (! valid) request.error = invalid;
    return request;
}

juce::var Session::dispatch (const Request& request, const std::function<juce::Result (const Request&)>& execute)
{
    if (request.id > 0 && request.id <= lastId)
        return ack (request.id, request.id == lastId ? lastResult : juce::Result::ok());
    const auto result = request.error.failed() ? request.error : execute (request);
    if (request.id > 0) { lastId = request.id; lastResult = result; }
    return ack (request.id, result);
}
}
