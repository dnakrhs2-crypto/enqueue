#include "MuteGroups.h"

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
    apply();

    if (onChanged)
        onChanged();
}

void MuteGroups::apply()
{
    auto& engine = document.getEngine();
    const auto& session = document.getSession();

    for (const auto& c : session.channels)
        engine.setChannelMuted (c.id, micMuted && c.muteGroup);

    for (const auto& f : session.fx)
        engine.setFxMuted (f.id, fxMuted && f.muteGroup);
}

void MuteGroups::reset()
{
    const bool was = micMuted || fxMuted;
    micMuted = false;
    fxMuted = false;
    apply();

    if (was && onChanged)
        onChanged();
}

} // namespace gocue::livemix
