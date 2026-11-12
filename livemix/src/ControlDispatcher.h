#pragma once

#include "ControlState.h"

#include <functional>

namespace gocue::livemix
{

/** Synchronous message-thread-only command application. No locks, callbacks installed, or direct engine access.
    The server serializes connections here after authentication, replay/expiry/rate checks. Existing callbacks
    run normally inside setters and must only notify the server of pending capture, never reenter dispatch.
    requestState returns a typed snapshotRevision; the server queues its ack then the current full state. */
class ControlDispatcher
{
public:
    using Result = std::variant<ControlProtocol::Ack, ControlProtocol::ErrorResponse>;
    struct PluginGroupWindowsRequest
    {
        int index = 1;   // wire numbering: 1..5
        std::optional<juce::Uuid> channelId;   // absent: every mic
        std::optional<bool> open;             // absent: toggle
    };
    using PluginGroupWindowsResponse = std::variant<ControlProtocol::PluginGroupWindowsResult, ControlProtocol::ErrorCode>;
    using PluginGroupWindowsHandler = std::function<PluginGroupWindowsResponse (const PluginGroupWindowsRequest&)>;
    /** Optional UI handler; window commands fail with INTERNAL_ERROR when it is absent. */
    ControlDispatcher (MixDocument&, MuteGroups&, ControlState&, juce::Uuid instanceId, std::function<bool()> audioRunning,
                       PluginGroupWindowsHandler = {});
    Result dispatch (const ControlProtocol::Command&);

private:
    MixDocument& document;
    MuteGroups& groups;
    ControlState& state;
    const juce::Uuid instanceId;
    std::function<bool()> audioRunning;
    PluginGroupWindowsHandler pluginGroupWindows;
};

} // namespace gocue::livemix
