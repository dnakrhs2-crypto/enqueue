#include "AutoLevelTestSupport.h"

#include <limits>

namespace gocue::tests
{
using namespace auto_level;

class AutoLevelTests : public juce::UnitTest
{
public:
    AutoLevelTests() : UnitTest ("AutoLevel DSP", "Enqueue") {}
    void runTest() override
    {
        beginTest ("1. convergence at 20 s, measured with independent K weighting");
        Rig ride;
        ride.feed (20, -26);
        metric ("20 s gain dB", ride.leveler.getGainDb());
        metric ("20 s output S LUFS", ride.output.lufs());
        expectWithinAbsoluteError (ride.leveler.getGainDb(), 10.0, 0.7);
        expectWithinAbsoluteError (ride.output.lufs(), -16.0, 0.7);

        beginTest ("2-3. 10 ms slopes, acceleration, and a gentle start in both directions");
        slopes (ride.gains, 2.55, 10.1);   // 10 dB too quiet: 2.5 dB/s
        Rig down;
        down.feed (20, -6);
        slopes (down.gains, 2.55, 10.1);
        gentleStart (ride.gains, 2.5, 2.0);   // a new song is judged after 2 s
        gentleStart (down.gains, 10.0, 0.1);  // 10 LU over the target: far too loud, 0.1 s
        Rig mild;
        mild.feed (20, -11);
        slopes (mild.gains, 2.55, 10.1);
        gentleStart (mild.gains, 4.0, 0.5);   // 5 LU over: the ordinary 0.5 s

        beginTest ("4. a 1 dB error stays in the 0.1 dB/s slow zone");
        Rig slow;
        slow.feed (30, -17);
        double fastest = 0;
        for (size_t i = 1; i < slow.gains.size(); ++i) fastest = juce::jmax (fastest, std::abs (slow.gains[i] - slow.gains[i - 1]) * 100);
        metric ("slow zone dB/s", fastest);
        expect (fastest <= 0.12);

        beginTest ("5. a 2 s dip cannot start an upward ride");
        Rig dip;
        dip.feed (30, -16);
        const auto beforeDip = dip.leveler.getGainDb();
        dip.feed (2, -26);
        expect (dip.leveler.getGainDb() - beforeDip <= 0.2);

        beginTest ("6. sustained +10 LU: under target+2 by 6 s, no undershoot below target-1");
        Rig loud;
        loud.feed (30, -16);
        loud.feed (6, -6);
        metric ("loud step output S at 6 s", loud.output.lufs());
        expect (loud.output.lufs() <= -14.0);
        double lowest = loud.output.lufs();
        for (int i = 0; i < 2400; ++i) { loud.step (-6); lowest = juce::jmin (lowest, loud.output.lufs()); }
        metric ("loud step lowest settled S", lowest);
        expect (lowest >= -17.0);
        slopes (loud.gains, 2.55, 10.1);

        beginTest ("7. flow preserves a 5 s quiet passage within the song");
        Rig passage;
        passage.feed (30, -16);
        const auto beforePassage = passage.leveler.getGainDb();
        passage.feed (5, -22);
        metric ("quiet passage gain increase", passage.leveler.getGainDb() - beforePassage);
        expect (passage.leveler.getGainDb() - beforePassage <= 1.5);

        beginTest ("8. 20 s silence and -70 LUFS noise do not lift the fader");
        Rig silence;
        silence.feed (45, -26);
        const auto beforeSilence = silence.leveler.getGainDb();
        const auto silenceFrom = silence.gains.size();
        silence.feed (20, -300);
        silence.feed (20, -70);
        double rise = 0.0, fastestReturn = 0.0;
        for (size_t i = silenceFrom; i < silence.gains.size(); ++i)
        {
            rise = juce::jmax (rise, silence.gains[i] - silence.gains[i - 1]);
            fastestReturn = juce::jmax (fastestReturn, (silence.gains[i - 1] - silence.gains[i]) * 100.0);
        }
        metric ("gain after 40 s of silence and -70 LUFS noise", silence.leveler.getGainDb());
        expect (rise <= 1.0e-9, "silence and noise never lift the fader");
        expect (fastestReturn <= 0.55, "it only drifts back towards 0 dB, slowly");
        expectWithinAbsoluteError (silence.leveler.getGainDb(), 0.0, 0.05);   // 2 s, then 0.5 dB/s from about +10 dB
        juce::ignoreUnused (beforeSilence);

        Rig movingSilence;
        movingSilence.feed (6, -26);   // mid-ride: about 2 dB/s upwards
        const auto silenceStart = movingSilence.gains.size() - 1;
        movingSilence.feed (20, -300);
        metric ("silence glide after a moving ride", movingSilence.leveler.getGainDb() - movingSilence.gains[silenceStart]);
        // 0.4 s as a short gap, then the glide; checked up to the 2 s after which a long stop drifts back to 0 dB
        const std::vector<double> firstTwoSeconds (movingSilence.gains.begin(), movingSilence.gains.begin() + (long) silenceStart + 200);
        glideThenStill (firstTwoSeconds, silenceStart, 0.5 + 0.4, 2.0);
        const auto beforeNoise = movingSilence.leveler.getGainDb();
        movingSilence.feed (20, -70);
        expect (movingSilence.leveler.getGainDb() <= beforeNoise, "noise never lifts it");

        Rig pausedCue;
        pausedCue.feed (30, -26);
        const auto beforePause = pausedCue.leveler.getGainDb();
        pausedCue.leveler.setPaused (true);   // a cue is paused: the resume must sound as before
        pausedCue.feed (30, -300);
        expectWithinAbsoluteError (pausedCue.leveler.getGainDb(), beforePause, 0.05);
        pausedCue.leveler.setPaused (false);

        beginTest ("9. a 2 s gap clears both windows before an already-correct new song");
        const auto savedGain = silence.leveler.getGainDb();
        silence.feed (3, -16.0 - savedGain);
        expectWithinAbsoluteError (silence.leveler.getGainDb(), savedGain, 0.3);
        Rig shortGap;
        shortGap.feed (45, -26);
        shortGap.feed (2, -300);
        const auto gapGain = shortGap.leveler.getGainDb();
        shortGap.feed (3, -16.0 - gapGain);
        expectWithinAbsoluteError (shortGap.leveler.getGainDb(), gapGain, 0.3);

        beginTest ("10. hold lets go of a moving fader without a corner, excludes fading blocks, and releases gently");
        Rig held;
        held.feed (6, -26);
        const auto holdStart = held.gains.size() - 1;
        held.leveler.setHold (true);
        for (int i = 0; i < 500; ++i) held.step (-26.0 - 20.0 * i / 499.0);
        glideThenStill (held.gains, holdStart, 0.6);
        const auto beforeHold = held.leveler.getGainDb();
        held.leveler.setHold (false);
        held.feed (1, -16.0 - beforeHold);
        expectWithinAbsoluteError (held.leveler.getGainDb(), beforeHold, 0.1);

        beginTest ("11. impulses never exceed -1 dBFS and the 5 s input peak caps boost");
        Rig impulses;
        const float impact = (float) std::pow (10.0, -2.0 / 20.0);
        float maxPeak = 0;
        for (int b = 0; b < 2000; ++b)
        {
            impulses.signal.fill (impulses.buffer, -30);
            if (b % 25 == 0) impulses.buffer.setSample (b % 2, 17, impact);
            impulses.leveler.process (impulses.buffer, block);
            maxPeak = juce::jmax (maxPeak, impulses.buffer.getMagnitude (0, block));
        }
        metric ("impulse output peak dBFS", 20.0 * std::log10 ((double) maxPeak));
        metric ("peak-capped gain dB", impulses.leveler.getGainDb());
        expect ((double) maxPeak <= std::pow (10.0, -1.0 / 20.0));
        expect (impulses.leveler.getGainDb() <= 4.1);

        beginTest ("12. disabled is bit exact and zero latency across arbitrary block sizes and channels");
        AutoLeveler bypass;
        bypass.prepare (rate, 127, 4);
        juce::AudioBuffer<float> raw (4, 997), original (4, 997);
        juce::Random random (42);
        for (int ch = 0; ch < 4; ++ch)
            for (int i = 0; i < 997; ++i) raw.setSample (ch, i, i % 13 == 0 ? -0.0f : (random.nextFloat() * 2 - 1));
        original.makeCopyOf (raw);
        for (int repeat = 0; repeat < 10; ++repeat) bypass.process (raw, 997);
        for (int ch = 0; ch < 4; ++ch) expect (std::memcmp (raw.getReadPointer (ch), original.getReadPointer (ch), 997 * sizeof (float)) == 0);

        beginTest ("13. 20 ms transitions on a 1 kHz sine, including rapid reversal");
        AutoLeveler switching;
        switching.prepare (rate, block, 2);
        juce::AudioBuffer<float> sine (2, block);
        float previous = 0;
        double largestStep = 0, sineStep = 0;
        for (int b = 0; b < 150; ++b)
        {
            if (b == 50 || b == 53 || b == 100) switching.setEnabled (true);
            if (b == 52 || b == 80 || b == 125) switching.setEnabled (false);
            for (int i = 0; i < block; ++i)
            {
                const float x = (float) (0.2 * std::sin (juce::MathConstants<double>::twoPi * (b * block + i) / 48.0));
                for (int ch = 0; ch < 2; ++ch) sine.setSample (ch, i, x);
                const double prev = 0.2 * std::sin (juce::MathConstants<double>::twoPi * (b * block + i - 1) / 48.0);
                sineStep = juce::jmax (sineStep, std::abs (x - prev));
            }
            switching.process (sine, block);
            for (int i = 0; i < block; ++i)
            {
                const float x = sine.getSample (0, i);
                largestStep = juce::jmax (largestStep, (double) std::abs (x - previous)); previous = x;
            }
        }
        metric ("switch sample-step / sine sample-step", largestStep / sineStep);
        expect (largestStep <= sineStep * 1.2);

        beginTest ("14. 60 seconds of stereo processing, Release budget 1 second");
        AutoLeveler timed;
        timed.prepare (rate, block, 2);
        timed.setEnabled (true);
        Signal signal;
        juce::AudioBuffer<float> input (2, block), work (2, block);
        signal.fill (input, -26);
        const double start = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < 6000; ++i) { work.makeCopyOf (input, true); timed.process (work, block); }
        const double elapsed = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
        metric ("60 s stereo processing wall seconds", elapsed);
       #if ! JUCE_DEBUG
        expect (elapsed <= 1.0);
       #else
        expect (true);
       #endif

