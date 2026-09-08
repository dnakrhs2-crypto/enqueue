#include "ControlProtocol.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <type_traits>

namespace gocue::livemix
{

namespace
{
    using P = ControlProtocol;
    using Code = P::ErrorCode;

    struct ErrorInfo { const char* code; const char* message; bool retryable; };
    constexpr ErrorInfo errors[] {
        { "INVALID_JSON", "Invalid JSON object", false },
        { "INVALID_UTF8", "Invalid UTF-8", false },
        { "MESSAGE_TOO_LARGE", "Message exceeds the size limit", false },
        { "STATE_TOO_LARGE", "State exceeds the size limit", false },
        { "UNSUPPORTED_VERSION", "Unsupported protocol version", false },
        { "AUTH_REQUIRED", "Authentication required", true },
        { "AUTH_FAILED", "Authentication failed", true },
        { "HANDSHAKE_TIMEOUT", "Handshake timed out", true },
        { "INVALID_ARGUMENT", "Invalid request argument", false },
        { "UNKNOWN_COMMAND", "Unknown command", false },
        { "UNKNOWN_MESSAGE", "Unknown message type", false },
        { "CHANNEL_NOT_FOUND", "Channel does not exist", false },
        { "FX_NOT_FOUND", "FX channel does not exist", false },
        { "PLUGIN_GROUP_NOT_FOUND", "Plugin group does not exist", false },
        { "INSTANCE_CHANGED", "Control instance has changed", true },
        { "SESSION_CHANGED", "Session has changed", true },
        { "REVISION_CONFLICT", "State revision has changed", true },
        { "DUPLICATE_ID", "Request id has already been used", false },
        { "CONTROL_DISABLED", "External control is disabled", true },
        { "SERVER_STOPPING", "Server is stopping", true },
        { "COMMAND_EXPIRED", "Command expired before execution", false },
        { "RATE_LIMITED", "Request rate limit exceeded", true },
        { "SERVER_BUSY", "Server is busy", true },
        { "INTERNAL_ERROR", "Control operation failed", true }
    };
    static_assert (std::size (errors) == (size_t) Code::internalError + 1);

    const ErrorInfo& errorInfo (Code code)
    {
        const auto index = (size_t) code;
        return errors[index < std::size (errors) ? index : (size_t) Code::internalError];
    }

    bool integer (const juce::var& v, juce::int64 low, juce::int64 high)
    {
        return (v.isInt() || v.isInt64()) && (juce::int64) v >= low && (juce::int64) v <= high;
    }

    bool amount (double v) { return std::isfinite (v) && v >= 0.0 && v <= 1.0; }
    bool amount (const juce::var& v) { return (v.isInt() || v.isInt64() || v.isDouble()) && amount ((double) v); }
    bool group (P::MuteGroup g) { return g == P::MuteGroup::mic || g == P::MuteGroup::fx; }
    bool group (const juce::var& v) { return v.isString() && (v.toString() == "mic" || v.toString() == "fx"); }
    P::MuteGroup readGroup (const juce::var& v) { return v.toString() == "mic" ? P::MuteGroup::mic : P::MuteGroup::fx; }
    const char* groupName (P::MuteGroup g) { return g == P::MuteGroup::mic ? "mic" : "fx"; }
    bool uuid (const juce::var& v) { return v.isString() && P::isWireUuid (v.toString()); }
    bool textField (const juce::var& v, size_t maxBytes)
    {
        return v.isString() && v.toString().isNotEmpty() && v.toString().getNumBytesAsUTF8() <= maxBytes;
    }

