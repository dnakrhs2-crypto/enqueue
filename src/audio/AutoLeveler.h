#pragma once

#include "audio/LoudnessMeter.h"

namespace gocue
{
namespace tests { struct AutoLevelerTestAccess; }

/** Feed-forward master fader. prepare/reset belong to the audio context (prepare may allocate).
    Setters and the displayed gain are atomic; process never allocates, locks or logs. */
class AutoLeveler
{
public:
    void prepare (double sampleRate, int maxBlock, int numChannels);
    void reset() noexcept;
    void setEnabled (bool value) noexcept { enabled.store (value, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled.load (std::memory_order_relaxed); }
    void setTargetLufs (double value) noexcept;
    void setHold (bool value) noexcept { hold.store (value, std::memory_order_relaxed); }
    /** A cue is paused: a long silence then keeps the fader where it is (the resume must sound as before). */
    void setPaused (bool value) noexcept { paused.store (value, std::memory_order_relaxed); }
    double getGainDb() const noexcept { return displayedGain.load (std::memory_order_relaxed); }
    /** The last block's hold: the user's own level change was in progress (the hand let go). */
    bool isHeld() const noexcept { return hold.load (std::memory_order_relaxed); }
    void process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept;

private:
    friend struct tests::AutoLevelerTestAccess;
    void clearMeasurement() noexcept;
    void finishMeasurement() noexcept;
    void freezeGain() noexcept;
    /** Like a hand that lets go of a moving fader: the slew stops where the first smoother stage already is, so the
        applied gain glides to a halt (no corner in its slope). Used for long quiet, a user's own fade and no decision. */
    void easeOut() noexcept;
    /** One 0.1 s step towards 'goal'. A settled fader follows small differences very slowly; a bigger one has to last
        (waits) before a move starts; a started move goes on until it lands, slowing as it closes. 'countWaits' = false
        in a short gap: the waits pause instead of starting over. */
    void moveTowards (double goal, bool countWaits) noexcept;
    /** The newest second of the S window is more than 2 dB under the second before it (each with at least 5 active
        blocks, one burst - an effect - left out as in programPower): the program is still falling - a fade-out, a
        decay, the first second after a drop. */
    bool fallingNow() const noexcept;
    /** The loudest input peak of this section's first 4 s (sectionYoungBlocks) still within the last 5 s, one burst - an
        effect, a bang - left out as in programPower, told among all the section's blocks of those 5 s: any such peak may
        be the first of a beat, and a beat up to 5 s apart has shown its second hit (and the recurring peak takes over)
        by the time it drops out. */
    float sectionPeak() const noexcept;
    /** Far too quiet for 2.1 s (hushSectionBlocks), no longer falling, and the flow more than 3 LU over the run: the
        music has changed to a much quieter song (or part) - not a fader the recurring peaks keep under the target. The
        windows keep only that quiet run - the louder music's level, peaks and effects go, as after a gap - so the fader
        goes up at once instead of waiting for the 15 s flow to forget them. */
    void startQuietSection() noexcept;
    /** The quiet run's loudness (LUFS, input): its blocks are the flow's newest entries. */
    double hushLoudness() const noexcept;
    void planGainRamp() noexcept;
    /** The song's flow (LUFS): one short burst left out (see programPower in the .cpp), then without its loudest 5 %
        (at least one block). An effect or a single hit is not the song's level. Audio thread: sorts a fixed scratch
        array, no allocation. */
    double flowLoudness() noexcept;
    /** The highest input peak level reached twice at least 'apart' blocks apart in this section's last 10 s (the same
        whatever the ring position): 30 blocks = peaks that keep coming (drums, taps - any period up to 5 s); one sound
        effect, a bang or a knock-knock-knock, does not. */
    float recurringPeak (int apart) const noexcept;
    /** The loudest input block peak among the last 'blocks' blocks. */
    float recentPeak (int blocks) const noexcept;
    void fillDelay (const juce::AudioBuffer<float>& buffer, int start, int count) noexcept;
    double limit (float peak) noexcept;

    std::atomic<bool> enabled { false }, hold { false }, paused { false };
    std::atomic<double> target { -16.0 }, displayedGain { 0.0 };
    double rate = 48000.0;
    int channels = 0, blockLength = 4800, delayLength = 240, transitionLength = 960;
    bool wasEnabled = false, moving = false, blockHeld = false;
    double wet = 0.0;
    livemix::KWeightingFilter kLeft, kRight;
    std::array<double, 30> shortEnergy {}, shortScratch {};
    std::array<double, 150> flowEnergy {}, flowScratch {};
    std::array<float, 100> peaks {};   // this section's input block peaks, 10 s
    int shortPos = 0, flowPos = 0, flowCount = 0, peakPos = 0, inactive = 0;
    int measured = 0, upWait = 0, downWait = 0;
    bool haveDecision = false, travelling = false, travelUp = false, easing = false, urgent = false;
    // The hand: the slew moves continuously (never in steps) at slewSpeed, which changes by at most handAcceleration -
    // it speeds up and slows down like a hand on a fader - towards wantSpeed (this 0.1 s decision), and stops at stopAt.
    double slewSpeed = 0.0, wantSpeed = 0.0, stopAt = 0.0;
    static constexpr double handAcceleration = 20.0;   // dB/s per second
    // Far too loud (a blaring song after a quiet one): once it is clearly no effect, the hand pulls the fader down fast -
    // 4 dB/s per dB left up to 30 dB/s, speeding up by up to 100 and braking by up to 120 dB/s per second (what easing
    // in at 4 dB/s per dB from 30 dB/s needs). Rides up and ordinary moves keep the slow hand.
    static constexpr double pullDownRate = 4.0, pullDownSpeed = 30.0, pullDownAcceleration = 100.0, pullDownBraking = 120.0;
    // The way back up after an effect sets off gently (a hand eases the fader back), up to 20 dB/s.
    static constexpr double releaseAcceleration = 40.0;
    bool pullDown = false;   // a fast pull-down is the move in progress
    bool fastHand = false;   // the hand still moves at the fast rates (also braking after a pull-down) until it rests
    bool blaring = false;    // this decision: far too loud right now, 7 blocks in a row (not just the ceiling's 3 s after)
    // Far too quiet (heard more than 8 LU under the target) is the same hand the other way: no wait, 4 dB/s per dB up to
    // 30 dB/s. A gap neither counts nor breaks the run (quiet talk has its pauses); a louder block or the user's own fade
    // starts it again.
    bool riseUp = false;     // a fast ride up is the move in progress
    bool hushed = false;     // this decision: far too quiet right now (7 blocks), and so is the whole section
    bool hushSectionTaken = false;   // this quiet run has already become a section
    bool ceilingOn = false;  // the 3 s ceiling, held while most of the 3 s is still heard over the target
    bool loudNow = false;    // the last decision heard its last 0.4 s at the target or over it: no ride up starts
    int hushRun = 0;         // active blocks of the run
    int hushSpan = 0;        // blocks since the run began, its gaps included
    static constexpr int hushSectionBlocks = 21;   // 2.1 s: a 2 s dip in a song stays a dip
    // A section's first 4 s: its peaks bound the ride up, each for 5 s, before two of them can show a beat (sectionPeak).
    static constexpr int sectionYoungBlocks = 40;
    std::array<double, 50> pastEnergy {};   // the last 5 s of block powers by time (0 = not active), for sectionPeak
    int pastPos = 0;

    // A blare inside a song (far too loud 7 blocks in a row, with at least 3 s of the song before it): the fader is
    // pulled down at once and the blare's loud blocks wait outside the flow (blareHeld) until it is clear what it was.
    // 0.5 s back near the song's level (within 5 s, or later if it is not loud by then) = a sound effect (an airhorn
    // over talk): its blocks leave the S window and the fader goes straight back up (releaseRide, no wait); loud again
    // within 1.5 s (blareEnding) = it was a rest in a loud song: taken back, straight down again (snapBack). Still loud
    // after 5 s (anything but back after 7 s) = loud material: its blocks join the flow. The ceiling stays on while open.
    void startBlare() noexcept;
    void holdBackBlareBlock() noexcept;
    void endBlareAsEffect (double wanted) noexcept;
    void takeBackRelease() noexcept;
    void endBlareAsMaterial() noexcept;
    void forgetBlare() noexcept;
    /** Aims the hand where the song's own level puts the fader (desired, with the target and the cap it was set for): the
        target over the flow (which never had the blare in it), under the recurring peak's cap - the way back after an
        effect, also while the S window is too empty to judge. */
    void aimBackUp (double wanted) noexcept;
    bool blare = false, releaseRide = false, snapBack = false;
    int blareBlocks = 0, blareHeldCount = 0, blareEnding = 0, quietRun = 0;
    int sinceLoud = 0;   // blocks since the blare was last over the song's level: the song (and silence) after it
    double blareReference = 0.0;   // the song's level before it (LUFS, input)
    double blareLoud = 0.0;        // as block power: 6 LU over that - a blare block
    double releaseFrom = -100.0;   // where the hand was when it let a blare go as an effect: a take-back returns it there
    double takeBackTo = -100.0;    // after a take-back, the fader goes no higher than that while the blare is open
    double releaseTarget = 0.0;    // the target when releaseFrom was taken (a changed target moves the take-back bound along)
    double desiredTarget = 0.0;    // the target the last decision (desired) was made for (a held ceiling moves along with it)
    double desiredCap = 12.0;      // that decision's peak caps - not the target's, so a moved position stays under them
    std::array<double, 64> blareHeld {};
    std::array<int, 64> blareSlot {};   // where each held block sits in the flow (zeroed while it waits)
    std::array<double, 4> recentEnergy {};              // the last 0.4 s (0 = not active): far too loud right now
    int recentPos = 0;
    int linger = 0;          // 0.1 s blocks after a landing in which the same direction goes on without a new wait
    int urgentRun = 0;       // blocks in a row heard far too loud (a 0.5 s effect touches at most 6 of the 7 needed)
    int urgentLatch = 0;     // blocks the 0.4 s ceiling still applies, until the 3 s window has caught up
    int sectionBlocks = 0;   // active blocks since this section (song) began
    int sectionClock = 0;    // blocks (time, gaps included) since this section's first sound
    double earlyHold = -100.0;   // where the early peak bound stopped a hand that was already above it (-100 = none)
    bool earlyHoldRide = false;  // ...and it stopped a ride up in progress: that ride goes on when the bound lets go
    double energy = 0.0, desired = 0.0, gain = 0.0, smooth1 = 0.0, smooth2 = 0.0;
    float inputPeak = 0.0f;
    double linear = 1.0, linearStep = 0.0;
    int rampLeft = 0;

    // Linked limiter: sliding maximum, release envelope, then a lookahead-length moving average.
    // A peak stays in the maximum window until it reaches the output, so every term of the
    // moving average is safe by that instant. The attack is a ramp, never a gain step.
    juce::AudioBuffer<float> delay;
    int delayPos = 0;
    struct Peak { float value = 0.0f; juce::int64 sample = 0; };
    std::vector<Peak> maximum;
    int maxHead = 0, maxCount = 0;
    juce::int64 sampleIndex = 0;
    std::vector<double> attack;
    int attackPos = 0;
    double attackSum = 240.0, releaseGain = 1.0, releaseCoefficient = 0.0;
    static constexpr float ceiling = 0.891250908f; // rounded DOWN: never exceeds -1 dBFS
};

} // namespace gocue
