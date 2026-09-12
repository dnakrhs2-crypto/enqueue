#include "TimelineTransport.h"
#include "VideoPlaybackEngine.h"
#include "support/Platform.h"
#include <algorithm>
#include <cmath>

namespace gocue::recorder
{
const char* transportStateName(TransportState s) noexcept
{
    switch (s)
    {
        case TransportState::stopped: return "stopped"; case TransportState::preparing: return "preparing";
        case TransportState::ready: return "ready"; case TransportState::scheduled: return "scheduled";
        case TransportState::playing: return "playing"; case TransportState::paused: return "paused";
        case TransportState::buffering: return "buffering"; case TransportState::draining: return "draining";
        case TransportState::failed: return "failed";
    }
    return "failed";
}
TimelineTransport::TimelineTransport(std::uint32_t Fs, std::int64_t hz, PlaybackPcmQueue& q, Sample length)
    : rate(Fs), frequency(hz), queue(q), end(length), rtEnd(length)
{ if (!rate || rate > 768000 || hz <= 0 || length < 0) throw std::invalid_argument("Invalid transport timebase"); }
void TimelineTransport::send(Command c)
{
    c.timelineEnd = end;
    if (c.kind == Kind::prepare)
    {
        latestSeek.sequence.fetch_add(1);
        latestSeek.generation.store(c.generation); latestSeek.target.store(c.target); latestSeek.timelineEnd.store(c.timelineEnd);
        latestSeek.sequence.fetch_add(1);
    }
    else if (!commands.push(c)) throw std::runtime_error("Transport command queue full");
    wake->signal();
}
void TimelineTransport::seek(Sample sample)
{
    if (dubbingLocked) throw std::logic_error("더빙 중에는 탐색할 수 없습니다.");
    if (sample < 0) throw std::out_of_range("Negative timeline seek");
    send({Kind::prepare, requestedGeneration + 1, sample, 0});
    ++requestedGeneration; requestedSample = sample; armedGeneration = 0; stopAfterPrepare = false; scrubPending = false;
    timing = {}; timing.request = qpcNow();
}
void TimelineTransport::play()
{
    if (dubbingLocked) throw std::logic_error("더빙 중에는 재생을 변경할 수 없습니다.");
    if (scrubPending) seek(pendingScrub);
    wantPlay = true; const auto s = snapshot();
    if (playhead(s.callbackQpc) >= end)
    {
        wantPlay = false;
        if (s.generation == requestedGeneration) send({Kind::stopped, requestedGeneration, requestedSample, 0});
        return;
    }
    // A Play pressed immediately after Seek must keep that pending target.
    if (s.generation != requestedGeneration) return;
    if (s.state == TransportState::stopped || s.state == TransportState::paused)
        seek(s.outputOrigin >= 0 ? s.submittedEnd : s.frozenSample);
    else if (!requestedGeneration) seek(0);
}
void TimelineTransport::pause()
{
    if (dubbingLocked) throw std::logic_error("더빙 중에는 재생을 변경할 수 없습니다.");
    wantPlay = false;
    // Do not overwrite an unacknowledged Prepare with a Pause for a generation
    // the callback does not own yet. That Prepare already quiesces output.
    if (snapshot().generation == requestedGeneration) send({Kind::pause, requestedGeneration, requestedSample, 0});
}
void TimelineTransport::stop()
{
    if (dubbingLocked) throw std::logic_error("더빙 중에는 재생을 변경할 수 없습니다.");
    wantPlay = false;
    const auto s = snapshot();
    const auto target = playhead(s.callbackQpc);
    seek(target); stopAfterPrepare = true;
}
void TimelineTransport::goToStart() { seek(0); wantPlay = false; }
void TimelineTransport::stagePlan(std::shared_ptr<const CompiledRenderPlan> plan, std::vector<AudioSourceBinding> sources,
                                  std::vector<PlaybackVideoClip> videos, AudioSourceMask mask)
{
    if (dubbingLocked) throw std::logic_error("더빙 중에는 재생 계획을 바꿀 수 없습니다.");
    if (!plan || plan->Fs != rate || plan->timelineEnd < 0) throw std::invalid_argument("Invalid replacement playback plan");
    const auto s = snapshot();
    const auto target = s.generation != requestedGeneration ? requestedSample : audibleCursor(s, rate, frequency, s.callbackQpc);
    auto next = std::make_unique<PendingPlan>(PendingPlan{std::move(plan), std::move(sources), std::move(videos), std::move(mask)});
    const auto oldEnd = end; end = next->plan->timelineEnd;
    try { seek((std::clamp)(target, Sample{0}, end)); }
    catch (...) { end = oldEnd; throw; }
    pendingPlan = std::move(next);
}
void TimelineTransport::scrub(Sample sample, bool released, std::int64_t now)
{
    if (dubbingLocked) throw std::logic_error("더빙 중에는 탐색할 수 없습니다.");
    if (sample < 0) throw std::out_of_range("Negative timeline scrub");
    wantPlay = false; pendingScrub = sample; scrubPending = true;
    ++scrubInputs;
    if (released || !lastScrubQpc || now - lastScrubQpc >= frequency / 15)
    { seek(pendingScrub); lastScrubQpc = now; scrubPending = false; ++scrubDispatches; }
}
void TimelineTransport::prepared(std::int64_t output, bool start)
{
    if (dubbingLocked) throw std::logic_error("더빙 중에는 출력을 변경할 수 없습니다.");
    if (snapshot().generation != requestedGeneration) throw std::logic_error("Prepare requires callback generation acknowledgement");
    if (requestedSample >= end) { start = wantPlay = false; stopAfterPrepare = true; }
    send({start ? Kind::start : stopAfterPrepare ? Kind::stopped : Kind::ready, requestedGeneration, requestedSample, output});
    armedGeneration = requestedGeneration;
    timing.armed = qpcNow();
}
Sample TimelineTransport::playhead(std::int64_t now) const noexcept
{
    if (scrubPending) return pendingScrub;
    const auto s = snapshot();
    if (s.generation != requestedGeneration || s.state == TransportState::preparing) return requestedSample;
    return audibleCursor(s, rate, frequency, now);
}
Sample TimelineTransport::audibleCursor(const TransportSnapshot& s, std::uint32_t Fs, std::int64_t hz,
                                       std::int64_t now, std::int64_t lead) noexcept
{
    if (s.outputOrigin < 0 || !s.callbackQpc || !Fs || hz <= 0) return s.frozenSample;
    // A stalled callback may not extrapolate indefinitely. While draining, the
    // last submitted hardware tail is still audible, including after underrun.
    const auto elapsed = (std::max)(0.0, static_cast<double>(now - s.callbackQpc) + static_cast<double>((std::max)(std::int64_t{0}, lead)));
    const auto maximum = static_cast<Sample>(s.blockFrames) + (std::max)(0, s.outputLatency);
    const auto advance = static_cast<Sample>((std::min)(static_cast<double>(maximum), std::floor(elapsed * Fs / hz)));
    const auto submittedOutputEnd = s.outputOrigin + s.submittedEnd - s.timelineOrigin;
    const auto pendingHardware = submittedOutputEnd - (s.outputSample + advance - s.outputLatency);
    // renderedEnd includes read-ahead. Subtract software queue AND device tail.
    const auto cursor = s.renderedEnd - s.softwareQueuedSamples - pendingHardware;
    return (std::clamp)(cursor, s.timelineOrigin, s.submittedEnd);
}
void TimelineTransport::processOutput(const BlockStamp& stamp, float* l, float* r) noexcept
{
    std::fill_n(l, stamp.numSamples, 0.0f); std::fill_n(r, stamp.numSamples, 0.0f);
    Command command{}; bool received = false;
    for (unsigned i = 0; i < 64; ++i) { Command next{}; if (!commands.pop(next)) break; command = next; received = true; }
    // One bounded read, never a retry loop on ASIO. If the control writer is in
    // flight, keep silence/old state until the next block and adopt only its latest target.
    const auto seq = latestSeek.sequence.load();
    if (!(seq & 1))
    {
        const Command seek{Kind::prepare, latestSeek.generation.load(), latestSeek.target.load(), 0, latestSeek.timelineEnd.load()};
        if (seq == latestSeek.sequence.load() && seek.generation > rt.generation && (!received || seek.generation >= command.generation))
        { command = seek; received = true; }
    }
    if (received)
    {
        // A user pause/seek/stop fades a bounded piece of the OLD prepared PCM
        // before acknowledging quiescence. This runs before the generation is
        // replaced and before the control owner is allowed to reset the queue.
        if ((command.kind == Kind::prepare || command.kind == Kind::pause) && rt.state == TransportState::playing)
        {
            const auto count = static_cast<std::uint32_t>((std::min)({Sample(stamp.numSamples), rtEnd - rt.submittedEnd,
                (std::max)(Sample{1}, Sample(rate) * 3 / 1000)}));
            if (count && queue.consume(rt.submittedEnd, rt.generation, l, r, count))
            {
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const auto gain = static_cast<float>(count - i - 1) / static_cast<float>(count);
                    l[i] *= gain; r[i] *= gain;
                }
                rt.submittedEnd += count;
            }
        }
        if (command.kind == Kind::prepare)
        {
            rtEnd = command.timelineEnd;
            rt.generation = command.generation; rt.frozenSample = command.target;
            rt.timelineOrigin = rt.submittedEnd = command.target; rt.outputOrigin = -1;
            rt.firstBlockQpc = rt.firstAudibleQpc = 0; rt.state = TransportState::preparing;
        }
        else if (command.generation == rt.generation)
        {
            if (command.kind == Kind::start)
            {
                rt.timelineOrigin = rt.submittedEnd = rt.frozenSample = command.target;
                rt.outputOrigin = command.output; rt.state = TransportState::scheduled;
            }
            else if (command.kind == Kind::ready) rt.state = TransportState::ready;
            else if (command.kind == Kind::pause) rt.state = TransportState::paused;
            else if (command.kind == Kind::stopped) rt.state = TransportState::stopped;
            else rt.state = TransportState::failed;
        }
    }
    const bool valid = stamp.sampleRate == rate && stamp.numSamples > 0 && stamp.numSamples <= queue.blockFrames
        && (stamp.flags & (samplePositionValid | latenciesValid)) == (samplePositionValid | latenciesValid)
        && stamp.outputLatencySamples >= 0 && stamp.callbackQpc > 0;
    const bool discontinuity = havePrevious && (stamp.samplePosition != previous.samplePosition + previous.numSamples
        || stamp.callbackQpc <= previous.callbackQpc || stamp.xruns != previous.xruns || stamp.resets != previous.resets
        || stamp.resyncs != previous.resyncs || stamp.latencyChanges != previous.latencyChanges);
    if (!valid || (havePrevious && (stamp.resets != previous.resets || stamp.resyncs != previous.resyncs
        || stamp.samplePosition < previous.samplePosition || stamp.sampleRate != previous.sampleRate)))
    { rt.state = TransportState::failed; std::fill_n(l, stamp.numSamples, 0.0f); std::fill_n(r, stamp.numSamples, 0.0f); }
    else if (discontinuity && (rt.state == TransportState::playing || rt.state == TransportState::scheduled))
    { ++rt.underruns; rt.state = TransportState::buffering; }
    previous = stamp; havePrevious = true;
    std::uint32_t offset = 0;
    if (rt.state == TransportState::scheduled)
    {
        if (rt.outputOrigin < stamp.samplePosition) { ++rt.underruns; rt.state = TransportState::buffering; }
        else if (rt.outputOrigin < stamp.samplePosition + stamp.numSamples)
        { offset = static_cast<std::uint32_t>(rt.outputOrigin - stamp.samplePosition); rt.state = TransportState::playing; }
    }
    if (rt.state == TransportState::playing)
    {
        const auto count = static_cast<std::uint32_t>((std::min)(Sample(stamp.numSamples - offset), rtEnd - rt.submittedEnd));
        if (!queue.consume(rt.submittedEnd, rt.generation, l + offset, r + offset, count))
        { ++rt.underruns; rt.state = TransportState::buffering; }
        else
        {
            if (count && !rt.firstBlockQpc)
            {
                rt.firstBlockQpc = stamp.callbackQpc;
                rt.firstAudibleQpc = stamp.callbackQpc + static_cast<std::int64_t>((offset + static_cast<double>(stamp.outputLatencySamples)) * frequency / rate);
            }
            // Transport ramp; cut/revision microfades belong to the audio renderer.
            const auto ramp = (std::max)(Sample{1}, Sample(rate) * 3 / 1000);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto at = rt.submittedEnd + i;
                const auto n = (std::min)({ramp, at - rt.timelineOrigin + 1, rtEnd - at});
                const auto gain = static_cast<float>(n) / static_cast<float>(ramp);
                l[offset + i] *= gain; r[offset + i] *= gain;
            }
            rt.submittedEnd += count;
            if (rt.submittedEnd == rtEnd) rt.state = TransportState::draining;
        }
    }
    if (rt.state == TransportState::draining
        && stamp.samplePosition - stamp.outputLatencySamples >= rt.outputOrigin + rt.submittedEnd - rt.timelineOrigin)
        rt.state = TransportState::paused;
    publish(stamp);
}
void TimelineTransport::publish(const BlockStamp& s) noexcept
{
    rt.outputSample = s.samplePosition; rt.callbackQpc = s.callbackQpc;
    rt.blockFrames = s.numSamples; rt.outputLatency = s.outputLatencySamples;
    rt.softwareQueuedSamples = static_cast<Sample>(queue.queuedFrames()); rt.renderedEnd = rt.submittedEnd + rt.softwareQueuedSamples;
    // All publication fields are atomics: unlike a seqlock around a plain struct,
    // this is data-race-free in C++17. Only the control reader may retry.
    published.sequence.fetch_add(1);
    published.state.store(static_cast<int>(rt.state)); published.generation.store(rt.generation); published.underruns.store(rt.underruns);
    published.frozen.store(rt.frozenSample); published.origin.store(rt.timelineOrigin); published.outputOrigin.store(rt.outputOrigin);
    published.submitted.store(rt.submittedEnd); published.rendered.store(rt.renderedEnd); published.queued.store(rt.softwareQueuedSamples);
    published.output.store(rt.outputSample); published.qpc.store(rt.callbackQpc); published.frames.store(rt.blockFrames);
    published.latency.store(rt.outputLatency); published.first.store(rt.firstBlockQpc); published.audible.store(rt.firstAudibleQpc);
    published.sequence.fetch_add(1);
    wake->signal(); // preallocated event; never wait, allocate or acquire a mutex in ASIO
}
TransportSnapshot TimelineTransport::snapshot() const noexcept
{
    TransportSnapshot s;
    for (;;)
    {
        const auto seq = published.sequence.load(); if (seq & 1) continue;
        s.state = static_cast<TransportState>(published.state.load()); s.generation = published.generation.load(); s.underruns = published.underruns.load();
        s.frozenSample = published.frozen.load(); s.timelineOrigin = published.origin.load(); s.outputOrigin = published.outputOrigin.load();
        s.submittedEnd = published.submitted.load(); s.renderedEnd = published.rendered.load(); s.softwareQueuedSamples = published.queued.load();
        s.outputSample = published.output.load(); s.callbackQpc = published.qpc.load(); s.blockFrames = published.frames.load();
        s.outputLatency = published.latency.load(); s.firstBlockQpc = published.first.load(); s.firstAudibleQpc = published.audible.load();
        if (seq == published.sequence.load()) return s;
    }
}
void TimelineTransport::service(TimelineAudioRenderer& audio, VideoPlaybackEngine& video, IAudioOutput& output, std::int64_t now)
{
    if (dubbingLocked) return;
    video.setWakeEvent(wake);
    output.drainTiming();
    try
    {
        if (scrubPending && now - lastScrubQpc >= frequency / 15)
        { seek(pendingScrub); lastScrubQpc = now; scrubPending = false; ++scrubDispatches; }
        const auto deviceStatus = output.status(); if (deviceStatus.failed()) throw std::runtime_error(deviceStatus.getErrorMessage().toStdString());
        const auto s = snapshot();
        if (s.state == TransportState::failed) throw std::runtime_error("ASIO timing invalid; transport stopped");
        if (s.state == TransportState::buffering && s.generation == requestedGeneration
            && audibleCursor(s, rate, frequency, now) == s.submittedEnd)
            seek(s.submittedEnd); // drain accepted device tail, then reprepare both streams
        // Video has no callback-owned queue to reset. Cancel the old generation
        // immediately; audio still waits for the callback's quiescent ack below.
        if (pendingPlan && s.state == TransportState::preparing && s.generation == requestedGeneration)
        {
            audio.setPlan(pendingPlan->plan, std::move(pendingPlan->sources), std::move(pendingPlan->mask));
            video.handoff(std::move(pendingPlan->videos), requestedSample, requestedGeneration);
            videoGeneration = requestedGeneration; pendingPlan.reset();
        }
        if (!pendingPlan && requestedGeneration && videoGeneration != requestedGeneration)
        { video.seek(requestedSample, requestedGeneration); videoGeneration = requestedGeneration; }
        if (s.state == TransportState::preparing && s.generation == requestedGeneration)
        {
            if (preparingGeneration != s.generation)
            {
                timing.callbackAck = s.callbackQpc; timing.audioBegin = qpcNow();
                // A cursor beyond the last clip is valid for recording placement.
                // Prepare an empty audio tail while video selects the actual gap.
                audio.prepare((std::min)(requestedSample, end), s.generation); timing.audioEnd = qpcNow();
                preparingGeneration = s.generation;
            }
            const auto audioStatus = audio.status(), videoStatus = video.status();
            if (audioStatus.failed()) throw std::runtime_error(audioStatus.getErrorMessage().toStdString());
            if (videoStatus.failed()) throw std::runtime_error(videoStatus.getErrorMessage().toStdString());
            const bool audioReady = audio.ready(), videoReady = video.ready(requestedSample, s.generation);
            if (audioReady && !timing.audioReady) timing.audioReady = qpcNow();
            if (videoReady && !timing.videoReady) timing.videoReady = qpcNow();
            if (armedGeneration != s.generation && audioReady && videoReady)
                prepared(output.latestOutputSample() + Sample(queue.blockFrames) * 3, wantPlay && requestedSample < end);
        }
        else if (s.state == TransportState::ready && s.generation == requestedGeneration && wantPlay && requestedSample < end)
            prepared(output.latestOutputSample() + Sample(queue.blockFrames) * 3, true);
        const auto videoStatus = video.status(); if (videoStatus.failed()) throw std::runtime_error(videoStatus.getErrorMessage().toStdString());
        const auto audioStatus = audio.status(); if (audioStatus.failed()) throw std::runtime_error(audioStatus.getErrorMessage().toStdString());
        // Selection and the UI share the audible cursor. Decoder prefetch owns
        // lookahead; adding a fixed refresh period here displays the next frame
        // early and clears clip ends before the audio reaches them.
        auto cursor = audibleCursor(s, rate, frequency, now);
        if (s.generation != requestedGeneration || s.state == TransportState::preparing) cursor = requestedSample;
        const bool advancing = s.generation == requestedGeneration && (s.state == TransportState::playing
            || s.state == TransportState::draining || s.state == TransportState::buffering);
        if (!pendingPlan && requestedGeneration) video.requestFrames(cursor, requestedGeneration, advancing);
    }
    catch (const std::exception& e)
    {
        if (error.isEmpty()) { error = juce::String::fromUTF8(e.what()); send({Kind::fail, requestedGeneration, requestedSample, 0}); }
    }
}
juce::Result TimelineTransport::status() const { return error.isEmpty() ? juce::Result::ok() : juce::Result::fail(error); }
juce::var TimelineTransport::telemetry() const
{
    auto result = jsonObject(); const auto s = snapshot();
    jsonSet(result, "state", transportStateName(s.state)); jsonSet(result, "generation", s.generation);
    jsonSet(result, "requestedGeneration", requestedGeneration); jsonSet(result, "requestedSample", requestedSample);
    jsonSet(result, "scrubInputs", scrubInputs); jsonSet(result, "scrubDispatches", scrubDispatches);
    jsonSet(result, "planHandoffPending", bool(pendingPlan)); jsonSet(result, "timelineEnd", end);
    jsonSet(result, "requestQpc", timing.request); jsonSet(result, "callbackAcknowledgedQpc", timing.callbackAck);
    jsonSet(result, "audioPrepareBeginQpc", timing.audioBegin); jsonSet(result, "audioPrepareEndQpc", timing.audioEnd);
    jsonSet(result, "audioReadyObservedQpc", timing.audioReady); jsonSet(result, "videoReadyObservedQpc", timing.videoReady);
    jsonSet(result, "startCommandQpc", timing.armed); jsonSet(result, "reservedOutputSample", s.outputOrigin);
    jsonSet(result, "firstAudioBlockQpc", s.firstBlockQpc); jsonSet(result, "firstAudioAudibleEstimateQpc", s.firstAudibleQpc);
    jsonSet(result, "underruns", s.underruns); jsonSet(result, "error", error); return result;
}
}