        beginTest ("19. no stair-steps: a gappy program (100 ms rests every 400 ms) rides up in one continuous movement");
        Rig gappy;
        for (int i = 0; i < 2500; ++i) gappy.step (i % 40 < 30 ? -26.0 : -300.0);
        metric ("gappy ride gain at 25 s", gappy.leveler.getGainDb());
        expect (gappy.leveler.getGainDb() >= 9.0, "the rests do not stop it from reaching the target");
        slopes (gappy.gains, 2.55, 10.1);
        size_t from = 0, to = 0;
        for (size_t i = 0; i < gappy.gains.size(); ++i)
        {
            if (from == 0 && gappy.gains[i] >= 2.0) from = i;
            if (to == 0 && gappy.gains[i] >= 8.0) to = i;
        }
        expect (from > 0 && to > from);
        double slowest = 1.0e9;
        for (size_t i = from + 1; i < to; ++i) slowest = juce::jmin (slowest, (gappy.gains[i] - gappy.gains[i - 1]) * 100.0);
        metric ("slowest 10 ms slope between +2 and +8 dB on gappy material (dB/s)", slowest);
        expect (slowest >= 0.6, "no stalls between the rests");

        beginTest ("21. a very quiet song rides up in one movement: its soft moments are not pauses");
        Rig soft;
        for (int i = 0; i < 2500; ++i) soft.step (i < 300 ? -36.0 : (i / 37) % 3 == 0 ? -44.0 : -34.0);   // soft moments under -41
        metric ("very quiet song gain at 25 s", soft.leveler.getGainDb());
        expect (soft.leveler.getGainDb() >= 11.0, "it reaches the boost it needs");
        slopes (soft.gains, 4.05, 10.1);
        size_t rideFrom = 0, rideTo = 0;
        for (size_t i = 0; i < soft.gains.size(); ++i)
        {
            if (rideFrom == 0 && soft.gains[i] >= 2.0) rideFrom = i;
            if (rideTo == 0 && soft.gains[i] >= 10.0) rideTo = i;
        }
        expect (rideFrom > 0 && rideTo > rideFrom);
        double softest = 1.0e9;
        for (size_t i = rideFrom + 1; i < rideTo; ++i) softest = juce::jmin (softest, (soft.gains[i] - soft.gains[i - 1]) * 100.0);
        metric ("slowest 10 ms slope between +2 and +10 dB on a very quiet song (dB/s)", softest);
        expect (softest >= 0.6, "no stop-start");

