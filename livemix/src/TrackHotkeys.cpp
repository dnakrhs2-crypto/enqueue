#include "TrackHotkeys.h"

namespace gocue::livemix::TrackHotkeys
{

namespace
{
    int countFor (Kind kind) noexcept { return kind == Kind::mic ? MixSession::maxChannels : MixSession::maxFx; }

    int tracksIn (const MixSession& session, Kind kind) noexcept
    {
        return (int) (kind == Kind::mic ? session.channels.size() : session.fx.size());
    }

    /** "메인 마이크 ON/OFF": the name the track has now (the key follows the number, the row says what it reaches). */
    juce::String labelFor (const MixSession& session, Kind kind, int number)
    {
        const auto& name = kind == Kind::mic ? session.channels[(size_t) number - 1].name : session.fx[(size_t) number - 1].name;
        return name.trim() + " ON/OFF";
    }
}

int idFor (Kind kind, int number)
{
    jassert (number >= 1 && number <= countFor (kind));
    return firstId + (kind == Kind::fx ? MixSession::maxChannels : 0) + number - 1;
}

bool fromId (int id, Kind& kind, int& number)
{
    if (id < firstId || id > lastId)
        return false;

    const int index = id - firstId;
    kind = index < MixSession::maxChannels ? Kind::mic : Kind::fx;
    number = kind == Kind::mic ? index + 1 : index - MixSession::maxChannels + 1;
    return true;
}

juce::String getKey (const LiveMixSettings& settings, Kind kind, int number)
{
    return kind == Kind::mic ? settings.getMicOnHotkey (number) : settings.getFxOnHotkey (number);
}

void setKey (LiveMixSettings& settings, Kind kind, int number, const juce::String& description)
{
    if (kind == Kind::mic)
        settings.setMicOnHotkey (number, description);
    else
        settings.setFxOnHotkey (number, description);
}

std::vector<Track> tracks (const MixSession& session)
{
    std::vector<Track> list;

    for (const auto kind : { Kind::mic, Kind::fx })
        for (int number = 1; number <= juce::jmin (tracksIn (session, kind), countFor (kind)); ++number)
            list.push_back ({ kind, number, labelFor (session, kind, number) });

    return list;
}

std::vector<Track> settingsRows (const MixSession& session, const LiveMixSettings& settings)
{
    std::vector<Track> list;

    for (const auto kind : { Kind::mic, Kind::fx })
    {
        const int present = juce::jmin (tracksIn (session, kind), countFor (kind));

        for (int number = 1; number <= countFor (kind); ++number)
        {
            if (number <= present)
                list.push_back ({ kind, number, labelFor (session, kind, number), true });
            else if (getKey (settings, kind, number).isNotEmpty())
                list.push_back ({ kind, number, juce::String::fromUTF8 (kind == Kind::mic ? "마이크 " : "FX ") + juce::String (number) + " ON/OFF", false });
        }
    }

    return list;
}

std::vector<Registration> registrations (const MixSession& session, const LiveMixSettings& settings)
{
    std::vector<Registration> list;

    for (const auto kind : { Kind::mic, Kind::fx })
    {
        for (int number = 1; number <= countFor (kind); ++number)
        {
            Registration r;
            r.id = idFor (kind, number);

            if (number <= tracksIn (session, kind))
            {
                r.key = getKey (settings, kind, number);
                r.what = labelFor (session, kind, number);
            }

            list.push_back (std::move (r));
        }
    }

    return list;
}

juce::String toggle (MixDocument& document, Kind kind, int number)
{
    const auto& session = document.getSession();

    if (number < 1 || number > tracksIn (session, kind))
        return {};

    if (kind == Kind::mic)
    {
        const auto& c = session.channels[(size_t) number - 1];
        const auto id = c.id;
        const auto name = c.name.trim();
        const bool on = ! c.on;
        document.setChannelOn (id, on);   // the views refresh from here: nothing of the session is read after it
        return name + (on ? " ON" : " OFF");
    }

    const auto& f = session.fx[(size_t) number - 1];
    const auto id = f.id;
    const auto name = f.name.trim();
    const bool on = ! f.on;
    document.setFxOn (id, on);
    return name + (on ? " ON" : " OFF");
}

} // namespace gocue::livemix::TrackHotkeys
