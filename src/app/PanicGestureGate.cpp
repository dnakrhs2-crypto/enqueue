#include "app/PanicGestureGate.h"
#include <cmath>
#include <algorithm>

namespace gocue
{
std::optional<bool> PanicGestureGate::activate (uint64_t id, double time)
{
    if (! std::isfinite (time) || id == 0 || std::any_of (history.begin(), history.end(), [id] (const auto& e) { return e.id == id; })) return {};
    const bool hard = std::any_of (history.begin(), history.end(), [time] (const auto& e) { return std::abs (time - e.time) <= 500.0; });
    history.push_back ({ id, time });
    // Bounded to the maximum reserved MIDI backlog plus native/UI observations.
    if (history.size() > 8192) history.pop_front();
    return hard;
}
void PanicGestureGate::invalidate() { history.clear(); }
}
