#include "AutoLevelTestSupport.h"
#include "VolumeCueUiTestSupport.h"
#include "audio/AudioEngine.h"
#include "audio/LoudnessScan.h"
#include "audio/ReadAheadSource.h"
#include "audio/MediaFoundationAudioFormat.h"
#include "ui/AutoLevelDialog.h"
#include "MainComponentTestAccess.h"
#include "TestGainPlugin.h"
#include <deque>
#include <thread>

namespace gocue::tests
{
using namespace auto_level;

namespace
{
/** A 48 kHz 24-bit WAV of the calibrated multi-tone: 'parts' of (seconds, LUFS) in order (-300 = silence). */
juce::File writeTone (const juce::File& file, std::initializer_list<std::pair<double, double>> parts, int channels = 2)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (channels).withBitsPerSample (24));
    Signal signal;
    juce::AudioBuffer<float> buffer (channels, block);

    for (const auto& [seconds, lufs] : parts)
        for (int b = 0; b < (int) std::llround (seconds * 100.0); ++b)
        {
            signal.fill (buffer, lufs);
            writer->writeFromAudioSampleBuffer (buffer, 0, block);
        }

    writer.reset();
    return file;
}

/** The scan's result for a region once the worker has it (up to 20 s). */
std::optional<LoudnessScanResult> awaitResult (LoudnessScan& scan, const juce::File& file, double start = 0.0, double end = -1.0)
{
    for (int i = 0; i < 1000; ++i)
    {
        if (scan.getPendingCount() == 0)
            if (const auto found = scan.lookup (file, start, end))
                return found;

        juce::Thread::sleep (20);
    }

    return scan.lookup (file, start, end);
}

/** A source of 'length' samples of 0.5 (for the read-ahead's shortfall count). */
struct ConstantSource : juce::PositionableAudioSource
{
    explicit ConstantSource (juce::int64 lengthToUse) : length (lengthToUse) {}
    void prepareToPlay (int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
            juce::FloatVectorOperations::fill (info.buffer->getWritePointer (ch, info.startSample), 0.5f, info.numSamples);
        position += info.numSamples;
    }
    void setNextReadPosition (juce::int64 p) override { position = p; }
    juce::int64 getNextReadPosition() const override { return position; }
    juce::int64 getTotalLength() const override { return length; }
    bool isLooping() const override { return false; }
    juce::int64 length, position = 0;
};

/** An audio cue for 'file' (two channels, default levels). */
Cue audioCueFor (const juce::File& file, double seconds)
{
    Cue c;
    c.file = file;
    c.durationSeconds = seconds;
    c.numChannels = 2;
    c.levels.resize (2, 2);
    c.levels.setDefaults();
    return c;
}

/** A two-input mic cue (the test feeds its input). */
Cue micCueFor()
{
    Cue c;
    c.type = CueType::mic;
    c.mic.numInputs = 2;
    c.levels.resize (2, 2);
    c.levels.setDefaults();
    return c;
}

/** Power of the first output channel over 'count' blocks from 'from', dB. */
double windowDb (const std::vector<float>& rms, size_t from, size_t count)
{
    double sum = 0.0;
    for (size_t b = from; b < from + count && b < rms.size(); ++b)
        sum += (double) rms[b] * (double) rms[b];
    return 10.0 * std::log10 (juce::jmax (1.0e-30, sum / (double) juce::jmax<size_t> (1, count)));
}

/** A source of 'length' samples of 0.5 whose next read can be held (a share that stops answering), for the read-ahead. */
struct GatedSource : juce::PositionableAudioSource
{
    void prepareToPlay (int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        if (gated.load())
        {
            entered.signal();
            open.wait (-1);
        }
        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
            juce::FloatVectorOperations::fill (info.buffer->getWritePointer (ch, info.startSample), 0.5f, info.numSamples);
        position += info.numSamples;
    }
    void setNextReadPosition (juce::int64 p) override { position = p; }
    juce::int64 getNextReadPosition() const override { return position; }
    juce::int64 getTotalLength() const override { return length.load(); }
    bool isLooping() const override { return false; }
    std::atomic<juce::int64> length { 0 };
    juce::int64 position = 0;
    std::atomic<bool> gated { false };
    juce::WaitableEvent entered, open { true };
};

/** An audio format whose reads do not return until 'gate' opens: a file on a share that stopped answering. */
struct StuckReader : juce::AudioFormatReader
{
    StuckReader (juce::InputStream* in, juce::WaitableEvent& g) : juce::AudioFormatReader (in, "Stuck test"), gate (g)
    {
        sampleRate = 48000.0;
        bitsPerSample = 32;
        lengthInSamples = 48000 * 10;
        numChannels = 2;
        usesFloatingPointData = true;
    }
    bool readSamples (int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                      juce::int64, int numSamples) override
    {
        gate.wait (-1);
        for (int ch = 0; ch < numDestChannels; ++ch)
            if (destChannels[ch] != nullptr)
                std::fill_n (destChannels[ch] + startOffsetInDestBuffer, numSamples, 0);
        return true;
    }
    juce::WaitableEvent& gate;
};

struct StuckFormat : juce::AudioFormat
{
    explicit StuckFormat (juce::WaitableEvent& g) : juce::AudioFormat ("Stuck test", juce::StringArray { ".stuck" }), gate (g) {}
    juce::Array<int> getPossibleSampleRates() override { return { 48000 }; }
    juce::Array<int> getPossibleBitDepths() override { return { 32 }; }
    bool canDoStereo() override { return true; }
    bool canDoMono() override { return true; }
    juce::AudioFormatReader* createReaderFor (juce::InputStream* in, bool) override { return new StuckReader (in, gate); }
    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (std::unique_ptr<juce::OutputStream>&, const juce::AudioFormatWriterOptions&) override { return nullptr; }
    juce::WaitableEvent& gate;
};

/** A quiet tone (-30 LUFS) with a 1 ms click at -0.9 dBFS every 'everyBlocks' blocks: a match of +3 puts the clicks over full
    scale, which only the master's limiter may take. */
juce::File writeClicks (const juce::File& file, double seconds, int everyBlocks)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
    Signal tone;
    juce::AudioBuffer<float> buffer (2, block);

    for (int b = 0; b < (int) std::llround (seconds * 100.0); ++b)
    {
        tone.fill (buffer, -30.0);
        if (b % everyBlocks == everyBlocks / 2)
            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::fill (buffer.getWritePointer (ch, 100), 0.9f, 48);
        writer->writeFromAudioSampleBuffer (buffer, 0, block);
    }

    writer.reset();
    return file;
}

/** RMS of 'samples' over [from, from + count), against 'reference' (dB). */
double samplesDb (const std::vector<float>& samples, size_t from, size_t count, double reference)
{
    double sum = 0.0;
    for (size_t i = from; i < from + count && i < samples.size(); ++i)
        sum += (double) samples[i] * (double) samples[i];
    return 20.0 * std::log10 (juce::jmax (1.0e-15, std::sqrt (sum / (double) juce::jmax<size_t> (1, count)) / reference));
}

/** A loud mic has the master leveler cut it for 15 s; then it stops and a measured cue starts straight on - its match
    makes up for the master still being down. Renders block by block, keeping each block's RMS (first channel). */
struct CutScene
{
    CutScene (const juce::File& file, double fileSeconds, double target, double micLufs, double matchDb)
    {
        engine.prepare (rate, block);
        engine.setAutoLevel (true, target);
        cue = audioCueFor (file, fileSeconds);
        engine.setLoudnessMatchDb (cue.id, matchDb);
        engine.setLoudnessMatchActive (true);
        engine.play (mic);
        for (int b = 0; b < 1500; ++b)
            render (micLufs);
        cut = engine.getAutoLevelGainDb();
        engine.stop (mic.id);
        started = engine.play (cue);
        cueStart = rms.size();
    }
    void render (double micLufs = -200.0)
    {
        if (micLufs > -100.0)
            loud.fill (input, micLufs);
        else
            input.clear();
        engine.renderBlock (out, block, input.getArrayOfReadPointers(), 2);
        rms.push_back (out.getRMSLevel (0, 0, block));
    }
    double loudestFrom (size_t from) const
    {
        double loudest = -200.0;
        for (size_t b = from; b + 5 <= rms.size(); ++b)
            loudest = juce::jmax (loudest, windowDb (rms, b, 5));
        return loudest;
    }
    AudioEngine engine { 0 };
    Cue mic = micCueFor(), cue;
    Signal loud;
    juce::AudioBuffer<float> input { 2, block }, out { 2, block };
    std::vector<float> rms;
    double cut = 0.0;
    bool started = false;
    size_t cueStart = 0;
};
} // namespace

class LoudnessScanTests : public juce::UnitTest
{
public:
    LoudnessScanTests() : UnitTest ("Loudness scan", "Enqueue") {}

