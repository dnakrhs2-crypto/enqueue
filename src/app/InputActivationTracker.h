#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <tuple>

namespace gocue
{
enum class InputKind { keyboard, midi, ui };
struct InputToken
{
    InputKind kind = InputKind::keyboard;
    uint64_t source = 0, connection = 0;
    int control = 0;
    bool operator< (const InputToken& b) const { return std::tie (kind, source, connection, control) < std::tie (b.kind, b.source, b.connection, b.control); }
    bool operator== (const InputToken& b) const { return std::tie (kind, source, connection, control) == std::tie (b.kind, b.source, b.connection, b.control); }
};
struct InputInvocation
{
    InputKind kind = InputKind::keyboard;
    juce::String id;
    int commandID = 0;
    bool active = true;
    InputToken token;
    double observedTimeMs = 0;
    uint64_t eventID = 0;
    static uint64_t nextEventID() noexcept;
};
/** Message-thread GO group. Suppressed aliases still hold the group closed. */
class InputActivationTracker
{
public:
    bool press (const InputToken&, bool eligible, bool requireKeyUp);
    void hold (const InputToken&);
    void release (const InputToken&);
    void releaseSource (InputKind, uint64_t source);
    bool anyHeld() const noexcept { return ! held.empty(); }
    bool isLatched() const noexcept { return latched; }
    /** Consume the common controller release once per physical GO group. Keyboard
        key-up metadata can still be delivered later, for example after capture. */
    bool consumeRelease() noexcept;
    void clear();
private:
    std::map<InputToken, bool> held;
    bool latched = false;
    bool releasePending = false;
};
}