        beginTest ("22. a 0.5 s sound effect 10 LU over the music does not move the hand");
        Rig effect;
        effect.feed (30, -16);
        const auto beforeEffect = effect.leveler.getGainDb();
        effect.feed (0.5, -6);
        effect.feed (5, -16);
        metric ("gain moved by a 0.5 s effect, 5 s later (dB)", effect.leveler.getGainDb() - beforeEffect);
        expect (std::abs (effect.leveler.getGainDb() - beforeEffect) <= 0.15, "no dip after the effect");
        effect.feed (15, -16);
        expect (std::abs (effect.leveler.getGainDb() - beforeEffect) <= 0.15, "nor a slow drift while it is in the flow");

        beginTest ("23. switched off and on again within the 20 ms fade-out: the ride carries on, no level jump");
        Rig toggled;
        toggled.feed (20, -26);
        const auto rideGain = toggled.leveler.getGainDb();
        const auto outputBefore = toggled.buffer.getRMSLevel (0, 0, block);
        toggled.leveler.setEnabled (false);
        toggled.step (-26);   // 10 ms: half way out
        toggled.leveler.setEnabled (true);
        float quietest = 1.0e9f;
        for (int i = 0; i < 30; ++i) { toggled.step (-26); quietest = juce::jmin (quietest, toggled.buffer.getRMSLevel (0, 0, block)); }
        metric ("gain after a 10 ms off-on", toggled.leveler.getGainDb());
        expectWithinAbsoluteError (toggled.leveler.getGainDb(), rideGain, 0.1);
        expect (quietest >= outputBefore * 0.8f, "no dip from a reset under the half-wet mix");

        beginTest ("24. one non-finite block does not break the measurement, the limiter or the output");
        Rig poisoned;
        poisoned.feed (6, -26);
        poisoned.signal.fill (poisoned.buffer, -26);
        poisoned.buffer.setSample (0, 100, std::numeric_limits<float>::quiet_NaN());
        poisoned.buffer.setSample (1, 200, std::numeric_limits<float>::infinity());
        poisoned.leveler.process (poisoned.buffer, block);
        bool finite = true;
        for (int i = 0; i < 2000; ++i)
        {
            poisoned.step (-26);
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < block; ++s)
                    finite = finite && std::isfinite (poisoned.buffer.getSample (ch, s));
        }
        expect (finite, "the output stays finite");
        metric ("gain 20 s after a non-finite block", poisoned.leveler.getGainDb());
        expectWithinAbsoluteError (poisoned.leveler.getGainDb(), 10.0, 0.2);

        beginTest ("25. gappy material with one loud effect: the 3 s ceiling stays out of it");
        Rig talk;
        for (int i = 0; i < 3000; ++i) talk.step (i % 10 < 3 ? -16.0 : -300.0);   // 30 % active, at the target
        const auto talkGain = talk.leveler.getGainDb();
        for (int i = 0; i < 50; ++i) talk.step (-6.0);                           // a 0.5 s effect, 10 LU over
        for (int i = 0; i < 500; ++i) talk.step (i % 10 < 3 ? -16.0 : -300.0);
        metric ("gain moved by an effect in gappy material (dB)", talk.leveler.getGainDb() - talkGain);
        expect (std::abs (talk.leveler.getGainDb() - talkGain) <= 0.3);

        beginTest ("26. 'go on in the same direction' runs out by the clock, also through a pause");
        Rig lingering;
        lingering.feed (30, -26);   // landed at about +10
        lingering.feed (1.9, -300); // a pause just short of a new section
        const auto afterPause = lingering.leveler.getGainDb();
        lingering.feed (2.0, -32);  // 6 dB quieter: well past the 3 s since the landing, so this must wait again
        metric ("gain moved in the first 2 s after the pause (dB)", lingering.leveler.getGainDb() - afterPause);
        expect (lingering.leveler.getGainDb() - afterPause <= 0.3, "a new move waits once the 3 s are over");

        beginTest ("27. taps that keep coming (one every 1.2 s) still cap the boost");
        Rig taps;
        const float tapLevel = (float) std::pow (10.0, -2.0 / 20.0);
        for (int b = 0; b < 3000; ++b)
        {
            taps.signal.fill (taps.buffer, -30);
            if (b % 120 == 60) taps.buffer.setSample (0, 17, tapLevel);
            taps.leveler.process (taps.buffer, block);
        }
        metric ("gain with a -2 dBFS tap every 1.2 s", taps.leveler.getGainDb());
        expect (taps.leveler.getGainDb() <= 4.1, "the limiter does not work more than 3 dB on every tap");

