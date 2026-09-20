#include "app/InputActivationTracker.h"
#include <atomic>

namespace gocue
{
uint64_t InputInvocation::nextEventID() noexcept
{
    static std::atomic<uint64_t> serial { 0 };
    static_assert (std::atomic<uint64_t>::is_always_lock_free);
    return serial.fetch_add (1, std::memory_order_relaxed) + 1;
}
bool InputActivationTracker::press (const InputToken& token, bool eligible, bool requireKeyUp)
{
    held[token] = true;
    releasePending = true;
    if (! eligible || (latched && requireKeyUp)) return false;
    latched = true;
    return true;
}
void InputActivationTracker::hold (const InputToken& token) { held[token] = true; latched = true; releasePending = true; }
void InputActivationTracker::release (const InputToken& token) { held.erase (token); if (held.empty()) latched = false; }
void InputActivationTracker::releaseSource (InputKind kind, uint64_t source)
{
    for (auto it = held.begin(); it != held.end();)
        if (it->first.kind == kind && it->first.source == source) it = held.erase (it); else ++it;
    if (held.empty()) latched = false;
}
bool InputActivationTracker::consumeRelease() noexcept
{
    if (! releasePending || ! held.empty()) return false;
    releasePending = false;
    return true;
}
void InputActivationTracker::clear() { held.clear(); latched = false; releasePending = false; }
}
