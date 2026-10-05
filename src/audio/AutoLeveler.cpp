#include "audio/AutoLeveler.h"

namespace gocue
{

static_assert (std::atomic<double>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);

namespace
{
/** Mean power of a window of blocks given in time order - ring[(start + i) % size] for i < length, 0 = not active -
    using 'scratch' (room for 'length'). One short burst is not the program's level: blocks more than 8 LU over the
    median of the active ones are left out when there are at most 6 of them - one effect, wherever it falls in the
    0.1 s blocks (a 0.5 s one touches at most 6), also early in a song while the window is short, or the bang a cue
    opens with once the song has as many blocks - together with their neighbours in time that are more than 3 LU over
    the median (the effect's partly filled first and last block). More of them are a loud part of the program and count.
    The newest block, if that much louder, waits for the next one: it may be the partly filled first block of an effect
    that has only just begun. 'trimLoudest' then also leaves out the loudest 5 % (at least one block) of what remains. */
double programPower (const double* ring, int size, int start, int length, double* scratch, bool trimLoudest) noexcept
{
    const auto at = [ring, size, start, length] (int i) { return i >= 0 && i < length ? ring[(start + i) % size] : 0.0; };
    int n = 0;
    for (int i = 0; i < length; ++i)
        if (at (i) > 0.0)
            scratch[n++] = at (i);
    if (n == 0)
        return 0.0;

    std::nth_element (scratch, scratch + n / 2, scratch + n);
    const double median = scratch[n / 2];
    const double burst = median * 6.309573444801933, edge = median * 1.9952623149688795;   // + 8 LU, + 3 LU
    int loud = 0;
    for (int i = 0; i < length; ++i)
        if (at (i) > burst)
            ++loud;
    const bool oneBurst = loud > 0 && loud <= 6;

    n = 0;
    for (int i = 0; i < length; ++i)
    {
        const double p = at (i);
        const bool partOfBurst = oneBurst && (p > burst || (p > edge && (at (i - 1) > burst || at (i + 1) > burst)));
        const bool waits = i == length - 1 && p > edge && loud <= 6;
        if (p <= 0.0 || partOfBurst || waits)
            continue;
        scratch[n++] = p;   // the median block itself is never left out: n >= 1
    }

    int keep = n;
    if (trimLoudest)
    {
        keep = juce::jmax (1, n - juce::jmax (1, n / 20));
        std::nth_element (scratch, scratch + keep, scratch + n);
    }
    double sum = 0.0;
    for (int i = 0; i < keep; ++i)
        sum += scratch[i];
    return sum / keep;
}
} // namespace

void AutoLeveler::prepare (double sampleRate, int maxBlock, int numChannels)
{
    juce::ignoreUnused (maxBlock);
    rate = std::isfinite (sampleRate) && sampleRate > 0.0 ? sampleRate : 48000.0;
    channels = juce::jmax (1, numChannels);
    blockLength = juce::jmax (1, (int) std::llround (rate * 0.1));
    delayLength = juce::jmax (1, (int) std::llround (rate * 0.005));
    transitionLength = juce::jmax (1, (int) std::llround (rate * 0.020));
    delay.setSize (channels, delayLength);
    maximum.resize ((size_t) delayLength + 2);
    attack.resize ((size_t) delayLength);
    releaseCoefficient = std::exp (-1.0 / (rate * 0.150));
    kLeft.prepare (rate);
    kRight.prepare (rate);
    reset();
}

void AutoLeveler::setTargetLufs (double value) noexcept
{
    target.store (std::round (juce::jlimit (-40.0, -6.0, std::isfinite (value) ? value : -16.0) * 10.0) / 10.0,
                  std::memory_order_relaxed);
}

void AutoLeveler::clearMeasurement() noexcept
{
    kLeft.reset();
    kRight.reset();
    shortEnergy.fill (0.0);
    flowEnergy.fill (0.0);
    peaks.fill (0.0f);
    shortPos = flowPos = flowCount = peakPos = inactive = measured = upWait = downWait = 0;
    energy = 0.0;
    inputPeak = 0.0f;
    blockHeld = moving = false;
    haveDecision = travelling = travelUp = easing = urgent = false;
    recentEnergy.fill (0.0);
    recentPos = linger = sectionBlocks = urgentRun = urgentLatch = 0;
    earlyHold = -100.0;
    pullDown = fastHand = blaring = releaseRide = false;
    forgetBlare();
}

void AutoLeveler::reset() noexcept
{
    clearMeasurement();
    desired = gain = smooth1 = smooth2 = wet = 0.0;
    slewSpeed = wantSpeed = stopAt = 0.0;
    linear = releaseGain = 1.0;
    linearStep = 0.0;
    rampLeft = delayPos = maxHead = maxCount = attackPos = 0;
    sampleIndex = 0;
    wasEnabled = false;
    delay.clear();
    std::fill (attack.begin(), attack.end(), 1.0);
    attackSum = (double) delayLength;
    displayedGain.store (0.0, std::memory_order_relaxed);
}

void AutoLeveler::freezeGain() noexcept
{
    // Switched off: the ride stops where it is (the crossfade to the bypass hides it).
    gain = stopAt = smooth1 = smooth2 = 20.0 * std::log10 (linear);
    slewSpeed = wantSpeed = 0.0;
    pullDown = fastHand = snapBack = false;   // the way back up after an effect (releaseRide) carries on if switched on within the fade
    earlyHold = -100.0;   // the hand moved to where the gain really is: a stop point set before means nothing now
    rampLeft = upWait = downWait = 0;
    linearStep = 0.0;
    moving = travelling = easing = false;
}

void AutoLeveler::easeOut() noexcept
{
    if (easing)
        return;

    // The hand slows down at handAcceleration and the two smoothing stages round it off: the applied gain glides to a
    // halt (about 0.7 dB further at 2 dB/s, 1.6 dB at 4 dB/s) and neither its slope nor the slope's change jumps.
    // Stopping dead is the stair-step a machine makes.
    wantSpeed = 0.0;
    easing = true;
    travelling = false;
    pullDown = releaseRide = false;   // a fast hand still brakes fast (fastHand) until it rests
}

void AutoLeveler::moveTowards (double goal, bool countWaits) noexcept
{
    easing = false;   // the hand takes the fader again from where it glided to
    const double error = goal - gain;
    const double size = std::abs (error);
    const bool up = error > 0.0;

    if (travelling && (size < 0.1 || up != travelUp))
    {
        // Landed: if the song keeps going the same way (a crescendo, a long fade in the music) within 3 s, the hand
        // follows on without a new wait - one continuous ride, not move-stop-move. Turned round: a new move waits.
        linger = size < 0.1 ? 30 : 0;
        travelling = false;
        pullDown = false;
        if (travelUp)
            releaseRide = false;   // the ride back up after an effect has landed (a pull-down turned round keeps it)
    }
    if (travelling && ! travelUp && (blaring || snapBack))
    {
        pullDown = fastHand = true;   // an ordinary move down that turns out far too loud speeds up
        snapBack = false;
    }

    if (countWaits)
    {
        upWait = error > 1.5 ? upWait + 1 : 0;
        downWait = error < -1.5 ? downWait + 1 : 0;
    }

    stopAt = goal;

    if (! travelling)
    {
        const bool goBack = releaseRide && up && size > 0.3;   // after an effect: straight back, no wait
        if (! goBack && ! (linger > 0 && up == travelUp && size > 0.3))
        {
            releaseRide = false;
            if (size <= 1.5 && ! (snapBack && ! up))   // a take-back goes back down at once, however little it is
            {
                // Settled: a small difference is followed at 0.1 dB/s - what a hand leaves alone, a machine would chase.
                wantSpeed = size > 1.0e-6 ? std::copysign (0.1, error) : 0.0;
                return;
            }

            // A bigger difference has to last: 3 s before turning up inside a song, 2 s at the start of a new one, 0.5 s
            // before turning down - none when it is far too loud right now (that took 0.7 s to tell from an effect).
            // The first observation starts the clock (the 100 ms before it had no such known difference).
            const int upNeeded = sectionBlocks < 100 ? 20 : 30;
            if ((up && upWait <= upNeeded) || (! up && ! blaring && ! snapBack && downWait <= 5))
            {
                wantSpeed = 0.0;
                return;
            }
        }

        travelling = true;
        travelUp = up;
        pullDown = ! up && (blaring || snapBack);
        fastHand = fastHand || pullDown || goBack;
        if (pullDown)
            snapBack = false;
    }

    // A move in progress goes on until it lands: as fast as the difference asks, slower as it closes (1 dB/s per dB
    // left), so it neither stalls short of the target nor overshoots it. Up 2 dB/s, rising without a step to 4 dB/s
    // when far too quiet (a new song much quieter than the last); down 4 dB/s, rising to 10 dB/s when far too loud
    // (a hand pulls a blaring fader down fast, then eases in).
    const double cap = up ? juce::jlimit (2.0, 4.0, 2.0 + 0.25 * (size - 8.0))
                          : juce::jlimit (4.0, 10.0, 4.0 + 1.5 * (size - 6.0));
    wantSpeed = pullDown ? -juce::jlimit (0.1, pullDownSpeed, pullDownRate * size)
              : releaseRide && up ? juce::jlimit (0.1, 20.0, 3.0 * size)   // back up after an effect: 3 dB/s per dB, at most 20
              : std::copysign (juce::jlimit (0.1, cap, size), error);
}

void AutoLeveler::finishMeasurement() noexcept
{
    double power = energy / (double) measured;
    if (! std::isfinite (power))
    {
        power = 0.0;
        kLeft.reset();   // whatever got in, the filters start clean
        kRight.reset();
    }
    const double wanted = target.load (std::memory_order_relaxed);

    // What counts as program: at the start of a section, nothing under 25 LU below the target (room tone, hiss and
    // breath are not boosted); once the song has 3 s of flow, its own soft moments count down to 15 LU under that
    // flow (never under -60 LUFS) - a very quiet song must not look like a string of pauses (a stop-start ride).
    double gateLufs = juce::jmax (-50.0, wanted - 25.0);
    if (flowCount >= 30)
        gateLufs = juce::jmax (-60.0, juce::jmin (gateLufs, flowLoudness() - 15.0));
    const bool active = ! blockHeld && power >= std::pow (10.0, (gateLufs + 0.691) / 10.0);
    shortEnergy[(size_t) shortPos] = active ? power : 0.0;
    shortPos = (shortPos + 1) % (int) shortEnergy.size();
    recentEnergy[(size_t) recentPos] = active ? power : 0.0;
    recentPos = (recentPos + 1) % (int) recentEnergy.size();
    peaks[(size_t) peakPos] = inputPeak;
    peakPos = (peakPos + 1) % (int) peaks.size();

    if (active)
    {
        inactive = 0;
        sectionBlocks = juce::jmin (sectionBlocks + 1, 100000);
        flowEnergy[(size_t) flowPos] = power;
        flowPos = (flowPos + 1) % (int) flowEnergy.size();
        flowCount = juce::jmin (flowCount + 1, (int) flowEnergy.size());
    }
    else if (++inactive >= 20)
    {
        // A new section must not borrow even the last second of the preceding song's S window, nor its decision.
        shortEnergy.fill (0.0);
        flowEnergy.fill (0.0);
        flowCount = flowPos = 0;
        inactive = 20;
        haveDecision = travelling = pullDown = false;
        upWait = downWait = linger = sectionBlocks = urgentRun = urgentLatch = 0;
        forgetBlare();   // its held blocks were in the flow that has just gone
        peaks.fill (0.0f);   // nor the last song's peaks: one of its effects and one of the new song's are no pattern
        peakPos = 0;
        earlyHold = -100.0;
        if (inactive == 20)
        {
            kLeft.reset();
            kRight.reset();
        }
    }

    // the clocks of 'go on in the same direction' and 'far too loud': they run out in time, whatever the branch below
    if (linger > 0)
        --linger;
    if (urgentLatch > 0)
        --urgentLatch;

    // 'far too loud' is blocks in a row: a gap, a quieter block or the user's own fade starts the count again
    const double applied = 20.0 * std::log10 (linear);
    urgentRun = active && -0.691 + 10.0 * std::log10 (power) + applied > wanted + 8.0 ? urgentRun + 1 : 0;

    const bool blareBlock = active && power > blareLoud;
    if (blare)
    {
        ++blareBlocks;
        if (blockHeld)
            forgetBlare();            // the user's own fade: theirs to judge
        else if (blareBlock)
            holdBackBlareBlock();
        if (blare && blareBlocks > 50 && (blareBlock || blareBlocks > 70))
            endBlareAsMaterial();     // still loud after 5 s (anything but back after 7 s): loud material, not an effect
    }
    else if (blareEnding > 0)
    {
        ++blareBlocks;
        --blareEnding;
        if (blockHeld)
            forgetBlare();
        else if (blareBlock)
            takeBackRelease();        // loud again within 1.5 s: it was a rest in a loud song
        else if (blareEnding == 0)
            forgetBlare();            // it stays over: its held blocks go for good
    }
    else if (active && urgentRun == 7 && flowCount >= 37)
    {
        startBlare();
    }

    // the last 0.4 s heard (active blocks): 'far too loud right now', and whether a blare is over
    int quick = 0;
    double quickSum = 0.0;
    for (const double p : recentEnergy)
        if (p > 0.0) { quickSum += p; ++quick; }
    const double momentary = quick > 0 ? -0.691 + 10.0 * std::log10 (quickSum / quick) : -100.0;
    if (blare)
    {
        const double songPower = std::pow (10.0, (blareReference + 3.0 + 0.691) / 10.0);
        quietRun = active ? (power <= songPower ? quietRun + 1 : 0) : quietRun;   // silence neither counts nor breaks it
        sinceLoud = active && power > songPower ? 0 : sinceLoud + 1;
        if (quietRun >= 5)
            endBlareAsEffect();      // 0.5 s back at the song's level: it was a sound effect
    }
    blaring = urgentRun >= 7;

    int count = 0;
    for (const double p : shortEnergy)
        if (p > 0.0)
            ++count;

    if (active && count >= 8)
    {
        const double shortLufs = -0.691 + 10.0 * std::log10 (programPower (shortEnergy.data(), (int) shortEnergy.size(), shortPos,
                                                                            (int) shortEnergy.size(), shortScratch.data(), false));
        const double flowLufs = flowCount >= 30 ? flowLoudness() : shortLufs;

        // The song's flow sets the level. A louder stretch comes down in seconds, without the flow having to forget the
        // quieter part first: the last 3 s may not run more than 1.5 LU over the target - when most of those 3 s was
        // really that loud. A few sound effects make the average jump but not most of the blocks: they stay untouched.
        double goal = wanted - flowLufs;
        int loudBlocks = 0;
        const double loudPower = std::pow (10.0, (wanted + 1.5 - applied + 0.691) / 10.0);
        for (const double p : shortEnergy)
            if (p > 0.0 && p > loudPower) ++loudBlocks;
        if (loudBlocks >= juce::jmax (10, (count + 1) / 2))
            goal = juce::jmin (goal, wanted + 1.5 - shortLufs);

        // Far too loud right now and staying so (7 blocks in a row, each heard more than 8 LU over the target - counted
        // block by block, so an effect does not last longer than it is, and a 0.5 s effect touches at most 6 blocks
        // wherever it falls; e.g. a blaring song straight after a quiet one): come down at once to 1 LU over the last
        // 0.4 s, and keep that ceiling for 3 s, until the 3 s window has caught up. A shorter blast (an effect, two
        // with a gap between them) is the limiter's job, not the hand's.
        urgent = false;
        if (blare && takeBackTo > -99.0)
            goal = juce::jmin (goal, takeBackTo + (wanted - releaseTarget));   // a rest in a loud song let go too soon: back where it was (after a stop too)
        if (quick >= 3)
        {
            if (urgentRun >= 7)
                urgentLatch = 30;
            if (urgentLatch > 0 || blare)   // a blare keeps the ceiling until it is clear what it was
            {
                goal = juce::jmin (goal, wanted + 1.0 - momentary);
                urgent = true;
            }
        }
        else if (blare && blareBlock && haveDecision)
        {
            goal = juce::jmin (goal, desired + (wanted - desiredTarget));   // still blaring after a stop: the last ceiling holds until 0.3 s is heard again
        }

        goal = juce::jlimit (-20.0, 12.0, goal);
        // Headroom for a boost: peaks that keep coming (taps, drums - the same level at least 3 s apart) keep the limiter
        // under 3 dB of work. One effect's peaks (a bang, a knock-knock-knock) are the limiter's job: by the time the hand
        // could move the effect is over, and only the music would dip.
        const float peak = recurringPeak (30);
        if (peak >= 0.001f)
            goal = juce::jmin (goal, 2.0 - 20.0 * std::log10 ((double) peak));
        // A louder level reached twice at least 1 s apart (more than one effect's own blocks) and heard again within the
        // last 3 s may be a pattern the 3 s test cannot see yet - a song's first beats, drums coming in. Until that test
        // takes it over, or it stops coming back (a knock-knock-knock), it bounds the ride up: below it the hand goes no
        // higher, above it the hand stops where it can - never pulled down by it.
        // the highest such level: reached by a pair and, at least as loud, within the last 3 s (an older, louder pair
        // that has gone quiet does not hide the taps still coming just under it)
        const float early = juce::jmin (recurringPeak (10), recentPeak (30));
        if (early > peak && early >= 0.001f)
        {
            const double cap = 2.0 - 20.0 * std::log10 ((double) early);
            if (cap >= gain)
            {
                earlyHold = -100.0;
            }
            else
            {
                if (earlyHold < gain)   // none yet, or one the hand is already above: it never pulls the hand down
                {
                    earlyHold = gain + (slewSpeed > 0.0 ? slewSpeed * slewSpeed / (2.0 * (fastHand ? pullDownBraking : handAcceleration)) : 0.0);
                    earlyHoldRide = travelling && travelUp;
                }
                if (earlyHoldRide)
                    linger = juce::jmax (linger, 30);   // a ride it stopped goes on at once when it lets go, no new wait
            }
            goal = juce::jmin (goal, cap >= gain ? cap : earlyHold);
        }
        else
        {
            earlyHold = -100.0;
        }

        desired = goal;
        desiredTarget = wanted;
        haveDecision = true;
        moveTowards (goal, true);
        moving = true;
    }
    else if (! blockHeld && active && releaseRide)
    {
        // back up after an effect that filled the S window: towards the song's own level until there is S to judge -
        // worked out afresh every block (the target may change on the way)
        desired = releaseGoal();
        desiredTarget = wanted;
        moveTowards (desired, false);
        moving = true;
    }
    else if (! blockHeld && haveDecision && inactive > 0 && inactive < 5)
    {
        // A gap shorter than 0.5 s inside the music (between words, a rest): the hand keeps going, the waits pause - to where
        // the last decision put it, moved along with the target if that has changed since.
        moveTowards (desired + (wanted - desiredTarget), false);
        moving = true;
    }
    else if (! blockHeld && inactive >= 20 && ! paused.load (std::memory_order_relaxed))
    {
        // Stopped for more than 2 s (nothing paused): the hand drifts back towards 0 dB at 0.5 dB/s, so a cue fired
        // minutes later does not inherit a boost (or a cut) made for another song. A short gap barely moves it.
        easing = false;
        travelling = false;
        stopAt = 0.0;
        wantSpeed = std::abs (gain) > 1.0e-6 ? std::copysign (0.5, -gain) : 0.0;
        moving = true;
    }
    else
    {
        easeOut();   // longer quiet, the user's own fade, or too little to judge yet: glide to a halt
    }
    measured = 0;
    energy = 0.0;
    inputPeak = 0.0f;
    blockHeld = false;
}

void AutoLeveler::startBlare() noexcept
{
    // the 7 loud blocks just heard are the flow's newest entries: they leave it and wait
    const int size = (int) flowEnergy.size();
    blareHeldCount = 0;
    for (int k = 7; k >= 1; --k)
    {
        const int slot = (flowPos - k + size) % size;
        blareHeld[(size_t) blareHeldCount] = flowEnergy[(size_t) slot];
        blareSlot[(size_t) blareHeldCount++] = slot;
        flowEnergy[(size_t) slot] = 0.0;
    }
    blareReference = flowLoudness();
    blareLoud = std::pow (10.0, (blareReference + 6.0 + 0.691) / 10.0);
    blareBlocks = 7;
    blare = true;
}

void AutoLeveler::holdBackBlareBlock() noexcept
{
    if (blareHeldCount >= (int) blareHeld.size())
        return;
    const int size = (int) flowEnergy.size();
    const int slot = (flowPos - 1 + size) % size;   // this block's own entry
    blareHeld[(size_t) blareHeldCount] = flowEnergy[(size_t) slot];
    blareSlot[(size_t) blareHeldCount++] = slot;
    flowEnergy[(size_t) slot] = 0.0;
}

void AutoLeveler::endBlareAsEffect() noexcept
{
    // forget it: its blocks leave the S and 0.4 s windows too, its ceiling goes, and the fader goes straight back up
    const int size = (int) shortEnergy.size();
    for (int i = 0; i < juce::jmin (blareBlocks, size); ++i)
    {
        auto& p = shortEnergy[(size_t) ((shortPos - 1 - i + 2 * size) % size)];
        if (p > blareLoud)
            p = 0.0;
    }
    for (auto& p : recentEnergy)
        if (p > blareLoud)
            p = 0.0;
    // its peaks leave the history too (one long effect's own peaks are no pattern to cap a boost): from its first block
    // (blareBlocks - 1 blocks ago) to its last one over the song's level - the song after it keeps its own
    const int peakSize = (int) peaks.size();
    for (int age = sinceLoud; age < juce::jmin (blareBlocks, peakSize); ++age)
        peaks[(size_t) ((peakPos - 1 - age + 2 * peakSize) % peakSize)] = 0.0f;
    urgentLatch = urgentRun = 0;
    urgent = false;
    releaseRide = true;
    releaseFrom = gain;
    releaseTarget = desiredTarget = target.load (std::memory_order_relaxed);
    desired = releaseGoal();
    haveDecision = true;
    // over - unless it is loud again within 1.5 s: until then its held blocks are kept for taking it back
    blare = false;
    blareEnding = 15;
    quietRun = 0;
}

void AutoLeveler::takeBackRelease() noexcept
{
    // a rest in a loud song, not the end of an effect: the blare goes on and the fader goes straight back down
    blare = true;
    blareEnding = 0;
    releaseRide = false;
    urgentLatch = 30;
    snapBack = true;
    takeBackTo = releaseFrom;   // back to where the hand was, however little it rose
    holdBackBlareBlock();   // this loud block too
}

double AutoLeveler::releaseGoal() noexcept
{
    double back = juce::jlimit (-20.0, 12.0, target.load (std::memory_order_relaxed) - flowLoudness());
    const float recurring = recurringPeak (30);
    if (recurring >= 0.001f)
        back = juce::jmin (back, 2.0 - 20.0 * std::log10 ((double) recurring));
    return back;
}

void AutoLeveler::endBlareAsMaterial() noexcept
{
    // loud material: its blocks take their places in the flow (at most 64 blocks back, the flow keeps 150), and the
    // ceiling stays a little longer while the flow catches up
    for (int i = 0; i < blareHeldCount; ++i)
        flowEnergy[(size_t) blareSlot[(size_t) i]] = blareHeld[(size_t) i];
    urgentLatch = 30;
    forgetBlare();
}

void AutoLeveler::forgetBlare() noexcept
{
    blare = snapBack = false;
    blareBlocks = blareHeldCount = blareEnding = quietRun = sinceLoud = 0;
    releaseFrom = takeBackTo = -100.0;
}

float AutoLeveler::recurringPeak (int apart) const noexcept
{
    const int size = (int) peaks.size();
    decltype (peaks) byAge {};   // 0 = the newest block
    for (int age = 0; age < size; ++age)
        byAge[(size_t) age] = peaks[(size_t) ((peakPos - 1 - age + 2 * size) % size)];

    // the highest level reached twice 'apart' blocks or more apart: 'older' = the largest peak that much older
    float older = 0.0f, recurring = 0.0f;
    for (int age = size - 1 - apart; age >= 0; --age)
    {
        older = juce::jmax (older, byAge[(size_t) (age + apart)]);
        recurring = juce::jmax (recurring, juce::jmin (byAge[(size_t) age], older));
    }
    return recurring;
}

float AutoLeveler::recentPeak (int blocks) const noexcept
{
    const int size = (int) peaks.size();
    float loudest = 0.0f;
    for (int age = 0; age < juce::jmin (blocks, size); ++age)
        loudest = juce::jmax (loudest, peaks[(size_t) ((peakPos - 1 - age + 2 * size) % size)]);
    return loudest;
}

double AutoLeveler::flowLoudness() noexcept
{
    if (flowCount == 0)
        return -100.0;

    // in time order: from the section's first block until the ring is full, then from its oldest entry
    const int size = (int) flowEnergy.size();
    const double power = programPower (flowEnergy.data(), size, flowCount < size ? 0 : flowPos, flowCount, flowScratch.data(), true);
    return power > 0.0 ? -0.691 + 10.0 * std::log10 (power) : -100.0;
}

void AutoLeveler::planGainRamp() noexcept
{
    rampLeft = juce::jmin (32, blockLength - measured);

    // The hand moves the slew continuously: its speed changes by at most handAcceleration (the fast hand of a pull-down:
    // pullDownAcceleration, braking by pullDownBraking), and it stops where the decision said (landing exactly, never
    // past it).
    const double dt = (double) rampLeft / rate;
    const bool braking = slewSpeed != 0.0 && (wantSpeed * slewSpeed < 0.0 || std::abs (wantSpeed) < std::abs (slewSpeed));
    const double limit = ! fastHand ? handAcceleration
                       : braking ? pullDownBraking
                       : releaseRide ? releaseAcceleration : pullDownAcceleration;
    slewSpeed += juce::jlimit (-limit * dt, limit * dt, wantSpeed - slewSpeed);
    const double before = gain;
    gain += slewSpeed * dt;
    if ((slewSpeed > 0.0 && before <= stopAt && gain > stopAt) || (slewSpeed < 0.0 && before >= stopAt && gain < stopAt))
    {
        gain = stopAt;
        slewSpeed = wantSpeed = 0.0;
    }

    if (fastHand && ! pullDown && ! releaseRide && std::abs (slewSpeed) < 0.05)
        fastHand = false;   // at rest after a pull-down (or the way back up): the ordinary hand again

    // Exact evolution of two identical continuous one-poles for a constant G, then <=32-sample
    // linear interpolation of their linear-gain endpoints. No sample-rate-dependent time constants.
    const double t = (double) rampLeft / (rate * 0.150);
    const double a = std::exp (-t);
    smooth2 = gain + (smooth2 - gain + (smooth1 - gain) * t) * a;
    smooth1 = gain + (smooth1 - gain) * a;
    linearStep = (std::pow (10.0, smooth2 / 20.0) - linear) / rampLeft;
}

void AutoLeveler::fillDelay (const juce::AudioBuffer<float>& buffer, int start, int count) noexcept
{
    while (count > 0)
    {
        const int n = juce::jmin (count, delayLength - delayPos);
        for (int ch = 0; ch < channels; ++ch)
        {
            if (ch < buffer.getNumChannels())
            {
                delay.copyFrom (ch, delayPos, buffer, ch, start, n);
                auto* kept = delay.getWritePointer (ch, delayPos);
                for (int i = 0; i < n; ++i)
                    if (! std::isfinite (kept[i]))
                        kept[i] = 0.0f;   // the bypass passes it on untouched; the switch-on must not play it again
            }
            else
            {
                delay.clear (ch, delayPos, n);
            }
        }
        delayPos = (delayPos + n) % delayLength;
        start += n;
        count -= n;
    }
}

double AutoLeveler::limit (float peak) noexcept
{
    const int capacity = (int) maximum.size();
    while (maxCount > 0 && maximum[(size_t) maxHead].sample < sampleIndex - delayLength)
    {
        maxHead = (maxHead + 1) % capacity;
        --maxCount;
    }
    while (maxCount > 0 && maximum[(size_t) ((maxHead + maxCount - 1) % capacity)].value <= peak)
        --maxCount;
    maximum[(size_t) ((maxHead + maxCount++) % capacity)] = { peak, sampleIndex++ };
    const double largest = maximum[(size_t) maxHead].value;
    const double required = largest > ceiling ? ceiling / largest : 1.0;
    releaseGain = required < releaseGain ? required : required + releaseCoefficient * (releaseGain - required);
    attackSum += releaseGain - attack[(size_t) attackPos];
    attack[(size_t) attackPos] = releaseGain;
    attackPos = (attackPos + 1) % delayLength;
    return juce::jlimit (0.0, 1.0, attackSum / delayLength);
}

void AutoLeveler::process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
{
    if (numSamples <= 0 || channels == 0) return;
    const juce::ScopedNoDenormals noDenormals;
    const bool on = enabled.load (std::memory_order_relaxed);
    const bool held = hold.load (std::memory_order_relaxed);
    if (on && ! wasEnabled && wet == 0.0)   // fully off before: start fresh (back on mid-fade-out: carry on, no jump)
    {
        clearMeasurement();
        desired = gain = smooth1 = smooth2 = 0.0;
        slewSpeed = wantSpeed = stopAt = 0.0;
        linear = releaseGain = 1.0;
        linearStep = 0.0;
        rampLeft = maxHead = maxCount = attackPos = 0;
        sampleIndex = 0;
        std::fill (attack.begin(), attack.end(), 1.0);
        attackSum = (double) delayLength;
    }
    wasEnabled = on;
    if (! on && moving) freezeGain();
    if (on && held) easeOut();   // the user's own fade starts now: let go at once, without a corner
    if (! on && wet == 0.0)
    {
        fillDelay (buffer, 0, numSamples); // bypass is zero latency and bit-for-bit, including signed zero
        displayedGain.store (0.0, std::memory_order_relaxed);
        return;
    }
    const int used = juce::jmin (channels, buffer.getNumChannels());
    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < used; ++ch)
            if (! std::isfinite (buffer.getSample (ch, i)))
                buffer.setSample (ch, i, 0.0f);   // NaN / Inf: silence instead of a measurement that never recovers

