#include "app/VolumeCue.h"

#include <cmath>
#include <set>

namespace gocue::VolumeCue
{
namespace
{
juce::String ko (const char* text) { return juce::String::fromUTF8 (text); }
}

juce::String formatDb (double db)
{
    const double rounded = std::round (db * 10.0) / 10.0;
    return (rounded > 0.0 ? "+" : "") + juce::String (rounded == 0.0 ? 0.0 : rounded,
        rounded == std::trunc (rounded) ? 0 : 1) + " dB";
}

juce::String targetText (const Cue* target, double offsetDb)
{
    const auto label = target != nullptr ? ko ("→ ") + (target->number.isNotEmpty() ? target->number + " " : juce::String()) + target->name
                                         : ko ("대상 없음");
    return label + ko (" · ") + (offsetDb == 0.0 ? ko ("원래 볼륨") : formatDb (offsetDb));
}

std::optional<double> parseOffset (const juce::String& input)
{
    const auto text = input.trim();
    int digits = 0, dots = 0;
    for (int i = 0; i < text.length(); ++i)
    {
        const auto c = text[i];
        if (i == 0 && (c == '+' || c == '-')) continue;
        if (c >= '0' && c <= '9') { ++digits; continue; }
        if (c == '.' && ++dots == 1) continue;
        return std::nullopt;
    }
    const double value = text.getDoubleValue();
    if (digits == 0 || ! std::isfinite (value)) return std::nullopt;
    return juce::jlimit (Cue::minGainDb, Cue::maxGainDb, value);
}

juce::String badgeText (const Cue& cue, const AudioEngine::PlayingCue& playing, bool otherFadeRunning)
{
    const double offset = playing.liveGainDb - cue.gainDb;
    if (! cue.makesSound() || playing.loaded || playing.fadingOut || otherFadeRunning
        || ! std::isfinite (offset) || std::abs (offset) + 1.0e-9 < 0.05)
        return {};
    return ko ("볼륨 ") + (playing.liveGainDb <= Cue::minGainDb ? ko ("무음") : formatDb (offset));
}

Badges badgesFor (const std::vector<AudioEngine::PlayingCue>& playing, const std::vector<FadeRunner::Info>& fades,
                  const std::function<const Cue* (const juce::Uuid&)>& findCue)
{
    std::set<juce::Uuid> otherTargets;
    for (const auto& fade : fades)
        if (const auto* cue = findCue (fade.fadeId); cue != nullptr && cue->isFade() && cue->fade.mode != FadeMode::volume)
            otherTargets.insert (fade.targetId);
    Badges result;
    for (const auto& p : playing)
        if (const auto* cue = findCue (p.id))
        {
            const auto text = badgeText (*cue, p, otherTargets.count (p.id) != 0);
            if (text.isNotEmpty()) result.emplace (std::make_pair (p.id, p.startOrder), text);
        }
    return result;
}
}
