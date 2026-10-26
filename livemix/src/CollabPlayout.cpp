#include "CollabPlayout.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace gocue::livemix::collab
{
void CollabPlayout::prepare (double rate, int maxBlock, uint32_t roomRate)
{
    if (! isValidWireRate (roomRate)) return;
    wireRate = roomRate; packetMs = packetDurationMs (wireRate);
    deviceRate = rate; blockSize = std::max (1, maxBlock);
    slots = std::make_unique<Slot[]> (slotCount);
    asrcStorage.resize ((lm_asrc_size (channels) + sizeof (double) - 1) / sizeof (double));
    asrc = reinterpret_cast<lm_asrc*> (asrcStorage.data());
    lm_asrc_init (asrc, channels, wireRate, rate);
    output.assign ((size_t) blockSize * channels, 0.0f);
    readyFrames.store (0); publishedEpoch.store (0); cursorEpoch.store (0); cursor.store (0);
    producerEpoch = audioEpoch = 0; sender = 0;
    seenRebuffer = rebufferRequest.load(); seenMeasurement = measurementRequest.load();
    measuring = true; delayCount = 0; measuredFloor.store (packetMs); measuredVersion.store (0); seenVersion = 0;
    target = 10.0; floor = packetMs; stableSeconds = lossClock = 0.0; holdFloor = holdSeconds = 0.0; lossBucket = 0;
    recentTotal.fill (0); recentLost.fill (0);
    received.store (0); lost.store (0); late.store (0); underruns.store (0);
    playing = false; packetPosition = framesPerPacket; packetReal = concealed = false;
    lastInput.fill (0); lastOutput.fill (0); releaseTail.fill (0);
    fadeIn = releaseRemaining = 0; releaseFrames = std::max (1, (int) std::lround (rate * 0.005));
    resetDrift(); state.store (State::buffering); targetMs.store (target); fillMs.store (0); lossPercent.store (0);
}

void CollabPlayout::resetDrift() noexcept
{
    lm_drift_init (&drift, target * wireRate / 1000.0, deviceRate / std::max (1, blockSize));
    // Headroom beyond a +/-1000 ppm device is needed to recover its acquisition offset.
    drift.max_ppm = 2000.0;
    drift.acquire_slew_per_s = 2000.0;
    drift.slew_per_s = 2000.0;
    drift.kp = 6.0;
    drift.ki = 0.3;
    lm_asrc_set_correction_ppm (asrc, 0); ppm.store (0);
}

void CollabPlayout::measure (const Header& h, uint64_t now) noexcept
{
    const auto request = measurementRequest.load();
    if (request != seenMeasurement) { seenMeasurement = request; delayCount = 0; measuring = true; }
    if (! measuring) return;
    const double frameTime = (double) h.firstFrame * 1000.0 / wireRate;
    for (size_t i = 0; i < delayCount; ++i) if (frameTimes[i] == frameTime) return;
    if (delayCount == 0) measurementStart = now;
    const double relative = (double) now / 1.0e6 - frameTime;
    if (delayCount < delays.size())
    {
        frameTimes[delayCount] = frameTime;
        delays[delayCount++] = relative;
    }
    if (now - measurementStart < 1000000000ULL || delayCount < 2) return;
    // On a steady link, fit the clock slope before packet quantisation can mask a slow negative
    // clock for tens of seconds. Only a complete, almost perfectly linear window may seed PI:
    // a loss or arrival jitter (residual > one wire sample) leaves acquisition entirely to fill.
    double sx = 0, sy = 0, xx = 0, xy = 0, low = frameTimes[0], high = low;
    for (size_t i = 0; i < delayCount; ++i)
    {
        const double x = frameTimes[i] - frameTimes[0], y = delays[i] - delays[0];
        sx += x; sy += y; xx += x * x; xy += x * y;
        low = std::min (low, frameTimes[i]); high = std::max (high, frameTimes[i]);
    }
    const double variance = xx - sx * sx / delayCount;
    const double slope = variance > 0 ? (xy - sx * sy / delayCount) / variance : 0;
    const double intercept = (sy - slope * sx) / delayCount;
    double residual = 0;
    for (size_t i = 0; i < delayCount; ++i)
        residual = std::max (residual, std::abs (delays[i] - delays[0] - intercept - slope * (frameTimes[i] - frameTimes[0])));
    const double estimate = (1.0 / (1.0 + slope) - 1.0) * 1.0e6;
    const bool complete = std::abs (high - low - (delayCount - 1) * packetMs) < 0.001;
    measuredClock.store (complete && residual < 1000.0 / wireRate && std::abs (estimate) <= 1500.0
                         ? estimate : std::numeric_limits<double>::quiet_NaN());
    std::sort (delays.begin(), delays.begin() + delayCount);
    const auto percentile = (size_t) std::ceil (0.99 * (double) delayCount) - 1;
    measuredFloor.store (std::clamp (delays[percentile] - delays[0] + packetMs, packetMs, 80.0));
    measuredVersion.fetch_add (1); measuring = false;
}

bool CollabPlayout::push (const Header& h, const uint8_t* payload, uint64_t arrivalNs) noexcept
{
    if (! slots || payload == nullptr || h.kind != Kind::audio || h.frames != framesPerPacket
        || h.payloadBytes != audioBytes || h.firstFrame % framesPerPacket != 0) return false;
    return pushPacket (h, payload, arrivalNs, false);
}

bool CollabPlayout::pushV2 (const contracts::HeaderV2& h, const uint8_t* payload, uint64_t arrivalNs) noexcept
{
    if (! slots || payload == nullptr || h.kind != contracts::Kind::audio || ! validHeaderV2 (h)) return false;
    Header packetHeader;
    packetHeader.sender = h.origin; packetHeader.seq = h.seq; packetHeader.firstFrame = h.firstFrame;
    packetHeader.frames = h.frames; packetHeader.payloadBytes = h.payloadBytes;
    return pushPacket (packetHeader, payload, arrivalNs, h.flags == contracts::Flags::monoPcm16);
}

bool CollabPlayout::pushPacket (const Header& h, const uint8_t* payload, uint64_t arrivalNs, bool mono) noexcept
{
    if (producerEpoch == 0 || sender != h.sender)
    {
        sender = h.sender; ++producerEpoch;
        delayCount = 0; measuring = true;
    }
    // Late packets belong to the delay distribution too, but duplicates must not weight its tail.
    measure (h, arrivalNs);
    if (cursorEpoch.load() == producerEpoch && h.firstFrame < cursor.load()) { late.fetch_add (1); return false; }
    auto& slot = slots[(h.firstFrame / framesPerPacket) % slotCount];
    unsigned expected = slot.state.load (std::memory_order_acquire);
    if (expected == 1 || ! slot.state.compare_exchange_strong (expected, 1, std::memory_order_acquire)) return false;
    if (expected == 2)
    {
        if (slot.epoch == producerEpoch && slot.frame == h.firstFrame)
        {
            slot.state.store (2, std::memory_order_release); return false;
        }
        readyFrames.fetch_sub (framesPerPacket);
    }
    slot.frame = h.firstFrame; slot.epoch = producerEpoch;
    if (mono)
    {
        std::array<float, framesPerPacket> samples;
        unpackPcm16 (payload, samples.data(), framesPerPacket);
        for (size_t i = 0; i < samples.size(); ++i) slot.pcm[i * 2] = slot.pcm[i * 2 + 1] = samples[i];
    }
    else unpackPcm24 (payload, slot.pcm.data(), framesPerPacket);
    readyFrames.fetch_add (framesPerPacket);
    slot.state.store (2, std::memory_order_release);
    publishedEpoch.store (producerEpoch);
    // The consumer may have crossed this packet while its slot was being filled. Retire it now,
    // otherwise stale PCM would incorrectly inflate fill until the ring wrapped around.
    if (cursorEpoch.load() == producerEpoch && h.firstFrame < cursor.load())
    {
        unsigned ready = 2;
        if (slot.state.compare_exchange_strong (ready, 1, std::memory_order_acquire))
        {
            readyFrames.fetch_sub (framesPerPacket); slot.state.store (0, std::memory_order_release); late.fetch_add (1);
            return false;
        }
    }
    received.fetch_add (1);
    return true;
}

int CollabPlayout::queued() const noexcept
{
    return std::max (0, readyFrames.load()) + (packetReal ? framesPerPacket - packetPosition : 0);
}

void CollabPlayout::beginRefill() noexcept
{
    const bool hadAudio = playing || lastOutput[0] != 0.0f || lastOutput[1] != 0.0f;
    playing = false; packetPosition = framesPerPacket; packetReal = false;
    releaseTail = lastOutput;
    releaseRemaining = hadAudio ? releaseFrames : 0;
    fadeIn = framesPerPacket; concealed = false; stableSeconds = 0;
    lm_asrc_reset (asrc);
    // Pause PI across a discontinuity without losing the learned device clock.
    drift.lp = (drift.ppm - drift.integ) / drift.kp; drift.primed = 1;
    state.store (State::buffering);
}

void CollabPlayout::resetPlayback (uint64_t epoch, bool newSender) noexcept
{
    audioEpoch = epoch;
    beginRefill();
    for (size_t i = 0; i < slotCount; ++i)
    {
        auto& slot = slots[i]; unsigned expected = 2;
        if (! slot.state.compare_exchange_strong (expected, 1, std::memory_order_acquire)) continue;
        const bool discard = newSender ? slot.epoch != epoch : true;
        if (discard) readyFrames.fetch_sub (framesPerPacket);
        slot.state.store (discard ? 0 : 2, std::memory_order_release);
    }
    if (newSender)
    {
        cursorEpoch.store (0);
        target = 10.0; floor = packetMs; holdFloor = holdSeconds = 0.0; resetDrift();
        recentTotal.fill (0); recentLost.fill (0); lossPercent.store (0);
    }
}

bool CollabPlayout::startPlayback (int frames) noexcept
{
    // Fill is measured AFTER each device block. Reserve its consumption while priming, so a large
    // device period does not repeatedly empty a perfectly steady one-packet network buffer.
    const double reserve = frames * (double) wireRate / deviceRate * (1.0 + drift.ppm * 1.0e-6);
    if (releaseRemaining > 0 || queued() < target * wireRate / 1000.0 + reserve) return false;
    uint64_t earliest = std::numeric_limits<uint64_t>::max();
    for (size_t i = 0; i < slotCount; ++i)
    {
        auto& slot = slots[i]; unsigned expected = 2;
        if (! slot.state.compare_exchange_strong (expected, 1, std::memory_order_acquire)) continue;
        if (slot.epoch == audioEpoch) earliest = std::min (earliest, slot.frame);
        slot.state.store (2, std::memory_order_release);
    }
    if (earliest == std::numeric_limits<uint64_t>::max()) return false;
    // A target reduction or a long callback pause seeks at packet boundaries after the release.
    // Do not ask the clock controller to burn off a network backlog by changing pitch.
    // Centre the operating point within a datagram's fill sawtooth. Retain a partial first
    // packet as well, so a refill does not introduce an arbitrary whole-packet PI offset.
    const int wanted = (int) std::ceil (target * wireRate / 1000.0 + reserve + framesPerPacket * 0.5);
    if (queued() < wanted) return false;
    for (size_t i = 0; i < slotCount && queued() >= wanted + framesPerPacket; ++i, earliest += framesPerPacket)
    {
        auto& slot = slots[(earliest / framesPerPacket) % slotCount]; unsigned expected = 2;
        if (! slot.state.compare_exchange_strong (expected, 1, std::memory_order_acquire)) continue;
        const bool discard = slot.epoch == audioEpoch && slot.frame == earliest;
        if (discard) readyFrames.fetch_sub (framesPerPacket);
        slot.state.store (discard ? 0 : 2, std::memory_order_release);
    }
    nextFrame = earliest; cursor.store (nextFrame); cursorEpoch.store (audioEpoch);
    skipAtStart = std::clamp (queued() - wanted, 0, framesPerPacket - 1);
    playing = true; fadeIn = framesPerPacket; state.store (State::playing);
    return true;
}

void CollabPlayout::recordPacket (bool missing) noexcept
{
    ++recentTotal[lossBucket];
    if (missing) { ++recentLost[lossBucket]; lost.fetch_add (1); }
}

bool CollabPlayout::loadPacket() noexcept
{
    cursor.store (nextFrame + framesPerPacket);
    auto& slot = slots[(nextFrame / framesPerPacket) % slotCount];
    unsigned expected = 2;
    bool found = false;
    if (slot.state.compare_exchange_strong (expected, 1, std::memory_order_acquire))
    {
        found = slot.epoch == audioEpoch && slot.frame == nextFrame;
        if (found) { packet = slot.pcm; readyFrames.fetch_sub (framesPerPacket); }
        slot.state.store (found ? 0 : 2, std::memory_order_release);
    }
    if (! found && readyFrames.load() == 0)
    {
        recordPacket (true); underruns.fetch_add (1);
        target = std::min (120.0, target + packetMs); holdFloor = target; holdSeconds = underrunHoldSeconds; beginRefill();
        return false;
    }
    recordPacket (! found);
    const int start = skipAtStart; skipAtStart = 0;
    if (! found)
    {
        concealed = true; stableSeconds = 0;
        for (int i = 0; i < framesPerPacket; ++i)
            for (int ch = 0; ch < channels; ++ch)
                packet[(size_t) i * channels + ch] = lastInput[(size_t) ch] * (1.0f - (float) (i + 1) / framesPerPacket);
        fadeIn = framesPerPacket;
    }
    else if (fadeIn > 0)
    {
        for (int i = start; i < framesPerPacket && fadeIn > 0; ++i)
        {
            const float gain = 1.0f - (float) --fadeIn / framesPerPacket;
            for (int ch = 0; ch < channels; ++ch) packet[(size_t) i * channels + ch] *= gain;
        }
    }
    lastInput = { packet[packet.size() - 2], packet.back() };
    packetPosition = start; packetReal = found;
    nextFrame += framesPerPacket;
    return true;
}

void CollabPlayout::pull (float* left, float* right, int frames) noexcept
{
    if (frames <= 0) return;
    if (asrc == nullptr) { std::fill_n (left, frames, 0.0f); std::fill_n (right, frames, 0.0f); return; }
    const auto epoch = publishedEpoch.load();
    if (epoch != audioEpoch) resetPlayback (epoch, true);
    const auto request = rebufferRequest.load();
    if (request != seenRebuffer)
    {
        seenRebuffer = request; measurementRequest.fetch_add (1);
        resetPlayback (audioEpoch, false); target = 10.0; floor = packetMs; holdFloor = holdSeconds = 0.0;
    }
    const auto measurement = measuredVersion.load();
    if (measurement != seenVersion)
    {
        seenVersion = measurement; floor = measuredFloor.load();
        if (std::abs (target - floor) > packetMs) beginRefill();
        target = floor;
        const double estimate = measuredClock.load();
        if (std::isfinite (estimate))
        {
            drift.integ = drift.ppm = estimate; drift.lp = 0; drift.primed = 1;
            lm_asrc_set_correction_ppm (asrc, estimate);
        }
    }
    for (int offset = 0; offset < frames; offset += blockSize)
    {
        const int n = std::min (blockSize, frames - offset);
        std::fill_n (output.data(), n * channels, 0.0f);
        concealed = false;
        if (! playing) startPlayback (n);
        int made = 0;
        while (playing && made < n)
        {
            if (packetPosition == framesPerPacket && ! loadPacket()) break;
            int used = 0;
            made += lm_asrc_process (asrc, packet.data() + (size_t) packetPosition * channels, framesPerPacket - packetPosition,
                                     &used, output.data() + (size_t) made * channels, n - made);
            packetPosition += used;
        }
        if (! playing && releaseRemaining > 0)
        {
            // A mid-block underrun releases from this block's last sample, never from the preceding block.
            if (made > 0) releaseTail = { output[(size_t) made * 2 - 2], output[(size_t) made * 2 - 1] };
            for (int i = made; i < n && releaseRemaining > 0; ++i)
            {
                const float gain = (float) --releaseRemaining / releaseFrames;
                for (int ch = 0; ch < channels; ++ch) output[(size_t) i * 2 + ch] = releaseTail[(size_t) ch] * gain;
            }
        }
        const double dt = n / deviceRate;
        holdSeconds = std::max (0.0, holdSeconds - dt);
        if (playing && ! concealed && fadeIn == 0)
        {
            stableSeconds += dt;
            // 10/16 two-PC test: each return to the 5.4 ms first-second floor ended in the next delay spike's dropout
            if (stableSeconds >= 30.0)
            {
                target = std::max ({ floor, holdSeconds > 0 ? holdFloor : floor, target - packetMs * 0.5 });
                stableSeconds = 0;
            }
            drift.target = target * wireRate / 1000.0;
            // One datagram is indivisible. A half-packet deadband keeps PI from chasing its
            // arrival phase, which otherwise looks like a slow clock error at small ppm offsets.
            const double error = queued() - drift.target;
            const double clockError = std::copysign (std::max (0.0, std::abs (error) - framesPerPacket * 0.5), error);
            lm_drift_update (&drift, drift.target + clockError, dt);
            lm_asrc_set_correction_ppm (asrc, drift.ppm);
        }
        else stableSeconds = 0;
        lossClock += dt;
        while (lossClock >= 0.1)
        {
            lossClock -= 0.1; lossBucket = (lossBucket + 1) % recentTotal.size();
            recentTotal[lossBucket] = recentLost[lossBucket] = 0;
        }
        for (int i = 0; i < n; ++i) { left[offset + i] = output[(size_t) i * 2]; right[offset + i] = output[(size_t) i * 2 + 1]; }
        lastOutput = { left[offset + n - 1], right[offset + n - 1] };
    }
    uint32_t total = 0, missing = 0;
    for (size_t i = 0; i < recentTotal.size(); ++i) { total += recentTotal[i]; missing += recentLost[i]; }
    lossPercent.store (total == 0 ? 0.0 : missing * 100.0 / total);
    targetMs.store (target); fillMs.store (queued() * 1000.0 / wireRate); ppm.store (drift.ppm);
}

CollabPlayout::Stats CollabPlayout::getStats() const noexcept
{
    return { state.load(), targetMs.load(), fillMs.load(), ppm.load(), lossPercent.load(), received.load(), lost.load(), late.load(), underruns.load() };
}
}
