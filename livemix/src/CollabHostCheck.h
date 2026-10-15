#pragma once
#include "CollabUpnp.h"

namespace gocue::livemix::collab
{
struct CollabHostCheck
{
    enum class Cause { none, upnpDisabled, sharedAddress, firewall };
    struct Result
    {
        bool reachable = false;
        Cause cause = Cause::none;
        juce::String reason;
        juce::String message() const;
    };
    static Result evaluate (bool probeReceived, const CollabUpnp::Result&, const juce::String& publicIp);
};
}
