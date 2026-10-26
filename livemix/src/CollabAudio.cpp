#include "CollabAudio.h"
#include "CollabTransport.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace gocue::livemix::collab
{
namespace c = contracts;
static_assert (std::atomic<unsigned>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free
               && std::atomic<double>::is_always_lock_free && std::atomic<float>::is_always_lock_free
               && std::atomic<const void*>::is_always_lock_free);
// Sequential consistency orders entry before gate observation, and gate closure before reclamation.
// All three realtime users take this guard; only lifecycle code waits.
struct CollabAudio::Access
{
    explicit Access (const CollabAudio& a) : owner (a) { owner.readers.fetch_add (1); active = owner.ready.load(); }
    ~Access() { owner.readers.fetch_sub (1); }
    const CollabAudio& owner;
    bool active;
};

struct CollabAudio::SlotTable
{
    struct Slot
    {
        c::ParticipantId id = 0;
        c::StreamEpoch epoch = 0;
        c::ParticipantKind kind = c::ParticipantKind::none;
        bool connected = false;
        std::shared_ptr<CollabPlayout> playout;
    };
    std::array<Slot, c::remoteSlots> remote;
    uint64_t session = 0;
    c::Revision revision = 0;
    c::ParticipantId self = 0;
    bool v2 = false;
};

CollabAudio::CollabAudio (const juce::String& name) : CollabAudio (name, {}) {}
CollabAudio::CollabAudio (const juce::String& name, const juce::String& name2)
    : ownedSlots (std::make_unique<SlotTable>()), peerObs (name),
      peerObs2 (name2.isEmpty() ? name + ".DisabledPeer2" : name2), secondObs (name2.isNotEmpty())
{
    ownedSlots->remote[0].playout = std::make_shared<CollabPlayout>();
    publishedSlots.store (ownedSlots.get());
}
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
    maxBlock = std::max (1, maxBlock);
    sender.prepare (rate, maxBlock, wireRate.load());
    // R3 detects a changed device rate and rebases its wire clock for the new epoch.
    // Only a same-rate v2 restart needs a source-clock rewind as its restart signal;
    // rewinding on a rate change as well would ask R3 for two successive epochs.
    // v1 always keeps the source clock, as before.
    if (ownedSlots->v2 && (uint32_t) rate == (uint32_t) publishedRate.load()) sender.resetSession();
    for (auto& slot : ownedSlots->remote)
        if (slot.playout) slot.playout->prepare (rate, maxBlock, wireRate.load());
    rampFrames = std::max (1, (int) std::lround (rate * 0.005));
    for (auto& slot : monitor)
    {
        slot.left.assign ((size_t) maxBlock, 0); slot.right.assign ((size_t) maxBlock, 0);
        slot.gain = slot.gainTarget = monitorOn.load() ? slot.volume.load() : 0.0f;
        slot.gainRemaining = 0;
    }
    tailRemaining = 0; tailL = tailR = 0; wasConnected = false;
    peakLeft.store (0.0f); peakRight.store (0.0f);
    peerObs.deviceStarted (rate);
    if (secondObs) peerObs2.deviceStarted (rate);
    publishedRate.store (rate);
    ready.store (true);
}

bool CollabAudio::setWireRate (uint32_t rate)
{
    if (! isValidWireRate (rate)) return false;
    const juce::ScopedLock lock (lifecycle);
    if (wireRate.load() != rate)
    {
        wireRate.store (rate);
        if (ready.load()) prepare (publishedRate.load(), (int) monitor[0].left.size());
    }
    return true;
}

