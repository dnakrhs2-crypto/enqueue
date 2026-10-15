#include "CollabAudio.h"
#include "CollabTransport.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace gocue::livemix::collab
{
static_assert (std::atomic<unsigned>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free
               && std::atomic<double>::is_always_lock_free && std::atomic<float>::is_always_lock_free);
// Sequential consistency orders entry before gate observation, and gate closure before reclamation.
// All three realtime users take this guard; only lifecycle code waits.
struct CollabAudio::Access
{
    explicit Access (CollabAudio& a) : owner (a) { owner.readers.fetch_add (1); active = owner.ready.load(); }
    ~Access() { owner.readers.fetch_sub (1); }
    CollabAudio& owner;
    bool active;
};

CollabAudio::CollabAudio (const juce::String& name) : peerObs (name) {}
CollabAudio::~CollabAudio() { detach(); suspend(); }
void CollabAudio::suspend()
{
    ready.store (false);
    while (readers.load() != 0) std::this_thread::yield();
}

void CollabAudio::prepare (double rate, int maxBlock)
{
    const juce::ScopedLock lock (lifecycle);
    suspend();
    sender.prepare (rate, maxBlock); playout.prepare (rate, maxBlock);
    left.assign ((size_t) maxBlock, 0); right.assign ((size_t) maxBlock, 0);
    rampFrames = std::max (1, (int) std::lround (rate * 0.005));
    gain = gainTarget = monitorOn.load() ? volume.load() : 0.0f;
    gainRemaining = tailRemaining = 0; tailL = tailR = 0; wasConnected = false;
    peerObs.deviceStarted (rate); publishedRate.store (rate);
    ready.store (true);
}

void CollabAudio::attach (CollabTransport& t)
{
    detach();
    const juce::ScopedLock lock (lifecycle);
    const bool prepared = ready.load(); suspend();
    sender.resetSession(); playout.rebuffer();
    transport = &t; attached.store (true);
    ready.store (prepared);
    t.attachAudio (*this);
}

void CollabAudio::detach()
{
    const juce::ScopedLock lock (lifecycle);
    if (transport != nullptr) transport->detachAudio (*this);
    transportStopped();
}
void CollabAudio::transportStopped() noexcept
{
    transport = nullptr; attached.store (false); connected.store (false);
}
void CollabAudio::startSession()
{
    const juce::ScopedLock lock (lifecycle);
    const bool prepared = ready.load(); suspend();
    sender.resetSession(); playout.rebuffer();
    ready.store (prepared);
}
void CollabAudio::setConnected (bool value) noexcept
{
    const bool previous = connected.exchange (value);
    if (previous != value) playout.rebuffer();
}
void CollabAudio::setMonitor (float v, bool on) noexcept
{
    volume.store (std::clamp (std::isfinite (v) ? v : 0.0f, 0.0f, 1.0f)); monitorOn.store (on);
}
void CollabAudio::pushLocal (const float* l, const float* r, int n) noexcept
{
    const Access access (*this);
    if (access.active && attached.load()) sender.push (l, r, n);
}
void CollabAudio::receive (const Header& h, const uint8_t* payload, uint64_t ns) noexcept
{
    const Access access (*this);
    if (access.active) playout.push (h, payload, ns);
}
void CollabAudio::drain (const CollabSender::Sink& sink)
{
    const Access access (*this);
    if (access.active) sender.drain (sink);
}

void CollabAudio::renderRemote (float* const* outputs, int count, int first, int frames) noexcept
{
    const Access access (*this);
    if (! access.active) { peerObs.writeSilence (frames); return; }
    const bool online = connected.load();
    if (wasConnected && ! online) tailRemaining = rampFrames;
    wasConnected = online;
    for (int offset = 0; offset < frames; offset += (int) left.size())
    {
        const int n = std::min ((int) left.size(), frames - offset);
        if (online) playout.pull (left.data(), right.data(), n);
        else
        {
            std::fill_n (left.data(), n, 0.0f); std::fill_n (right.data(), n, 0.0f);
            for (int i = 0; i < n && tailRemaining > 0; ++i)
            {
                const float ramp = (float) --tailRemaining / rampFrames;
                left[(size_t) i] = tailL * ramp; right[(size_t) i] = tailR * ramp;
            }
        }
        // Both OBS sources have the device clock; the peer tap is independent of headphone controls.
        peerObs.write (left.data(), right.data(), n);
        const float target = monitorOn.load() ? volume.load() : 0.0f;
        if (target != gainTarget)
        {
            gainTarget = target; gainRemaining = rampFrames; gainStep = (target - gain) / rampFrames;
        }
        for (int i = 0; i < n; ++i)
        {
            if (gainRemaining > 0) { gain += gainStep; if (--gainRemaining == 0) gain = gainTarget; }
            // Skip exact silence, preserving every bit of the pre-collab output, including signed zero.
            if (first >= 0 && first < count && outputs[first] != nullptr && left[(size_t) i] != 0.0f)
                outputs[first][offset + i] += left[(size_t) i] * gain;
            if (first + 1 >= 0 && first + 1 < count && outputs[first + 1] != nullptr && right[(size_t) i] != 0.0f)
                outputs[first + 1][offset + i] += right[(size_t) i] * gain;
        }
        if (online) { tailL = left[(size_t) n - 1]; tailR = right[(size_t) n - 1]; }
    }
}
}
