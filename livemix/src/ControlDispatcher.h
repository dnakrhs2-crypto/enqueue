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
    ControlDispatcher (MixDocument&, MuteGroups&, ControlState&, juce::Uuid instanceId, std::function<bool()> audioRunning);
    Result dispatch (const ControlProtocol::Command&);

private:
    MixDocument& document;
    MuteGroups& groups;
    ControlState& state;
    const juce::Uuid instanceId;
    std::function<bool()> audioRunning;
};

} // namespace gocue::livemix
