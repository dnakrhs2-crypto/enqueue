#include "ControlProtocol.h"

#include <algorithm>
#include <limits>

namespace gocue::tests
{

using P = gocue::livemix::ControlProtocol;

class ControlProtocolTests : public juce::UnitTest
{
public:
    ControlProtocolTests() : juce::UnitTest ("LiveMix control protocol", "LiveMix") {}

    static std::string command (const std::string& name, const std::string& args)
    {
        return "{\"v\":1,\"type\":\"command\",\"id\":\"2\",\"instanceId\":\"aaaaaaaaaaaa4aaa8aaaaaaaaaaaaaaa\","
               "\"sessionId\":\"bbbbbbbbbbbb4bbb8bbbbbbbbbbbbbbb\",\"command\":\"" + name + "\",\"args\":" + args + "}";
    }

    juce::var decode (const std::string& wire)
    {
        P::Decoder decoder;
        const auto lines = decoder.push (wire);
        expectEquals ((int) lines.size(), 1);
        if (lines.size() != 1) return {};
        const auto* value = std::get_if<juce::var> (&lines[0]);
        expect (value != nullptr);
        return value != nullptr ? *value : juce::var();
    }

    juce::var encoded (const P::ServerMessage& message)
    {
        const auto wire = P::encode (message);
        const auto* bytes = std::get_if<std::string> (&wire);
        expect (bytes != nullptr);
        if (bytes == nullptr) return {};
        expect (! bytes->empty() && bytes->back() == '\n');
        expectEquals ((int) std::count (bytes->begin(), bytes->end(), '\n'), 1);
        return decode (*bytes);
    }

    void framingError (const std::string& wire, P::ErrorCode code)
    {
        P::Decoder decoder;
        const auto lines = decoder.push (wire + "\n{}\n");
        expectEquals ((int) lines.size(), 1);   // do not reinterpret bytes after the first bad frame
        if (lines.size() == 1)
        {
            const auto* error = std::get_if<P::Error> (&lines[0]);
            expect (error != nullptr);
            if (error != nullptr) { expect (error->code == code); expect (error->id.isEmpty()); }
        }
        expect (decoder.hasFailed());
        expect (decoder.push ("{}\n").empty());
    }

    void invalid (const juce::var& json, P::ErrorCode code = P::ErrorCode::invalidArgument)
    {
        const auto result = P::validate (json);
        const auto* error = std::get_if<P::Error> (&result);
        expect (error != nullptr);
        if (error != nullptr) expectEquals (juce::String (P::errorCode (error->code)), juce::String (P::errorCode (code)));
    }

