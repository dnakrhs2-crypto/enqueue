#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace gocue::livemix
{

/** NDJSON v1 wire values and codec. No document, engine, socket or thread affinity. Values own their data;
    a decoder belongs to one connection. Authentication, request ordering and replay protection belong to the server. */
class ControlProtocol
{
public:
    static constexpr int version = 1;
    static constexpr size_t maxLineBytes = 64 * 1024;   // LF excluded; a CR in CRLF counts towards the limit
    static constexpr int maxDepth = 16;                // the root object is depth 1
    static constexpr juce::int64 maxSafeInteger = 9007199254740991LL;

    enum class ErrorCode
    {
        invalidJson, invalidUtf8, messageTooLarge, stateTooLarge, unsupportedVersion,
        authRequired, authFailed, handshakeTimeout, invalidArgument, unknownCommand, unknownMessage,
        channelNotFound, fxNotFound, pluginGroupNotFound, instanceChanged, sessionChanged, revisionConflict,
        duplicateId, controlDisabled, serverStopping, commandExpired, rateLimited, serverBusy, internalError
    };

    struct Error
    {
        ErrorCode code = ErrorCode::internalError;
        juce::String id;   // empty encodes as null; never echo an invalid request id
    };

    static const char* errorCode (ErrorCode);
    static const char* errorMessage (ErrorCode);
    static bool isRetryable (ErrorCode);
    static bool isWireUuid (const juce::String&);
    static bool isRequestId (const juce::String&);

    enum class MuteGroup { mic, fx };
    struct SetChannelOn { juce::Uuid channelId; bool on = false; };
    struct ToggleChannel { juce::Uuid channelId; };
    struct SetAllChannelsOn { bool on = false; };
    struct ToggleMuteGroup { MuteGroup group = MuteGroup::mic; };
    struct SetMuteGroup { MuteGroup group = MuteGroup::mic; bool muted = false; };
    struct SetPluginGroupOff { juce::Uuid channelId; int index = 1; bool off = false; };
    struct SetSend { juce::Uuid channelId, fxId; std::optional<double> amount; std::optional<bool> pre; };
    struct RequestState {};
    using CommandArgs = std::variant<SetChannelOn, ToggleChannel, SetAllChannelsOn, ToggleMuteGroup,
                                     SetMuteGroup, SetPluginGroupOff, SetSend, RequestState>;
    struct Command
    {
        juce::String id;
        juce::Uuid instanceId = juce::Uuid::null();
        std::optional<juce::Uuid> sessionId;
        std::optional<juce::int64> ifRevision;
        CommandArgs args = RequestState {};
    };
    struct Hello
    {
        juce::String id, token, clientName, clientVersion;
        std::vector<int> supportedVersions;
    };
    struct Ping { juce::String id; juce::Uuid instanceId; };
    using ClientMessage = std::variant<Hello, Command, Ping>;
    using ValidationResult = std::variant<ClientMessage, Error>;

    /** Validates the client direction, envelope and all command arguments. Unknown optional fields are ignored.
        requestState is a command with args {}, and may omit sessionId. Handshake phase is the server's concern. */
    static ValidationResult validate (const juce::var&);
    /** Also used by dispatch to reject invalid values in a directly constructed typed command. */
    static std::optional<Error> validateCommand (const Command&);

    using DecodedLine = std::variant<juce::var, Error>;
    class Decoder
    {
    public:
        std::vector<DecodedLine> push (const void* bytes, size_t size);
        std::vector<DecodedLine> push (const std::string& bytes) { return push (bytes.data(), bytes.size()); }
        /** EOF with an unfinished line is an error, even if its JSON would otherwise be complete. */
        std::optional<Error> finish();
        bool hasFailed() const noexcept { return failed; }
        size_t bufferedBytes() const noexcept { return pending.size(); }
        void reset();   // only for a new connection
    private:
        std::string pending;
        bool failed = false;
    };

    struct Session
    {
        juce::String name;
        bool dirty = false;
        bool operator== (const Session& o) const { return std::tie (name, dirty) == std::tie (o.name, o.dirty); }
    };
    struct PluginGroup
    {
        int index = 1;
        bool off = false;
        bool operator== (const PluginGroup& o) const { return std::tie (index, off) == std::tie (o.index, o.off); }
    };
    struct Send
    {
        juce::Uuid fxId;
        double amount = 0.0;
        bool pre = false;
        bool operator== (const Send& o) const { return std::tie (fxId, amount, pre) == std::tie (o.fxId, o.amount, o.pre); }
    };
    struct Channel
    {
        juce::Uuid id;
        juce::String name;
        bool on = false, muteGroup = false;
        std::vector<PluginGroup> pluginGroups;
        std::vector<Send> sends;
        bool operator== (const Channel& o) const
        {
            return std::tie (id, name, on, muteGroup, pluginGroups, sends)
                == std::tie (o.id, o.name, o.on, o.muteGroup, o.pluginGroups, o.sends);
        }
    };
    struct Fx
    {
        juce::Uuid id;
        juce::String name;
        bool muteGroup = false;
        double returnAmount = 1.0;
        bool operator== (const Fx& o) const
        {
            return std::tie (id, name, muteGroup, returnAmount) == std::tie (o.id, o.name, o.muteGroup, o.returnAmount);
        }
    };
    struct Projection
    {
        Session session;
        bool audioRunning = false;
        std::vector<Channel> channels;
        std::vector<Fx> fx;
        bool micMuted = false, fxMuted = false;
        bool operator== (const Projection& o) const
        {
            return std::tie (session, audioRunning, channels, fx, micMuted, fxMuted)
                == std::tie (o.session, o.audioRunning, o.channels, o.fx, o.micMuted, o.fxMuted);
        }
    };
    struct ChannelChanges
    {
        juce::Uuid id;
        std::optional<juce::String> name;
        std::optional<bool> on, muteGroup;
        std::optional<std::vector<PluginGroup>> pluginGroups;
        std::optional<std::vector<Send>> sends;
        bool empty() const;
    };
    struct FxChanges
    {
        juce::Uuid id;
        std::optional<juce::String> name;
        std::optional<bool> muteGroup;
        std::optional<double> returnAmount;
        bool empty() const;
    };
    struct Changes
    {
        std::optional<juce::String> sessionName;
        std::optional<bool> sessionDirty, audioRunning, micMuted, fxMuted;
        std::vector<ChannelChanges> channels;
        std::vector<FxChanges> fx;
        bool empty() const;
    };

    struct Context
    {
        juce::Uuid instanceId, sessionId;
        juce::int64 revision = 0;
    };
    struct OnResult { bool on = false; };
    struct AllChannelsResult { bool on = false; int count = 0; };
    struct MuteGroupResult { MuteGroup group = MuteGroup::mic; bool muted = false; };
    struct PluginGroupResult { int index = 1; bool off = false; };
    struct SendResult { double amount = 0.0; bool pre = false; };
    struct RequestStateResult { juce::int64 snapshotRevision = 0; };
    using CommandResult = std::variant<OnResult, AllChannelsResult, MuteGroupResult, PluginGroupResult, SendResult, RequestStateResult>;
    struct Ack { juce::String id; Context context; bool changed = false; CommandResult result; };
    struct ErrorResponse
    {
        Error error;
        std::optional<Context> context;   // omitted before authentication
        std::optional<int> retryAfterMs;
        std::vector<int> supportedVersions;
    };
    struct HelloAck { juce::String id; juce::Uuid instanceId; juce::String serverVersion; };
    enum class StateReason { initial, requested, structureChanged, sessionChanged, resync };
    struct State { Context context; StateReason reason = StateReason::initial; juce::String requestId; Projection state; };
    struct StateDelta { Context context; juce::int64 baseRevision = 0; Changes changes; };
    struct Pong { juce::String id; Context context; };
    enum class Status { disabled, stopping };
    enum class StatusReason { controlDisabled, shutdown, restart };
    struct ServerStatus { juce::Uuid instanceId; Status status = Status::disabled; StatusReason reason = StatusReason::controlDisabled; };
    using ServerMessage = std::variant<HelloAck, State, StateDelta, Ack, ErrorResponse, Pong, ServerStatus>;
    using EncodeResult = std::variant<std::string, Error>;
    /** Compact UTF-8 plus one LF. Oversize states return STATE_TOO_LARGE without truncating names. */
    static EncodeResult encode (const ServerMessage&);
    static juce::var toVar (const Projection&);
    static juce::var toVar (const Changes&);
};

} // namespace gocue::livemix
