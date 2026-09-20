#pragma once
#include <cstdint>
#include <deque>
#include <optional>

namespace gocue
{
/** One message-thread gesture across native keys, JUCE, UI and MIDI. Event times
    may arrive out of order. Duplicate delivery cannot become a second press. */
class PanicGestureGate
{
public:
    std::optional<bool> activate (uint64_t eventID, double observedTimeMs);
    void invalidate();
private:
    struct Event { uint64_t id; double time; };
    std::deque<Event> history;
};
}