void CollabAudio::attach (CollabTransport& t)
{
    detach();
    const juce::ScopedLock lock (lifecycle);
    const bool prepared = ready.load(); suspend();
    sender.resetSession();
    for (auto& slot : ownedSlots->remote) if (slot.playout) slot.playout->rebuffer();
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
    sender.resetSession();
    for (auto& slot : ownedSlots->remote) if (slot.playout) slot.playout->rebuffer();
    ready.store (prepared);
}
void CollabAudio::setConnected (bool value) noexcept
{
    const bool previous = connected.exchange (value);
    const Access access (*this);
    const auto* table = publishedSlots.load();
    if (previous != value && ! table->v2 && table->remote[0].playout)
        table->remote[0].playout->rebuffer();
}
void CollabAudio::setMonitor (float v, bool on) noexcept
{
    for (unsigned slot = 0; slot < monitor.size(); ++slot) setSlotMonitorVolume (slot, v);
    monitorOn.store (on);
}
void CollabAudio::setSlotMonitorVolume (unsigned slot, float v) noexcept
{
    if (slot < monitor.size()) monitor[slot].volume.store (std::clamp (std::isfinite (v) ? v : 0.0f, 0.0f, 1.0f));
}
void CollabAudio::setObsEnabled (bool enabled)
{
    obsWanted = enabled;
    peerObs.setEnabled (obsWanted || collabOpen);
    if (secondObs) peerObs2.setEnabled (obsWanted || collabOpen);
}
void CollabAudio::setCollabOpen (bool open)
{
    if (collabOpen == open) return;
    collabOpen = open;
    setObsEnabled (obsWanted);
}
ObsSender& CollabAudio::getPeerObs (unsigned slot) noexcept
{
    jassert (slot < c::remoteSlots);
    return slot == 1 ? peerObs2 : peerObs;
}
CollabPlayout::Stats CollabAudio::getStats (unsigned slot) const noexcept
{
    const Access access (*this);
    if (slot >= c::remoteSlots) return {};
    const auto& table = *publishedSlots.load();
    const auto& source = table.remote[slot];
    auto stats = source.playout ? source.playout->getStats() : CollabPlayout::Stats {};
    if (table.v2 ? ! source.connected : slot != 0 || ! connected.load()) stats.state = CollabPlayout::State::off;
    return stats;
}
void CollabAudio::rebuffer() noexcept
{
    const Access access (*this);
    for (const auto& slot : publishedSlots.load()->remote) if (slot.playout) slot.playout->rebuffer();
}

