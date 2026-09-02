#include "model/Cue.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace gocue
{

float Cue::gainLinear() const noexcept
{
    const double db = juce::jlimit (minGainDb, maxGainDb, gainDb);
    return juce::Decibels::decibelsToGain ((float) db, (float) minGainDb);
}

Cue Cue::duplicated() const
{
    Cue copy (*this);
    copy.id = juce::Uuid();
    return copy;
}

double Cue::regionEnd() const noexcept
{
    if (audio.endSeconds >= 0.0)
        return durationSeconds > 0.0 ? std::min (audio.endSeconds, durationSeconds) : audio.endSeconds;

    return durationSeconds;
}

double Cue::regionLength() const noexcept
{
    return std::max (0.0, regionEnd() - regionStart());
}

double Cue::passLength() const noexcept
{
    if (type == CueType::fade)
        return fade.durationSeconds;

    return regionLength() / std::max (AudioCueData::minRate, audio.rate);
}

//==============================================================================
void FadeCueData::resizeActive (int inputs, int outputs)
{
    inputs = juce::jlimit (0, LevelMatrix::maxInputs, inputs);
    outputs = juce::jlimit (0, LevelMatrix::maxOutputs, outputs);
    inputActive.resize ((size_t) inputs, 0);
    outputActive.resize ((size_t) outputs, 0);
    crosspointActive.resize ((size_t) inputs);

    for (auto& row : crosspointActive)
        row.resize ((size_t) outputs, 0);
}

bool FadeCueData::isInputActive (int i) const noexcept
{
    return i >= 0 && i < (int) inputActive.size() && inputActive[(size_t) i] != 0;
}

bool FadeCueData::isOutputActive (int o) const noexcept
{
    return o >= 0 && o < (int) outputActive.size() && outputActive[(size_t) o] != 0;
}

bool FadeCueData::isCrosspointActive (int i, int o) const noexcept
{
    return i >= 0 && i < (int) crosspointActive.size() && o >= 0 && o < (int) crosspointActive[(size_t) i].size()
        && crosspointActive[(size_t) i][(size_t) o] != 0;
}

void FadeCueData::setInputActive (int i, bool on)
{
    if (i >= 0 && i < (int) inputActive.size())
        inputActive[(size_t) i] = on ? 1 : 0;
}

void FadeCueData::setOutputActive (int o, bool on)
{
    if (o >= 0 && o < (int) outputActive.size())
        outputActive[(size_t) o] = on ? 1 : 0;
}

void FadeCueData::setCrosspointActive (int i, int o, bool on)
{
    if (i >= 0 && i < (int) crosspointActive.size() && o >= 0 && o < (int) crosspointActive[(size_t) i].size())
        crosspointActive[(size_t) i][(size_t) o] = on ? 1 : 0;
}

void FadeCueData::setAllActive (bool on)
{
    mainActive = on;
    std::fill (inputActive.begin(), inputActive.end(), (char) (on ? 1 : 0));
    std::fill (outputActive.begin(), outputActive.end(), (char) (on ? 1 : 0));

    for (auto& row : crosspointActive)
        std::fill (row.begin(), row.end(), (char) (on ? 1 : 0));
}

void FadeCueData::sanitise() noexcept
{
    if (! std::isfinite (durationSeconds) || durationSeconds < 0.0)
        durationSeconds = 0.0;

    durationSeconds = std::min (durationSeconds, maxDurationSeconds);

    if (! std::isfinite (mainDb))
        mainDb = 0.0;

    mainDb = LevelMatrix::clampDb (mainDb);
    levels.sanitise();
    resizeActive (levels.numInputs(), levels.numOutputs());

    if (! std::isfinite (rate) || rate <= 0.0)
        rate = 1.0;

    rate = juce::jlimit (AudioCueData::minRate, AudioCueData::maxRate, rate);

    for (auto& p : params)
    {
        p.slot = std::max (0, p.slot);
        p.parameter = std::max (0, p.parameter);
        p.value = std::isfinite (p.value) ? juce::jlimit (0.0f, 1.0f, p.value) : 0.0f;
    }

    curve.sanitise();
}

double Cue::effectiveLength() const noexcept
{
    if (type == CueType::fade)
        return fade.durationSeconds;

    return audio.infiniteLoop ? -1.0 : passLength() * (double) audio.playCount;
}

void Cue::sanitise() noexcept
{
    fade.sanitise();

    auto fixSeconds = [] (double& v, double hi)
    {
        if (! std::isfinite (v) || v < 0.0)
            v = 0.0;

        v = std::min (v, hi);
    };

    fixSeconds (preWaitSeconds, maxWaitSeconds);
    fixSeconds (postWaitSeconds, maxWaitSeconds);
    color = juce::jlimit (0, 20, color);
    secondColor = juce::jlimit (0, 20, secondColor);
    wallClock.hour = juce::jlimit (0, 23, wallClock.hour);
    wallClock.minute = juce::jlimit (0, 59, wallClock.minute);
    wallClock.second = juce::jlimit (0, 59, wallClock.second);
    wallClock.daysMask &= 0x7f;
    fixSeconds (fadeStopOthers.seconds, 600.0);
    fixSeconds (duck.seconds, 600.0);

    if (! std::isfinite (duck.levelDb))
        duck.levelDb = -12.0;

    duck.levelDb = juce::jlimit (minGainDb, maxGainDb, duck.levelDb);

    fadeOutMs = juce::jlimit (0, maxFadeMs, fadeOutMs);

    if (! std::isfinite (gainDb))
        gainDb = 0.0;

    numChannels = juce::jlimit (0, LevelMatrix::maxInputs, numChannels);
    levels.sanitise();
    trim.sanitise();

    gainDb = juce::jlimit (minGainDb, maxGainDb, gainDb);

    if (! std::isfinite (durationSeconds) || durationSeconds < 0.0)
        durationSeconds = 0.0;

    auto& a = audio;

    if (! std::isfinite (a.startSeconds) || a.startSeconds < 0.0)
        a.startSeconds = 0.0;

    if (! std::isfinite (a.endSeconds) || a.endSeconds < 0.0)
        a.endSeconds = -1.0;

    if (a.endSeconds >= 0.0 && a.endSeconds <= a.startSeconds)   // empty region: play to the end instead of nothing
        a.endSeconds = -1.0;

    if (durationSeconds > 0.0 && a.startSeconds >= durationSeconds)
        a.startSeconds = 0.0;

    a.playCount = juce::jlimit (1, AudioCueData::maxPlayCount, a.playCount);

    if (! std::isfinite (a.rate))
        a.rate = 1.0;

    a.rate = juce::jlimit (AudioCueData::minRate, AudioCueData::maxRate, a.rate);
    a.envelope.sanitise();
}

} // namespace gocue
