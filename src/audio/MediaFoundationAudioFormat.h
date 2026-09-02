#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace gocue
{

/** Reads AAC / M4A / MP4 (and whatever else the OS decoders handle) through Windows Media Foundation.
    Decoded to 32-bit float PCM at the file's own sample rate. Seeks by re-positioning the source reader
    and discarding the pre-roll the decoder emits before the requested sample. Read-only (no writer). */
class MediaFoundationAudioFormat : public juce::AudioFormat
{
public:
    MediaFoundationAudioFormat();
    ~MediaFoundationAudioFormat() override;

    juce::Array<int> getPossibleSampleRates() override;
    juce::Array<int> getPossibleBitDepths() override;
    bool canDoStereo() override { return true; }
    bool canDoMono() override   { return true; }
    bool isCompressed() override { return true; }

    /** Needs a juce::FileInputStream (Media Foundation opens the file by path). */
    juce::AudioFormatReader* createReaderFor (juce::InputStream* sourceStream, bool deleteStreamIfOpeningFails) override;
    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (std::unique_ptr<juce::OutputStream>&, const juce::AudioFormatWriterOptions&) override { return nullptr; }

    /** True when Media Foundation could be started on this machine (N editions of Windows may lack it). */
    static bool isAvailable();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MediaFoundationAudioFormat)
};

} // namespace gocue
