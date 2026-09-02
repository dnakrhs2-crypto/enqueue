#pragma once

#include "model/Cue.h"

#include <vector>

namespace gocue::CueNumbering
{

/** "1", "1.5", "2.25": up to three decimals, trailing zeros removed. */
juce::String format (double value);

/** True when 'text' is a plain (positive) decimal number. */
bool isNumeric (const juce::String& text);

/** True when no other cue (than 'except') carries this number. Empty numbers are always fine. */
bool isUnique (const std::vector<Cue>& cues, const juce::String& number, const juce::Uuid& except = juce::Uuid::null());

/** The number for a cue that is being inserted at 'insertIndex' (0..size): the previous cue's number
    + increment when that is numeric, else the largest numeric number in the list + increment, else the
    increment itself. Bumped by the increment until it is unique. Empty when increment <= 0. */
juce::String next (const std::vector<Cue>& cues, int insertIndex, double increment);

struct RenumberOptions
{
    double start = 1.0;
    double increment = 1.0;
    juce::String prefix, suffix;
};

/** Numbers for 'count' cues in order: prefix + format(start + i * increment) + suffix. */
std::vector<juce::String> generate (int count, const RenumberOptions& options);

} // namespace gocue::CueNumbering