void CollabAudio::applyRoster (const RosterSnapshot& roster, c::ParticipantId selfId)
{
    const juce::ScopedLock lock (lifecycle);
    const auto& old = *ownedSlots;
    const bool sameRoom = old.session == roster.session && old.self == selfId;
    if (sameRoom && roster.memberCount != 0 && roster.revision <= old.revision) return;
    auto next = std::make_unique<SlotTable>();
    next->session = roster.session; next->revision = roster.revision; next->self = selfId;
    next->v2 = roster.mode == c::RoomMode::v2;
    std::array<const RosterMember*, c::remoteSlots> seats {};
    if (sameRoom)
        for (size_t i = 0; i < seats.size(); ++i)
            if (old.remote[i].id != 0 && old.remote[i].id != selfId)
                seats[i] = findMember (roster, old.remote[i].id);
    const auto seat = [&] (const RosterMember* member)
    {
        if (member == nullptr || member->participantId == selfId || member->participantId == 0) return;
        for (const auto* seated : seats) if (seated == member) return;
        for (auto& empty : seats) if (empty == nullptr) { empty = member; return; }
    };
    // Initially the host is seat 1 for a guest; existing occupants never move to fill a hole.
    seat (findMember (roster, c::hostParticipant));
    for (size_t i = 0; i < roster.memberCount && i < roster.members.size(); ++i) seat (&roster.members[i]);
    for (size_t i = 0; i < seats.size(); ++i)
    {
        auto& slot = next->remote[i];
        if (const auto* member = seats[i])
        {
            slot.id = member->participantId; slot.epoch = member->streamEpoch; slot.kind = member->kind;
            slot.connected = member->state == c::ConnectionState::connected;
        }
        if (! next->v2 && i != 0) continue;
        if (next->v2 && slot.id == 0) continue;
        const auto& previous = old.remote[i];
        if ((sameRoom && old.v2 == next->v2 && previous.id == slot.id
             && previous.epoch == slot.epoch && previous.kind == slot.kind)
            || (! old.v2 && ! next->v2 && (old.session == 0 || sameRoom)))
            slot.playout = previous.playout;
        if (! slot.playout)
        {
            slot.playout = std::make_shared<CollabPlayout>();
            if (ready.load()) slot.playout->prepare (publishedRate.load(), (int) monitor[0].left.size(), wireRate.load());
        }
    }
    publishedSlots.store (next.get());
    auto retired = std::move (ownedSlots);
    ownedSlots = std::move (next);
    // Publication precedes retirement. New readers already use the new table, without a closed audio gate.
    // Old playouts are destroyed here, never by a receiver/audio callback or by a RenderedSources consumer.
    while (readers.load() != 0) std::this_thread::yield();
}
void CollabAudio::pushLocal (const float* l, const float* r, int n) noexcept
{
    const Access access (*this);
    if (access.active && attached.load()) sender.push (l, r, n);
}
bool CollabAudio::hasInstalledRoster (uint64_t session, c::Revision revision) const noexcept
{
    const Access access (*this);
    const auto& table = *publishedSlots.load();
    return table.v2 && table.session == session && table.revision >= revision;
}
void CollabAudio::receive (const Header& h, const uint8_t* payload, uint64_t ns) noexcept
{
    const Access access (*this);
    if (! access.active) return;
    const auto& table = *publishedSlots.load();
    if (! table.v2 && table.remote[0].playout) table.remote[0].playout->push (h, payload, ns);
}
void CollabAudio::receiveV2 (const c::HeaderV2& h, const uint8_t* payload, uint64_t ns) noexcept
{
    const Access access (*this);
    if (! access.active || payload == nullptr || h.kind != c::Kind::audio || ! validHeaderV2 (h)) return;
    const auto& table = *publishedSlots.load();
    if (! table.v2 || h.session != table.session) return;
    if (h.rosterRevision > table.revision)
    {
        futureRevisionDrops.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    for (const auto& slot : table.remote)
        if (slot.id != 0 && slot.id == h.origin && slot.epoch == h.streamEpoch && slot.playout)
        {
            const auto format = slot.kind == c::ParticipantKind::web ? c::Flags::monoPcm16 : c::Flags::stereoPcm24;
            if (h.flags == format) slot.playout->pushV2 (h, payload, ns);
            return;
        }
}
void CollabAudio::drain (const CollabSender::Sink& sink)
{
    const Access access (*this);
    if (access.active) sender.drain (sink);
}

contracts::RenderedSources CollabAudio::renderSources (int frames) noexcept
{
    const Access access (*this);
    c::RenderedSources sources; sources.frames = std::max (0, frames);
    if (frames <= 0) return sources;
    if (! access.active)
    {
        peerObs.writeSilence (frames); if (secondObs) peerObs2.writeSilence (frames);
        return sources;
    }
    jassert (frames <= (int) monitor[0].left.size());
    sources.frames = std::min (frames, (int) monitor[0].left.size());
    const auto& table = *publishedSlots.load();
    const bool online = ! table.v2 && connected.load();
    if (wasConnected && ! online && ! table.v2) tailRemaining = rampFrames;
    if (table.v2) tailRemaining = 0;
    wasConnected = online;
    for (unsigned index = 0; index < monitor.size(); ++index)
    {
        auto& buffer = monitor[index];
        const auto& slot = table.remote[index];
        auto& view = sources.remote[index];
        const int n = sources.frames;
        const bool active = table.v2 ? slot.id != 0 : index == 0 && (online || tailRemaining > 0);
        if (! active)
        {
            getPeerObs (index).writeSilence (n);
            continue;
        }
        view.participantId = slot.id; view.streamEpoch = slot.epoch;
        view.kind = table.v2 ? slot.kind : c::ParticipantKind::native;
        view.channels = view.kind == c::ParticipantKind::web ? 1 : 2;
        view.connected = table.v2 ? slot.connected : online;
        if (slot.playout && (table.v2 || online)) slot.playout->pull (buffer.left.data(), buffer.right.data(), n);
        else
        {
            std::fill_n (buffer.left.data(), n, 0.0f); std::fill_n (buffer.right.data(), n, 0.0f);
            for (int i = 0; i < n && tailRemaining > 0; ++i)
            {
                const float ramp = (float) --tailRemaining / rampFrames;
                buffer.left[(size_t) i] = tailL * ramp; buffer.right[(size_t) i] = tailR * ramp;
            }
        }
        view.left = buffer.left.data(); view.right = view.channels == 1 ? view.left : buffer.right.data();
        getPeerObs (index).write (view.left, view.right, n);
        float peakL = 0.0f, peakR = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            peakL = std::max (peakL, std::abs (view.left[i]));
            peakR = std::max (peakR, std::abs (view.right[i]));
        }
        const auto publish = [] (std::atomic<float>& cell, float peak)
        {
            if (! std::isfinite (peak)) return;
            float previous = cell.load (std::memory_order_relaxed);
            while (peak > previous && ! cell.compare_exchange_weak (previous, peak, std::memory_order_relaxed)) {}
        };
        publish (peakLeft, peakL); publish (peakRight, peakR);
        if (online && index == 0) { tailL = view.left[n - 1]; tailR = view.right[n - 1]; }
    }
    return sources;
}

