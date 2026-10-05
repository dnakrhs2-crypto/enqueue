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
        slopes (down.gains, 2.55, 30.1, 2.05);   // far too loud: the hand pulls the fader down fast
        gentleStart (ride.gains, 2.5, 2.0);   // a new song is judged after 2 s
        gentleStart (down.gains, 30.0, 0.0);  // 10 LU over the target: far too loud, no further wait
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
        slopes (loud.gains, 2.55, 30.1, 2.05);

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

        beginTest ("34. a blaring song right after a quiet one is pulled down fast once it is clearly no effect");
        for (const double gap : { 0.0, 2.5 })   // straight on, and after a gap that makes it a new song
        {
            Rig blare;
            blare.feed (20.0, -26);   // +10 dB
            blare.feed (gap, -300);
            const auto onset = blare.gains.size();
            const auto startGain = blare.leveler.getGainDb();
            blare.feed (6.0, -12);    // 14 LU over the target with the boost still on
            const auto finalGain = blare.leveler.getGainDb();
            double sixDown = -1.0, landed = -1.0, lowest = 100.0;
            for (size_t i = onset; i < blare.gains.size(); ++i)
            {
                const double s = (double) (i - onset + 1) * 0.01;
                if (sixDown < 0.0 && blare.gains[i] <= startGain - 6.0) sixDown = s;
                if (landed < 0.0 && blare.gains[i] <= finalGain + 1.0) landed = s;
                lowest = juce::jmin (lowest, blare.gains[i]);
            }
            const auto label = gap > 0.0 ? juce::String (" after a 2.5 s gap") : juce::String (" straight on");
            double threeDown = -1.0;
            for (size_t i = onset; i < blare.gains.size() && threeDown < 0.0; ++i)
                if (blare.gains[i] <= startGain - 3.0) threeDown = (double) (i - onset + 1) * 0.01;
            metric ("blare" + label + ": 3 dB down after (s)", threeDown);
            metric ("blare" + label + ": 6 dB down after (s)", sixDown);
            metric ("blare" + label + ": within 1 dB of where it settles after (s)", landed);
            metric ("blare" + label + ": settles at (dB)", finalGain);
            expect (sixDown > 0.0 && sixDown <= 1.45, "6 dB down within 1.45 s (was 2.0 s)" + label);
            expect (landed > 0.0 && landed <= 2.0, "and nearly all the way within 2 s" + label);
            expect (lowest >= finalGain - 0.5, "without going past it" + label);
            slopes (blare.gains, 4.05, 30.1, 2.05);
        }

        beginTest ("35. a long loud effect (0.8 to 3 s) inside a song is ducked and then the fader goes straight back");
        for (const double seconds : { 0.8, 1.5, 3.0 })
        {
            Rig longFx;
            longFx.feed (20.0, -26);   // +10 dB
            const auto before = longFx.leveler.getGainDb();
            longFx.feed (seconds, -6); // 20 LU over the target once boosted: an airhorn over quiet talk
            double lowestFx = before;
            int backAt = -1;
            for (int i = 0; i < 1000; ++i)
            {
                longFx.step (-26);
                lowestFx = juce::jmin (lowestFx, longFx.leveler.getGainDb());
                if (backAt < 0 && lowestFx < before - 0.5 && longFx.leveler.getGainDb() >= before - 0.5) backAt = i;
            }
            const auto label = juce::String (seconds) + " s effect";
            metric ("a " + label + " 20 LU over: deepest dip (dB)", before - lowestFx);
            metric ("a " + label + " 20 LU over: back within 0.5 dB after it ends (s)", backAt < 0 ? -1.0 : (backAt + 1) * 0.01);
            metric ("a " + label + ": gain 10 s after it ends vs before (dB)", longFx.leveler.getGainDb() - before);
            expect (backAt >= 0 && backAt + 1 <= 350, "the " + label + " is let go: back within 3.5 s after it ends");
            expect (std::abs (longFx.leveler.getGainDb() - before) <= 0.3, "and nothing of it stays in the level: " + label);
            if (seconds > 2.0)
            {
                juce::String ride;
                for (size_t i = longFx.gains.size() - 1000; i < longFx.gains.size(); i += 50)
                    ride << juce::String ((double) (i - (longFx.gains.size() - 1000)) * 0.01, 1) << "s " << juce::String (longFx.gains[i], 2) << "  ";
                logMessage ("after the 3 s effect: " + ride);
            }
            slopes (longFx.gains, 20.05, 30.1, 2.05);
        }

        beginTest ("35b. an effect that ends just before the 5 s mark is still an effect");
        Rig nearly;
        nearly.feed (20.0, -26);
        const auto nearlyBefore = nearly.leveler.getGainDb();
        nearly.feed (4.9, -6);
        int nearlyBack = -1;
        double nearlyLowest = nearlyBefore;
        for (int i = 0; i < 1000; ++i)
        {
            nearly.step (-26);
            nearlyLowest = juce::jmin (nearlyLowest, nearly.leveler.getGainDb());
            if (nearlyBack < 0 && nearlyLowest < nearlyBefore - 0.5 && nearly.leveler.getGainDb() >= nearlyBefore - 0.5) nearlyBack = i;
        }
        metric ("a 4.9 s effect: back within 0.5 dB after it ends (s)", nearlyBack < 0 ? -1.0 : (nearlyBack + 1) * 0.01);
        expect (nearlyBack >= 0 && nearlyBack + 1 <= 350, "let go like any effect");
        expect (std::abs (nearly.leveler.getGainDb() - nearlyBefore) <= 0.3, "and nothing of it stays");

        beginTest ("35c. a short rest inside a loud song does not let the fader back up");
        for (const int rest : { 2, 6 })   // 0.2 s and 0.6 s
        {
            Rig resting;
            resting.feed (20.0, -26);   // +10 dB
            resting.feed (2.0, -12);    // a much louder song: pulled down
            const auto restStart = resting.leveler.getGainDb();
            double restHighest = restStart;
            for (int i = 0; i < rest * 10; ++i) { resting.step (-26); restHighest = juce::jmax (restHighest, resting.leveler.getGainDb()); }
            for (int i = 0; i < 150; ++i) { resting.step (-12); restHighest = juce::jmax (restHighest, resting.leveler.getGainDb()); }
            const auto afterRest = resting.leveler.getGainDb();
            resting.feed (4.0, -12);
            const auto label = juce::String (rest / 10.0) + " s rest";
            metric ("a " + label + " in a loud song: highest gain above where it was (dB)", restHighest - restStart);
            metric ("a " + label + ": 1.5 s after the song goes on, vs before (dB)", afterRest - restStart);
            expect (restHighest - restStart <= 3.0, "at most a brief lift: " + label);
            expect (afterRest - restStart <= 0.5, "and back down within 1.5 s: " + label);
            expect (resting.leveler.getGainDb() - restStart <= 0.5, "and it stays down: " + label);
        }

        beginTest ("35d. the target changes while the fader goes back up after an effect: it follows the new target");
        Rig retarget;
        retarget.feed (20.0, -26);
        retarget.feed (3.0, -6);    // fills the whole 3 s window
        double atChange = 0.0, highestAfter = -100.0;
        for (int i = 0; i < 800; ++i)
        {
            if (i == 50)   // 0.5 s after it ends: the way back up is under way
            {
                retarget.leveler.setTargetLufs (-40.0);
                atChange = retarget.leveler.getGainDb();
            }
            retarget.step (-26);
            if (i >= 50)
                highestAfter = juce::jmax (highestAfter, retarget.leveler.getGainDb());
        }
        metric ("gain when the target drops to -40 (dB)", atChange);
        metric ("highest gain after that (dB)", highestAfter);
        metric ("gain 7.5 s later (dB)", retarget.leveler.getGainDb());
        expect (highestAfter <= atChange + 2.5, "it turns round instead of going on to the old target");
        expect (retarget.leveler.getGainDb() <= -13.0, "and gets to the new one (-14 dB)");

        beginTest ("27j. a recurring peak met on the fast way back up after an effect still stops the ride where it can");
        const auto backUp = [] (int tapAt)
        {
            Rig r;
            const float full = 1.0f;
            r.feed (20.0, -26);
            for (int b = 0; b < 10; ++b) { r.signal.fill (r.buffer, -26); if (b == 9) r.buffer.setSample (0, 17, full); r.leveler.process (r.buffer, block); r.gains.push_back (r.leveler.getGainDb()); }
            r.feed (1.0, -6);   // a 1 s effect
            for (int b = 0; b < 600; ++b)
            {
                r.signal.fill (r.buffer, -26);
                if (b == tapAt) r.buffer.setSample (0, 17, full);
                r.leveler.process (r.buffer, block);
                r.gains.push_back (r.leveler.getGainDb());
            }
            return r.gains;
        };
        const auto plainBack = backUp (-1);
        const size_t effectEnd = plainBack.size() - 600;
        size_t bottom = effectEnd;   // the bottom of the dip, then 2 dB up on the fast way back
        for (size_t i = effectEnd; i < plainBack.size(); ++i)
            if (plainBack[i] < plainBack[bottom]) bottom = i;
        int crossing = -1;
        for (size_t i = bottom; i < plainBack.size() && crossing < 0; ++i)
            if (plainBack[i] >= plainBack[bottom] + 3.0) crossing = (int) (i - effectEnd);
        metric ("the dip's bottom (dB)", plainBack[bottom]);
        expect (crossing > 0, "the way back up passes 3 dB over the bottom");
        if (crossing > 0)
        {
            const auto tapped = backUp (crossing);
            double plainTop = -100.0, tappedTop = -100.0;
            const size_t from = effectEnd + (size_t) crossing, to = juce::jmin (plainBack.size(), from + 250);   // while the bound is on
            for (size_t i = from; i < to; ++i) { plainTop = juce::jmax (plainTop, plainBack[i]); tappedTop = juce::jmax (tappedTop, tapped[i]); }
            metric ("way back up without / with a 0 dBFS tap 1 s+ after the last one, highest gain (dB)", tappedTop);
            metric ("without", plainTop);
            expect (tappedTop <= plainTop - 2.0, "the tap's bound stops the fast ride well short of the top");
        }

        beginTest ("36. loud for more than 5 s is loud material, not an effect: the fader stays down");
        Rig louder;
        louder.feed (20.0, -26);   // +10 dB
        louder.feed (8.0, -12);    // a much louder song straight on
        const auto louderAt8 = louder.leveler.getGainDb();
        louder.feed (4.0, -12);
        metric ("gain 8 s into a much louder song (dB)", louderAt8);
        metric ("and 4 s later (dB)", louder.leveler.getGainDb());
        expect (louderAt8 <= -2.0, "it came down for the louder song");
        expect (louder.leveler.getGainDb() <= louderAt8 + 0.5, "and stays down: no going back up as if it were an effect");

        beginTest ("35e. a rest in a loud song, then a silence just after the song comes back: still straight back down");
        for (const int rest : { 6, 10 })        // 0.6 s and 1.0 s of rest (the fader on its way back up)
            for (const int first : { 1, 2 })    // 0.1 s or 0.2 s of the song before a 0.3 s stop
            {
                Rig gapped;
                gapped.feed (20.0, -26);   // +10 dB
                gapped.feed (2.0, -12);    // a much louder song: pulled down
                const auto restStart = gapped.leveler.getGainDb();
                gapped.feed (rest / 10.0, -26);
                gapped.feed (first / 10.0, -12);   // the song comes back: taken back
                gapped.feed (0.3, -300);           // a stop in the song
                const auto goesOn = gapped.gains.size();
                gapped.feed (4.0, -12);
                double highestLater = -100.0;
                for (size_t i = goesOn + 150; i < gapped.gains.size(); ++i) highestLater = juce::jmax (highestLater, gapped.gains[i]);
                const auto label = juce::String (rest / 10.0) + " s rest, " + juce::String (first / 10.0) + " s of the song, a 0.3 s stop";
                metric (label + ": gain when the song goes on, vs before the rest (dB)", gapped.gains[goesOn] - restStart);
                metric (label + ": 1.5 s later (dB)", gapped.gains[goesOn + 150] - restStart);
                metric (label + ": highest from 1.5 s on (dB)", highestLater - restStart);
                expect (gapped.gains[goesOn + 150] - restStart <= 0.5, "back down within 1.5 s: " + label);
                expect (highestLater - restStart <= 0.5, "and nothing lifts it after that: " + label);
                expect (gapped.leveler.getGainDb() - restStart <= 0.5, "and it stays down: " + label);
                slopes (gapped.gains, 20.05, 30.1, 2.05);
            }

        beginTest ("35h. a rest in a loud song, then a stop before the song comes back: turned straight back on the first block");
        for (const int rest : { 6, 10 })   // the way back up started 0.1 s or 0.5 s before the stop, and went on through it
        {
            Rig late;
            late.feed (20.0, -26);   // +10 dB
            late.feed (2.0, -12);    // a much louder song: pulled down
            const auto restStart = late.leveler.getGainDb();
            late.feed (rest / 10.0, -26);
            late.feed (0.3, -300);
            const auto back = late.gains.size();
            const auto atBack = late.gains[back - 1];
            late.feed (4.0, -12);    // the song is back: its first block alone decides
            double rise = 0.0;
            size_t peakAt = back;
            for (size_t i = back; i < late.gains.size(); ++i)
                if (late.gains[i] - atBack > rise) { rise = late.gains[i] - atBack; peakAt = i; }
            const auto label = juce::String (rest / 10.0) + " s rest and a 0.3 s stop";
            metric (label + ": gain when the song is back, vs before the rest (dB)", atBack - restStart);
            metric (label + ": further rise after that, the way up's momentum through the stop (dB)", rise);
            metric (label + ": it peaks after (s)", (double) (peakAt - back) * 0.01);
            metric (label + ": 1.5 s later vs before the rest (dB)", late.gains[back + 150] - restStart);
            // the way up went on through the stop (as it does through any gap) and has its momentum and the 0.15 s smoothing:
            // about 4.4 dB more on screen; deciding on the first block back adds nothing (0.2 s more ride, 4.7-5.1 dB, without that)
            expect (rise <= 4.6, "it turns on the first block back, no further than its own momentum: " + label);
            expect (late.gains[back + 150] - restStart <= 0.5, "back down within 1.5 s: " + label);
            expect (late.leveler.getGainDb() - restStart <= 0.5, "and it stays down: " + label);
            slopes (late.gains, 20.05, 30.1, 2.05);
        }

        beginTest ("34b. a blaring song with a stop in its first second is still pulled down fast");
        double landedStraight = 2.0;
        for (const double silence : { 0.0, 0.3 })
        {
            Rig stop;
            stop.feed (20.0, -26);
            const auto onset = stop.gains.size();
            const auto startGain = stop.leveler.getGainDb();
            stop.feed (1.0, -12);
            stop.feed (silence, -300);
            stop.feed (6.0, -12);
            const auto finalGain = stop.leveler.getGainDb();
            double sixDown = -1.0, landed = -1.0, lowest = 100.0;
            for (size_t i = onset; i < stop.gains.size(); ++i)
            {
                const double s = (double) (i - onset + 1) * 0.01;
                if (sixDown < 0.0 && stop.gains[i] <= startGain - 6.0) sixDown = s;
                if (landed < 0.0 && stop.gains[i] <= finalGain + 1.0) landed = s;
                lowest = juce::jmin (lowest, stop.gains[i]);
            }
            const auto label = juce::String (silence) + " s stop";
            metric (label + ": 6 dB down after (s)", sixDown);
            metric (label + ": within 1 dB of where it settles after (s)", landed);
            metric (label + ": settles at (dB)", finalGain);
            expect (sixDown > 0.0 && sixDown <= 1.45, "6 dB down within 1.45 s: " + label);
            if (silence == 0.0)
                landedStraight = landed;
            // nothing is heard during the stop, and its first block (the end of the sound) reads a little quieter: the stop
            // may cost at most its own length (2.85 s before the last ceiling held through it)
            expect (landed > 0.0 && landed <= (silence > 0.0 ? landedStraight + silence : 2.0), "nearly all the way within 2 s, a stop costing at most its length: " + label);
            expect (lowest >= finalGain - 0.5, "without going past it: " + label);
            slopes (stop.gains, 4.05, 30.1, 2.05);
        }

        beginTest ("35g. an effect that ends in a pause before the talk goes on is let go like any effect");
        for (const double seconds : { 0.8, 1.5, 3.0 })
        {
            Rig pausing;
            pausing.feed (20.0, -26);   // +10 dB
            const auto before = pausing.leveler.getGainDb();
            pausing.feed (seconds, -6);
            pausing.feed (0.3, -300);   // a pause in the talk
            double lowest = before;
            int backAt = -1;
            for (int i = 0; i < 1000; ++i)
            {
                pausing.step (-26);
                lowest = juce::jmin (lowest, pausing.leveler.getGainDb());
                if (backAt < 0 && lowest < before - 0.5 && pausing.leveler.getGainDb() >= before - 0.5) backAt = i;
            }
            const auto label = juce::String (seconds) + " s effect and a 0.3 s pause";
            metric ("a " + label + ": deepest dip (dB)", before - lowest);
            metric ("a " + label + ": back within 0.5 dB after the effect ends (s)", backAt < 0 ? -1.0 : (backAt + 1) * 0.01 + 0.3);
            metric ("a " + label + ": gain 10 s after vs before (dB)", pausing.leveler.getGainDb() - before);
            expect (backAt >= 0 && backAt + 1 + 30 <= 380, "let go: back within 3.8 s after it ends: " + label);
            expect (std::abs (pausing.leveler.getGainDb() - before) <= 0.3, "and nothing of it stays: " + label);
            slopes (pausing.gains, 20.05, 30.1, 2.05);
        }

        beginTest ("35f. a tap just before a long effect and the same tap just after it: the pair still caps the way back up");
        const auto tapsAround = [] (int tapAfter)   // in 10 ms steps after the effect ends; -1 = no second tap
        {
            Rig r;
            r.feed (20.0, -26);
            for (int b = 0; b < 10; ++b) { r.signal.fill (r.buffer, -26); if (b == 9) r.buffer.setSample (0, 17, 1.0f); r.leveler.process (r.buffer, block); r.gains.push_back (r.leveler.getGainDb()); }
            r.feed (3.0, -6);   // a 3 s effect
            for (int b = 0; b < 600; ++b)
            {
                r.signal.fill (r.buffer, -26);
                if (b == tapAfter) r.buffer.setSample (0, 17, 1.0f);
                r.leveler.process (r.buffer, block);
                r.gains.push_back (r.leveler.getGainDb());
            }
            return r.gains;
        };
        const auto topAfter = [] (const std::vector<double>& g) { double top = -100.0; for (size_t i = g.size() - 600; i < g.size(); ++i) top = juce::jmax (top, g[i]); return top; };
        const auto oneTop = topAfter (tapsAround (-1));
        metric ("one tap before the effect only: highest gain in the 6 s after it (dB)", oneTop);
        expect (oneTop >= 9.5, "one tap is no pattern: the fader goes all the way back");
        for (const int tapAfter : { 5, 20, 45 })   // 0.05, 0.2 and 0.45 s into the song after the effect
        {
            const auto pairTop = topAfter (tapsAround (tapAfter));
            const auto label = juce::String (tapAfter / 100.0) + " s after the effect";
            metric ("the second tap " + label + ": highest gain in the 6 s after it (dB)", pairTop);
            expect (pairTop <= 2.5, "the pair (more than 3 s apart) caps the boost at 2 dB over 0 dBFS: " + label);
        }

        beginTest ("37. reset or prepare on the fast way back up after an effect: exactly a fresh start");
        for (const bool viaPrepare : { false, true })
        {
            Rig back;
            back.feed (20.0, -26);
            back.feed (3.0, -6);
            back.feed (0.8, -26);   // the fast way back up is under way
            if (viaPrepare) back.leveler.prepare (rate, block, 2); else back.leveler.reset();
            Rig fresh;
            fresh.signal.position = back.signal.position;
            const auto from = back.gains.size();
            back.feed (6.0, -26);
            fresh.feed (6.0, -26);
            double worst = 0.0;
            for (size_t i = 0; i < fresh.gains.size(); ++i) worst = juce::jmax (worst, std::abs (back.gains[from + i] - fresh.gains[i]));
            const auto label = viaPrepare ? juce::String ("prepare") : juce::String ("reset");
            metric (label + " on the way back up: largest gain difference from a fresh leveler (dB)", worst);
            metric (label + ": gain 1 s after (dB)", back.gains[from + 100]);
            expect (worst <= 1.0e-9, label + " leaves nothing of the way back up behind");
        }

        beginTest ("37b. the user's own fade on the fast way back up after an effect: a glide to a halt, then the usual ride");
        Rig handed;
        handed.feed (20.0, -26);
        const auto handedBefore = handed.leveler.getGainDb();
        handed.feed (3.0, -6);
        handed.feed (0.8, -26);   // the fast way back up
        const auto holdAt = handed.gains.size();
        handed.leveler.setHold (true);
        handed.feed (2.0, -26);
        handed.leveler.setHold (false);
        const auto letGo = handed.gains.size();
        handed.feed (14.0, -26);
        {
            double glide = 0.0, stillHeld = 0.0, stillAfter = 0.0, upAfter = 0.0;
            for (size_t i = holdAt; i < letGo; ++i) glide = juce::jmax (glide, std::abs (handed.gains[i] - handed.gains[holdAt]));
            for (size_t i = holdAt + 160; i < letGo; ++i) stillHeld = juce::jmax (stillHeld, std::abs (handed.gains[i] - handed.gains[holdAt + 160]));
            for (size_t i = letGo; i < letGo + 290; ++i) stillAfter = juce::jmax (stillAfter, std::abs (handed.gains[i] - handed.gains[letGo]));
            for (size_t i = letGo + 1; i < handed.gains.size(); ++i) upAfter = juce::jmax (upAfter, (handed.gains[i] - handed.gains[i - 1]) * 100.0);
            int backAt = -1;
            for (size_t i = letGo; i < handed.gains.size() && backAt < 0; ++i)
                if (handed.gains[i] >= handedBefore - 0.5) backAt = (int) (i - letGo);
            metric ("the fade on the way back up: glide (dB)", glide);
            metric ("movement while held, from 1.6 s (dB)", stillHeld);
            metric ("movement in the 2.9 s after the fade (dB)", stillAfter);
            metric ("fastest rise after the fade (dB/s)", upAfter);
            metric ("back within 0.5 dB after the fade (s)", backAt < 0 ? -1.0 : backAt * 0.01);
            expect (glide <= 3.0, "the fast ride glides to a halt");
            expect (stillHeld <= 0.01, "and stays put while the user fades");
            expect (stillAfter <= 0.01, "after the fade the hand waits as for any ride up");
            expect (upAfter <= 4.05, "then rides up at the ordinary hand's pace");
            expect (backAt >= 0 && backAt <= 1200, "and gets back to the song's level");
            slopes (handed.gains, 20.05, 30.1, 2.05);
        }

        beginTest ("37c. switched off and on within the 20 ms fade-out on the fast way back up: it carries on");
        const auto wayBack = [] (bool flick)
        {
            Rig r;
            r.feed (20.0, -26);
            r.feed (3.0, -6);
            r.feed (0.8, -26);   // the fast way back up
            if (flick) r.leveler.setEnabled (false);
            r.step (-26);        // 10 ms: half way out
            if (flick) r.leveler.setEnabled (true);
            r.feed (8.0, -26);
            return r.gains;
        };
        {
            const auto plain = wayBack (false), flicked = wayBack (true);
            const size_t at = plain.size() - 801;   // the step switched off
            const double before = plain[1999];
            const auto backAfter = [&] (const std::vector<double>& g) { for (size_t i = at; i < g.size(); ++i) if (g[i] >= before - 0.5) return (int) (i - at); return -1; };
            const int plainBack = backAfter (plain), flickedBack = backAfter (flicked);
            metric ("back within 0.5 dB without the off-on (s)", plainBack * 0.01);
            metric ("with a 10 ms off-on (s)", flickedBack * 0.01);
            metric ("level step at the switch (dB)", flicked[at] - flicked[at - 1]);
            expect (flickedBack >= 0 && flickedBack <= plainBack + 50, "the way back up carries on: back within 0.5 s of the ride without it");
            expect (std::abs (flicked[at] - flicked[at - 1]) <= 0.2, "no level jump at the switch");
            slopes (std::vector<double> (flicked.begin() + (std::ptrdiff_t) at, flicked.end()), 20.05, 30.1, 2.05);   // from the switch on
        }

        beginTest ("34c. the target is raised during a stop in a blare: what holds through the stop follows the new target");
        for (const int changeAt : { 10, 30 })   // 0.1 s into the stop (its first block, the sound's end, was decided as before), or as it ends
        {
            Rig raised;
            raised.feed (20.0, -26);   // +10 dB at -16
            raised.feed (0.8, -6);     // far too loud: pulled down towards -9 dB
            raised.feed (changeAt / 100.0, -300);
            const auto raisedAt = raised.leveler.getGainDb();
            raised.leveler.setTargetLufs (-6.0);   // 10 LU louder: the sound's place is now +1 dB
            const auto raisedFrom = raised.gains.size();
            raised.feed ((30 - changeAt) / 100.0, -300);   // the rest of the 0.3 s stop
            raised.feed (7.0, -6);     // the loud sound goes on
            double raisedLowest = 100.0;
            for (size_t i = raisedFrom; i < raised.gains.size(); ++i) raisedLowest = juce::jmin (raisedLowest, raised.gains[i]);
            const auto label = changeAt == 10 ? juce::String ("0.1 s into the stop") : juce::String ("as the stop ends");
            metric ("target raised " + label + ": gain then (dB)", raisedAt);
            metric ("lowest after that (dB)", raisedLowest);
            metric ("7 s later (dB)", raised.leveler.getGainDb());
            // as it ends the fast hand is already under +1 dB on its way to -9: its braking takes it about 4 dB further, no more
            expect (raisedLowest >= (changeAt == 10 ? 0.0 : -5.0), "nothing pulls on towards the old target's ceiling: " + label);
            expect (raised.leveler.getGainDb() >= -1.0, "and it rides back up to the new target: " + label);
        }

        beginTest ("37d. switched off for good on the fast way back up, then on again: exactly a fresh start");
        Rig gone;
        gone.feed (20.0, -26);
        gone.feed (3.0, -6);
        gone.feed (0.8, -26);   // the fast way back up
        gone.leveler.setEnabled (false);
        gone.feed (0.1, -26);   // all the way off (the 20 ms fade is over)
        gone.leveler.setEnabled (true);
        Rig anew;
        anew.signal.position = gone.signal.position;
        const auto goneOn = gone.gains.size();
        gone.feed (6.0, -26);
        anew.feed (6.0, -26);
        double goneWorst = 0.0;
        for (size_t i = 0; i < anew.gains.size(); ++i) goneWorst = juce::jmax (goneWorst, std::abs (gone.gains[goneOn + i] - anew.gains[i]));
        metric ("switched on again: largest gain difference from a fresh leveler (dB)", goneWorst);
        expect (goneWorst <= 1.0e-9, "nothing of the way back up survives switching off");

        beginTest ("37e. a 10 ms off-on while the user's own fade holds the way back up: nothing changes");
        const auto heldWayBack = [] (bool flick)
        {
            Rig r;
            r.feed (20.0, -26);
            r.feed (3.0, -6);
            r.feed (0.8, -26);   // the fast way back up
            r.leveler.setHold (true);
            r.feed (1.8, -26);   // the glide to a halt is over
            if (flick) r.leveler.setEnabled (false);
            r.step (-26);
            if (flick) r.leveler.setEnabled (true);
            r.feed (1.0, -26);
            r.leveler.setHold (false);
            r.feed (14.0, -26);
            return r.gains;
        };
        {
            // switched off, nothing is measured for those 10 ms: the 0.1 s blocks after it sit 10 ms later, so the ride after
            // the fade may start one block later - the same ride otherwise
            const auto heldPlain = heldWayBack (false), heldFlicked = heldWayBack (true);
            const size_t letGoAt = heldPlain.size() - 1400;
            double heldMoved = 0.0, stillAfter = 0.0, upAfter = 0.0;
            for (size_t i = letGoAt - 120; i < letGoAt; ++i) heldMoved = juce::jmax (heldMoved, std::abs (heldFlicked[i] - heldPlain[i]));
            for (size_t i = letGoAt; i < letGoAt + 290; ++i) stillAfter = juce::jmax (stillAfter, std::abs (heldFlicked[i] - heldFlicked[letGoAt]));
            for (size_t i = letGoAt + 1; i < heldFlicked.size(); ++i) upAfter = juce::jmax (upAfter, (heldFlicked[i] - heldFlicked[i - 1]) * 100.0);
            const auto backAt = [&] (const std::vector<double>& g) { for (size_t i = letGoAt; i < g.size(); ++i) if (g[i] >= heldPlain[1999] - 0.5) return (int) (i - letGoAt); return -1; };
            const int plainBack = backAt (heldPlain), flickedBack = backAt (heldFlicked);
            metric ("a 10 ms off-on during the fade: movement while held (dB)", heldMoved);
            metric ("back within 0.5 dB after the fade, without / with it (s)", plainBack * 0.01);
            metric ("with it", flickedBack * 0.01);
            expect (heldMoved <= 0.001, "the held fader stays put");
            expect (stillAfter <= 0.01, "after the fade the hand waits as for any ride up");
            expect (upAfter <= 4.05, "then rides up at the ordinary hand's pace");
            expect (plainBack >= 0 && flickedBack >= 0 && std::abs (flickedBack - plainBack) <= 20, "and gets back when it would have (one block either way)");
            slopes (heldFlicked, 20.05, 30.1, 2.05);
        }

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
        paused.feed (0.9, -6);   // a much louder song: the (fast) ride down is under way
        const auto pauseStart = paused.gains.size() - 1;
        paused.feed (1.9, -300);   // just short of the 2 s that would make the next music a new section
        paused.feed (6, -6);
        slopes (paused.gains, 2.55, 30.1, 2.05);
        double during = 0.0;
        for (size_t i = pauseStart + 161; i <= pauseStart + 190; ++i) during += std::abs (paused.gains[i] - paused.gains[i - 1]);
        metric ("movement in the last 0.3 s of the pause dB", during);
        expect (during <= 0.05, "halted before the pause ends");   // the fast hand's braking leaves a 0.15 s smoothing tail
        expect (paused.leveler.getGainDb() <= paused.gains[pauseStart] - 4.0, "and the ride down went on after it");

        extraCoverage();
        trace();
    }

private:
    void metric (const juce::String& label, double value) { logMessage (label + " = " + juce::String (value, 6)); }
    void slopes (const std::vector<double>& gains, double up, double down, double corner = 0.2)
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
        if (maxChange > corner)
        {
            juce::String around;
            for (size_t i = worst > 6 ? worst - 6 : 1; i < juce::jmin (gains.size(), worst + 6); ++i)
                around << juce::String ((gains[i] - gains[i - 1]) * 100, 3) << (i == worst ? "* " : " ");
            metric ("corner at index " + juce::String ((int) worst) + " of " + juce::String ((int) gains.size()) + ", slopes: " + around, maxChange);
        }
        metric ("max up dB/s", maxUp); metric ("max down dB/s", maxDown); metric ("adjacent slope difference dB/s", maxChange);
        expect (maxUp <= up); expect (maxDown <= down); expect (maxChange <= corner);
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
