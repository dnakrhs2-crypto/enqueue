#pragma once

#include "LiveMixSettings.h"
#include "MixDocument.h"

#include <vector>

namespace gocue::livemix
{

/** The per-track ON/OFF hotkeys: one for each mic position (1 .. MixSession::maxChannels) and each FX position
    (1 .. maxFx). A key belongs to the number, not to one track: another session's mic 1 answers the same key. A number
    the open session has no track for is not registered, so the other programs keep that key meanwhile. */
namespace TrackHotkeys
{
    enum class Kind { mic, fx };

    /** The GlobalHotkeys ids: after the mute groups (1, 2), the window (3) and plugin groups 1 .. 5 (4 .. 8). */
    constexpr int firstId = 9;
    constexpr int lastId = firstId + MixSession::maxChannels + MixSession::maxFx - 1;
    int idFor (Kind kind, int number);
    bool fromId (int id, Kind& kind, int& number);

    juce::String getKey (const LiveMixSettings& settings, Kind kind, int number);
    void setKey (LiveMixSettings& settings, Kind kind, int number, const juce::String& description);

    /** The open session's tracks in card / tab order, each with the name its hotkey goes by ("메인 마이크 ON/OFF"). */
    struct Track
    {
        Kind kind = Kind::mic;
        int number = 1;
        juce::String label;
        bool inSession = true;   // false: a number the session lacks that still keeps a key (settingsRows)
    };
    std::vector<Track> tracks (const MixSession& session);

    /** The settings' ON/OFF rows: the session's tracks, plus each number it lacks that still keeps a key ("마이크 3
        ON/OFF", not in session) - so that key can be seen and cleared, and is not given to a second row. Mics, then
        FX, each by number. */
    std::vector<Track> settingsRows (const MixSession& session, const LiveMixSettings& settings);

    /** Every track id once, in order: the key to register for it, or an empty key (nothing for that id) when there is
        no key or the session has no track at that number. 'what' names it in a refusal. */
    struct Registration
    {
        int id = 0;
        juce::String key, what;
    };
    std::vector<Registration> registrations (const MixSession& session, const LiveMixSettings& settings);

    /** Switches the track at 'number' ON <-> OFF and returns the status line ("메인 마이크 OFF"); an empty string, and
        nothing changed, when the session has no track there. */
    juce::String toggle (MixDocument& document, Kind kind, int number);
}

} // namespace gocue::livemix
