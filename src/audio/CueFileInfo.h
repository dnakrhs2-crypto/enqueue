#pragma once

#include "model/Cue.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <vector>

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

/** True when any of the dropped paths is an audio file or a folder (which may contain some). */
inline bool containsAudioOrFolder (juce::AudioFormatManager& formats, const juce::StringArray& paths)
{
    for (const auto& path : paths)
    {
        const juce::File file (path);

        if (file.isDirectory() || isSupportedAudioFile (formats, file))
            return true;
    }

    return false;
}

/** Expands dropped paths into audio files: folders are searched recursively and sorted by path,
    other files are kept in drop order. */
inline juce::StringArray collectAudioFiles (juce::AudioFormatManager& formats, const juce::StringArray& paths)
{
    juce::StringArray result;

    for (const auto& path : paths)
    {
        const juce::File file (path);

        if (file.isDirectory())
        {
            std::vector<juce::File> found;

            for (const auto& child : file.findChildFiles (juce::File::findFiles, true))
                if (isSupportedAudioFile (formats, child))
                    found.push_back (child);

            std::sort (found.begin(), found.end(), [] (const juce::File& a, const juce::File& b)
            {
                return a.getFullPathName().compareNatural (b.getFullPathName()) < 0;
            });

            for (const auto& f : found)
                result.add (f.getFullPathName());
        }
        else if (isSupportedAudioFile (formats, file))
        {
            result.add (path);
        }
    }

    return result;
}

} // namespace gocue
