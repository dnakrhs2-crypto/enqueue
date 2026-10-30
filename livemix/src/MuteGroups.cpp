#include "MuteGroups.h"

#include <algorithm>

namespace gocue::livemix
{

MuteGroups::MuteGroups (MixDocument& doc) : document (doc) {}

void MuteGroups::toggle (Group group)
{
    set (group, ! isMuted (group));
}

void MuteGroups::set (Group group, bool muted)
{
    auto& flag = group == Group::mic ? micMuted : fxMuted;

    if (flag == muted)
        return;

    flag = muted;
    changed();
}

bool MuteGroups::hasMembers (Group group) const
{
    const auto& session = document.getSession();

    if (group == Group::mic)
        return std::any_of (session.channels.begin(), session.channels.end(), [] (const auto& c) { return c.muteGroup; });

    return std::any_of (session.fx.begin(), session.fx.end(), [] (const auto& f) { return f.muteGroup; });
}

void MuteGroups::setAll (Group group, bool muted)
{
    auto& flag = group == Group::mic ? allMics : allFx;

    if (flag == muted)
        return;

    flag = muted;
    changed();
}

void MuteGroups::release (Group group)
{
    auto& all = group == Group::mic ? allMics : allFx;
    auto& marked = group == Group::mic ? micMuted : fxMuted;

    if (! all && ! marked)
        return;

    all = false;
    marked = false;
    changed();
}

void MuteGroups::changed()
{
    apply();

    if (onChanged)
        onChanged();
}

void MuteGroups::apply()
{
    auto& engine = document.getEngine();
    const auto& session = document.getSession();

    // everything of a kind is the engine's own switch (a channel built later is silent from its first block); the
    // groups go on each member
    engine.setAllChannelsMuted (allMics);
    engine.setAllFxMuted (allFx);

    for (const auto& c : session.channels)
        engine.setChannelMuted (c.id, micMuted && c.muteGroup);

    for (const auto& f : session.fx)
        engine.setFxMuted (f.id, fxMuted && f.muteGroup);
}

void MuteGroups::reset()
{
    const bool was = micMuted || fxMuted || allMics || allFx;
    micMuted = false;
    fxMuted = false;
    allMics = false;
    allFx = false;
    apply();

    if (was && onChanged)
        onChanged();
}

} // namespace gocue::livemix
