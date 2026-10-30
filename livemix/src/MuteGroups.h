#pragma once

#include "MixDocument.h"

#include <functional>

namespace gocue::livemix
{

/** The two mute groups: the mic channels that carry the "뮤트그룹" mark, and the FX channels that carry theirs. A
    hotkey each mutes / unmutes every member at once. Besides the groups, every mic or every FX at once (the top bar's
    '마이크 뮤트' / 'FX 뮤트' and their hotkeys), marks or not. The channels' own switches (a mic's ON/OFF, an FX's
    ON/OFF and return) are untouched: releasing brings back exactly what was there. The state is live only - a new or
    opened session starts unmuted; the marks are saved with the session. */
class MuteGroups
{
public:
    enum class Group { mic, fx };

    explicit MuteGroups (MixDocument& document);

    void toggle (Group group);
    void set (Group group, bool muted);
    bool isMuted (Group group) const noexcept { return group == Group::mic ? micMuted : fxMuted; }
    /** At least one mic / FX carries the group's "뮤트그룹" mark. */
    bool hasMembers (Group group) const;

    /** Every mic / every FX, marked or not. */
    void setAll (Group group, bool muted);
    bool isAllMuted (Group group) const noexcept { return group == Group::mic ? allMics : allFx; }
    /** Something of that kind is muted: all of it, or its group. */
    bool isAnyMuted (Group group) const noexcept { return isAllMuted (group) || isMuted (group); }
    /** Everything of that kind released at once - all, and the group (one change). */
    void release (Group group);

    bool mutes (const MixChannel& channel) const noexcept { return allMics || (micMuted && channel.muteGroup); }
    bool mutes (const MixFx& fx) const noexcept { return allFx || (fxMuted && fx.muteGroup); }

    /** Pushes the state to the engine for every channel (muted while all of its kind is, or its group with it in;
        anything else is not): after a membership edit, a session load, or the engine being rebuilt. */
    void apply();

    /** Everything released (a session opened / a new one). */
    void reset();

    /** The state changed (a toggle, a reset). */
    std::function<void()> onChanged;

private:
    void changed();

    MixDocument& document;
    bool micMuted = false, fxMuted = false, allMics = false, allFx = false;
};

} // namespace gocue::livemix
