#include "Localisation.h"
#include "TrackHotkeys.h"

namespace gocue::livemix::TrackHotkeys
{

namespace
{
    int countFor (Kind kind) noexcept
    {
        return kind == Kind::mic ? MixSession::maxChannels : kind == Kind::fx ? MixSession::maxFx : MixSession::maxAux;
    }

    int tracksIn (const MixSession& session, Kind kind) noexcept
    {
        return (int) (kind == Kind::mic ? session.channels.size() : kind == Kind::fx ? session.fx.size() : session.auxes.size());
    }

    /** "메인 마이크 ON/OFF": the name the track has now (the key follows the number, the row says what it reaches). */
    juce::String labelFor (const MixSession& session, Kind kind, int number)
    {
        const auto index = (size_t) number - 1;
        const auto& name = kind == Kind::mic ? session.channels[index].name
            : kind == Kind::fx ? session.fx[index].name : session.auxes[index].name;
        return name.trim() + " ON/OFF";
    }
}

int idFor (Kind kind, int number)
{
    jassert (number >= 1 && number <= countFor (kind));
    return (kind == Kind::aux ? firstAuxId : firstId + (kind == Kind::fx ? MixSession::maxChannels : 0)) + number - 1;
}

bool fromId (int id, Kind& kind, int& number)
{
    if (id >= firstAuxId && id <= lastId)
    {
        kind = Kind::aux;
        number = id - firstAuxId + 1;
        return true;
    }
    if (id < firstId || id > lastMicFxId)
        return false;

    const int index = id - firstId;
    kind = index < MixSession::maxChannels ? Kind::mic : Kind::fx;
    number = kind == Kind::mic ? index + 1 : index - MixSession::maxChannels + 1;
    return true;
}

juce::String getKey (const LiveMixSettings& settings, Kind kind, int number)
{
    return kind == Kind::mic ? settings.getMicOnHotkey (number)
        : kind == Kind::fx ? settings.getFxOnHotkey (number) : settings.getAuxOnHotkey (number);
}

void setKey (LiveMixSettings& settings, Kind kind, int number, const juce::String& description)
{
    if (kind == Kind::mic)
        settings.setMicOnHotkey (number, description);
    else if (kind == Kind::fx)
        settings.setFxOnHotkey (number, description);
    else
        settings.setAuxOnHotkey (number, description);
}

std::vector<Track> tracks (const MixSession& session)
{
    std::vector<Track> list;

    for (const auto kind : { Kind::mic, Kind::fx, Kind::aux })
        for (int number = 1; number <= juce::jmin (tracksIn (session, kind), countFor (kind)); ++number)
            list.push_back ({ kind, number, labelFor (session, kind, number) });

    return list;
}

std::vector<Track> settingsRows (const MixSession& session, const LiveMixSettings& settings)
{
    std::vector<Track> list;

    for (const auto kind : { Kind::mic, Kind::fx, Kind::aux })
    {
        const int present = juce::jmin (tracksIn (session, kind), countFor (kind));

        for (int number = 1; number <= countFor (kind); ++number)
        {
            if (number <= present)
                list.push_back ({ kind, number, labelFor (session, kind, number), true });
            else if (getKey (settings, kind, number).isNotEmpty())
                list.push_back ({ kind, number, (kind == Kind::mic ? trf ("마이크 {0} ON/OFF", number)
                    : kind == Kind::fx ? trf ("FX {0} ON/OFF", number) : trf ("억스 {0} ON/OFF", number)), false });
        }
    }

    return list;
}

std::vector<Registration> registrations (const MixSession& session, const LiveMixSettings& settings)
{
    std::vector<Registration> list;

    for (const auto kind : { Kind::mic, Kind::fx, Kind::aux })
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

    if (kind == Kind::aux)
    {
        const auto& a = session.auxes[(size_t) number - 1];
        const auto id = a.id;
        const auto name = a.name.trim();
        const bool on = ! a.on;
        document.setAuxOn (id, on);
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
