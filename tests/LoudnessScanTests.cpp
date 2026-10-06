#include "AutoLevelTestSupport.h"
#include "VolumeCueUiTestSupport.h"
#include "audio/AudioEngine.h"
#include "audio/LoudnessScan.h"
#include "audio/ReadAheadSource.h"
#include "audio/MediaFoundationAudioFormat.h"
#include "ui/AutoLevelDialog.h"
#include "MainComponentTestAccess.h"
#include <deque>

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
            Cue cue;
            cue.file = file;
            cue.durationSeconds = 12.0;
            cue.numChannels = 2;
            cue.levels.resize (2, 2);
            cue.levels.setDefaults();

            const auto firstBlocks = [&] (double matchDb, bool active, std::vector<float>& rms, int blocks, std::function<void (AudioEngine&, int)> during = {})
            {
                AudioEngine engine (0);
                engine.prepare (rate, block);
                engine.setLoudnessMatchDb (cue.id, matchDb);
                engine.setLoudnessMatchActive (active);
                expect (engine.play (cue));
                juce::AudioBuffer<float> out (2, block);
                for (int b = 0; b < blocks; ++b)
                {
                    if (during) during (engine, b);
                    engine.renderBlock (out, block, nullptr, 0);
                    rms.push_back (out.getRMSLevel (0, 0, block));
                }
            };

            std::vector<float> plain, matched, inactive;
            firstBlocks (0.0, true, plain, 100);
            firstBlocks (10.0, true, matched, 20);
            firstBlocks (10.0, false, inactive, 20);
            const double firstGain = 20.0 * std::log10 ((double) matched[1] / (double) plain[1]);
            logMessage ("second block, matched +10 vs plain: " + juce::String (firstGain, 3) + " dB");
            expectWithinAbsoluteError (firstGain, 10.0, 0.05);
            expectWithinAbsoluteError ((double) inactive[1], (double) plain[1], 1.0e-6, "matching off: the file as it is");

            // switched off while playing: back to the file over a quarter second, at most 40 dB/s
            std::vector<float> off;
            firstBlocks (10.0, true, off, 100, [] (AudioEngine& e, int b) { if (b == 20) e.setLoudnessMatchActive (false); });
            const auto gainAt = [&plain] (const std::vector<float>& run, size_t b) { return 20.0 * std::log10 ((double) run[b] / (double) plain[b]); };
            double steepest = 0.0;
            for (size_t b = 21; b < off.size(); ++b)
                steepest = juce::jmax (steepest, std::abs (gainAt (off, b) - gainAt (off, b - 1)) * 100.0);
            const double endGain = gainAt (off, off.size() - 1);
            logMessage ("switched off: steepest " + juce::String (steepest, 2) + " dB/s, then " + juce::String (endGain, 3) + " dB from the plain file");
            expect (steepest <= 42.0, "no step: 40 dB/s, read through blocks of a linear ramp");
            expectWithinAbsoluteError (endGain, 0.0, 0.05);

            // a new measurement while it plays is for its next start: two hands on one fader would overshoot
            std::vector<float> kept;
            firstBlocks (10.0, true, kept, 60, [&] (AudioEngine& e, int b) { if (b == 20) e.setLoudnessMatchDb (cue.id, 0.0); });
            expectWithinAbsoluteError (gainAt (kept, kept.size() - 1), 10.0, 0.05);
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
            for (const auto& c : { plain, skipping, repeating })
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
            expectEquals (counts[0], 1, "only the plain cue is one to match");
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