        beginTest ("27b. taps that keep coming 2.5 or 2.8 s apart cap the boost too, all the time");
        for (const int period : { 250, 280 })
        {
            Rig spaced;
            double highest = -100.0;
            for (int b = 0; b < 4000; ++b)
            {
                spaced.signal.fill (spaced.buffer, -30);
                if (b % period == 60) spaced.buffer.setSample (0, 17, tapLevel);
                spaced.leveler.process (spaced.buffer, block);
                if (b >= 1500) highest = juce::jmax (highest, spaced.leveler.getGainDb());
            }
            metric ("highest gain after 15 s with a -2 dBFS tap every " + juce::String (period / 100.0) + " s", highest);
            expect (highest <= 4.3, "the limiter does not work more than 3 dB on every tap");
        }

        beginTest ("27c. regular 0 dBFS taps from a song's start: the ride stops at the cap, never over it and back");
        Rig opening;
        double openingHighest = -100.0;
        for (int b = 0; b < 2000; ++b)
        {
            opening.signal.fill (opening.buffer, -30);
            if (b % 120 == 60) opening.buffer.setSample (0, 17, 1.0f);
            opening.leveler.process (opening.buffer, block);
            openingHighest = juce::jmax (openingHighest, opening.leveler.getGainDb());
        }
        metric ("highest gain with 0 dBFS taps every 1.2 s from the start (dB)", openingHighest);
        metric ("gain at 20 s (dB)", opening.leveler.getGainDb());
        expect (openingHighest <= 2.3, "never over the +2 dB the taps allow");
        expect (opening.leveler.getGainDb() >= 1.7, "and it gets there");

        beginTest ("27e. 0 dBFS taps 2.8 s apart from a song's start: the ride stops at the cap, no gap where it rides on");
        Rig slowTaps;
        double slowTapsHighest = -100.0;
        for (int b = 0; b < 3000; ++b)
        {
            slowTaps.signal.fill (slowTaps.buffer, -30);
            if (b % 280 == 60) slowTaps.buffer.setSample (0, 17, 1.0f);
            slowTaps.leveler.process (slowTaps.buffer, block);
            slowTapsHighest = juce::jmax (slowTapsHighest, slowTaps.leveler.getGainDb());
        }
        metric ("highest gain with 0 dBFS taps every 2.8 s from the start (dB)", slowTapsHighest);
        expect (slowTapsHighest <= 2.3, "never over the +2 dB the taps allow");
        expect (slowTaps.leveler.getGainDb() >= 1.7, "and it gets there");

        beginTest ("27f. knock-knock-knock during a song's first ride up: the ride may pause, never comes down");
        Rig knocked;
        double knockedPeak = -100.0, knockedDrop = 0.0, knockedAt9 = 0.0, pausedLow = 100.0, pausedHigh = -100.0;
        juce::String knockedRide;
        for (int b = 0; b < 2500; ++b)
        {
            knocked.signal.fill (knocked.buffer, -30);
            if (b == 350 || b == 440 || b == 530) knocked.buffer.setSample (0, 17, 1.0f);
            knocked.leveler.process (knocked.buffer, block);
            knockedPeak = juce::jmax (knockedPeak, knocked.leveler.getGainDb());
            knockedDrop = juce::jmax (knockedDrop, knockedPeak - knocked.leveler.getGainDb());
            if (b >= 250 && b <= 1400 && b % 50 == 49)
                knockedRide << juce::String ((b + 1) / 100.0, 1) << "s " << juce::String (knocked.leveler.getGainDb(), 2) << "  ";
            if (b == 899)
                knockedAt9 = knocked.leveler.getGainDb();
            if (b >= 650 && b < 850)   // between the stop and 3 s after the last knock
            {
                pausedLow = juce::jmin (pausedLow, knocked.leveler.getGainDb());
                pausedHigh = juce::jmax (pausedHigh, knocked.leveler.getGainDb());
            }
        }
        logMessage ("ride through the knocks (3.5 / 4.4 / 5.3 s): " + knockedRide);
        metric ("largest drop below the ride's highest point after the knocks (dB)", knockedDrop);
        metric ("gain at 25 s (dB)", knocked.leveler.getGainDb());
        expect (knockedDrop <= 0.1, "no pull-down for one knock sequence");
        expect (knockedAt9 >= 6.3, "the ride goes on about 3 s after the last knock, without a new wait");
        metric ("movement while the ride is stopped, 6.5 to 8.5 s (dB)", pausedHigh - pausedLow);
        expect (pausedHigh - pausedLow <= 0.05, "it really stops in between");

        beginTest ("27i. a fader at rest held by a knock sequence waits as usual for a bigger target once the bound lets go");
        Rig rested;
        double restedAt27 = 0.0;
        for (int b = 0; b < 3500; ++b)
        {
            if (b == 2470)
                rested.leveler.setTargetLufs (-13.0);   // asks for +3 dB more, just before the bound lets go (24.8 s)
            rested.signal.fill (rested.buffer, -26);
            if (b == 2000 || b == 2090 || b == 2180) rested.buffer.setSample (0, 17, 1.0f);
            rested.leveler.process (rested.buffer, block);
            if (b == 2699)
                restedAt27 = rested.leveler.getGainDb();
        }
        metric ("gain at 27 s, 2.3 s after the bigger target (dB)", restedAt27);
        metric ("gain at 35 s (dB)", rested.leveler.getGainDb());
        expect (restedAt27 <= 10.1, "no move without the usual wait: the hand was not riding when the bound caught it");
        expect (rested.leveler.getGainDb() >= 11.5, "and then it does move");
        expect (knocked.leveler.getGainDb() >= 11.5, "and the song still gets its level");

