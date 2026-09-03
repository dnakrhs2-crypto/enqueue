#include "audio/MediaFoundationAudioFormat.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

/** WMA goes through the Media Foundation reader (gom asked whether .wma plays, 2026-09-03). The asset is a 2 s,
    440 Hz, 44.1 kHz stereo sine encoded with ffmpeg (wmav2). */
class WmaDecodeTests : public juce::UnitTest
{
public:
    WmaDecodeTests() : juce::UnitTest ("WMA decoding (Media Foundation)", "Enqueue") {}

    static juce::File findAsset()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

        for (int i = 0; i < 8; ++i)
        {
            const auto candidate = dir.getChildFile ("tests").getChildFile ("assets").getChildFile ("tone440.wma");

            if (candidate.existsAsFile())
                return candidate;

            dir = dir.getParentDirectory();
        }

        return {};
    }

    void runTest() override
    {
        beginTest ("a .wma file is accepted by the format manager and decodes to the sine it holds");

        if (! MediaFoundationAudioFormat::isAvailable())
        {
            logMessage ("Media Foundation not available on this machine: skipped");
            return;
        }

        const auto file = findAsset();
        expect (file.existsAsFile(), "tests/assets/tone440.wma missing");

        if (! file.existsAsFile())
            return;

        juce::AudioFormatManager manager;
        manager.registerBasicFormats();
        manager.registerFormat (new MediaFoundationAudioFormat(), false);

        expect (manager.findFormatForFileExtension ("wma") != nullptr, "wma is in the extension list");
        expect (manager.getWildcardForAllFormats().containsIgnoreCase ("*.wma"), "the file choosers offer wma");

        std::unique_ptr<juce::AudioFormatReader> reader (manager.createReaderFor (file));
        expect (reader != nullptr, "a reader opens the file");

        if (reader == nullptr)
            return;

        expectEquals ((int) reader->numChannels, 2);
        expectWithinAbsoluteError (reader->sampleRate, 44100.0, 1.0);
        expectGreaterThan ((double) reader->lengthInSamples / reader->sampleRate, 1.9);
        expectLessThan ((double) reader->lengthInSamples / reader->sampleRate, 2.2);

        juce::AudioBuffer<float> buffer (2, 44100);
        expect (reader->read (&buffer, 0, 44100, 22050, true, true));   // the middle second

        const float rms = buffer.getRMSLevel (0, 0, 44100);
        expectGreaterThan (rms, 0.2f);   // ffmpeg's sine is full scale-ish: rms ~ 0.7, lossy but far from silent

        int crossings = 0;
        const float* s = buffer.getReadPointer (0);

        for (int i = 1; i < 44100; ++i)
            if ((s[i - 1] < 0.0f) != (s[i] < 0.0f))
                ++crossings;

        expect (std::abs (crossings - 880) <= 8, "440 Hz sine: zero crossings " + juce::String (crossings));
    }
};

static WmaDecodeTests wmaDecodeTests;

} // namespace gocue::tests