    // JUCE accepts trailing commas, single-quoted values, non-JSON escapes and trailing text. Check the full
    // grammar first. The separate nonrecursive preflight below bounds depth BEFORE any recursive parser runs.
    class JsonSyntax
    {
    public:
        explicit JsonSyntax (std::string_view input) : source (input) {}
        bool check() { whitespace(); return peek() == '{' && value() && (whitespace(), pos == source.size()); }
        Code error = Code::invalidJson;
    private:
        char peek() const { return pos < source.size() ? source[pos] : '\0'; }
        bool take (char c) { if (peek() != c) return false; ++pos; return true; }
        static bool digit (char c) { return c >= '0' && c <= '9'; }
        void whitespace() { while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n') ++pos; }
        bool literal (std::string_view s)
        {
            if (source.substr (pos, s.size()) != s) return false;
            pos += s.size();
            return true;
        }
        bool string()
        {
            if (! take ('"')) return false;
            while (pos < source.size())
            {
                const auto c = (unsigned char) source[pos++];
                if (c == '"') return true;
                if (c < 0x20) return false;
                if (c != '\\') continue;
                if (pos == source.size()) return false;
                const auto escape = source[pos++];
                if (escape == 'u')
                {
                    for (int i = 0; i < 4; ++i)
                    {
                        const auto h = peek();
                        if (! digit (h) && ! (h >= 'a' && h <= 'f') && ! (h >= 'A' && h <= 'F')) return false;
                        ++pos;
                    }
                }
                else if (std::string_view ("\"\\/bfnrt").find (escape) == std::string_view::npos)
                    return false;
            }
            return false;
        }
        bool number()
        {
            const auto start = pos;
            take ('-');
            const auto digitsStart = pos;
            if (! take ('0'))
            {
                if (peek() < '1' || peek() > '9') return false;
                while (digit (peek())) ++pos;
            }
            const auto digitsEnd = pos;
            bool floating = false;
            if (take ('.'))
            {
                floating = true;
                if (! digit (peek())) return false;
                while (digit (peek())) ++pos;
            }
            if (take ('e') || take ('E'))
            {
                floating = true;
                if (! take ('+')) take ('-');
                if (! digit (peek())) return false;
                while (digit (peek())) ++pos;
            }
            // Avoid signed integer overflow inside JUCE's parser. No v1 numeric field needs more than int64.
            const auto digits = source.substr (digitsStart, digitsEnd - digitsStart);
            if (! floating && (digits.size() > 19 || (digits.size() == 19 && digits > "9223372036854775807")))
            {
                error = Code::invalidArgument;
                return false;
            }
            const auto token = juce::String::fromUTF8 (source.data() + start, (int) (pos - start));
            if (! std::isfinite (token.getDoubleValue()))
            {
                error = Code::invalidArgument;
                return false;
            }
            return true;
        }
        bool value()
        {
            whitespace();
            if (peek() == '"') return string();
            if (peek() == '-' || digit (peek())) return number();
            if (take ('{'))
            {
                whitespace();
                if (take ('}')) return true;
                do
                {
                    whitespace();
                    if (! string()) return false;
                    whitespace();
                    if (! take (':') || ! value()) return false;
                    whitespace();
                    if (take ('}')) return true;
                } while (take (','));
                return false;
            }
            if (take ('['))
            {
                whitespace();
                if (take (']')) return true;
                do
                {
                    if (! value()) return false;
                    whitespace();
                    if (take (']')) return true;
                } while (take (','));
                return false;
            }
            return literal ("true") || literal ("false") || literal ("null");
        }
        std::string_view source;
        size_t pos = 0;
    };

    P::DecodedLine decodeLine (std::string_view line)
    {
        // Unlike isValidString alone, inspect embedded NULs too: they must not hide bytes from the parser.
        if (! juce::CharPointer_UTF8::isValidString (line.data(), (int) line.size()))
            return P::Error { Code::invalidUtf8, {} };
        if (line.find ('\0') != std::string_view::npos)
            return P::Error { Code::invalidJson, {} };
        int depth = 0;
        bool inString = false, escaped = false;
        for (const auto c : line)
        {
            if (inString)
            {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inString = false;
            }
            else if (c == '"') inString = true;
            else if (c == '{' || c == '[')
            {
                if (++depth > P::maxDepth) return P::Error { Code::invalidArgument, {} };
            }
            else if (c == '}' || c == ']')
            {
                if (--depth < 0) return P::Error { Code::invalidJson, {} };
            }
        }
        JsonSyntax syntax (line);
        if (! syntax.check()) return P::Error { syntax.error, {} };
        juce::var result;
        if (juce::JSON::parse (juce::String::fromUTF8 (line.data(), (int) line.size()), result).failed()
            || result.getDynamicObject() == nullptr)
            return P::Error { Code::invalidJson, {} };
        return result;
    }

