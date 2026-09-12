#pragma once
#include "AudioRenderFixtures.h"
#include "TestSupport.h"

namespace recorder_timeline_gap
{
using namespace gocue::recorder;
// The source fixture has ten seconds on each independent lane. The two cases
// represent an initial recording at 00:15, or 00:00-00:05 followed by 00:15-00:20.
inline RecorderProject project(const recorder_audio_fixture::Fixture& source, bool leading)
{
    auto p = source.project;
    for (auto& t : p.tracks)
    {
        auto& clips = t.clips.edit();
        if (leading) clips[0].timelineStartSample = Sample(p.Fs) * 15;
        else
        {
            clips[0].lengthSamples = Sample(p.Fs) * 5;
            auto later = clips[0]; later.clipId = newId(); later.timelineStartSample = Sample(p.Fs) * 15;
            later.sourceIn = Sample(p.Fs) * 5; clips.push_back(later);
        }
    }
    const auto valid = p.validate(); recorder_test::require(valid.wasOk(), valid.getErrorMessage().toRawUTF8()); return p;
}
inline Sample sourceAt(Sample at, unsigned Fs, bool leading)
{
    if (leading) return at < 15 * Sample(Fs) || at >= 25 * Sample(Fs) ? -1 : at - 15 * Sample(Fs);
    if (at < 5 * Sample(Fs)) return at;
    return at < 15 * Sample(Fs) || at >= 20 * Sample(Fs) ? -1 : at - 10 * Sample(Fs);
}
inline bool silentInterior(Sample at, unsigned Fs, bool leading)
{ return at >= 512 && sourceAt(at - 512, Fs, leading) < 0 && sourceAt(at + 512, Fs, leading) < 0; }
}
