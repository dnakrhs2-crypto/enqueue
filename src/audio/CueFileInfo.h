#pragma once

#include "model/Cue.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace gocue
{

/** Refreshes the runtime file facts of a cue: fileMissing and (for readable files) durationSeconds. */
inline void refreshCueFileInfo (juce::AudioFormatManager& formats, Cue& cue)
{
    cue.fileMissing = cue.file != juce::File() && ! cue.file.existsAsFile();

    if (cue.fileMissing || cue.file == juce::File())
        return;

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (cue.file));

    if (reader != nullptr && reader->sampleRate > 0.0)
        cue.durationSeconds = (double) reader->lengthInSamples / reader->sampleRate;
    else
        cue.durationSeconds = 0.0;
}

/** True when the file extension matches one of the registered audio formats. */
inline bool isSupportedAudioFile (juce::AudioFormatManager& formats, const juce::File& file)
{
    return file.hasFileExtension (formats.getWildcardForAllFormats().replace ("*", ""));
}

} // namespace gocue