    juce::var object() { return juce::var (new juce::DynamicObject()); }
    void put (juce::var& v, const char* key, const juce::var& value) { v.getDynamicObject()->setProperty (key, value); }
    template <typename T>
    void putOptional (juce::var& v, const char* key, const std::optional<T>& value)
    {
        if (value) put (v, key, *value);
    }
    juce::var pluginGroups (const std::vector<P::PluginGroup>& groups)
    {
        juce::Array<juce::var> out;
        for (const auto& g : groups)
        {
            auto v = object();
            put (v, "index", g.index); put (v, "off", g.off);
            out.add (v);
        }
        return out;
    }
    juce::var sends (const std::vector<P::Send>& values)
    {
        juce::Array<juce::var> out;
        for (const auto& s : values)
        {
            auto v = object();
            put (v, "fxId", s.fxId.toString()); put (v, "amount", s.amount); put (v, "pre", s.pre);
            out.add (v);
        }
        return out;
    }
    void context (juce::var& v, const P::Context& c)
    {
        put (v, "instanceId", c.instanceId.toString()); put (v, "sessionId", c.sessionId.toString()); put (v, "revision", c.revision);
    }
    juce::var commandResult (const P::CommandResult& result)
    {
        auto v = object();
        std::visit ([&v] (const auto& r)
        {
            using T = std::decay_t<decltype (r)>;
            if constexpr (std::is_same_v<T, P::OnResult> || std::is_same_v<T, P::AllChannelsResult>) put (v, "on", r.on);
            if constexpr (std::is_same_v<T, P::AllChannelsResult>) put (v, "count", r.count);
            if constexpr (std::is_same_v<T, P::MuteGroupResult>) { put (v, "group", groupName (r.group)); put (v, "muted", r.muted); }
            if constexpr (std::is_same_v<T, P::PluginGroupResult>) { put (v, "index", r.index); put (v, "off", r.off); }
            if constexpr (std::is_same_v<T, P::SendResult>) { put (v, "amount", r.amount); put (v, "pre", r.pre); }
            if constexpr (std::is_same_v<T, P::RequestStateResult>) put (v, "snapshotRevision", r.snapshotRevision);
        }, result);
        return v;
    }
}

const char* ControlProtocol::errorCode (ErrorCode code) { return errorInfo (code).code; }
const char* ControlProtocol::errorMessage (ErrorCode code) { return errorInfo (code).message; }
bool ControlProtocol::isRetryable (ErrorCode code) { return errorInfo (code).retryable; }

bool ControlProtocol::isWireUuid (const juce::String& s)
{
    return s.length() == 32 && s.containsOnly ("0123456789abcdef") && s != "00000000000000000000000000000000";
}

bool ControlProtocol::isRequestId (const juce::String& s)
{
    return s.isNotEmpty() && s.length() <= 16 && s[0] != '0' && s.containsOnly ("0123456789")
        && s.getLargeIntValue() <= maxSafeInteger;
}

ControlProtocol::ValidationResult ControlProtocol::validate (const juce::var& v)
{
    const auto id = v["id"].isString() && isRequestId (v["id"].toString()) ? v["id"].toString() : juce::String();
    const auto fail = [&id] (Code code) -> ValidationResult { return Error { code, id }; };
    if (v.getDynamicObject() == nullptr || id.isEmpty() || ! integer (v["v"], 0, maxSafeInteger) || ! v["type"].isString())
        return fail (Code::invalidArgument);
    if ((juce::int64) v["v"] != version) return fail (Code::unsupportedVersion);
    const auto type = v["type"].toString();
    if (type == "hello")
    {
        if (! textField (v["token"], maxLineBytes) || v["client"].getDynamicObject() == nullptr
            || ! textField (v["client"]["name"], 128) || ! textField (v["client"]["version"], maxLineBytes)
            || ! v["supportedVersions"].isArray() || v["supportedVersions"].size() == 0)
            return fail (Code::invalidArgument);
        Hello h { id, v["token"].toString(), v["client"]["name"].toString(), v["client"]["version"].toString(), {} };
        for (const auto& major : *v["supportedVersions"].getArray())
        {
            if (! integer (major, 1, std::numeric_limits<int>::max())) return fail (Code::invalidArgument);
            h.supportedVersions.push_back ((int) major);
        }
        if (std::find (h.supportedVersions.begin(), h.supportedVersions.end(), version) == h.supportedVersions.end())
            return fail (Code::unsupportedVersion);
        return ClientMessage { std::move (h) };
    }
    if (type != "ping" && type != "command") return fail (Code::unknownMessage);
    if (! uuid (v["instanceId"])) return fail (Code::invalidArgument);
    if (type == "ping") return ClientMessage { Ping { id, juce::Uuid (v["instanceId"].toString()) } };
    if (! textField (v["command"], 64) || v["args"].getDynamicObject() == nullptr) return fail (Code::invalidArgument);
    Command c;
    c.id = id;
    c.instanceId = juce::Uuid (v["instanceId"].toString());
    if (v.hasProperty ("sessionId"))
    {
        if (! uuid (v["sessionId"])) return fail (Code::invalidArgument);
        c.sessionId = juce::Uuid (v["sessionId"].toString());
    }
    if (v.hasProperty ("ifRevision"))
    {
        if (! integer (v["ifRevision"], 0, maxSafeInteger)) return fail (Code::invalidArgument);
        c.ifRevision = (juce::int64) v["ifRevision"];
    }
    const auto name = v["command"].toString();
    const auto& a = v["args"];
    if (name == "setChannelOn" || name == "toggleChannel" || name == "setPluginGroupOff" || name == "setSend")
    {
        if (! uuid (a["channelId"])) return fail (Code::invalidArgument);
        const juce::Uuid channelId (a["channelId"].toString());
        if (name == "setChannelOn")
        {
            if (! a["on"].isBool()) return fail (Code::invalidArgument);
            c.args = SetChannelOn { channelId, (bool) a["on"] };
        }
        else if (name == "toggleChannel") c.args = ToggleChannel { channelId };
        else if (name == "setPluginGroupOff")
        {
            if (! integer (a["index"], 1, 5) || ! a["off"].isBool()) return fail (Code::invalidArgument);
            c.args = SetPluginGroupOff { channelId, (int) a["index"], (bool) a["off"] };
        }
        else
        {
            if (! uuid (a["fxId"]) || (! a.hasProperty ("amount") && ! a.hasProperty ("pre"))) return fail (Code::invalidArgument);
            SetSend s { channelId, juce::Uuid (a["fxId"].toString()), {}, {} };
            if (a.hasProperty ("amount"))
            {
                if (! amount (a["amount"])) return fail (Code::invalidArgument);
                s.amount = (double) a["amount"];
            }
            if (a.hasProperty ("pre"))
            {
                if (! a["pre"].isBool()) return fail (Code::invalidArgument);
                s.pre = (bool) a["pre"];
            }
            c.args = s;
        }
    }
    else if (name == "setAllChannelsOn")
    {
        if (! a["on"].isBool()) return fail (Code::invalidArgument);
        c.args = SetAllChannelsOn { (bool) a["on"] };
    }
    else if (name == "toggleMuteGroup" || name == "setMuteGroup")
    {
        if (! group (a["group"])) return fail (Code::invalidArgument);
        if (name == "toggleMuteGroup") c.args = ToggleMuteGroup { readGroup (a["group"]) };
        else
        {
            if (! a["muted"].isBool()) return fail (Code::invalidArgument);
            c.args = SetMuteGroup { readGroup (a["group"]), (bool) a["muted"] };
        }
    }
    else if (name != "requestState") return fail (Code::unknownCommand);
    if (const auto error = validateCommand (c)) return *error;
    return ClientMessage { std::move (c) };
}

std::optional<ControlProtocol::Error> ControlProtocol::validateCommand (const Command& c)
{
    bool valid = isRequestId (c.id) && ! c.instanceId.isNull()
        && (! c.sessionId || ! c.sessionId->isNull())
        && (c.sessionId || std::holds_alternative<RequestState> (c.args))
        && (! c.ifRevision || (*c.ifRevision >= 0 && *c.ifRevision <= maxSafeInteger));
    valid = valid && std::visit ([] (const auto& a)
    {
        using T = std::decay_t<decltype (a)>;
        if constexpr (std::is_same_v<T, SetChannelOn> || std::is_same_v<T, ToggleChannel>) return ! a.channelId.isNull();
        else if constexpr (std::is_same_v<T, SetPluginGroupOff>) return ! a.channelId.isNull() && a.index >= 1 && a.index <= 5;
        else if constexpr (std::is_same_v<T, SetSend>) return ! a.channelId.isNull() && ! a.fxId.isNull()
            && (a.amount || a.pre) && (! a.amount || amount (*a.amount));
        else if constexpr (std::is_same_v<T, SetMuteGroup> || std::is_same_v<T, ToggleMuteGroup>) return group (a.group);
        else return true;
    }, c.args);
    if (! valid) return Error { Code::invalidArgument, isRequestId (c.id) ? c.id : juce::String() };
    return {};
}

std::vector<ControlProtocol::DecodedLine> ControlProtocol::Decoder::push (const void* bytes, size_t size)
{
    std::vector<DecodedLine> out;
    if (failed || size == 0) return out;
    jassert (bytes != nullptr);
    const auto* data = static_cast<const char*> (bytes);
    for (size_t i = 0; i < size; ++i)
    {
        if (data[i] == '\n')
        {
            auto line = std::string_view (pending);
            if (! line.empty() && line.back() == '\r') line.remove_suffix (1);
            out.push_back (decodeLine (line));
            pending.clear();
            if (std::holds_alternative<Error> (out.back())) { failed = true; break; }
        }
        else
        {
            if (pending.size() == maxLineBytes)
            {
                out.push_back (Error { Code::messageTooLarge, {} });
                pending.clear();
                failed = true;
                break;
            }
            pending.push_back (data[i]);
        }
    }
    return out;
}

std::optional<ControlProtocol::Error> ControlProtocol::Decoder::finish()
{
    if (failed || pending.empty()) return {};
    pending.clear();
    failed = true;
    return Error { Code::invalidJson, {} };
}

void ControlProtocol::Decoder::reset() { pending.clear(); failed = false; }
bool ControlProtocol::ChannelChanges::empty() const { return ! name && ! on && ! muteGroup && ! pluginGroups && ! sends; }
bool ControlProtocol::FxChanges::empty() const { return ! name && ! muteGroup && ! returnAmount; }
bool ControlProtocol::Changes::empty() const
{
    return ! sessionName && ! sessionDirty && ! audioRunning && ! micMuted && ! fxMuted && channels.empty() && fx.empty();
}

juce::var ControlProtocol::toVar (const Projection& p)
{
    auto v = object(), session = object(), audio = object(), muted = object();
    put (session, "name", p.session.name); put (session, "dirty", p.session.dirty);
    put (audio, "running", p.audioRunning); put (muted, "mic", p.micMuted); put (muted, "fx", p.fxMuted);
    put (v, "session", session); put (v, "audio", audio); put (v, "muteGroups", muted);
    juce::Array<juce::var> channels, fx;
    for (const auto& c : p.channels)
    {
        auto item = object();
        put (item, "id", c.id.toString()); put (item, "name", c.name); put (item, "on", c.on); put (item, "muteGroup", c.muteGroup);
        put (item, "pluginGroups", pluginGroups (c.pluginGroups)); put (item, "sends", sends (c.sends));
        channels.add (item);
    }
    for (const auto& f : p.fx)
    {
        auto item = object();
        put (item, "id", f.id.toString()); put (item, "name", f.name); put (item, "muteGroup", f.muteGroup); put (item, "return", f.returnAmount);
        fx.add (item);
    }
    put (v, "channels", channels); put (v, "fx", fx);
    return v;
}

juce::var ControlProtocol::toVar (const Changes& p)
{
    auto v = object();
    if (p.sessionName || p.sessionDirty)
    {
        auto s = object(); putOptional (s, "name", p.sessionName); putOptional (s, "dirty", p.sessionDirty); put (v, "session", s);
    }
    if (p.audioRunning) { auto a = object(); put (a, "running", *p.audioRunning); put (v, "audio", a); }
    if (p.micMuted || p.fxMuted)
    {
        auto m = object(); putOptional (m, "mic", p.micMuted); putOptional (m, "fx", p.fxMuted); put (v, "muteGroups", m);
    }
    if (! p.channels.empty())
    {
        juce::Array<juce::var> values;
        for (const auto& c : p.channels)
        {
            auto item = object(); put (item, "id", c.id.toString());
            putOptional (item, "name", c.name); putOptional (item, "on", c.on); putOptional (item, "muteGroup", c.muteGroup);
            if (c.pluginGroups) put (item, "pluginGroups", pluginGroups (*c.pluginGroups));
            if (c.sends) put (item, "sends", sends (*c.sends));
            values.add (item);
        }
        put (v, "channels", values);
    }
    if (! p.fx.empty())
    {
        juce::Array<juce::var> values;
        for (const auto& f : p.fx)
        {
            auto item = object(); put (item, "id", f.id.toString());
            putOptional (item, "name", f.name); putOptional (item, "muteGroup", f.muteGroup); putOptional (item, "return", f.returnAmount);
            values.add (item);
        }
        put (v, "fx", values);
    }
    return v;
}

ControlProtocol::EncodeResult ControlProtocol::encode (const ServerMessage& message)
{
    auto v = object();
    put (v, "v", version);
    std::visit ([&v] (const auto& m)
    {
        using T = std::decay_t<decltype (m)>;
        if constexpr (std::is_same_v<T, HelloAck>)
        {
            put (v, "type", "helloAck"); put (v, "id", m.id); put (v, "instanceId", m.instanceId.toString());
            auto server = object(); put (server, "name", "LiveMix"); put (server, "version", m.serverVersion); put (v, "server", server);
            juce::Array<juce::var> capabilities;
            for (const auto* name : { "stateDelta", "mic", "muteGroups", "pluginGroups", "sends" }) capabilities.add (name);
            put (v, "capabilities", capabilities); put (v, "eventIntervalMs", 100); put (v, "heartbeatIntervalMs", 5000);
        }
        else if constexpr (std::is_same_v<T, State>)
        {
            put (v, "type", "state"); context (v, m.context);
            constexpr const char* reasons[] { "initial", "requested", "structureChanged", "sessionChanged", "resync" };
            put (v, "reason", reasons[(size_t) m.reason]);
            if (m.reason == StateReason::requested) put (v, "requestId", m.requestId);
            put (v, "state", toVar (m.state));
        }
        else if constexpr (std::is_same_v<T, StateDelta>)
        {
            put (v, "type", "stateDelta"); context (v, m.context); put (v, "baseRevision", m.baseRevision); put (v, "changes", toVar (m.changes));
        }
        else if constexpr (std::is_same_v<T, Ack>)
        {
            put (v, "type", "ack"); put (v, "id", m.id); context (v, m.context);
            put (v, "changed", m.changed); put (v, "result", commandResult (m.result));
        }
        else if constexpr (std::is_same_v<T, ErrorResponse>)
        {
            put (v, "type", "error"); put (v, "id", m.error.id.isEmpty() ? juce::var() : juce::var (m.error.id));
            put (v, "code", errorCode (m.error.code)); put (v, "message", errorMessage (m.error.code)); put (v, "retryable", isRetryable (m.error.code));
            if (m.context) context (v, *m.context);
            putOptional (v, "retryAfterMs", m.retryAfterMs);
            if (! m.supportedVersions.empty())
            {
                juce::Array<juce::var> versions;
                for (auto major : m.supportedVersions) versions.add (major);
                put (v, "supportedVersions", versions);
            }
        }
        else if constexpr (std::is_same_v<T, Pong>)
        {
            put (v, "type", "pong"); put (v, "id", m.id); context (v, m.context);
        }
        else if constexpr (std::is_same_v<T, ServerStatus>)
        {
            put (v, "type", "serverStatus"); put (v, "instanceId", m.instanceId.toString());
            put (v, "status", m.status == Status::disabled ? "disabled" : "stopping");
            constexpr const char* reasons[] { "controlDisabled", "shutdown", "restart" };
            put (v, "reason", reasons[(size_t) m.reason]);
        }
    }, message);
    const auto json = juce::JSON::toString (v, true, 17);
    if (json.getNumBytesAsUTF8() > maxLineBytes)
        return Error { std::holds_alternative<State> (message) || std::holds_alternative<StateDelta> (message)
                         ? Code::stateTooLarge : Code::messageTooLarge, {} };
    return std::string (json.toRawUTF8(), json.getNumBytesAsUTF8()) + '\n';
}

} // namespace gocue::livemix
