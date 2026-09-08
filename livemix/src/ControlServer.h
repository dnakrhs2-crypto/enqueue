#pragma once

#include "ControlState.h"
#include "ControlSocket.h"

#include <functional>
#include <memory>

namespace gocue::livemix
{

/** All public methods, construction and destruction are message-thread-only. Does not install or replace
    document/group callbacks. The caller forwards notifications after its normal apply/reset/UI work, and
    calls beginShutdown before the final save, then stop before destroying the document or mute groups. */
class ControlServer
{
public:
    struct Options
    {
        bool enabled = false;
        int preferredPort = ControlSocket::preferredPort;
        juce::String appVersion;       // empty uses this target's application version
        juce::File discoveryDirectory; // empty -> %APPDATA%/LiveMix/control; otherwise the exact directory
        juce::File logDirectory;       // empty -> %APPDATA%/LiveMix/logs; tests use an isolated directory
    };
    struct Status
    {
        bool enabled = false;
        int port = 0, connectedCount = 0; // only completed hellos count
        juce::String error;              // stable diagnostic code, never a path, token or raw request
        bool starting = false;
        juce::String host = "127.0.0.1";
        juce::String address() const { return port > 0 ? host + ":" + juce::String (port) : juce::String(); }
    };
    using ChangeKind = ControlState::ChangeKind;

    ControlServer (MixDocument&, MuteGroups&, std::function<bool()> audioRunning);
    ~ControlServer();
    void start (Options);
    void setEnabled (bool);
    void beginShutdown();
    void stop();
    void documentChanged (ChangeKind);
    void muteGroupsChanged();
    Status getStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace gocue::livemix