        beginTest ("27g. a louder pair that has gone quiet does not hide the taps still coming just under it");
        Rig masked;
        double maskedHighest = -100.0, maskedPeak = -100.0, maskedDrop = 0.0;
        for (int b = 0; b < 1500; ++b)
        {
            masked.signal.fill (masked.buffer, -30);
            if (b == 60 || b == 160) masked.buffer.setSample (0, 17, 1.0f);
            if (b == 290 || b == 570 || b == 850 || b == 1130 || b == 1410) masked.buffer.setSample (0, 17, 0.999f);
            masked.leveler.process (masked.buffer, block);
            maskedHighest = juce::jmax (maskedHighest, masked.leveler.getGainDb());
            maskedPeak = juce::jmax (maskedPeak, masked.leveler.getGainDb());
            maskedDrop = juce::jmax (maskedDrop, maskedPeak - masked.leveler.getGainDb());
        }
        metric ("highest gain with 1.0 taps that stop and 0.999 taps that go on (dB)", maskedHighest);
        expect (maskedHighest <= 2.3, "never over the cap the taps allow");
        expect (maskedDrop <= 0.1, "nor over it and back");

        beginTest ("27h. switched off for 10 ms while a held ride comes down, then on with a target that asks for more: no pull-down");
        Rig flicker;
        double flickerAtOn = 0.0, flickerLowest = 100.0;
        bool flickerOn = true;
        for (int b = 0; b < 3500; ++b)
        {
            if (b == 2050)
                flicker.leveler.setTargetLufs (-40.0);   // the ride comes down
            if (b == 2190)
                flicker.leveler.setEnabled (false);       // off for 10 ms, just after the 21.9 s decision
            if (b == 2191)
            {
                flicker.leveler.setEnabled (true);        // back on within the fade: no fresh start
                flicker.leveler.setTargetLufs (-16.0);    // and the target wants the boost back
            }
            flicker.signal.fill (flicker.buffer, -30);
            if (b == 2000 || b == 2090 || b == 2180) flicker.buffer.setSample (0, 17, 1.0f);
            flicker.leveler.process (flicker.buffer, block);
            if (b == 2191) { flickerAtOn = flicker.leveler.getGainDb(); flickerOn = true; }
            if (b > 2191 && flickerOn) flickerLowest = juce::jmin (flickerLowest, flicker.leveler.getGainDb());
        }
        metric ("gain when switched on again (dB)", flickerAtOn);
        metric ("lowest gain after that (dB)", flickerLowest);
        metric ("gain at 35 s (dB)", flicker.leveler.getGainDb());
        expect (flickerAtOn - flickerLowest <= 0.3, "the target wants more: nothing pulls the fader further down");
        expect (flicker.leveler.getGainDb() >= 11.5, "and it rides back up");

        beginTest ("27d. a new song opening with knock-knock-knock while the fader is still up from the last one: no pull-down");
        Rig carried;
        carried.feed (20.0, -30);   // +12 dB
        carried.feed (2.1, -300);   // a new section; the fader has barely started back towards 0
        const auto carriedStart = carried.leveler.getGainDb();
        double carriedLowest = carriedStart;
        for (int b = 0; b < 800; ++b)
        {
            carried.signal.fill (carried.buffer, -30);
            if (b == 50 || b == 140 || b == 230) carried.buffer.setSample (0, 17, tapLevel);
            carried.leveler.process (carried.buffer, block);
            carriedLowest = juce::jmin (carriedLowest, carried.leveler.getGainDb());
        }
        metric ("dip after an opening knock-knock-knock (dB)", carriedStart - carriedLowest);
        expect (carriedStart - carriedLowest <= 0.3);

        beginTest ("28. one loud effect over a boosted program is the limiter's job, wherever it falls in the 0.1 s blocks");
        double worstMove = 0.0;
        juce::String worstCase;
        struct EffectCase { int offset; double seconds, lufs; };
        for (const auto c : { EffectCase { 0, 0.5, -16 }, EffectCase { 5, 0.5, -16 }, EffectCase { 0, 0.5, -6 },
                              EffectCase { 5, 0.5, -6 }, EffectCase { 8, 0.5, -6 }, EffectCase { 5, 0.2, -6 } })
        {
            Rig boosted;
            boosted.feed (20.0 + c.offset * 0.01, -26);   // about +10 dB; the effect starts c.offset x 10 ms into a block
            const auto before = boosted.leveler.getGainDb();
            boosted.feed (c.seconds, c.lufs);              // 10 or 20 LU over the target once boosted
            double moved = 0.0;
            for (int i = 0; i < 800; ++i)
            {
                boosted.step (-26);
                moved = juce::jmax (moved, std::abs (boosted.leveler.getGainDb() - before));
            }
            if (moved > worstMove)
            {
                worstMove = moved;
                worstCase = juce::String (c.seconds) + " s at " + juce::String (c.lufs) + " LUFS, +" + juce::String (c.offset * 10) + " ms";
            }
        }
        metric ("largest move in the 8 s after one effect over a boosted program (dB), " + worstCase, worstMove);
        expect (worstMove <= 0.3, "no dip after the effect: " + worstCase);