void CollabAudio::renderMonitor (const c::RenderedSources& sources, float* const* outputs, int count, int first) noexcept
{
    renderMonitor (sources, outputs, count, first, false, 0);
}
void CollabAudio::renderMonitor (const c::RenderedSources& sources, float* const* outputs, int count, int first,
                                 bool alreadyRouted, int offset) noexcept
{
    const Access access (*this);
    if (! access.active) return;
    if (! alreadyRouted)
    {
        const auto destination = getRoute();
        if (destination == Route::obs) count = 0;
        else if (destination == Route::direct) first = routeFirst.load();
    }
    const bool on = monitorOn.load();
    for (size_t index = 0; index < monitor.size(); ++index)
    {
        auto& slot = monitor[index];
        const auto& source = sources.remote[index];
        const float target = on ? slot.volume.load() : 0.0f;
        if (target != slot.gainTarget)
        {
            slot.gainTarget = target; slot.gainRemaining = rampFrames; slot.gainStep = (target - slot.gain) / rampFrames;
        }
        for (int i = 0; i < sources.frames; ++i)
        {
            if (slot.gainRemaining > 0) { slot.gain += slot.gainStep; if (--slot.gainRemaining == 0) slot.gain = slot.gainTarget; }
            // Preserve the legacy additive monitor, including signed zero when the source is silent.
            if (outputs != nullptr && first >= 0 && first < count && outputs[first] != nullptr && source.left != nullptr && source.left[i] != 0.0f)
                outputs[first][offset + i] += source.left[i] * slot.gain;
            if (outputs != nullptr && first + 1 >= 0 && first + 1 < count && outputs[first + 1] != nullptr && source.right != nullptr && source.right[i] != 0.0f)
                outputs[first + 1][offset + i] += source.right[i] * slot.gain;
        }
    }
}

void CollabAudio::renderRemote (float* const* outputs, int count, int first, int frames) noexcept
{
    const Access access (*this);
    if (! access.active) { renderSources (frames); return; }
    // Older callers already select the route and can supply more than one prepared block.
    for (int offset = 0; offset < frames; offset += (int) monitor[0].left.size())
    {
        const auto sources = renderSources (std::min ((int) monitor[0].left.size(), frames - offset));
        renderMonitor (sources, outputs, count, first, true, offset);
    }
}
}