    void runTest() override
    {
        volume_ui::Scratch scratch;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        beginTest ("a region's integrated loudness and recurring peak, as outputs 1-2 hear it");
        {
            const auto steady = writeTone (scratch.folder.getChildFile ("steady.wav"), { { 10.0, -23.0 } });
            const auto result = LoudnessScan::measure (formats, steady, 0.0, -1.0);
            logMessage ("steady -23 LUFS tone: " + juce::String (result.integratedLufs, 3) + " LUFS, peak " + juce::String (result.peakDb, 2) + " dBFS");
            expect (result.valid);
            expectWithinAbsoluteError (result.integratedLufs, -23.0, 0.05);
            expectWithinAbsoluteError (result.seconds, 10.0, 0.01);

            // a mono file plays on outputs 1 and 2: it is heard like the same samples on both channels of a stereo one
            const auto mono = writeTone (scratch.folder.getChildFile ("mono.wav"), { { 10.0, -23.0 } }, 1);
            const auto monoResult = LoudnessScan::measure (formats, mono, 0.0, -1.0);
            expect (monoResult.valid);
            expectWithinAbsoluteError (monoResult.integratedLufs, result.integratedLufs, 0.05);

            // the cue's region alone: a quiet start and a loud rest are measured apart
            const auto parts = writeTone (scratch.folder.getChildFile ("parts.wav"), { { 5.0, -30.0 }, { 10.0, -15.0 } });
            const auto quiet = LoudnessScan::measure (formats, parts, 0.0, 5.0);
            const auto loud = LoudnessScan::measure (formats, parts, 5.0, -1.0);
            logMessage ("region 0-5 s: " + juce::String (quiet.integratedLufs, 3) + ", 5 s-end: " + juce::String (loud.integratedLufs, 3));
            expectWithinAbsoluteError (quiet.integratedLufs, -30.0, 0.1);
            expectWithinAbsoluteError (loud.integratedLufs, -15.0, 0.1);
        }

        beginTest ("too short or silent: nothing to match");
        {
            const auto shortFile = writeTone (scratch.folder.getChildFile ("short.wav"), { { 2.5, -20.0 } });
            const auto silent = writeTone (scratch.folder.getChildFile ("silent.wav"), { { 10.0, -300.0 } });
            expect (! LoudnessScan::measure (formats, shortFile, 0.0, -1.0).valid, "a 2.5 s effect is not matched");
            expect (! LoudnessScan::measure (formats, silent, 0.0, -1.0).valid, "silence is not matched");
            expect (! LoudnessScan::measure (formats, scratch.folder.getChildFile ("missing.wav"), 0.0, -1.0).valid);
            expectEquals (loudnessMatchDb (LoudnessScan::measure (formats, shortFile, 0.0, -1.0), -16.0), 0.0);
        }

        beginTest ("the peak that recurs: one spike is the limiter's job");
        {
            const auto file = scratch.folder.getChildFile ("spike.wav");
            {
                std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
                juce::WavAudioFormat wav;
                auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
                Signal signal;
                juce::AudioBuffer<float> buffer (2, block);

                for (int b = 0; b < 1000; ++b)
                {
                    signal.fill (buffer, -30.0);
                    if (b == 500) buffer.setSample (0, 17, 0.99f);
                    writer->writeFromAudioSampleBuffer (buffer, 0, block);
                }
            }
            const auto plain = LoudnessScan::measure (formats, writeTone (scratch.folder.getChildFile ("plain.wav"), { { 10.0, -30.0 } }), 0.0, -1.0);
            const auto spiked = LoudnessScan::measure (formats, file, 0.0, -1.0);
            logMessage ("recurring peak without / with one 0 dBFS spike: " + juce::String (plain.peakDb, 2) + " / " + juce::String (spiked.peakDb, 2));
            expectWithinAbsoluteError (spiked.peakDb, plain.peakDb, 0.01);
        }

        beginTest ("the match: to the target, a boost bounded by +12 dB and by the recurring peak, a cut by -20 dB");
        {
            LoudnessScanResult r;
            r.valid = true;
            r.integratedLufs = -26.0;
            r.peakDb = -12.0;
            expectWithinAbsoluteError (loudnessMatchDb (r, -16.0), 10.0, 1.0e-9);
            r.peakDb = -1.0;
            expectWithinAbsoluteError (loudnessMatchDb (r, -16.0), 3.0, 1.0e-9);   // the peak 2 dB over full scale at most
            r.peakDb = -30.0;
            r.integratedLufs = -40.0;
            expectWithinAbsoluteError (loudnessMatchDb (r, -16.0), 12.0, 1.0e-9);
            r.integratedLufs = -2.0;
            expectWithinAbsoluteError (loudnessMatchDb (r, -16.0), -14.0, 1.0e-9);
            r.integratedLufs = 10.0;
            expectWithinAbsoluteError (loudnessMatchDb (r, -16.0), -20.0, 1.0e-9);
            r.integratedLufs = -26.0;
            r.peakDb = 3.0;   // a float file over full scale: no boost, never a cut for it
            expectWithinAbsoluteError (loudnessMatchDb (r, -16.0), 0.0, 1.0e-9);
            r.valid = false;
            expectEquals (loudnessMatchDb (r, -16.0), 0.0);
        }

        beginTest ("the scan: queued on the first look, read once, kept across sessions, read again when the file changes");
        {
            const auto cache = scratch.folder.getChildFile ("loudness-cache.json");
            const auto song = writeTone (scratch.folder.getChildFile ("song.wav"), { { 8.0, -26.0 } });
            {
                LoudnessScan scan (formats, cache);
                expect (! scan.lookup (song, 0.0, -1.0).has_value(), "nothing known yet: queued");
                const auto found = awaitResult (scan, song);
                expect (found.has_value() && found->valid);
                if (found) expectWithinAbsoluteError (found->integratedLufs, -26.0, 0.05);
                expectEquals (scan.getMeasuredCount(), 1);
                scan.lookup (song, 0.0, -1.0);
                juce::Thread::sleep (100);
                expectEquals (scan.getMeasuredCount(), 1, "asked again: not read again");
            }
            expect (cache.existsAsFile(), "the cache is saved");
            {
                LoudnessScan scan (formats, cache);
                expect (! scan.lookup (song, 0.0, -1.0).has_value(), "the next session answers once the file is checked");
                const auto known = awaitResult (scan, song);
                expect (known.has_value() && known->valid);
                expectEquals (scan.getMeasuredCount(), 0, "an unchanged file is not read again");
            }
            writeTone (song, { { 9.0, -18.0 } });   // the same path, new content (and size)
            {
                LoudnessScan scan (formats, cache);
                expect (! scan.lookup (song, 0.0, -1.0).has_value(), "never the old answer for a file changed since");
                const auto fresh = awaitResult (scan, song);
                expectEquals (scan.getMeasuredCount(), 1, "a changed file is read again");
                expect (fresh.has_value());
                if (fresh) expectWithinAbsoluteError (fresh->integratedLufs, -18.0, 0.05);
            }
        }

        beginTest ("a file overwritten while the app runs: noticed by the recheck, measured again");
        {
            LoudnessScan scan (formats, scratch.folder.getChildFile ("edited-cache.json"));
            scan.setRecheckInterval (100, 100);
            const auto file = writeTone (scratch.folder.getChildFile ("edited.wav"), { { 6.0, -26.0 } });
            const auto first = awaitResult (scan, file);
            expect (first.has_value() && first->valid);
            writeTone (file, { { 7.0, -14.0 } });   // louder, saved over the same file
            std::optional<LoudnessScanResult> fresh;
            for (int i = 0; i < 1000 && ! fresh; ++i)
            {
                if (const auto now = scan.lookup (file, 0.0, -1.0); now && std::abs (now->integratedLufs + 14.0) < 0.05)
                    fresh = now;
                juce::Thread::sleep (10);
            }
            expect (fresh.has_value(), "the new loudness, without a new session");
            expectEquals (scan.getMeasuredCount(), 2);
        }

        beginTest ("a file that cannot be read: tried again next session, not over and over in this one");
        {
            const auto cache = scratch.folder.getChildFile ("retry-cache.json");
            const auto bad = scratch.folder.getChildFile ("bad.wav");
            bad.replaceWithText ("not audio");
            const auto effect = writeTone (scratch.folder.getChildFile ("effect.wav"), { { 1.0, -20.0 } });
            {
                LoudnessScan scan (formats, cache);
                scan.setRecheckInterval (50, 50);
                const auto failed = awaitResult (scan, bad);
                expect (failed.has_value() && ! failed->valid && failed->failed, "could not be read");
                const auto shortOne = awaitResult (scan, effect);
                expect (shortOne.has_value() && ! shortOne->valid && ! shortOne->failed, "too short: an answer");
                juce::Thread::sleep (500);   // ten rechecks
                expectEquals (scan.getMeasuredCount(), 2, "the unreadable file is not read again and again");
            }
            {
                LoudnessScan scan (formats, cache);
                awaitResult (scan, bad);
                awaitResult (scan, effect);
                expectEquals (scan.getMeasuredCount(), 1, "next session: the unreadable file again, the short effect not");
            }
        }

        beginTest ("how long a 4-minute song takes to measure (WAV, and MP3 / M4A when ffmpeg is at hand)");
        {
            const auto wavSong = writeTone (scratch.folder.getChildFile ("song4.wav"), { { 240.0, -18.0 } });
            juce::AudioFormatManager all;
            all.registerBasicFormats();
            #if JUCE_WINDOWS
            if (MediaFoundationAudioFormat::isAvailable())
                all.registerFormat (new MediaFoundationAudioFormat(), false);
            #endif
            const juce::File ffmpeg ("C:/Users/claude/ffmpeg/ffmpeg-8.0.1-essentials_build/bin/ffmpeg.exe");
            juce::StringArray report;
            for (const auto* extension : { ".wav", ".mp3", ".m4a" })
            {
                auto file = wavSong;
                if (juce::String (extension) != ".wav")
                {
                    if (! ffmpeg.existsAsFile())
                        continue;
                    file = scratch.folder.getChildFile (juce::String ("song4") + extension);
                    juce::ChildProcess encode;
                    encode.start (juce::StringArray { ffmpeg.getFullPathName(), "-v", "error", "-y", "-i", wavSong.getFullPathName(), "-b:a", "192k", file.getFullPathName() });
                    encode.waitForProcessToFinish (120000);
                    if (! file.existsAsFile())
                        continue;
                }
                const auto started = juce::Time::getMillisecondCounterHiRes();
                const auto result = LoudnessScan::measure (all, file, 0.0, -1.0);
                const auto seconds = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
                report.add (juce::String (extension) + " " + juce::String (seconds, 2) + " s (" + juce::String (result.integratedLufs, 2) + " LUFS)");
                expect (result.valid, juce::String ("measured: ") + extension);
                expectWithinAbsoluteError (result.integratedLufs, -18.0, 0.3);
            }
            logMessage ("a 4-minute song: " + report.joinIntoString (", "));
        }

        beginTest ("while cues play the scan reads slowly (about 10x real time)");
        {
            const auto longFile = writeTone (scratch.folder.getChildFile ("long.wav"), { { 20.0, -20.0 } });
            const auto timed = [&] (bool busy)
            {
                LoudnessScan scan (formats, scratch.folder.getChildFile (busy ? "busy-cache.json" : "idle-cache.json"));
                scan.setBusy (busy);
                const auto started = juce::Time::getMillisecondCounterHiRes();
                awaitResult (scan, longFile);
                return (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
            };
            const double idle = timed (false), busy = timed (true);
            logMessage ("20 s file: " + juce::String (idle, 2) + " s idle, " + juce::String (busy, 2) + " s with cues playing");
            expect (idle < 1.5, "idle: as fast as the disk allows");
            expect (busy > 1.5, "with cues playing: paced");
        }

        beginTest ("a matched cue is at its level from the first sample; switched off it glides back, never a step");
        {
            const auto file = writeTone (scratch.folder.getChildFile ("cue.wav"), { { 12.0, -26.0 } });
            const auto cue = audioCueFor (file, 12.0);
            // the master switched on first and at rest (its 20 ms fade-in done): a measured cue's match applies while it is on
            const auto run = [&] (std::optional<double> matchDb, bool on, std::vector<float>& rms, int blocks,
                                  std::function<void (AudioEngine&, int)> during = {})
            {
                AudioEngine engine (0);
                engine.prepare (rate, block);
                if (matchDb)
                    engine.setLoudnessMatchDb (cue.id, *matchDb);
                engine.setLoudnessMatchActive (on);
                engine.setAutoLevel (on, -16.0);
                juce::AudioBuffer<float> out (2, block);
                for (int b = 0; b < 10; ++b)
                    engine.renderBlock (out, block, nullptr, 0);
                expect (engine.play (cue));
                for (int b = 0; b < blocks; ++b)
                {
                    if (during)
                        during (engine, b);
                    engine.renderBlock (out, block, nullptr, 0);
                    rms.push_back (out.getRMSLevel (0, 0, block));
                }
            };

            std::vector<float> onPlain, matched, offPlain, inactive;
            run (std::nullopt, true, onPlain, 40);    // the master on, the cue not measured: untouched for 0.4 s (no decision yet)
            run (10.0, true, matched, 40);
            run (std::nullopt, false, offPlain, 150); // the file as it is
            run (10.0, false, inactive, 20);          // measured, auto level off
            const double first = 20.0 * std::log10 ((double) matched[1] / (double) onPlain[1]);
            logMessage ("second block, matched +10 vs plain: " + juce::String (first, 3) + " dB");
            expectWithinAbsoluteError (first, 10.0, 0.05);
            expectWithinAbsoluteError ((double) inactive[5], (double) offPlain[5], 1.0e-6, "auto level off: the file as it is");

            // switched off while playing: back to the file, at most 40 dB/s (100 ms windows against the file: the bypass's
            // 20 ms crossfade between the 5 ms lookahead and the dry signal combs a 50 ms window by a few tenths of a dB)
            std::vector<float> off;
            run (10.0, true, off, 150, [] (AudioEngine& e, int b)
            {
                if (b == 20)
                {
                    e.setLoudnessMatchActive (false);
                    e.setAutoLevel (false, -16.0);
                }
            });
            const auto windowGain = [&] (size_t b) { return windowDb (off, b, 10) - windowDb (offPlain, b, 10); };
            double steepest = 0.0;
            for (size_t b = 20; b + 20 <= off.size(); ++b)
                steepest = juce::jmax (steepest, std::abs (windowGain (b + 10) - windowGain (b)) / 0.1);
            const double endGain = windowGain (off.size() - 10);
            logMessage ("switched off: steepest " + juce::String (steepest, 2) + " dB/s, then " + juce::String (endGain, 3) + " dB from the file");
            expect (steepest <= 45.0, "no step: 40 dB/s");
            expectWithinAbsoluteError (endGain, 0.0, 0.05);

            // a new measurement while it plays is for its next start
            std::vector<float> kept;
            run (10.0, true, kept, 40, [&cue] (AudioEngine& e, int b) { if (b == 20) e.setLoudnessMatchDb (cue.id, 0.0); });
            expectWithinAbsoluteError (20.0 * std::log10 ((double) kept[39] / (double) onPlain[39]), 10.0, 0.1);
        }

        beginTest ("a matched cue after the leveler rode something else up: the two together are the match, no blast");
        {
            const auto quiet = writeTone (scratch.folder.getChildFile ("before.wav"), { { 12.0, -26.0 } });
            const auto next = writeTone (scratch.folder.getChildFile ("next.wav"), { { 6.0, -26.0 } });
            const auto cueFor = [] (const juce::File& f, double seconds)
            {
                Cue c;
                c.file = f;
                c.durationSeconds = seconds;
                c.numChannels = 2;
                c.levels.resize (2, 2);
                c.levels.setDefaults();
                return c;
            };
            const auto unmeasured = cueFor (quiet, 12.0), measured = cueFor (next, 6.0);
            const auto run = [&]
            {
                AudioEngine engine (0);
                engine.prepare (rate, block);
                engine.setAutoLevel (true, -16.0);
                engine.setLoudnessMatchDb (measured.id, 10.0);   // measured: +10 brings it to the target
                engine.setLoudnessMatchActive (true);
                juce::AudioBuffer<float> out (2, block);
                Meter meter;
                expect (engine.play (unmeasured));                // not measured (a mic, a short sound): the leveler rides it up
                for (int b = 0; b < 1000; ++b) engine.renderBlock (out, block, nullptr, 0);
                const double ridden = engine.getAutoLevelGainDb();
                engine.stop (unmeasured.id);
                expect (engine.play (measured));                  // straight on: no gap that would bring the leveler home
                double loudest = -100.0;
                for (int b = 0; b < 300; ++b)
                {
                    engine.renderBlock (out, block, nullptr, 0);
                    meter.process (out);
                    if (b >= 50) loudest = juce::jmax (loudest, meter.lufs());
                }
                return std::make_pair (ridden, loudest);
            };
            const auto [ridden, loudest] = run();
            logMessage ("leveler up " + juce::String (ridden, 2) + " dB for the unmeasured cue; the matched cue then peaks at "
                        + juce::String (loudest, 2) + " LUFS (3 s window) against -16");
            expect (ridden > 8.0, "the leveler had ridden the quiet unmeasured cue up");
            expect (loudest <= -15.0, "no blast: the match and the leveler together make +10, not +20");
        }

        beginTest ("switched on while a measured cue plays: the match and the leveler do not both push it up");
        {
            const auto file = writeTone (scratch.folder.getChildFile ("playing.wav"), { { 20.0, -26.0 } });
            Cue cue;
            cue.file = file;
            cue.durationSeconds = 20.0;
            cue.numChannels = 2;
            cue.levels.resize (2, 2);
            cue.levels.setDefaults();
            const auto run = [&] (bool measured)
            {
                AudioEngine engine (0);
                engine.prepare (rate, block);
                if (measured)
                    engine.setLoudnessMatchDb (cue.id, 10.0);    // measured in an earlier session: +10 brings it to -16
                expect (engine.play (cue));                       // auto level off: the file as it is
                juce::AudioBuffer<float> out (2, block);
                for (int b = 0; b < 200; ++b)
                    engine.renderBlock (out, block, nullptr, 0);
                engine.setAutoLevel (true, -16.0);                // the main component's order: the leveler, then the matches
                engine.setLoudnessMatchActive (true);
                livemix::KWeightingFilter kl, kr;
                kl.prepare (rate);
                kr.prepare (rate);
                std::deque<double> window;                        // 400 ms: a bump of half a second shows
                double windowSum = 0.0, loudest = -100.0, last = -100.0;
                for (int b = 0; b < 1000; ++b)
                {
                    engine.renderBlock (out, block, nullptr, 0);
                    double sum = 0.0;
                    for (int i = 0; i < block; ++i)
                    {
                        const double l = kl.process (out.getSample (0, i)), r = kr.process (out.getSample (1, i));
                        sum += l * l + r * r;
                    }
                    window.push_back (sum);
                    windowSum += sum;
                    if (window.size() > 40)
                    {
                        windowSum -= window.front();
                        window.pop_front();
                    }
                    if (window.size() == 40)
                    {
                        last = -0.691 + 10.0 * std::log10 (juce::jmax (1.0e-30, windowSum / (40.0 * block)));
                        loudest = juce::jmax (loudest, last);
                    }
                }
                return std::make_pair (loudest, last);
            };
            const auto [matchedPeak, matchedEnd] = run (true);
            const auto [riddenPeak, riddenEnd] = run (false);
            logMessage ("switched on mid-cue, 400 ms loudness against -16: measured cue peaks at " + juce::String (matchedPeak, 2)
                        + ", ends at " + juce::String (matchedEnd, 2) + "; the leveler alone peaks at " + juce::String (riddenPeak, 2)
                        + ", ends at " + juce::String (riddenEnd, 2));
            expect (matchedPeak <= -15.0, "no overshoot from two hands on one fader");
            expectWithinAbsoluteError (matchedEnd, -16.0, 0.5);
        }

        beginTest ("switched off while a matched cue plays under a cutting master: never louder than before the switch");
        {
            const auto file = writeTone (scratch.folder.getChildFile ("under.wav"), { { 30.0, -26.0 } });
            CutScene s (file, 30.0, -16.0, -6.0, 10.0);
            expect (s.started);
            for (int b = 0; b < 100; ++b)
                s.render();
            const size_t at = s.rms.size();
            const double before = windowDb (s.rms, at - 10, 10);
            s.engine.setAutoLevel (false, -16.0);   // the main component's order: the leveler, then the matches
            s.engine.setLoudnessMatchActive (false);
            for (int b = 0; b < 200; ++b)
                s.render();
            const double loudest = s.loudestFrom (at), after = windowDb (s.rms, s.rms.size() - 10, 10);
            logMessage ("master " + juce::String (s.cut, 2) + " dB; the cue " + juce::String (before, 2) + " dB before the switch, its loudest 50 ms after "
                        + juce::String (loudest, 2) + ", 2 s later " + juce::String (after, 2));
            expect (s.cut < -8.0, "the loud mic had the master down");
            expect (loudest <= before + 0.5, "never louder than before the switch");
            expectWithinAbsoluteError (after, before - 10.0, 0.5, "then the file as it is (the match was +10)");
        }

        beginTest ("the device restarts while a matched cue plays under a cutting master: the cue keeps its level");
        {
            const auto file = writeTone (scratch.folder.getChildFile ("restart.wav"), { { 30.0, -26.0 } });
            CutScene s (file, 30.0, -16.0, -6.0, 10.0);
            expect (s.started);
            for (int b = 0; b < 100; ++b)
                s.render();
            const size_t at = s.rms.size();
            const double before = windowDb (s.rms, at - 10, 10);
            s.engine.prepare (rate, block);          // the device opened again, same format: the leveler starts over
            for (int b = 0; b < 100; ++b)
                s.render();
            const double loudest = s.loudestFrom (at), after = windowDb (s.rms, s.rms.size() - 10, 10);
            logMessage ("the cue " + juce::String (before, 2) + " dB before the restart, its loudest 50 ms after " + juce::String (loudest, 2)
                        + ", 1 s later " + juce::String (after, 2));
            expect (loudest <= before + 0.5, "never louder than before the restart");
            expectWithinAbsoluteError (after, before, 0.5, "the cue at its match again");
        }

        beginTest ("a match and a master far apart still add up at the start (+12 on a master at -20)");
        {
            const auto file = writeTone (scratch.folder.getChildFile ("far.wav"), { { 30.0, -42.0 } });
            CutScene s (file, 30.0, -30.0, -6.0, 12.0);
            expect (s.started);
            for (int b = 0; b < 40; ++b)
                s.render();                          // 0.4 s: before the leveler judges the new sound
            AudioEngine plainEngine (0);             // the file as it is, the same moment of its run
            plainEngine.prepare (rate, block);
            expect (plainEngine.play (audioCueFor (file, 30.0)));
            std::vector<float> plain;
            juce::AudioBuffer<float> out (2, block);
            for (int b = 0; b < 40; ++b)
            {
                plainEngine.renderBlock (out, block, nullptr, 0);
                plain.push_back (out.getRMSLevel (0, 0, block));
            }
            const double gain = windowDb (s.rms, s.cueStart + 20, 20) - windowDb (plain, 20, 20);
            logMessage ("master " + juce::String (s.cut, 2) + " dB; the cue starts " + juce::String (gain, 2) + " dB over the file (its match +12)");
            expect (s.cut < -19.0, "the master at its -20 dB floor");
            expectWithinAbsoluteError (gain, 12.0, 0.5, "the match and the master add up exactly");
        }

        beginTest ("a cue whose slices skip or repeat parts gets no match - the leveler rides it; a plain one does");
        {
            volume_ui::Fixture fixture;
            const auto file = writeTone (fixture.scratch.folder.getChildFile ("sliced.wav"), { { 8.0, -26.0 } });
            const auto plain = audioCueFor (file, 8.0);
            auto skipping = audioCueFor (file, 8.0);
            skipping.audio.slices.push_back ({ 4.0, 0 });       // the second half skipped
            auto repeating = audioCueFor (file, 8.0);
            repeating.audio.firstSliceCount = 3;               // the first half three times
            repeating.audio.slices.push_back ({ 4.0, 1 });
            auto atEnd = audioCueFor (file, 8.0);
            atEnd.audio.slices.push_back ({ 8.0, 0 });          // a file that got shorter put the marker right at its end
            for (const auto& c : { plain, skipping, repeating, atEnd })
                fixture.document().cues.add (c);
            AutoLevelDialog::Content dialog (fixture.document(), fixture.engine);
            auto* on = volume_ui::child<juce::TextButton> (dialog, [] (const auto& b) { return b.getButtonText() == ko ("켜기"); });
            expect (on != nullptr);
            if (on != nullptr)
                on->onClick();
            for (int i = 0; i < 1000 && ! fixture.engine.getLoudnessMatchDb (plain.id).has_value(); ++i)
            {
                volume_ui::dispatch();
                juce::Thread::sleep (20);
            }
            const auto counts = ReopenLastProjectTestAccess::loudnessMatchCounts (*fixture.main);
            logMessage ("counts " + juce::String (counts[0]) + "/" + juce::String (counts[1]) + "/" + juce::String (counts[2]));
            expect (fixture.engine.getLoudnessMatchDb (plain.id).has_value(), "the plain cue is matched");
            expect (! fixture.engine.getLoudnessMatchDb (skipping.id).has_value(), "a skipped slice: no match");
            expect (! fixture.engine.getLoudnessMatchDb (repeating.id).has_value(), "a repeated slice: no match");
            expect (fixture.engine.getLoudnessMatchDb (atEnd.id).has_value(), "a marker at the end starts nothing: matched");
            expectEquals (counts[0], 2, "the plain cue and the one with its marker at the end");
        }

        beginTest ("the cues to match are counted once each when lists are switched, and again when one is removed");
        {
            volume_ui::Fixture fixture;
            auto& document = fixture.document();
            const auto fileA = writeTone (fixture.scratch.folder.getChildFile ("listA.wav"), { { 5.0, -26.0 } });
            const auto fileB = writeTone (fixture.scratch.folder.getChildFile ("listB.wav"), { { 5.0, -20.0 } });
            document.cues.add (audioCueFor (fileA, 5.0));
            const int second = document.addContainer ("B", false);
            document.setActiveContainer (second);
            document.cues.add (audioCueFor (fileB, 5.0));
            document.setActiveContainer (0);
            AutoLevelDialog::Content dialog (document, fixture.engine);
            auto* on = volume_ui::child<juce::TextButton> (dialog, [] (const auto& b) { return b.getButtonText() == ko ("켜기"); });
            expect (on != nullptr);
            if (on != nullptr)
                on->onClick();
            const auto counts = [&fixture]
            {
                volume_ui::dispatch();
                return ReopenLastProjectTestAccess::loudnessMatchCounts (*fixture.main);
            };
            const auto show = [] (const std::array<int, 3>& c) { return juce::String (c[0]) + "/" + juce::String (c[1]) + "/" + juce::String (c[2]); };
            for (int i = 0; i < 1000 && counts()[1] < 2; ++i)
                juce::Thread::sleep (20);
            const std::array<int, 3> both { 2, 2, 0 }, one { 1, 1, 0 };
            auto now = counts();
            expect (now == both, "both lists' cues measured: " + show (now));
            document.setActiveContainer (second);
            now = counts();
            expect (now == both, "the other list shown: still two, " + show (now));
            document.setActiveContainer (0);
            now = counts();
            expect (now == both, "and back: " + show (now));
            document.removeContainer (second);
            now = counts();
            expect (now == one, "the other list removed: " + show (now));
        }

        beginTest ("a session that only reads the cache keeps the use times (the oldest go first when it is full)");
        {
            const auto cache = scratch.folder.getChildFile ("used-cache.json");
            const auto song = writeTone (scratch.folder.getChildFile ("used.wav"), { { 4.0, -20.0 } });
            const auto usedOnDisk = [&cache]
            {
                const auto parsed = juce::JSON::parse (cache.loadFileAsString());
                const auto* list = parsed.getProperty ("entries", juce::var()).getArray();
                return list != nullptr && ! list->isEmpty() ? (juce::int64) list->getReference (0).getProperty ("used", 0) : (juce::int64) 0;
            };
            {
                LoudnessScan scan (formats, cache);
                awaitResult (scan, song);
            }
            const auto first = usedOnDisk();
            juce::Thread::sleep (30);
            {
                LoudnessScan scan (formats, cache);
                expect (awaitResult (scan, song).has_value());
                expectEquals (scan.getMeasuredCount(), 0);
            }
            expect (first > 0 && usedOnDisk() > first, "this session's use is on disk: " + juce::String (first) + " -> " + juce::String (usedOnDisk()));
        }

        beginTest ("a file saved over: every region of it measured again, first, not held back by a busy queue");
        {
            LoudnessScan scan (formats, scratch.folder.getChildFile ("regions-cache.json"));
            scan.setRecheckInterval (100, 100);
            const auto file = writeTone (scratch.folder.getChildFile ("regions.wav"), { { 10.0, -26.0 } });
            expect (awaitResult (scan, file).has_value() && awaitResult (scan, file, 2.0, 6.0).has_value());
            std::vector<juce::File> longOnes;
            for (int i = 0; i < 3; ++i)
                longOnes.push_back (writeTone (scratch.folder.getChildFile ("long" + juce::String (i) + ".wav"), { { 20.0, -20.0 } }));
            writeTone (file, { { 11.0, -14.0 } });   // saved over, louder
            scan.setBusy (true);                     // cues playing: the long files take about 2 s each
            for (const auto& f : longOnes)
                scan.lookup (f, 0.0, -1.0);
            std::optional<LoudnessScanResult> whole, part;
            for (int i = 0; i < 1500 && ! (whole && part); ++i)
            {
                if (const auto w = scan.lookup (file, 0.0, -1.0); w && std::abs (w->integratedLufs + 14.0) < 0.05) whole = w;
                if (const auto p = scan.lookup (file, 2.0, 6.0); p && std::abs (p->integratedLufs + 14.0) < 0.05) part = p;
                juce::Thread::sleep (10);
            }
            const int pending = scan.getPendingCount();
            logMessage ("both regions new with " + juce::String (pending) + " long file(s) still to measure");
            expect (whole.has_value() && part.has_value(), "both regions of the file measured again");
            expect (pending > 0, "before the long files queued after them: the recheck is not starved");
            scan.setBusy (false);
        }

        beginTest ("sweep: switched off, back on, a restart, a cue started while the master glides - never above the match or before");
        {
            // one file pair per match, at the level the match brings to -16 on one output: A is routed to output 1 only,
            // B to output 2 only, so each is measured alone (one channel is 3 dB under the stereo file)
            struct Pair { double matchDb; juce::File a, b; double refA, refB; };
            std::vector<Pair> pairs;
            for (const double matchDb : { 10.0, 0.0, -6.0 })
            {
                const double fileLufs = -16.0 - matchDb + 3.0;
                Pair p { matchDb, writeTone (scratch.folder.getChildFile ("sweepA" + juce::String ((int) matchDb) + ".wav"), { { 12.0, fileLufs } }),
                         writeTone (scratch.folder.getChildFile ("sweepB" + juce::String ((int) matchDb) + ".wav"), { { 12.0, fileLufs } }), 0.0, 0.0 };
                for (int side = 0; side < 2; ++side)
                {
                    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (side == 0 ? p.a : p.b));
                    juce::AudioBuffer<float> all ((int) reader->numChannels, (int) reader->lengthInSamples);
                    reader->read (&all, 0, (int) reader->lengthInSamples, 0, true, true);
                    (side == 0 ? p.refA : p.refB) = all.getRMSLevel (side, 0, all.getNumSamples());
                }
                pairs.push_back (p);
            }
            const auto routed = [] (const juce::File& f, int output)
            {
                auto c = audioCueFor (f, 12.0);
                c.levels.outputDb[(size_t) (1 - output)] = LevelMatrix::silentDb;   // only this output
                return c;
            };
            const char* actions[] = { "off", "off, on next block", "off, on 250 ms later", "device restart", "off, on 250 ms later + B starts" };
            juce::StringArray failures;
            int scenarios = 0;
            double worstOver = -100.0;

            for (const int blockSize : { 64, 480, 4096 })
                for (const double micLufs : { -6.0, -13.0, -28.0 })   // the master's hand about -10, -3, +12
                    for (const auto& pair : pairs)
                        for (int action = 0; action < 5; ++action)
                        {
                            ++scenarios;
                            AudioEngine engine (0);
                            engine.prepare (rate, blockSize);
                            engine.setAutoLevel (true, -16.0);
                            const auto mic = micCueFor();
                            const auto cueA = routed (pair.a, 0), cueB = routed (pair.b, 1);
                            engine.setLoudnessMatchDb (cueA.id, pair.matchDb);
                            engine.setLoudnessMatchDb (cueB.id, pair.matchDb);
                            engine.setLoudnessMatchActive (true);
                            juce::AudioBuffer<float> input (2, blockSize), out (2, blockSize);
                            Signal loud;
                            std::vector<float> left, right;
                            const auto render = [&] (double seconds, bool micOn, bool keep)
                            {
                                const int blocks = juce::jmax (1, (int) std::ceil (seconds * rate / blockSize));
                                for (int b = 0; b < blocks; ++b)
                                {
                                    if (micOn)
                                        loud.fill (input, micLufs);
                                    else
                                        input.clear();
                                    engine.renderBlock (out, blockSize, input.getArrayOfReadPointers(), 2);
                                    if (keep)
                                    {
                                        left.insert (left.end(), out.getReadPointer (0), out.getReadPointer (0) + blockSize);
                                        right.insert (right.end(), out.getReadPointer (1), out.getReadPointer (1) + blockSize);
                                    }
                                }
                            };
                            // the main component's order: the matches, then the master's switch
                            const auto switchOff = [&] { engine.setLoudnessMatchActive (false); engine.setAutoLevel (false, -16.0); };
                            const auto switchOn = [&] { engine.setLoudnessMatchActive (true); engine.setAutoLevel (true, -16.0); };

                            engine.play (mic);
                            render (6.0, true, false);
                            const double hand = engine.getAutoLevelGainDb();
                            engine.stop (mic.id);
                            engine.play (cueA);
                            render (0.6, false, true);
                            const size_t at = left.size();
                            size_t bAt = 0;

                            switch (action)
                            {
                                case 0: switchOff(); render (2.0, false, true); break;
                                case 1: switchOff(); render (0.001, false, true); switchOn(); render (2.0, false, true); break;
                                case 2: switchOff(); render (0.25, false, true); switchOn(); render (2.0, false, true); break;
                                case 3: engine.prepare (rate, blockSize); render (2.0, false, true); break;
                                default: switchOff(); render (0.25, false, true); switchOn(); bAt = right.size(); engine.play (cueB);
                                         render (2.0, false, true); break;
                            }

                            const size_t w = 1920;   // 40 ms windows, 10 ms apart
                            const double before = samplesDb (left, at - w, w, pair.refA);
                            double afterMax = -200.0, afterMin = 200.0;
                            for (size_t s = at; s + w <= left.size(); s += 480)
                            {
                                const double g = samplesDb (left, s, w, pair.refA);
                                afterMax = juce::jmax (afterMax, g);
                                afterMin = juce::jmin (afterMin, g);
                            }
                            const double end = samplesDb (left, left.size() - 4800, 4800, pair.refA);   // the last 100 ms
                            const double limitA = action == 3 ? before + 0.5 : juce::jmax (before, 0.0) + 0.5;
                            const double endWanted = action == 0 ? 0.0 : pair.matchDb;
                            const double endTolerance = action == 0 || action == 3 ? 0.3 : (action == 4 ? 3.5 : 0.6);   // two cues: the master pulls the 3 dB louder mix down
                            const auto where = juce::String (blockSize) + " samples, hand " + juce::String (hand, 1) + " dB, match "
                                               + juce::String (pair.matchDb, 0) + ", " + actions[action] + ": ";
                            worstOver = juce::jmax (worstOver, afterMax - limitA);

                            if (afterMax > limitA)
                                failures.add (where + "A " + juce::String (afterMax, 2) + " dB over a bound of " + juce::String (limitA, 2));
                            // nor a dip on the way: no lower than where it was, where it ends, or - while switched off - the
                            // file itself, less the fader's cut if it was down: such a fader rises only once no matched sound
                            // is left on its way (with B, the master also pulls the 3 dB louder mix down by that much)
                            const double offLevel = action == 3 ? before : juce::jmin (0.0, hand);
                            const double floorA = juce::jmin (juce::jmin (before, endWanted), offLevel) - (action == 4 ? 4.0 : 1.0);
                            if (afterMin < floorA)
                                failures.add (where + "A dips to " + juce::String (afterMin, 2) + " dB under a floor of " + juce::String (floorA, 2));
                            if (std::abs (end - endWanted) > endTolerance)
                                failures.add (where + "A ends at " + juce::String (end, 2) + " dB, not " + juce::String (endWanted, 1));

                            if (action == 4)
                            {
                                double bMax = -200.0;
                                for (size_t s = bAt; s + w <= right.size(); s += 480)
                                    bMax = juce::jmax (bMax, samplesDb (right, s, w, pair.refB));
                                const double bStart = samplesDb (right, bAt + 4800, 9600, pair.refB);   // 100 .. 300 ms
                                worstOver = juce::jmax (worstOver, bMax - (pair.matchDb + 0.5));
                                if (bMax > pair.matchDb + 0.5)
                                    failures.add (where + "B " + juce::String (bMax, 2) + " dB over its match");
                                if (std::abs (bStart - pair.matchDb) > 0.5)
                                    failures.add (where + "B starts at " + juce::String (bStart, 2) + " dB, not its match");
                            }
                        }

            logMessage (juce::String (scenarios) + " scenarios, worst margin over the bound " + juce::String (worstOver, 2) + " dB, "
                        + juce::String (failures.size()) + " failing");
            for (int i = 0; i < juce::jmin (12, failures.size()); ++i)
                logMessage ("  " + failures[i]);
            expectEquals (failures.size(), 0, "no matched cue louder than its match or than before");
        }

        beginTest ("an insert with latency after the cues: no switch-off or restart plays a cue louder than before");
        {
            juce::StringArray failures;

            for (const double matchDb : { 10.0, 0.0, -6.0 })   // the file at the level the match brings to -16
            for (const double micLufs : { -6.0, -28.0 })       // the master's hand down (-10) or up (+12)
                for (const bool restart : { false, true })
                {
                    const auto file = writeTone (scratch.folder.getChildFile ("latent" + juce::String ((int) matchDb) + ".wav"),
                                                 { { 30.0, -16.0 - matchDb } });
                    double reference = 0.0;
                    {
                        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                        juce::AudioBuffer<float> all ((int) reader->numChannels, (int) reader->lengthInSamples);
                        reader->read (&all, 0, (int) reader->lengthInSamples, 0, true, true);
                        reference = all.getRMSLevel (0, 0, all.getNumSamples());
                    }
                    AudioEngine engine (0);
                    engine.prepare (rate, block);
                    auto latent = std::make_unique<TestGainPlugin> (1.0f);
                    latent->latencySamples = 4800;              // 100 ms: a linear-phase EQ, say
                    engine.getMasterChain().addPlugin (std::move (latent));
                    engine.setAutoLevel (true, -16.0);
                    const auto mic = micCueFor();
                    const auto cue = audioCueFor (file, 30.0);
                    engine.setLoudnessMatchDb (cue.id, matchDb);
                    engine.setLoudnessMatchActive (true);
                    juce::AudioBuffer<float> input (2, block), out (2, block);
                    Signal loud;
                    std::vector<float> left;
                    const auto render = [&] (int blocks, bool micOn, bool keep)
                    {
                        for (int b = 0; b < blocks; ++b)
                        {
                            if (micOn)
                                loud.fill (input, micLufs);
                            else
                                input.clear();
                            engine.renderBlock (out, block, input.getArrayOfReadPointers(), 2);
                            if (keep)
                                left.insert (left.end(), out.getReadPointer (0), out.getReadPointer (0) + block);
                        }
                    };
                    engine.play (mic);
                    render (600, true, false);
                    const double hand = engine.getAutoLevelGainDb();
                    engine.stop (mic.id);
                    engine.play (cue);
                    render (80, false, true);                    // 0.8 s: the cue at its match, through the insert
                    const size_t at = left.size();

                    if (restart)
                    {
                        engine.prepare (rate, block);            // the device opened again, same format
                    }
                    else
                    {
                        engine.setLoudnessMatchActive (false);
                        engine.setAutoLevel (false, -16.0);
                    }

                    render (300, false, true);
                    const double before = samplesDb (left, at - 1920, 1920, reference);
                    double afterMax = -200.0, afterMin = 200.0;
                    for (size_t s = at; s + 1920 <= left.size(); s += 480)
                    {
                        const double g = samplesDb (left, s, 1920, reference);
                        afterMax = juce::jmax (afterMax, g);
                        afterMin = juce::jmin (afterMin, g);
                    }
                    const double end = samplesDb (left, left.size() - 4800, 4800, reference);
                    const double bound = restart ? before + 0.5 : juce::jmax (before, 0.0) + 0.5;
                    // the safe side's price: a master that comes down at once while a cue's match going up still crosses the
                    // insert leaves that cue up to 40 dB/s x the latency (4 dB here) quieter for that moment - waiting with the
                    // master as well would make a shorter path louder instead
                    // ... and a fader that was down rises only once no matched sound is left in the insert: the cue dips by
                    // its cut for that moment
                    const double floor = (restart ? before : juce::jmin (juce::jmin (before, 0.0), hand)) - 1.0 - (restart ? 0.0 : 40.0 * 0.1);
                    const auto where = juce::String ("match ") + juce::String (matchDb, 0) + ", hand " + juce::String (hand, 1)
                                       + (restart ? " dB, restart: " : " dB, switch-off: ");
                    logMessage (where + "before " + juce::String (before, 2) + ", after " + juce::String (afterMin, 2) + " .. "
                                + juce::String (afterMax, 2) + ", end " + juce::String (end, 2));
                    if (afterMax > bound)
                        failures.add (where + juce::String (afterMax, 2) + " dB over a bound of " + juce::String (bound, 2));
                    if (afterMin < floor)
                        failures.add (where + "dips to " + juce::String (afterMin, 2) + " under " + juce::String (floor, 2));
                    if (std::abs (end - (restart ? before : 0.0)) > 0.5)
                        failures.add (where + "ends at " + juce::String (end, 2));
                }

            expectEquals (failures.size(), 0, failures.joinIntoString ("; "));
        }

        beginTest ("off and on again between two blocks with the cue's match gone: it goes to the file, never toward silence");
        {
            const auto file = writeTone (scratch.folder.getChildFile ("between.wav"), { { 20.0, -26.0 } });
            const auto cue = audioCueFor (file, 20.0);
            double reference = 0.0;
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                juce::AudioBuffer<float> all ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&all, 0, (int) reader->lengthInSamples, 0, true, true);
                reference = all.getRMSLevel (0, 0, all.getNumSamples());
            }
            AudioEngine engine (0);
            engine.prepare (rate, block);
            engine.setLoudnessMatchDb (cue.id, 10.0);
            engine.setLoudnessMatchActive (true);
            engine.setAutoLevel (true, -16.0);
            expect (engine.play (cue));
            juce::AudioBuffer<float> out (2, block);
            std::vector<float> left;
            const auto render = [&] (int blocks)
            {
                for (int b = 0; b < blocks; ++b)
                {
                    engine.renderBlock (out, block, nullptr, 0);
                    left.insert (left.end(), out.getReadPointer (0), out.getReadPointer (0) + block);
                }
            };
            render (50);
            // its slices changed (no longer matchable) and auto level went off and on again before the next block
            engine.setLoudnessMatchActive (false);
            engine.setAutoLevel (false, -16.0);
            engine.clearLoudnessMatches();
            engine.setLoudnessMatchActive (true);
            engine.setAutoLevel (true, -16.0);
            const size_t at = left.size();
            render (40);
            double lowest = 100.0;
            for (size_t s = at; s + 1920 <= left.size(); s += 480)
                lowest = juce::jmin (lowest, samplesDb (left, s, 1920, reference));
            const double settled = samplesDb (left, at + 14400, 4800, reference);   // 300 .. 400 ms after
            logMessage ("after the quick off and on: lowest " + juce::String (lowest, 2) + " dB, at 0.3 s " + juce::String (settled, 2) + " dB from the file");
            expect (lowest > -1.0, "never toward silence");
            expectWithinAbsoluteError (settled, 0.0, 1.0, "the file as it is: the match it no longer has is gone");
        }

        beginTest ("a file changed while another region of it is measured: that region's result from before is not published");
        {
            LoudnessScan scan (formats, scratch.folder.getChildFile ("inflight-cache.json"));
            scan.setRecheckInterval (100, 100);
            // loud first, saved over with a quiet version: a read of both (5 s of the loud one, the rest quiet) comes out
            // near -22.5, the file as it now is at -26
            const auto file = writeTone (scratch.folder.getChildFile ("inflight.wav"), { { 60.0, -14.0 } });
            const auto quieter = writeTone (scratch.folder.getChildFile ("inflight-quieter.wav"), { { 60.0, -26.0 } });   // same length, same header
            juce::MemoryBlock quieterBytes;
            quieter.loadFileAsData (quieterBytes);
            expect (awaitResult (scan, file, 0.0, 4.0).has_value());     // region A answered
            scan.setBusy (true);                                        // region B, the whole minute: about 6 s
            scan.lookup (file, 0.0, -1.0);
            juce::Thread::sleep (500);                                  // B in flight
            {
                juce::FileOutputStream over (file);                    // saved over in place while B reads it: B sees old, then new
                expect (over.openedOk(), "the file can be changed while it is read");
                over.setPosition (0);
                over.write (quieterBytes.getData(), quieterBytes.getSize());
            }
            for (int i = 0; i < 300 && scan.lookup (file, 0.0, 4.0).has_value(); ++i)
                juce::Thread::sleep (10);                               // the recheck withdraws A ...
            scan.setRecheckInterval (60000, 60000);                     // ... and no more rounds: B answers with what B publishes
            std::optional<LoudnessScanResult> first;
            for (int i = 0; i < 3000 && ! first; ++i)
            {
                first = scan.lookup (file, 0.0, -1.0);
                juce::Thread::sleep (5);
            }
            logMessage ("B's first answer: " + (first ? juce::String (first->integratedLufs, 2) : juce::String ("none")) + " LUFS (the file now -26)");
            expect (first.has_value() && std::abs (first->integratedLufs + 26.0) < 0.05, "B's answer is the changed file's, never a read of both");
            scan.setBusy (false);
        }

        beginTest ("a looping marker in the region's last millisecond: not matched, as the cue really plays it (sample by sample)");
        {
            const auto file = writeTone (scratch.folder.getChildFile ("lastms.wav"), { { 12.0, -26.0 } });
            auto cue = audioCueFor (file, 12.0);
            cue.audio.endSeconds = 10.0005;
            cue.audio.slices.push_back ({ 10.0, -1 });                  // the last 24 samples loop forever
            double reference = 0.0;
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                juce::AudioBuffer<float> all ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&all, 0, (int) reader->lengthInSamples, 0, true, true);
                reference = all.getRMSLevel (0, 0, all.getNumSamples());
            }
            AudioEngine engine (0);
            engine.prepare (rate, block);
            engine.setLoudnessMatchDb (cue.id, 10.0);
            engine.setLoudnessMatchActive (true);
            engine.setAutoLevel (true, -16.0);
            expect (engine.play (cue));
            juce::AudioBuffer<float> out (2, block);
            std::vector<float> left;
            for (int b = 0; b < 30; ++b)
            {
                engine.renderBlock (out, block, nullptr, 0);
                left.insert (left.end(), out.getReadPointer (0), out.getReadPointer (0) + block);
            }
            const double gain = samplesDb (left, 4800, 9600, reference);   // 100 .. 300 ms
            logMessage ("a cue that loops its last 24 samples starts " + juce::String (gain, 2) + " dB from the file");
            expectWithinAbsoluteError (gain, 0.0, 0.3, "no match: what it plays is not the region measured");
        }

        beginTest ("switched off as the device restarts: the limiter stays until the match is gone, never over the file's own peak");
        {
            // a quiet tone with clicks at -0.9 dBFS every 100 ms: a match of +3 puts the clicks over full scale, which only
            // the master's limiter may take
            const auto clicks = scratch.folder.getChildFile ("clicks.wav");
            {
                clicks.deleteFile();
                std::unique_ptr<juce::OutputStream> stream (clicks.createOutputStream());
                juce::WavAudioFormat wav;
                auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
                Signal tone;
                juce::AudioBuffer<float> buffer (2, block);
                for (int b = 0; b < 600; ++b)
                {
                    tone.fill (buffer, -30.0);
                    if (b % 10 == 5)
                        for (int ch = 0; ch < 2; ++ch)
                            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch, 100), 0.9f, 48);
                    writer->writeFromAudioSampleBuffer (buffer, 0, block);
                }
            }
            AudioEngine engine (0);
            engine.prepare (rate, block);
            engine.setAutoLevel (true, -16.0);
            const auto cue = audioCueFor (clicks, 6.0);
            engine.setLoudnessMatchDb (cue.id, 3.0);
            engine.setLoudnessMatchActive (true);
            expect (engine.play (cue));
            juce::AudioBuffer<float> out (2, block);
            for (int b = 0; b < 30; ++b)
                engine.renderBlock (out, block, nullptr, 0);    // 0.3 s: before the leveler's first decision
            engine.setLoudnessMatchActive (false);
            engine.setAutoLevel (false, -16.0);
            engine.prepare (rate, block);                        // the device restarts at that moment
            float loudest = 0.0f;
            for (int b = 0; b < 100; ++b)
            {
                engine.renderBlock (out, block, nullptr, 0);
                if (b >= 3)                                      // after the master's 20 ms fade-in
                    loudest = juce::jmax (loudest, out.getMagnitude (0, block));
            }
            logMessage ("switched off as the device restarted: loudest sample " + juce::String (juce::Decibels::gainToDecibels (loudest), 2) + " dBFS");
            expect (loudest <= 0.9f + 1.0e-4f, "never over the file's own clicks (-0.9 dBFS)");
        }

        beginTest ("switched off with a latent insert: the limiter stays until the last matched sound has left the insert");
        {
            const auto clicks = writeClicks (scratch.folder.getChildFile ("latent-clicks.wav"), 6.0, 10);
            AudioEngine engine (0);
            engine.prepare (rate, block);
            auto latent = std::make_unique<TestGainPlugin> (1.0f);
            latent->latencySamples = 4800;                       // 100 ms
            engine.getMasterChain().addPlugin (std::move (latent));
            engine.setAutoLevel (true, -16.0);
            const auto cue = audioCueFor (clicks, 6.0);
            engine.setLoudnessMatchDb (cue.id, 3.0);
            engine.setLoudnessMatchActive (true);
            expect (engine.play (cue));
            juce::AudioBuffer<float> out (2, block);
            for (int b = 0; b < 30; ++b)
                engine.renderBlock (out, block, nullptr, 0);     // 0.3 s: the master at rest, the clicks limited
            engine.setLoudnessMatchActive (false);
            engine.setAutoLevel (false, -16.0);
            float loudest = 0.0f;
            for (int b = 0; b < 150; ++b)
            {
                engine.renderBlock (out, block, nullptr, 0);
                loudest = juce::jmax (loudest, out.getMagnitude (0, block));
            }
            logMessage ("switched off through a 100 ms insert: loudest sample " + juce::String (juce::Decibels::gainToDecibels (loudest), 2) + " dBFS");
            expect (loudest <= 0.9f + 1.0e-4f, "never over the file's own clicks (-0.9 dBFS)");
        }

        beginTest ("an insert's latency grows during the switch-off: no cue louder than before the switch");
        {
            const auto file = writeTone (scratch.folder.getChildFile ("growing.wav"), { { 30.0, -26.0 } });
            double reference = 0.0;
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                juce::AudioBuffer<float> all ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&all, 0, (int) reader->lengthInSamples, 0, true, true);
                reference = all.getRMSLevel (0, 0, all.getNumSamples());
            }
            AudioEngine engine (0);
            engine.prepare (rate, block);
            auto plugin = std::make_unique<TestGainPlugin> (1.0f);   // no latency yet
            auto* insert = plugin.get();
            engine.getMasterChain().addPlugin (std::move (plugin));
            engine.setAutoLevel (true, -16.0);
            const auto mic = micCueFor();
            const auto cue = audioCueFor (file, 30.0);
            engine.setLoudnessMatchDb (cue.id, 10.0);
            engine.setLoudnessMatchActive (true);
            juce::AudioBuffer<float> input (2, block), out (2, block);
            Signal loud;
            std::vector<float> left;
            const auto render = [&] (int blocks, bool micOn, bool keep)
            {
                for (int b = 0; b < blocks; ++b)
                {
                    if (micOn)
                        loud.fill (input, -6.0);
                    else
                        input.clear();
                    engine.renderBlock (out, block, input.getArrayOfReadPointers(), 2);
                    if (keep)
                        left.insert (left.end(), out.getReadPointer (0), out.getReadPointer (0) + block);
                }
            };
            engine.play (mic);
            render (600, true, false);                           // the master cuts a loud mic (-10)
            engine.stop (mic.id);
            engine.play (cue);
            render (80, false, true);
            const size_t at = left.size();
            engine.setLoudnessMatchActive (false);
            engine.setAutoLevel (false, -16.0);
            render (5, false, true);                             // 50 ms into the way home ...
            insert->setLatencyLive (4800);                       // ... the insert's look-ahead grows to 100 ms
            engine.consumePluginStateChanges();                  // (the message thread hears of it)
            render (250, false, true);
            const double before = samplesDb (left, at - 1920, 1920, reference);
            double loudest = -200.0;
            for (size_t s = at; s + 1920 <= left.size(); s += 480)
                loudest = juce::jmax (loudest, samplesDb (left, s, 1920, reference));
            logMessage ("the latency grew during the switch-off: before " + juce::String (before, 2) + " dB, loudest after " + juce::String (loudest, 2));
            expect (loudest <= before + 0.5, "no cue louder than before the switch");
        }

        beginTest ("a restart with a new format: the matched cues' clicks are limited from the first sample");
        {
            const auto clicks = writeClicks (scratch.folder.getChildFile ("restart-clicks.wav"), 6.0, 1);   // a click every 10 ms
            AudioEngine engine (0);
            engine.prepare (rate, block);
            engine.setAutoLevel (true, -16.0);
            const auto cue = audioCueFor (clicks, 6.0);
            engine.setLoudnessMatchDb (cue.id, 3.0);
            engine.setLoudnessMatchActive (true);
            expect (engine.play (cue));
            juce::AudioBuffer<float> out (2, block);
            for (int b = 0; b < 30; ++b)
                engine.renderBlock (out, block, nullptr, 0);
            engine.prepare (rate, 960);                           // another block size: everything prepared again
            juce::AudioBuffer<float> wide (2, 960);
            float loudest = 0.0f;
            for (int b = 0; b < 30; ++b)
            {
                engine.renderBlock (wide, 960, nullptr, 0);
                loudest = juce::jmax (loudest, wide.getMagnitude (0, 960));
            }
            logMessage ("after a restart with a new block size: loudest sample " + juce::String (juce::Decibels::gainToDecibels (loudest), 2) + " dBFS");
            expect (loudest <= 0.9f + 1.0e-4f, "never over the file's own clicks, from the first sample");
        }

        beginTest ("a cue that repeats its whole region (play count 2, or looping) keeps its match");
        {
            const auto file = writeTone (scratch.folder.getChildFile ("repeats.wav"), { { 4.0, -26.0 } });
            double reference = 0.0;
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                juce::AudioBuffer<float> all ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&all, 0, (int) reader->lengthInSamples, 0, true, true);
                reference = all.getRMSLevel (0, 0, all.getNumSamples());
            }
            for (const bool forever : { false, true })
            {
                auto cue = audioCueFor (file, 4.0);
                cue.audio.playCount = 2;
                cue.audio.infiniteLoop = forever;
                AudioEngine engine (0);
                engine.prepare (rate, block);
                engine.setLoudnessMatchDb (cue.id, 10.0);
                engine.setLoudnessMatchActive (true);
                engine.setAutoLevel (true, -16.0);
                expect (engine.play (cue));
                juce::AudioBuffer<float> out (2, block);
                std::vector<float> left;
                for (int b = 0; b < 30; ++b)
                {
                    engine.renderBlock (out, block, nullptr, 0);
                    left.insert (left.end(), out.getReadPointer (0), out.getReadPointer (0) + block);
                }
                const double gain = samplesDb (left, 4800, 9600, reference);
                logMessage (juce::String (forever ? "looping" : "played twice") + ": starts " + juce::String (gain, 2) + " dB from the file");
                expectWithinAbsoluteError (gain, 10.0, 0.3, "the whole region again is the same sound: matched");
            }
        }

        beginTest ("a cue that ended just before the switch-off: what it left in a latent insert stays limited");
        {
            const auto clicks = writeClicks (scratch.folder.getChildFile ("ended-clicks.wav"), 1.0, 10);

            for (const bool stopped : { false, true })
            {
                AudioEngine engine (0);
                engine.prepare (rate, block);
                auto latent = std::make_unique<TestGainPlugin> (1.0f);
                latent->latencySamples = 24000;                      // 500 ms
                engine.getMasterChain().addPlugin (std::move (latent));
                engine.setAutoLevel (true, -16.0);
                const auto cue = audioCueFor (clicks, 1.0);
                engine.setLoudnessMatchDb (cue.id, 3.0);
                engine.setLoudnessMatchActive (true);
                expect (engine.play (cue));
                juce::AudioBuffer<float> out (2, block);
                for (int b = 0; b < (stopped ? 70 : 105); ++b)
                    engine.renderBlock (out, block, nullptr, 0);
                if (stopped)
                {
                    engine.stop (cue.id);
                    for (int b = 0; b < 20; ++b)
                        engine.renderBlock (out, block, nullptr, 0);
                }
                expect (! engine.isPlaying (cue.id), "the cue has ended before the switch-off");
                engine.setLoudnessMatchActive (false);
                engine.setAutoLevel (false, -16.0);
                float loudest = 0.0f;
                for (int b = 0; b < 150; ++b)
                {
                    engine.renderBlock (out, block, nullptr, 0);
                    loudest = juce::jmax (loudest, out.getMagnitude (0, block));
                }
                logMessage (juce::String (stopped ? "stopped" : "ended") + " just before the switch-off, 500 ms insert: loudest sample "
                            + juce::String (juce::Decibels::gainToDecibels (loudest), 2) + " dBFS");
                expect (loudest <= 0.9f + 1.0e-4f, "never over the file's own clicks (-0.9 dBFS)");
            }
        }

        beginTest ("the inserts' delay shrinks during the switch-off, or rings past what they report: no cue louder than before");
        {
            const auto file = writeTone (scratch.folder.getChildFile ("ringing.wav"), { { 30.0, -26.0 } });
            double reference = 0.0;
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                juce::AudioBuffer<float> all ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&all, 0, (int) reader->lengthInSamples, 0, true, true);
                reference = all.getRMSLevel (0, 0, all.getNumSamples());
            }

            for (const bool echo : { false, true })
            {
                AudioEngine engine (0);
                engine.prepare (rate, block);
                TestGainPlugin* first = nullptr;

                if (echo)
                {
                    // a 100 % wet echo: one repeat a second later, told as a 1 s tail and no latency
                    auto plugin = std::make_unique<TestGainPlugin> (1.0f, 1.0);
                    plugin->latencySamples = 48000;
                    plugin->reportLatency = false;
                    engine.getMasterChain().addPlugin (std::move (plugin));
                }
                else
                {
                    // two look-ahead inserts in series, 1 s each: the first one's goes to 0 during the switch-off
                    for (int i = 0; i < 2; ++i)
                    {
                        auto plugin = std::make_unique<TestGainPlugin> (1.0f);
                        plugin->latencySamples = 48000;
                        if (i == 0)
                            first = plugin.get();
                        engine.getMasterChain().addPlugin (std::move (plugin));
                    }
                }

                engine.setAutoLevel (true, -16.0);
                const auto mic = micCueFor();
                const auto cue = audioCueFor (file, 30.0);
                engine.setLoudnessMatchDb (cue.id, 10.0);
                engine.setLoudnessMatchActive (true);
                juce::AudioBuffer<float> input (2, block), out (2, block);
                Signal loud;
                std::vector<float> left;
                const auto render = [&] (int blocks, bool micOn, bool keep)
                {
                    for (int b = 0; b < blocks; ++b)
                    {
                        if (micOn)
                            loud.fill (input, -6.0);
                        else
                            input.clear();
                        engine.renderBlock (out, block, input.getArrayOfReadPointers(), 2);
                        if (keep)
                            left.insert (left.end(), out.getReadPointer (0), out.getReadPointer (0) + block);
                    }
                };
                engine.play (mic);
                render (800, true, false);                           // the master cuts a loud mic (-10), heard through the inserts
                engine.stop (mic.id);
                engine.play (cue);
                render (300, false, true);                           // the matched cue through the inserts, steady
                const size_t at = left.size();
                engine.setLoudnessMatchActive (false);
                engine.setAutoLevel (false, -16.0);
                render (130, false, true);                           // the match back at 0 dB (0.5 s), 0.8 s more ...
                if (first != nullptr)
                {
                    first->setLatencyLive (0);                       // ... and the first insert's look-ahead goes
                    engine.consumePluginStateChanges();
                }
                render (400, false, true);
                const double before = samplesDb (left, at - 1920, 1920, reference);
                double loudest = -200.0;
                for (size_t s = at; s + 1920 <= left.size(); s += 480)
                    loudest = juce::jmax (loudest, samplesDb (left, s, 1920, reference));
                logMessage (juce::String (echo ? "an echo told as a tail" : "a delay that shrank") + " during the switch-off: before "
                            + juce::String (before, 2) + " dB, loudest after " + juce::String (loudest, 2));
                expect (loudest <= before + 0.5, "no cue louder than before the switch");
            }
        }

        beginTest ("a switch and a GO between a block's switch and its cues: the cue starts as its first block's switch says");
        {
            const auto file = writeTone (scratch.folder.getChildFile ("together.wav"), { { 6.0, -26.0 } });
            double reference = 0.0;
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                juce::AudioBuffer<float> all ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&all, 0, (int) reader->lengthInSamples, 0, true, true);
                reference = all.getRMSLevel (0, 0, all.getNumSamples());
            }

            for (const bool switchOn : { true, false })
            {
                AudioEngine engine (0);
                engine.prepare (rate, block);
                const auto cue = audioCueFor (file, 6.0);
                engine.setLoudnessMatchDb (cue.id, switchOn ? -12.0 : 10.0);
                if (! switchOn)
                {
                    engine.setLoudnessMatchActive (true);
                    engine.setAutoLevel (true, -16.0);
                }
                juce::AudioBuffer<float> out (2, block);
                for (int b = 0; b < 10; ++b)
                    engine.renderBlock (out, block, nullptr, 0);
                bool fired = false;
                engine.onBlockStartForTests = [&]
                {
                    if (std::exchange (fired, true))
                        return;
                    engine.setLoudnessMatchActive (switchOn);    // the box ticked (or cleared) ...
                    engine.setAutoLevel (switchOn, -16.0);
                    engine.play (cue);                          // ... and a GO in the same moment
                };
                std::vector<float> left;
                for (int b = 0; b < 30; ++b)
                {
                    engine.renderBlock (out, block, nullptr, 0);
                    left.insert (left.end(), out.getReadPointer (0), out.getReadPointer (0) + block);
                }
                engine.onBlockStartForTests = nullptr;
                const double early = samplesDb (left, (size_t) (3 * block), (size_t) (3 * block), reference);   // 30-60 ms
                const double wanted = switchOn ? -12.0 : 0.0;
                logMessage (juce::String (switchOn ? "switched on" : "switched off") + " with a GO: 30-60 ms at " + juce::String (early, 2)
                            + " dB from the file (wanted " + juce::String (wanted, 0) + ")");
                expectWithinAbsoluteError (early, wanted, 0.5, "the cue starts as the switch of its first block says");
            }
        }

        beginTest ("a GO right after a restart with a new format, or as the switch goes on: its clicks limited from the first sample");
        {
            const auto clicks = writeClicks (scratch.folder.getChildFile ("go-clicks.wav"), 6.0, 1);   // a click every 10 ms

            for (const bool restart : { true, false })
            {
                AudioEngine engine (0);
                engine.prepare (rate, block);
                const auto cue = audioCueFor (clicks, 6.0);
                engine.setLoudnessMatchDb (cue.id, 3.0);
                if (restart)
                {
                    engine.setLoudnessMatchActive (true);
                    engine.setAutoLevel (true, -16.0);
                }
                juce::AudioBuffer<float> out (2, block);
                for (int b = 0; b < 30; ++b)
                    engine.renderBlock (out, block, nullptr, 0);   // nothing playing yet
                if (restart)
                {
                    engine.prepare (rate, 960);                     // another block size: everything prepared again ...
                }
                else
                {
                    engine.setLoudnessMatchActive (true);           // ... or the box ticked ...
                    engine.setAutoLevel (true, -16.0);
                }
                expect (engine.play (cue));                         // ... and a GO before the next block
                const int size = restart ? 960 : block;
                juce::AudioBuffer<float> next (2, size);
                float loudest = 0.0f;
                for (int b = 0; b < 30; ++b)
                {
                    engine.renderBlock (next, size, nullptr, 0);
                    loudest = juce::jmax (loudest, next.getMagnitude (0, size));
                }
                logMessage (juce::String (restart ? "a GO right after a restart" : "a GO as the switch goes on") + ": loudest sample "
                            + juce::String (juce::Decibels::gainToDecibels (loudest), 2) + " dBFS");
                expect (loudest <= 0.9f + 1.0e-4f, "never over the file's own clicks, from the first sample");
            }
        }

        beginTest ("a whole-region loop told to finish its pass keeps its match through an off and on");
        {
            const auto file = writeTone (scratch.folder.getChildFile ("devamp.wav"), { { 4.0, -26.0 } });
            double reference = 0.0;
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                juce::AudioBuffer<float> all ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&all, 0, (int) reader->lengthInSamples, 0, true, true);
                reference = all.getRMSLevel (0, 0, all.getNumSamples());
            }

            for (const bool stopAfter : { false, true })
            {
                for (const bool quick : { true, false })
                {
                    auto cue = audioCueFor (file, 4.0);
                    cue.audio.infiniteLoop = true;
                    AudioEngine engine (0);
                    engine.prepare (rate, block);
                    engine.setLoudnessMatchDb (cue.id, 10.0);
                    engine.setLoudnessMatchActive (true);
                    engine.setAutoLevel (true, -16.0);
                    expect (engine.play (cue));
                    juce::AudioBuffer<float> out (2, block);
                    for (int b = 0; b < 30; ++b)
                        engine.renderBlock (out, block, nullptr, 0);
                    expect (engine.finishCurrentPass (cue.id, stopAfter) > 0.0, "a loop pass to finish");
                    engine.setLoudnessMatchActive (false);
                    engine.setAutoLevel (false, -16.0);
                    if (! quick)
                        for (int b = 0; b < 60; ++b)
                            engine.renderBlock (out, block, nullptr, 0);
                    engine.setLoudnessMatchActive (true);
                    engine.setAutoLevel (true, -16.0);
                    std::vector<float> left;
                    for (int b = 0; b < 100; ++b)
                    {
                        engine.renderBlock (out, block, nullptr, 0);
                        left.insert (left.end(), out.getReadPointer (0), out.getReadPointer (0) + block);
                    }
                    const double gain = samplesDb (left, 48000 - 9600, 9600, reference);   // 0.8-1.0 s after it was back on
                    logMessage (juce::String (stopAfter ? "finishing with a stop" : "finishing") + (quick ? ", off and on at once" : ", off 0.6 s")
                                + ": " + juce::String (gain, 2) + " dB from the file");
                    expectWithinAbsoluteError (gain, 10.0, 0.5, "the same region pass after pass: still matched");
                }
            }
        }

        beginTest ("a file saved over while a long region is measured: its answer withdrawn without waiting for that region");
        {
            LoudnessScan scan (formats, scratch.folder.getChildFile ("long-cache.json"));
            scan.setRecheckInterval (100, 100);
            const auto answered = writeTone (scratch.folder.getChildFile ("answered.wav"), { { 4.0, -26.0 } });
            expect (awaitResult (scan, answered).has_value());
            const auto longOne = writeTone (scratch.folder.getChildFile ("long-region.wav"), { { 60.0, -20.0 } });
            scan.setBusy (true);                         // cues playing: 60 s takes about 6 s
            scan.lookup (longOne, 0.0, -1.0);
            juce::Thread::sleep (300);                   // well into it
            writeTone (answered, { { 5.0, -14.0 } });    // saved over
            bool withdrawn = false;
            for (int i = 0; i < 300 && ! withdrawn; ++i)
            {
                const auto now = scan.lookup (answered, 0.0, -1.0);
                withdrawn = ! now.has_value() || std::abs (now->integratedLufs + 14.0) < 0.05;
                juce::Thread::sleep (10);
            }
            const bool longDone = scan.lookup (longOne, 0.0, -1.0).has_value();
            expect (withdrawn, "the old answer withdrawn");
            expect (! longDone, "while the long region was still being measured");
            scan.setBusy (false);
        }

        beginTest ("app exit while a read does not return: what was measured is saved, the thread is not killed mid-read");
        {
            juce::WaitableEvent gate (true);
            juce::AudioFormatManager stuckFormats;
            stuckFormats.registerBasicFormats();
            stuckFormats.registerFormat (new StuckFormat (gate), false);
            const auto cache = scratch.folder.getChildFile ("exit-cache.json");
            const auto measuredFile = writeTone (scratch.folder.getChildFile ("exit-measured.wav"), { { 4.0, -20.0 } });
            const auto stuckFile = scratch.folder.getChildFile ("on-the-share.stuck");
            stuckFile.replaceWithText ("x");
            auto scan = std::make_unique<LoudnessScan> (stuckFormats, cache);
            scan->lookup (measuredFile, 0.0, -1.0);
            scan->lookup (stuckFile, 0.0, -1.0);         // next: a read that does not return
            for (int i = 0; i < 500 && ! scan->lookup (measuredFile, 0.0, -1.0).has_value(); ++i)
                juce::Thread::sleep (10);
            juce::Thread::sleep (200);                   // inside the stuck read by now
            expect (! cache.existsAsFile(), "nothing saved yet: the queue never ran dry");
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            const bool stopped = scan->stopForExit (300);
            const double waited = juce::Time::getMillisecondCounterHiRes() - t0;
            expect (! stopped, "still inside the read");
            expect (waited < 2000.0, "the exit does not wait for the share");
            expect (cache.loadFileAsString().contains ("exit-measured.wav"), "what was measured is saved");
            gate.signal();                               // the share answers again: the thread leaves by itself
            scan.reset();
        }

        beginTest ("app exit while a read does not return, the scan owning its formats: let go, it finishes on its own");
        {
            static juce::WaitableEvent gate (true);           // outlives the scan the test lets go, as the app lets it go
            auto own = std::make_unique<juce::AudioFormatManager>();
            own->registerFormat (new StuckFormat (gate), true);
            const auto stuckFile = scratch.folder.getChildFile ("owned-share.stuck");
            stuckFile.replaceWithText ("x");
            auto scan = std::make_unique<LoudnessScan> (std::move (own), scratch.folder.getChildFile ("owned-cache.json"));
            scan->lookup (stuckFile, 0.0, -1.0);
            juce::Thread::sleep (300);                         // inside the read that does not return
            expect (! scan->stopForExit (300), "still inside the read");
            static_cast<void> (scan.release());               // the main component lets it go: nothing it reads with goes away
            gate.signal();                                     // the share answers: the thread leaves, with its own formats
            juce::Thread::sleep (300);
            expect (stuckFile.exists());
        }

        beginTest ("the read-ahead after an edit: a length from before it never ends the refill's grace");
        {
            GatedSource upstream;
            upstream.length = 48000 * 5;                                   // a 5 s region ...
            juce::TimeSliceThread fillThread ("read-ahead fill");          // started only when the test wants a fill
            ReadAheadSource readAhead (upstream, fillThread, 48000, 2);
            readAhead.setNextReadPosition (48000 * 4 + 36000);
            readAhead.prepareToPlay (block, rate);                          // ... prefilled to its end
            juce::AudioBuffer<float> buffer (2, block);
            juce::AudioSourceChannelInfo info (&buffer, 0, block);
            for (int b = 0; b < 30; ++b)
                readAhead.getNextAudioBlock (info);                         // to its end and past it
            const int before = ReadAheadSource::getShortfallCount();
            upstream.length = 48000 * 15;                                   // the region grows at its start: the place jumps
            readAhead.invalidate (48000 * 14);
            readAhead.getNextAudioBlock (info);                             // nothing filled yet: the refill's grace
            upstream.gated = true;                                          // the next fill reads the new length, then the share stalls
            fillThread.startThread();
            upstream.entered.wait (5000);
            for (int b = 0; b < 5; ++b)
                readAhead.getNextAudioBlock (info);                         // the refill still on its way
            const int counted = ReadAheadSource::getShortfallCount() - before;
            upstream.gated = false;
            upstream.open.signal();
            readAhead.releaseResources();
            fillThread.stopThread (5000);
            expectEquals (counted, 0, "the refill after the edit is no shortfall");
        }

        beginTest ("the read-ahead counts the blocks it had not filled in time, not the refill after a jump");
        {
            ConstantSource upstream (48000 * 4);
            juce::TimeSliceThread idleThread ("never started");   // nothing fills the ring after the prefill
            ReadAheadSource readAhead (upstream, idleThread, 48000, 2);
            readAhead.prepareToPlay (block, rate);                // a quarter second prefilled on this thread
            juce::AudioBuffer<float> buffer (2, block);
            juce::AudioSourceChannelInfo info (&buffer, 0, block);
            const int before = ReadAheadSource::getShortfallCount();

            for (int b = 0; b < 25; ++b) readAhead.getNextAudioBlock (info);   // 0.25 s: all prefilled
            expectEquals (ReadAheadSource::getShortfallCount() - before, 0);

            for (int b = 0; b < 10; ++b) readAhead.getNextAudioBlock (info);   // past the prefill: starved
            expectEquals (ReadAheadSource::getShortfallCount() - before, 10);

            readAhead.setNextReadPosition (48000 * 3);                          // a jump: its refill gap is not counted
            const int atJump = ReadAheadSource::getShortfallCount();
            for (int b = 0; b < 5; ++b) readAhead.getNextAudioBlock (info);
            expectEquals (ReadAheadSource::getShortfallCount() - atJump, 0);
            readAhead.releaseResources();
        }

        beginTest ("the read-ahead: the end of the material is no shortfall");
        {
            ConstantSource upstream (9600);                       // 0.2 s
            juce::TimeSliceThread idleThread ("never started");
            ReadAheadSource readAhead (upstream, idleThread, 48000, 2);
            readAhead.prepareToPlay (block, rate);                // all of it prefilled
            juce::AudioBuffer<float> buffer (2, block);
            juce::AudioSourceChannelInfo info (&buffer, 0, block);
            const int before = ReadAheadSource::getShortfallCount();
            for (int b = 0; b < 40; ++b)
                readAhead.getNextAudioBlock (info);               // 0.4 s: past the end
            expectEquals (ReadAheadSource::getShortfallCount() - before, 0);
            readAhead.releaseResources();
        }
    }
};

static LoudnessScanTests loudnessScanTests;
} // namespace gocue::tests
