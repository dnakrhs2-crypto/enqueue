#pragma once

#include "MixDocument.h"

#include <functional>

namespace gocue::livemix
{

/** The two mute groups: the mic channels that carry the "뮤트그룹" mark, and the FX channels that carry theirs. A
    hotkey each mutes / unmutes every member at once. The members' own switches (a mic's ON/OFF, an FX's return)
    are untouched: releasing the group brings back exactly what was there. The group state is live only - a new or
    opened session starts unmuted; the marks are saved with the session. */
class MuteGroups
{
public:
    enum class Group { mic, fx };

    explicit MuteGroups (MixDocument& document);

    void toggle (Group group);
    void set (Group group, bool muted);
    bool isMuted (Group group) const noexcept { return group == Group::mic ? micMuted : fxMuted; }

    /** Pushes the state to the engine for every channel (a member is muted while its group is; anything else is
        not): after a membership edit, a session load, or the engine being rebuilt. */
    void apply();

    /** Both groups released (a session opened / a new one). */
    void reset();

    /** The state changed (a toggle, a reset). */
    std::function<void()> onChanged;

private:
    MixDocument& document;
    bool micMuted = false, fxMuted = false;
};

} // namespace gocue::livemix
