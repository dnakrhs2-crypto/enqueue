#include "audio/AutoLeveler.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace gocue::tests
{
namespace
{
/** Listening material, not a check: with ENQ_AUTOLEVEL_RENDER_IN and ENQ_AUTOLEVEL_RENDER_OUT set, a WAV goes through
    the leveler (target ENQ_AUTOLEVEL_RENDER_TARGET, default -16 LUFS) in 10 ms blocks; the output WAV (24 bit) and
    gain.csv beside it (time_s, gain_db, hold) are written for the graph and for listening. Without them: nothing. */
class AutoLevelRenderTests : public juce::UnitTest
{
public:
    AutoLevelRenderTests() : UnitTest ("AutoLevel file render", "Enqueue") {}

    void runTest() override
    {
        beginTest ("a file through the leveler (only with ENQ_AUTOLEVEL_RENDER_IN / _OUT)");
        const auto inPath = juce::SystemStats::getEnvironmentVariable ("ENQ_AUTOLEVEL_RENDER_IN", {});
        const auto outPath = juce::SystemStats::getEnvironmentVariable ("ENQ_AUTOLEVEL_RENDER_OUT", {});
        if (inPath.isEmpty() || outPath.isEmpty())
        {
            expect (true);
            return;
        }

        const double target = juce::SystemStats::getEnvironmentVariable ("ENQ_AUTOLEVEL_RENDER_TARGET", "-16").getDoubleValue();
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (inPath)));
        expect (reader != nullptr, "readable input");
        if (reader == nullptr)
            return;

        const double sampleRate = reader->sampleRate;
        const int channels = (int) reader->numChannels;
        const int block = juce::jmax (1, (int) std::lround (sampleRate / 100.0));   // 10 ms

        AutoLeveler leveler;
        leveler.prepare (sampleRate, block, channels);
        leveler.setTargetLufs (target);
        leveler.setEnabled (true);

        const juce::File outFile (outPath);
        outFile.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (outFile.createOutputStream());
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate)
                                                       .withNumChannels (channels).withBitsPerSample (24));
        expect (writer != nullptr, "writable output");
        if (writer == nullptr)
            return;

        const auto logFile = outFile.getSiblingFile ("gain.csv");
        logFile.deleteFile();
        juce::FileOutputStream log (logFile);
        expect (log.openedOk(), "gain.csv");
        log << "time_s,gain_db,hold\n";

        juce::AudioBuffer<float> buffer (channels, block);
        float outPeak = 0.0f;
        const auto started = juce::Time::getMillisecondCounterHiRes();

        for (juce::int64 position = 0; position < reader->lengthInSamples; position += block)
        {
            const int n = (int) juce::jmin<juce::int64> (block, reader->lengthInSamples - position);
            reader->read (&buffer, 0, n, position, true, true);
            leveler.process (buffer, n);

            for (int ch = 0; ch < channels; ++ch)
                outPeak = juce::jmax (outPeak, buffer.getMagnitude (ch, 0, n));

            writer->writeFromAudioSampleBuffer (buffer, 0, n);
            log << juce::String ((double) (position + n) / sampleRate, 2) << "," << juce::String (leveler.getGainDb(), 4) << ",0\n";
        }

        const double seconds = (double) reader->lengthInSamples / sampleRate;
        logMessage ("rendered " + juce::String (seconds, 1) + " s in " + juce::String (juce::Time::getMillisecondCounterHiRes() - started, 0)
                    + " ms, output peak " + juce::String (juce::Decibels::gainToDecibels (outPeak), 2) + " dBFS");
        expect (outPeak <= 0.891251f, "never above -1 dBFS");
    }
};

static AutoLevelRenderTests autoLevelRenderTests;
}
}