        beginTest ("29. a loud effect early in a song, or the bang a cue starts with, does not bend the first ride");
        const auto songWithEffect = [] (double songLufs, double effectAt, double effectLufs)
        {
            Rig r;
            for (int i = 0; i < 2000; ++i)
            {
                const double t = i * 0.01;
                r.step (effectAt >= 0.0 && t >= effectAt - 1.0e-9 && t < effectAt + 0.5 - 1.0e-9 ? effectLufs : songLufs);
            }
            return r.gains;
        };
        const auto plain = songWithEffect (-26.0, -1.0, 0.0);   // the same song without the effect: rides up to about +10
        double worstEarly = 0.0;
        juce::String worstEarlyCase;
        struct Early { double song, at, effect; };
        // 0.5 s, 10 or 20 LU over the song before it is boosted; at 1.55 s the effect's first and last 0.1 s blocks hold
        // half of it each (louder than the song, not 8 LU over it): a song that needs +2 dB, and one already at the target
        for (const auto e : { Early { -26, 1.5, -16 }, Early { -26, 1.5, -6 }, Early { -26, 5.03, -16 }, Early { -26, 5.03, -6 },
                              Early { -26, 12.0, -16 }, Early { -26, 12.0, -6 }, Early { -18, 1.55, -8 }, Early { -16, 1.55, -6 } })
        {
            const auto reference = e.song == -26 ? plain : songWithEffect (e.song, -1.0, 0.0);
            const auto g = songWithEffect (e.song, e.at, e.effect);
            double moved = 0.0;
            for (size_t i = 0; i < g.size(); ++i) moved = juce::jmax (moved, std::abs (g[i] - reference[i]));
            if (moved > worstEarly)
            {
                worstEarly = moved;
                worstEarlyCase = juce::String (e.effect) + " LUFS at " + juce::String (e.at) + " s in a " + juce::String (e.song) + " LUFS song";
            }
        }
        metric ("largest difference from the ride without the effect (dB), " + worstEarlyCase, worstEarly);
        expect (worstEarly <= 0.3, "the effect leaves the ride alone: " + worstEarlyCase);
        for (const double lufs : { -16.0, -6.0 })
        {
            const auto g = songWithEffect (-26.0, 0.0, lufs);   // the cue opens with a 0.5 s bang, then the song
            double lowest = 0.0;
            for (const auto v : g) lowest = juce::jmin (lowest, v);
            metric ("lowest gain after a " + juce::String (lufs) + " LUFS opening bang (dB)", lowest);
            metric ("gain at 15 s after the bang vs without (dB)", g[1499] - plain[1499]);
            expect (lowest >= -0.3, "no dip for the bang");
            expect (std::abs (g[1499] - plain[1499]) <= 0.3, "the song still gets its level in time");
        }

        beginTest ("30. two effects 0.1 s apart are not 'far too loud': a gap starts the count of loud blocks again");
        Rig pair;
        pair.feed (20.0, -26);   // about +10 dB
        const auto pairBefore = pair.leveler.getGainDb();
        pair.feed (0.4, -6);     // 4 blocks, 20 LU over once boosted
        pair.feed (0.1, -300);   // one silent block
        pair.feed (0.3, -6);     // 3 more: 7 loud blocks, never 7 in a row
        double pairMoved = 0.0;
        for (int i = 0; i < 600; ++i)
        {
            pair.step (-26);
            pairMoved = juce::jmax (pairMoved, std::abs (pair.leveler.getGainDb() - pairBefore));
        }
        metric ("largest move after two effects 0.1 s apart (dB)", pairMoved);
        expect (pairMoved <= 0.3);

        beginTest ("31. one knock-knock-knock (three -2 dBFS taps 0.9 s apart) is one effect, wherever it falls in the peak history");
        double worstKnock = 0.0;
        for (int offset = 0; offset < 10; ++offset)
        {
            Rig knock;
            knock.feed (20.0 + offset * 0.1, -30);   // +12 dB; the history's ring starts 'offset' blocks further on
            const auto before = knock.leveler.getGainDb();
            for (int b = 0; b < 800; ++b)
            {
                knock.signal.fill (knock.buffer, -30);
                if (b == 0 || b == 90 || b == 180) knock.buffer.setSample (0, 17, tapLevel);
                knock.leveler.process (knock.buffer, block);
                worstKnock = juce::jmax (worstKnock, std::abs (knock.leveler.getGainDb() - before));
            }
        }
        metric ("largest move after one knock sequence, any position in the peak history (dB)", worstKnock);
        expect (worstKnock <= 0.3);

        beginTest ("32. a new song does not inherit the last one's peaks");
        Rig songs;
        songs.feed (20.0, -30);   // +12 dB
        for (int b = 0; b < 100; ++b)   // one tap 1 s before the song ends
        {
            songs.signal.fill (songs.buffer, -30);
            if (b == 0) songs.buffer.setSample (0, 17, tapLevel);
            songs.leveler.process (songs.buffer, block);
        }
        songs.feed (2.1, -300);   // a new section
        const auto newSong = songs.leveler.getGainDb();
        double lowestNew = newSong;
        for (int b = 0; b < 800; ++b)   // the next song at the same level, one tap 0.5 s in: 3.6 s after the last one
        {
            songs.signal.fill (songs.buffer, -30);
            if (b == 50) songs.buffer.setSample (0, 17, tapLevel);
            songs.leveler.process (songs.buffer, block);
            lowestNew = juce::jmin (lowestNew, songs.leveler.getGainDb());
        }
        metric ("dip in the new song after one tap in each song (dB)", newSong - lowestNew);
        expect (newSong - lowestNew <= 0.3);

        beginTest ("33. sparse talk (one 0.1 s block in five) and an effect: the 3 s ceiling needs 10 loud blocks, not half of a few");
        Rig sparse;
        sparse.feed (20.0, -16);   // a flow at the target: nothing to correct
        for (int i = 0; i < 500; ++i) sparse.step ((i / 10) % 5 == 0 ? -16.0 : -300.0);
        const auto sparseBefore = sparse.leveler.getGainDb();
        sparse.feed (0.6, -6);     // 6 blocks, 10 LU over
        double sparseMoved = 0.0;
        for (int i = 0; i < 600; ++i)
        {
            sparse.step ((i / 10) % 5 == 0 ? -16.0 : -300.0);
            sparseMoved = juce::jmax (sparseMoved, std::abs (sparse.leveler.getGainDb() - sparseBefore));
        }
        metric ("largest move after an effect in sparse talk (dB)", sparseMoved);
        expect (sparseMoved <= 0.3);

