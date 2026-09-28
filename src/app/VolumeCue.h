#pragma once

#include "app/FadeRunner.h"

#include <map>
#include <optional>

namespace gocue::VolumeCue
{
/** Shared presentation of offsets and live levels. No engine access or mutations. */
juce::String formatDb (double db);
juce::String targetText (const Cue* target, double offsetDb);
std::optional<double> parseOffset (const juce::String& text);
juce::String badgeText (const Cue& cue, const AudioEngine::PlayingCue& playing, bool otherFadeRunning);

using Badges = std::map<std::pair<juce::Uuid, juce::int64>, juce::String>;
/** One badge per playing instance; retiring and newly restarted instances never share a level. */
Badges badgesFor (const std::vector<AudioEngine::PlayingCue>& playing, const std::vector<FadeRunner::Info>& fades,
                  const std::function<const Cue* (const juce::Uuid&)>& findCue);
}
