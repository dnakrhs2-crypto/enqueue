#pragma once

#include "audio/LoudnessMeter.h"

namespace gocue
{

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
    std::array<double, 4> recentEnergy {};              // the last 0.4 s (0 = not active): far too loud right now
    int recentPos = 0;
    int linger = 0;          // 0.1 s blocks after a landing in which the same direction goes on without a new wait
    int urgentRun = 0;       // blocks in a row heard far too loud (a 0.5 s effect touches at most 6 of the 7 needed)
    int urgentLatch = 0;     // blocks the 0.4 s ceiling still applies, until the 3 s window has caught up
    int sectionBlocks = 0;   // active blocks since this section (song) began: a new song is judged sooner
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
