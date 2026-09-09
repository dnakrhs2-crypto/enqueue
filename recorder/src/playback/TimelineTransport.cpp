#include "TimelineTransport.h"
#include "VideoPlaybackEngine.h"
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
    : rate(Fs), frequency(hz), queue(q), end(length)
{ if (!rate || rate > 768000 || hz <= 0 || length < 0) throw std::invalid_argument("Invalid transport timebase"); }
void TimelineTransport::send(Command c)
{ if (!commands.push(c)) throw std::runtime_error("Transport command queue full"); }
void TimelineTransport::seek(Sample sample)
{
    if (dubbingLocked) throw std::logic_error("더빙 중에는 탐색할 수 없습니다.");
    if (sample < 0 || sample > end) throw std::out_of_range("Seek outside timeline");
    send({Kind::prepare, requestedGeneration + 1, sample, 0});
    ++requestedGeneration; requestedSample = sample; armedGeneration = 0; stopAfterPrepare = false; scrubPending = false;
}
void TimelineTransport::play()
{
    if (dubbingLocked) throw std::logic_error("더빙 중에는 재생을 변경할 수 없습니다.");
    if (scrubPending) seek(pendingScrub);
    wantPlay = true; const auto s = snapshot();
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
void TimelineTransport::stop() { seek(0); wantPlay = false; stopAfterPrepare = true; }
void TimelineTransport::goToStart() { seek(0); wantPlay = false; }
void TimelineTransport::scrub(Sample sample, bool released, std::int64_t now)
{
    if (dubbingLocked) throw std::logic_error("더빙 중에는 탐색할 수 없습니다.");
    if (sample < 0 || sample > end) throw std::out_of_range("Scrub outside timeline");
    wantPlay = false; pendingScrub = sample; scrubPending = true;
    if (released || !lastScrubQpc || now - lastScrubQpc >= frequency / 15)
    { seek(pendingScrub); lastScrubQpc = now; scrubPending = false; }
}
void TimelineTransport::prepared(std::int64_t output, bool start)
{
    if (dubbingLocked) throw std::logic_error("더빙 중에는 출력을 변경할 수 없습니다.");
    if (snapshot().generation != requestedGeneration) throw std::logic_error("Prepare requires callback generation acknowledgement");
    send({start ? Kind::start : stopAfterPrepare ? Kind::stopped : Kind::ready, requestedGeneration, requestedSample, output});
    armedGeneration = requestedGeneration;
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
    if (received)
    {
        // A user pause/seek/stop fades a bounded piece of the OLD prepared PCM
        // before acknowledging quiescence. This runs before the generation is
        // replaced and before the control owner is allowed to reset the queue.
        if ((command.kind == Kind::prepare || command.kind == Kind::pause) && rt.state == TransportState::playing)
        {
            const auto count = static_cast<std::uint32_t>((std::min)({Sample(stamp.numSamples), end - rt.submittedEnd,
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
        const auto count = static_cast<std::uint32_t>((std::min)(Sample(stamp.numSamples - offset), end - rt.submittedEnd));
        if (!queue.consume(rt.submittedEnd, rt.generation, l + offset, r + offset, count))
        { ++rt.underruns; rt.state = TransportState::buffering; }
        else
        {
            if (count && !rt.firstBlockQpc)
            {
                rt.firstBlockQpc = stamp.callbackQpc;
                rt.firstAudibleQpc = stamp.callbackQpc + static_cast<std::int64_t>((offset + static_cast<double>(stamp.outputLatencySamples)) * frequency / rate);
            }
            // Transport onset/endpoint ramp only. Cut-boundary microfades belong
            // to round 14 and are explicitly rejected by this minimal renderer.
            const auto ramp = (std::max)(Sample{1}, Sample(rate) * 3 / 1000);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto at = rt.submittedEnd + i;
                const auto n = (std::min)({ramp, at - rt.timelineOrigin + 1, end - at});
                const auto gain = static_cast<float>(n) / static_cast<float>(ramp);
                l[offset + i] *= gain; r[offset + i] *= gain;
            }
            rt.submittedEnd += count;
            if (rt.submittedEnd == end) rt.state = TransportState::draining;
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
    output.drainTiming();
    try
    {
        if (scrubPending && now - lastScrubQpc >= frequency / 15)
        { seek(pendingScrub); lastScrubQpc = now; scrubPending = false; }
        const auto deviceStatus = output.status(); if (deviceStatus.failed()) throw std::runtime_error(deviceStatus.getErrorMessage().toStdString());
        const auto s = snapshot();
        if (s.state == TransportState::failed) throw std::runtime_error("ASIO timing invalid; transport stopped");
        if (s.state == TransportState::buffering && s.generation == requestedGeneration
            && audibleCursor(s, rate, frequency, now) == s.submittedEnd)
            seek(s.submittedEnd); // drain accepted device tail, then reprepare both streams
        if (s.state == TransportState::preparing && s.generation == requestedGeneration)
        {
            if (preparingGeneration != s.generation)
            {
                audio.prepare(requestedSample, s.generation); video.seek(requestedSample, s.generation);
                preparingGeneration = s.generation;
            }
            const auto audioStatus = audio.status(), videoStatus = video.status();
            if (audioStatus.failed()) throw std::runtime_error(audioStatus.getErrorMessage().toStdString());
            if (videoStatus.failed()) throw std::runtime_error(videoStatus.getErrorMessage().toStdString());
            if (armedGeneration != s.generation && audio.ready() && video.ready(requestedSample, s.generation))
                prepared(output.latestOutputSample() + Sample(queue.blockFrames) * 3, wantPlay && requestedSample < end);
        }
        else if (s.state == TransportState::ready && wantPlay && requestedSample < end)
            prepared(output.latestOutputSample() + Sample(queue.blockFrames) * 3, true);
        const auto videoStatus = video.status(); if (videoStatus.failed()) throw std::runtime_error(videoStatus.getErrorMessage().toStdString());
        const auto audioStatus = audio.status(); if (audioStatus.failed()) throw std::runtime_error(audioStatus.getErrorMessage().toStdString());
        auto cursor = audibleCursor(s, rate, frequency, now, frequency / 60);
        if (s.generation != requestedGeneration || s.state == TransportState::preparing) cursor = requestedSample;
        for (unsigned camera = 0; camera < 2; ++camera) video.requestFrame(camera, cursor, requestedGeneration);
    }
    catch (const std::exception& e)
    {
        if (error.isEmpty()) { error = juce::String::fromUTF8(e.what()); send({Kind::fail, requestedGeneration, requestedSample, 0}); }
    }
}
juce::Result TimelineTransport::status() const { return error.isEmpty() ? juce::Result::ok() : juce::Result::fail(error); }
}
