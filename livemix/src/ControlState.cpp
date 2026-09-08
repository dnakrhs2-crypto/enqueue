#include "ControlState.h"
#include "MixDocument.h"
#include "MuteGroups.h"

#include <juce_events/juce_events.h>

#include <algorithm>

namespace gocue::livemix
{

ControlState::Snapshot ControlState::capture (const MixDocument& document, const MuteGroups& groups, bool audioRunning)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    Snapshot result;
    result.sessionId = document.getSessionGeneration();
    auto& p = result.projection;
    p.session = { document.getDisplayName(), document.isDirty() };
    p.audioRunning = audioRunning;
    p.micMuted = groups.isMuted (MuteGroups::Group::mic);
    p.fxMuted = groups.isMuted (MuteGroups::Group::fx);
    const auto& session = document.getSession();
    for (const auto& c : session.channels)
    {
        ControlProtocol::Channel channel { c.id, c.name, c.on, c.muteGroup, {}, {} };
        for (size_t i = 0; i < c.pluginGroups.size(); ++i)
            channel.pluginGroups.push_back ({ (int) i + 1, c.pluginGroups[i].off });
        // Project actual FX in UI order. Never call the mutating sendFor() from a read path; a missing send has
        // the model's default amount/pre, and a stale send to a removed FX is not part of the wire projection.
        for (const auto& f : session.fx)
        {
            const auto send = std::find_if (c.sends.begin(), c.sends.end(), [&f] (const auto& s) { return s.fx == f.id; });
            channel.sends.push_back ({ f.id, send != c.sends.end() ? send->amount : 0.0, send != c.sends.end() && send->pre });
        }
        p.channels.push_back (std::move (channel));
    }
    for (const auto& f : session.fx)
        p.fx.push_back ({ f.id, f.name, f.muteGroup, f.returnAmount });
    return result;
}

bool ControlState::sameStructure (const ControlProtocol::Projection& a, const ControlProtocol::Projection& b)
{
    if (a.channels.size() != b.channels.size() || a.fx.size() != b.fx.size()) return false;
    for (size_t i = 0; i < a.channels.size(); ++i)
    {
        const auto& x = a.channels[i];
        const auto& y = b.channels[i];
        if (x.id != y.id || x.pluginGroups.size() != y.pluginGroups.size() || x.sends.size() != y.sends.size()) return false;
        for (size_t j = 0; j < x.pluginGroups.size(); ++j)
            if (x.pluginGroups[j].index != y.pluginGroups[j].index) return false;
        for (size_t j = 0; j < x.sends.size(); ++j)
            if (x.sends[j].fxId != y.sends[j].fxId) return false;
    }
    for (size_t i = 0; i < a.fx.size(); ++i)
        if (a.fx[i].id != b.fx[i].id) return false;
    return true;
}

ControlState::ControlState (Snapshot initial) : current (std::move (initial))
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    current.revision = 1;
    current.structureRevision = 1;
}

bool ControlState::update (Snapshot latest)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (latest.sessionId == current.sessionId && latest.projection == current.projection) return true;
    if (current.revision == ControlProtocol::maxSafeInteger) return false;
    latest.revision = current.revision + 1;
    latest.structureRevision = latest.sessionId != current.sessionId || ! sameStructure (current.projection, latest.projection)
        ? latest.revision : current.structureRevision;
    current = std::move (latest);
    return true;
}

ControlState::Difference ControlState::diff (const Snapshot& published, const Snapshot& current)
{
    Difference result;
    result.baseRevision = published.revision;
    result.revision = current.revision;
    const auto& a = published.projection;
    const auto& b = current.projection;
    if (published.sessionId != current.sessionId)
    {
        result.kind = ChangeKind::session;
        return result;
    }
    if (current.structureRevision > published.revision || ! sameStructure (a, b))
    {
        result.kind = ChangeKind::structure;
        return result;
    }
    if (published.revision == current.revision && a == b) return result;
    result.kind = ChangeKind::values;
    auto& changes = result.changes;
    if (a.session.name != b.session.name) changes.sessionName = b.session.name;
    if (a.session.dirty != b.session.dirty) changes.sessionDirty = b.session.dirty;
    if (a.audioRunning != b.audioRunning) changes.audioRunning = b.audioRunning;
    if (a.micMuted != b.micMuted) changes.micMuted = b.micMuted;
    if (a.fxMuted != b.fxMuted) changes.fxMuted = b.fxMuted;
    for (size_t i = 0; i < a.channels.size(); ++i)
    {
        const auto& x = a.channels[i];
        const auto& y = b.channels[i];
        ControlProtocol::ChannelChanges c;
        c.id = y.id;
        if (x.name != y.name) c.name = y.name;
        if (x.on != y.on) c.on = y.on;
        if (x.muteGroup != y.muteGroup) c.muteGroup = y.muteGroup;
        if (x.pluginGroups != y.pluginGroups) c.pluginGroups = y.pluginGroups;
        if (x.sends != y.sends) c.sends = y.sends;
        if (! c.empty()) changes.channels.push_back (std::move (c));
    }
    for (size_t i = 0; i < a.fx.size(); ++i)
    {
        const auto& x = a.fx[i];
        const auto& y = b.fx[i];
        ControlProtocol::FxChanges f;
        f.id = y.id;
        if (x.name != y.name) f.name = y.name;
        if (x.muteGroup != y.muteGroup) f.muteGroup = y.muteGroup;
        if (x.returnAmount != y.returnAmount) f.returnAmount = y.returnAmount;
        if (! f.empty()) changes.fx.push_back (std::move (f));
    }
    return result;
}

} // namespace gocue::livemix
