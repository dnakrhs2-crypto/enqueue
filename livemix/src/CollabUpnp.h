#pragma once
#include <juce_core/juce_core.h>
#include <memory>

namespace gocue::livemix::collab
{
// All methods run on the session worker, including removal during shutdown.
class CollabUpnp
{
public:
    struct Result
    {
        bool found = false, mapped = false;
        juce::String externalIp, lanIp;
    };
    virtual ~CollabUpnp() = default;
    virtual Result discover (int boundPort) = 0;
    virtual bool renew() = 0;
    virtual void removeMapping() = 0;
    static std::unique_ptr<CollabUpnp> makeMiniUpnp();
};
}