        beginTest ("24b. a non-finite sample while switched off never comes out after switching on");
        Rig dormant;
        dormant.leveler.setEnabled (false);
        dormant.feed (1, -26);
        dormant.signal.fill (dormant.buffer, -26);
        dormant.buffer.setSample (0, block - 20, std::numeric_limits<float>::quiet_NaN());   // inside the last 5 ms
        dormant.leveler.process (dormant.buffer, block);
        dormant.leveler.setEnabled (true);
        bool dormantFinite = true;
        for (int i = 0; i < 100; ++i)
        {
            dormant.step (-26);
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < block; ++s)
                    dormantFinite = dormantFinite && std::isfinite (dormant.buffer.getSample (ch, s));
        }
        expect (dormantFinite, "the first second after switching on is finite");

        beginTest ("20. a 1.9 s pause mid-move eases out and back in, never a corner");
        Rig paused;
        paused.feed (30, -26);
        paused.feed (2, -6);   // a much louder song: the ride down is under way
        const auto pauseStart = paused.gains.size() - 1;
        paused.feed (1.9, -300);   // just short of the 2 s that would make the next music a new section
        paused.feed (6, -6);
        slopes (paused.gains, 2.55, 10.1);
        double during = 0.0;
        for (size_t i = pauseStart + 161; i <= pauseStart + 190; ++i) during += std::abs (paused.gains[i] - paused.gains[i - 1]);
        metric ("movement in the last 0.3 s of the pause dB", during);
        expect (during <= 0.02, "halted before the pause ends");
        expect (paused.leveler.getGainDb() <= paused.gains[pauseStart] - 4.0, "and the ride down went on after it");

        extraCoverage();
        trace();
    }