        if (on)
        {
            const double left = used > 0 ? kLeft.process (buffer.getSample (0, i)) : 0.0;
            const double right = used > 1 ? kRight.process (buffer.getSample (1, i)) : 0.0;
            energy += left * left + right * right;
            blockHeld = blockHeld || held;
            if (moving)   // also while held: the glide that lets go finishes
            {
                if (rampLeft == 0) planGainRamp();
                linear += linearStep;
                --rampLeft;
            }
        }
        float peak = 0.0f;
        for (int ch = 0; ch < used; ++ch)
        {
            const float raw = buffer.getSample (ch, i);
            inputPeak = juce::jmax (inputPeak, std::abs (raw));
            peak = juce::jmax (peak, std::abs ((float) (raw * linear)));
        }
        const double attenuation = limit (peak);
        wet = on ? juce::jmin (1.0, wet + 1.0 / transitionLength) : juce::jmax (0.0, wet - 1.0 / transitionLength);
        if (wet < 1.0e-12) wet = 0.0;
        if (wet > 1.0 - 1.0e-12) wet = 1.0;
        for (int ch = 0; ch < used; ++ch)
        {
            const float raw = buffer.getSample (ch, i);
            const float delayed = delay.getSample (ch, delayPos);
            delay.setSample (ch, delayPos, (float) (raw * linear));
            const float limited = juce::jlimit (-ceiling, ceiling, (float) (delayed * attenuation));
            buffer.setSample (ch, i, wet == 0.0 ? raw : wet == 1.0 ? limited : (float) (raw + wet * (limited - raw)));
        }
        delayPos = (delayPos + 1) % delayLength;
        if (on && ++measured == blockLength) finishMeasurement();
        if (! on && wet == 0.0)
        {
            fillDelay (buffer, i + 1, numSamples - i - 1);
            break;
        }
    }
    displayedGain.store (wet > 0.0 ? 20.0 * std::log10 (linear) : 0.0, std::memory_order_relaxed);
}

} // namespace gocue