    void runTest() override
    {
        beginTest ("UTF-8, escaped strings, every split position, LF/CRLF and multiple lines in one read");
        {
            const std::string line = u8"{\"name\":\"곰 방송 😀\",\"escaped\":\"a\\n\\\"[{}]\\\\z\"}";
            for (size_t split = 0; split <= line.size(); ++split)
            {
                P::Decoder decoder;
                expect (decoder.push (line.substr (0, split)).empty());
                const auto result = decoder.push (line.substr (split) + "\r\n{}\n");
                expectEquals ((int) result.size(), 2);
                if (result.size() == 2)
                {
                    const auto* json = std::get_if<juce::var> (&result[0]);
                    expect (json != nullptr);
                    if (json != nullptr) expectEquals ((*json)["name"].toString(), juce::String::fromUTF8 ("곰 방송 😀"));
                    expect (std::holds_alternative<juce::var> (result[1]));
                }
                expect (! decoder.finish());
                expectEquals ((int) decoder.bufferedBytes(), 0);
            }
            P::Decoder decoder;
            for (const auto c : line) expect (decoder.push (&c, 1).empty());
            expectEquals ((int) decoder.push ("\n").size(), 1);
            expect (decoder.push ("{\"x\":").empty());
            expect (decoder.finish().has_value());
            expect (decoder.hasFailed());
            decoder.reset();
            expectEquals ((int) decoder.push ("{}\n").size(), 1);
        }

        beginTest ("strict JSON and Unicode reject malformed input and stop at the first framing error");
        {
            for (const auto* bad : { "", " ", "[]", "null", "42", "{}{}", "{} garbage", "{", "{\"x\":1,}",
                                    "{\"x\":[1,]}", "{\"x\":'text'}", "{\"x\":\"\\q\"}", "{\"x\":01}",
                                    "{\"x\":- 1}", "{\"x\":1.}", "{\"x\":1e}", "{\"x\":NaN}",
                                    "{\"x\":true false}", "{\"x\":\"\\uD800\"}", "{\"x\":\"\\uDC00\"}",
                                    "{\"x\":\"raw\tcontrol\"}", "GET / HTTP/1.1", "\xef\xbb\xbf{}" })
                framingError (bad, P::ErrorCode::invalidJson);
            framingError (std::string ("{}\0{}", 5), P::ErrorCode::invalidJson);
            for (const auto* bad : { "\xc0\x80", "\x80", "\xe0\x80\x80", "\xed\xa0\x80", "\xf0\x80\x80\x80",
                                    "\xf4\x90\x80\x80", "\xff", "\xe3\x81", "\xe3(" })
                framingError (std::string ("{\"x\":\"") + bad + "\"}", P::ErrorCode::invalidUtf8);
            framingError ("{\"n\":999999999999999999999999999}", P::ErrorCode::invalidArgument);
            framingError ("{\"n\":1e999}", P::ErrorCode::invalidArgument);
        }

        beginTest ("depth 16 and 64 KiB exact boundaries, including no-newline oversize input");
        {
            const auto nested = [] (int arrays) { return "{\"x\":" + std::string ((size_t) arrays, '[') + "0" + std::string ((size_t) arrays, ']') + "}"; };
            expect (decode (nested (15) + "\n").isObject());
            framingError (nested (16), P::ErrorCode::invalidArgument);
            expect (decode ("{}" + std::string (P::maxLineBytes - 2, ' ') + "\n").isObject());
            expect (decode ("{}" + std::string (P::maxLineBytes - 3, ' ') + "\r\n").isObject());
            framingError ("{}" + std::string (P::maxLineBytes - 1, ' '), P::ErrorCode::messageTooLarge);
            P::Decoder decoder;
            expect (decoder.push (std::string (P::maxLineBytes, 'x')).empty());
            const auto result = decoder.push ("x");
            expectEquals ((int) result.size(), 1);
            if (! result.empty()) expect (std::get<P::Error> (result[0]).code == P::ErrorCode::messageTooLarge);
            expect (decoder.hasFailed());
            expectEquals ((int) decoder.bufferedBytes(), 0);
        }

        beginTest ("all eight commands, hello negotiation, ping and session-free requestState have typed values");
        {
            const std::string channel = "\"channelId\":\"11111111111141118111111111111111\"";
            const std::string fx = "\"fxId\":\"22222222222242228222222222222222\"";
            const std::pair<std::string, std::string> commands[] {
                { "setChannelOn", "{" + channel + ",\"on\":false}" }, { "toggleChannel", "{" + channel + "}" },
                { "setAllChannelsOn", "{\"on\":true}" }, { "toggleMuteGroup", "{\"group\":\"mic\"}" },
                { "setMuteGroup", "{\"group\":\"fx\",\"muted\":false}" },
                { "setPluginGroupOff", "{" + channel + ",\"index\":2,\"off\":true}" },
                { "setSend", "{" + channel + "," + fx + ",\"amount\":0.38,\"pre\":false}" }, { "requestState", "{}" }
            };
            for (size_t i = 0; i < std::size (commands); ++i)
            {
                const auto result = P::validate (decode (command (commands[i].first, commands[i].second) + "\n"));
                const auto* message = std::get_if<P::ClientMessage> (&result);
                expect (message != nullptr);
                if (message != nullptr)
                {
                    const auto* c = std::get_if<P::Command> (message);
                    expect (c != nullptr);
                    if (c != nullptr) { expectEquals ((int) c->args.index(), (int) i); expect (c->sessionId.has_value()); }
                }
            }
            auto request = juce::JSON::parse (command ("requestState", "{}").c_str());
            request.getDynamicObject()->removeProperty ("sessionId");
            expect (std::holds_alternative<P::ClientMessage> (P::validate (request)));
            request.getDynamicObject()->setProperty ("futureOption", 1);
            expect (std::holds_alternative<P::ClientMessage> (P::validate (request)));
            auto hello = decode ("{\"v\":1,\"type\":\"hello\",\"id\":\"1\",\"supportedVersions\":[2,1],\"token\":\"test-token\","
                                 "\"client\":{\"name\":\"LiveMix Stream Deck\",\"version\":\"1.0.0\"}}\n");
            const auto h = P::validate (hello);
            expect (std::holds_alternative<P::ClientMessage> (h));
            if (const auto* m = std::get_if<P::ClientMessage> (&h)) expect (std::holds_alternative<P::Hello> (*m));
            hello.getDynamicObject()->setProperty ("supportedVersions", juce::Array<juce::var> { 2 });
            invalid (hello, P::ErrorCode::unsupportedVersion);
            // NamedValueSet's array equality can equate true with 2: replace the field so this really tests a bool.
            hello.getDynamicObject()->removeProperty ("supportedVersions");
            hello.getDynamicObject()->setProperty ("supportedVersions", juce::Array<juce::var> { true });
            invalid (hello);
            hello.getDynamicObject()->removeProperty ("supportedVersions");
            hello.getDynamicObject()->setProperty ("supportedVersions", juce::Array<juce::var> { 1 });
            hello["client"].getDynamicObject()->setProperty ("name", juce::String::repeatedString (juce::String::fromUTF8 ("곰"), 43));
            invalid (hello);   // 129 UTF-8 bytes, not 43 bytes
            const auto ping = P::validate (decode ("{\"v\":1,\"type\":\"ping\",\"id\":\"3\",\"instanceId\":\"aaaaaaaaaaaa4aaa8aaaaaaaaaaaaaaa\"}\n"));
            expect (std::holds_alternative<P::ClientMessage> (ping));
            if (const auto* m = std::get_if<P::ClientMessage> (&ping)) expect (std::holds_alternative<P::Ping> (*m));
        }

        beginTest ("strict ids, enums, required fields, bools, indexes, numeric ranges and unknown direction/command");
        {
            for (const auto* id : { "", "0", "01", "-1", "1.0", "9007199254740992", "10000000000000000", "1a" }) expect (! P::isRequestId (id));
            expect (P::isRequestId ("1")); expect (P::isRequestId ("9007199254740991"));
            for (const auto* id : { "", "00000000000000000000000000000000", "AAAAAAAAAAAA4AAA8AAAAAAAAAAAAAAA",
                                   "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "gaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" }) expect (! P::isWireUuid (id));
            expect (P::isWireUuid ("aaaaaaaaaaaa4aaa8aaaaaaaaaaaaaaa"));
            auto request = juce::JSON::parse (command ("requestState", "{}").c_str());
            for (const auto* field : { "id", "instanceId", "sessionId", "ifRevision", "args", "command", "v", "type" })
            {
                auto bad = request.clone(); bad.getDynamicObject()->setProperty (field, juce::var()); invalid (bad);
            }
            auto badVersion = request.clone(); badVersion.getDynamicObject()->setProperty ("v", 2); invalid (badVersion, P::ErrorCode::unsupportedVersion);
            auto badType = request.clone(); badType.getDynamicObject()->setProperty ("type", "ack"); invalid (badType, P::ErrorCode::unknownMessage);
            invalid (juce::JSON::parse (command ("launchApp", "{}").c_str()), P::ErrorCode::unknownCommand);
            const std::string channel = "\"channelId\":\"11111111111141118111111111111111\"";
            const std::string pair = channel + ",\"fxId\":\"22222222222242228222222222222222\"";
            for (const auto* value : { "null", "1", "\"true\"" }) invalid (juce::JSON::parse (command ("setChannelOn", "{" + channel + ",\"on\":" + value + "}").c_str()));
            for (const auto* value : { "0", "6", "1.5", "2.0", "true", "null", "\"2\"" }) invalid (juce::JSON::parse (command ("setPluginGroupOff", "{" + channel + ",\"index\":" + value + ",\"off\":true}").c_str()));
            for (const auto* value : { "-0.01", "1.01", "null", "true", "\"0.5\"" }) invalid (juce::JSON::parse (command ("setSend", "{" + pair + ",\"amount\":" + value + "}").c_str()));
            invalid (juce::JSON::parse (command ("setSend", "{" + pair + "}").c_str()));
            invalid (juce::JSON::parse (command ("setSend", "{" + pair + ",\"pre\":null}").c_str()));
            invalid (juce::JSON::parse (command ("toggleMuteGroup", "{\"group\":\"master\"}").c_str()));
            invalid (juce::JSON::parse (command ("setChannelOn", "{}").c_str()));
            auto noSession = juce::JSON::parse (command ("toggleChannel", "{" + channel + "}").c_str());
            noSession.getDynamicObject()->removeProperty ("sessionId"); invalid (noSession);
            for (const auto& value : { juce::var (-1), juce::var (1.5), juce::var (P::maxSafeInteger + 1), juce::var (true) })
            {
                auto bad = request.clone(); bad.getDynamicObject()->setProperty ("ifRevision", value); invalid (bad);
            }
            for (const auto* partial : { "\"amount\":0", "\"amount\":1", "\"pre\":false" })
                expect (std::holds_alternative<P::ClientMessage> (P::validate (juce::JSON::parse (command ("setSend", "{" + pair + "," + partial + "}").c_str()))));
            P::Command typed { "2", juce::Uuid(), juce::Uuid(), {}, P::SetSend { juce::Uuid(), juce::Uuid(), std::numeric_limits<double>::quiet_NaN(), {} } };
            expect (P::validateCommand (typed).has_value());
            std::get<P::SetSend> (typed.args).amount = std::numeric_limits<double>::infinity();
            expect (P::validateCommand (typed).has_value());
        }

        beginTest ("every server message and ack result encodes required fields, errors are stable and states are bounded");
        {
            const P::Context context { juce::Uuid(), juce::Uuid(), P::maxSafeInteger };
            const auto hello = encoded (P::HelloAck { "1", context.instanceId, "0.5.3" });
            expectEquals (hello["type"].toString(), juce::String ("helloAck"));
            expectEquals (hello["server"]["version"].toString(), juce::String ("0.5.3"));
            expectEquals (hello["capabilities"].size(), 5);
            expectEquals ((int) hello["eventIntervalMs"], 100); expectEquals ((int) hello["heartbeatIntervalMs"], 5000);
            P::Projection projection;
            projection.session.name = juce::String::fromUTF8 ("곰 \"방송\"\n");
            for (const auto reason : { P::StateReason::initial, P::StateReason::requested, P::StateReason::structureChanged, P::StateReason::sessionChanged, P::StateReason::resync })
            {
                const auto state = encoded (P::State { context, reason, "9", projection });
                expectEquals (state["type"].toString(), juce::String ("state"));
                expectEquals (state["state"]["session"]["name"].toString(), projection.session.name);
                expect (state.hasProperty ("requestId") == (reason == P::StateReason::requested));
                expectEquals ((juce::int64) state["revision"], P::maxSafeInteger);
            }
            const auto delta = encoded (P::StateDelta { context, context.revision - 2, {} });
            expectEquals (delta["type"].toString(), juce::String ("stateDelta"));
            expect (delta["changes"].isObject());
            expectEquals (delta["changes"].getDynamicObject()->getProperties().size(), 0);
            expectEquals ((juce::int64) delta["baseRevision"], context.revision - 2);
            const P::CommandResult results[] { P::OnResult { true }, P::AllChannelsResult { false, 2 }, P::MuteGroupResult { P::MuteGroup::fx, true },
                                              P::PluginGroupResult { 2, true }, P::SendResult { 0.38, false }, P::RequestStateResult { context.revision } };
            for (const auto& result : results)
            {
                const auto ack = encoded (P::Ack { "2", context, false, result });
                expectEquals (ack["type"].toString(), juce::String ("ack"));
                for (const auto* field : { "id", "instanceId", "sessionId", "revision", "changed", "result" }) expect (ack.hasProperty (field));
                expect (ack["changed"].isBool() && ! (bool) ack["changed"]);
                expect (ack["result"].isObject());
            }
            const auto pong = encoded (P::Pong { "3", context });
            expectEquals (pong["type"].toString(), juce::String ("pong"));
            expectEquals (pong["sessionId"].toString(), context.sessionId.toString());
            for (const auto reason : { P::StatusReason::controlDisabled, P::StatusReason::shutdown, P::StatusReason::restart })
            {
                const auto status = encoded (P::ServerStatus { context.instanceId, reason == P::StatusReason::controlDisabled ? P::Status::disabled : P::Status::stopping, reason });
                expectEquals (status["type"].toString(), juce::String ("serverStatus"));
                expect (! status.hasProperty ("id")); expect (! status.hasProperty ("sessionId"));
            }
            juce::StringArray codes;
            for (int i = 0; i <= (int) P::ErrorCode::internalError; ++i)
            {
                const auto code = (P::ErrorCode) i;
                const auto error = encoded (P::ErrorResponse { { code, "2" }, context, 100, { 1 } });
                expectEquals (error["code"].toString(), juce::String (P::errorCode (code)));
                expect (error["message"].toString().isNotEmpty()); expect (error["retryable"].isBool());
                expect (! codes.contains (error["code"].toString())); codes.add (error["code"].toString());
                expectEquals ((int) error["retryAfterMs"], 100); expectEquals (error["supportedVersions"].size(), 1);
            }
            const auto unauthenticated = encoded (P::ErrorResponse { { P::ErrorCode::invalidJson, {} }, {}, {}, {} });
            expect (unauthenticated["id"].isVoid()); expect (! unauthenticated.hasProperty ("instanceId"));
            projection.session.name = juce::String::repeatedString ("x", (int) P::maxLineBytes);
            const auto tooLarge = P::encode (P::State { context, P::StateReason::initial, {}, projection });
            expect (std::holds_alternative<P::Error> (tooLarge));
            if (const auto* error = std::get_if<P::Error> (&tooLarge)) expect (error->code == P::ErrorCode::stateTooLarge);
        }
    }
};

static ControlProtocolTests controlProtocolTests;

} // namespace gocue::tests