private:
    void metric (const juce::String& label, double value) { logMessage (label + " = " + juce::String (value, 6)); }
    void slopes (const std::vector<double>& gains, double up, double down)
    {
        double maxUp = 0, maxDown = 0, maxChange = 0, previousSlope = 0;
        size_t worst = 0;
        for (size_t i = 1; i < gains.size(); ++i)
        {
            const double slope = (gains[i] - gains[i - 1]) * 100;
            maxUp = juce::jmax (maxUp, slope); maxDown = juce::jmax (maxDown, -slope);
            if (std::abs (slope - previousSlope) > maxChange) { maxChange = std::abs (slope - previousSlope); worst = i; }
            previousSlope = slope;
        }
        if (maxChange > 0.2)
        {
            juce::String around;
            for (size_t i = worst > 6 ? worst - 6 : 1; i < juce::jmin (gains.size(), worst + 6); ++i)
                around << juce::String ((gains[i] - gains[i - 1]) * 100, 3) << (i == worst ? "* " : " ");
            metric ("corner at index " + juce::String ((int) worst) + " of " + juce::String ((int) gains.size()) + ", slopes: " + around, maxChange);
        }
        metric ("max up dB/s", maxUp); metric ("max down dB/s", maxDown); metric ("adjacent slope difference dB/s", maxChange);
        expect (maxUp <= up); expect (maxDown <= down); expect (maxChange <= 0.2);
    }
    /** From 'from' on: the fader may still travel 'maxTravel' dB (the glide), is still 0.6 s after that glide began
        (allowing 'leadSeconds' of going on first), and its slope never turns a corner (checked by slopes()). */
    void glideThenStill (const std::vector<double>& gains, size_t from, double leadSeconds, double maxTravel = 1.0)
    {
        const size_t settled = from + (size_t) std::llround ((leadSeconds + 1.0) * 100.0);
        expect (settled < gains.size());
        if (settled >= gains.size()) return;
        double travel = 0.0, still = 0.0;
        for (size_t i = from; i < gains.size(); ++i) travel = juce::jmax (travel, std::abs (gains[i] - gains[from]));
        for (size_t i = settled; i < gains.size(); ++i) still = juce::jmax (still, std::abs (gains[i] - gains[settled]));
        metric ("glide travel dB", travel); metric ("movement after the glide dB", still);
        expect (travel <= maxTravel, "a glide, not a ride on");
        expect (still <= 0.01, "then it stays");
        double previous = 0.0, corner = 0.0;
        for (size_t i = (from > 20 ? from - 20 : 1); i < gains.size(); ++i)
        {
            const double slope = (gains[i] - gains[i - 1]) * 100.0;
            if (i > (from > 20 ? from - 20 : 1)) corner = juce::jmax (corner, std::abs (slope - previous));
            previous = slope;
        }
        metric ("largest slope change through the glide dB/s", corner);
        expect (corner <= 0.2, "no corner");
    }
    void gentleStart (const std::vector<double>& gains, double maximum, double waitSeconds)
    {
        size_t first = 1;
        while (first + 10 < gains.size() && std::abs (gains[first] - gains[first - 1]) < 1.0e-8) ++first;
        expect (first + 10 < gains.size());
        expect ((first + 1) * 0.01 >= 0.8 + waitSeconds); // the full wait starts when S first becomes available
        if (first + 10 < gains.size())
        {
            const double slope = std::abs (gains[first + 9] - gains[first - 1]) * 10;
            metric ("first 100 ms average dB/s", slope); expect (slope < maximum * 0.5);
        }
    }
    void extraCoverage()
    {
        beginTest ("target edits use the same ride; enabling again starts at unity with empty measurements");
        Rig edited;
        edited.feed (20, -26);
        const auto oldGain = edited.leveler.getGainDb();
        edited.leveler.setTargetLufs (-20);
        edited.step (-26);
        expectWithinAbsoluteError (edited.leveler.getGainDb(), oldGain, 0.05);
        edited.feed (30, -26);
        expectWithinAbsoluteError (edited.leveler.getGainDb(), 6.0, 0.05);
        edited.leveler.setEnabled (false); edited.feed (0.1, -26);
        expectEquals (edited.leveler.getGainDb(), 0.0);
        edited.leveler.setEnabled (true); edited.feed (0.5, -26);
        expectEquals (edited.leveler.getGainDb(), 0.0);

        beginTest ("limiter attack is a lookahead ramp shared by all channels, never a step");
        AutoLeveler attack;
        attack.prepare (rate, block, 2); attack.setEnabled (true);
        juce::AudioBuffer<float> dc (2, block);
        for (int b = 0; b < 10; ++b)
        {
            juce::FloatVectorOperations::fill (dc.getWritePointer (0), 0.1f, block);
            dc.clear (1, 0, block); attack.process (dc, block);
        }
        juce::FloatVectorOperations::fill (dc.getWritePointer (0), 0.1f, block);
        dc.clear (1, 0, block); dc.setSample (1, 20, 2.0f); attack.process (dc, block);
        float largestChange = 0;
        for (int i = 1; i <= 260; ++i) largestChange = juce::jmax (largestChange, std::abs (dc.getSample (0, i) - dc.getSample (0, i - 1)));
        expect (largestChange < 0.0003f);
        expect (dc.getSample (0, 100) > dc.getSample (0, 260));
        expectWithinAbsoluteError (dc.getSample (0, 260) / dc.getSample (1, 260), 0.05f, 0.000001f);

        beginTest ("mono and multichannel limiter, unity transparency, reprepare, sample-rate/block invariance");
        for (const double fs : { 44100.0, 48000.0, 96000.0 })
        {
            AutoLeveler leveler;
            leveler.prepare (fs, 512, 4);
            leveler.setEnabled (true);
            juce::AudioBuffer<float> b (4, 512);
            for (int n = 0; n < 30; ++n) { b.clear(); leveler.process (b, 512); }
            b.clear();
            b.setSample (3, 0, 2.0f); b.setSample (0, 0, 0.2f);
            leveler.process (b, 512);
            const int latency = (int) std::llround (fs * 0.005);
            expect ((double) b.getSample (3, latency) <= std::pow (10.0, -1.0 / 20.0));
            expectWithinAbsoluteError (b.getSample (0, latency) / b.getSample (3, latency), 0.1f, 0.00001f);
            leveler.prepare (fs, 512, 1);
            juce::AudioBuffer<float> mono (1, 512);
            for (int n = 0; n < 4; ++n) { juce::FloatVectorOperations::fill (mono.getWritePointer (0), 0.1f, 512); leveler.process (mono, 512); }
            expectEquals (mono.getSample (0, 511), 0.1f);
        }
        AutoLeveler whole, pieces;
        whole.prepare (rate, block, 2); pieces.prepare (rate, 17, 2);
        whole.setEnabled (true); pieces.setEnabled (true);
        Signal signal;
        juce::AudioBuffer<float> a (2, block), b (2, block);
        double error = 0;
        for (int i = 0; i < 1000; ++i)
        {
            signal.fill (a, -26); b.makeCopyOf (a); whole.process (a, block);
            for (int pos = 0; pos < block; pos += 17)
            {
                const int n = juce::jmin (17, block - pos);
                juce::AudioBuffer<float> part (b.getArrayOfWritePointers(), 2, pos, n);
                pieces.process (part, n);
            }
            for (int n = 0; n < block; ++n) error = juce::jmax (error, (double) std::abs (a.getSample (0, n) - b.getSample (0, n)));
        }
        expectEquals (error, 0.0);
    }
    void trace()
    {
        beginTest ("15. optional 10 ms trace, never writes without ENQ_AUTOLEVEL_TRACE");
        const auto folder = juce::SystemStats::getEnvironmentVariable ("ENQ_AUTOLEVEL_TRACE", {});
        if (folder.isEmpty()) { expect (true); return; }
        const juce::File directory (folder);
        expect (directory.createDirectory().wasOk());
        const auto file = directory.getChildFile ("trace.csv");
        juce::FileOutputStream stream (file);
        expect (stream.openedOk());
        if (! stream.openedOk()) return;
        stream.setPosition (0); stream.truncate();
        stream << "time_s,input_short_lufs,output_short_lufs,gain_db,hold\n";
        Rig rig;
        for (int tick = 0; tick < 5900; ++tick)
        {
            const double t = tick * 0.01;
            const bool held = t >= 54;
            const double level = t < 20 ? -28 : t < 23 ? -300 : t < 43 ? -10 : t < 49 ? -16 : t < 54 ? -10 : -10 - 20 * (t - 54) / 5;
            rig.leveler.setHold (held);
            rig.signal.fill (rig.buffer, level);
            if (t >= 49 && t < 54 && tick % 40 < 2)
                for (int i = 0; i < block; ++i)
                    for (int ch = 0; ch < 2; ++ch)
                        rig.buffer.setSample (ch, i, (float) (std::pow (10.0, -1.0 / 20.0) * std::sin (juce::MathConstants<double>::twoPi * i / 48.0)));
            rig.input.process (rig.buffer); rig.leveler.process (rig.buffer, block); rig.output.process (rig.buffer);
            stream << juce::String (t + 0.01, 2) << "," << juce::String (rig.input.lufs(), 6) << ","
                   << juce::String (rig.output.lufs(), 6) << "," << juce::String (rig.leveler.getGainDb(), 6) << "," << (held ? "1\n" : "0\n");
        }
        stream.flush();
        expect (stream.getStatus().wasOk());
        logMessage ("trace: " + file.getFullPathName());
    }
};
static AutoLevelTests autoLevelTests;
} // namespace gocue::tests
