#include "TimelineAudioRenderer.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace gocue::recorder
{
namespace
{
std::size_t queueBlocks(std::uint32_t rate, std::uint32_t frames)
{
    if (!rate || rate > 768000 || !frames || frames > 262144) throw std::invalid_argument("Invalid playback PCM queue dimensions");
    return (std::max)(std::size_t{3}, static_cast<std::size_t>((rate + 2ull * frames - 1) / (2ull * frames)) + 1);
}
}
PlaybackPcmQueue::PlaybackPcmQueue(std::uint32_t Fs, std::uint32_t frames)
    : blockFrames(frames), capacityBlocks(queueBlocks(Fs, frames)), blocks(capacityBlocks), pcm(capacityBlocks * frames * 2, 0) {}
bool PlaybackPcmQueue::push(Sample first, std::uint64_t gen, const float* l, const float* r, std::uint32_t frames) noexcept
{
    if (!frames || frames > blockFrames) return false;
    const auto w = written.load(std::memory_order_relaxed);
    if (w - read.load(std::memory_order_acquire) == capacityBlocks) return false;
    const auto slot = static_cast<std::size_t>(w % capacityBlocks);
    auto* data = pcm.data() + slot * blockFrames * 2;
    std::memcpy(data, l, frames * sizeof(float)); std::memcpy(data + blockFrames, r, frames * sizeof(float));
    blocks[slot] = {first, gen, frames};
    queued.fetch_add(frames, std::memory_order_relaxed);
    written.store(w + 1, std::memory_order_release); return true;
}
bool PlaybackPcmQueue::consume(Sample first, std::uint64_t gen, float* l, float* r, std::uint32_t frames) noexcept
{
    if (!frames) return true;
    auto cursor = read.load(std::memory_order_relaxed); const auto w = written.load(std::memory_order_acquire);
    auto localOffset = offset; std::uint32_t remaining = frames; Sample expected = first;
    while (remaining && cursor < w)
    {
        const auto& b = blocks[static_cast<std::size_t>(cursor % capacityBlocks)];
        if (b.generation != gen || b.first + localOffset != expected || localOffset >= b.frames) return false;
        const auto count = (std::min)(remaining, b.frames - localOffset);
        remaining -= count; expected += count; ++cursor; localOffset = 0;
    }
    if (remaining) return false;
    cursor = read.load(std::memory_order_relaxed); remaining = frames; std::uint32_t copied = 0;
    while (remaining)
    {
        const auto slot = static_cast<std::size_t>(cursor % capacityBlocks); const auto& b = blocks[slot];
        const auto count = (std::min)(remaining, b.frames - offset);
        const auto* data = pcm.data() + slot * blockFrames * 2 + offset;
        std::memcpy(l + copied, data, count * sizeof(float)); std::memcpy(r + copied, data + blockFrames, count * sizeof(float));
        remaining -= count; copied += count; offset += count;
        queued.fetch_sub(count, std::memory_order_relaxed);
        if (offset == b.frames) { offset = 0; read.store(++cursor, std::memory_order_release); }
    }
    return true;
}
void PlaybackPcmQueue::reset() { written.store(0); read.store(0); queued.store(0); offset = 0; }
TimelineAudioRenderer::TimelineAudioRenderer(std::uint32_t Fs, std::uint32_t frames) : rate(Fs), pcmQueue(Fs, frames) {}
TimelineAudioRenderer::~TimelineAudioRenderer() { stopWorker(); }
void TimelineAudioRenderer::stopWorker() { stopping.store(true); if (worker.joinable()) worker.join(); }
void TimelineAudioRenderer::setPlan(std::vector<PlaybackAudioTrack> plan, Sample timelineEnd)
{
    if (timelineEnd < 0) throw std::invalid_argument("Negative timeline duration");
    stopWorker();
    for (auto& track : plan)
    {
        std::sort(track.clips.begin(), track.clips.end(), [](const auto& a, const auto& b) { return a.mapping.timelineStartSample < b.mapping.timelineStartSample; });
        Sample previousEnd = 0;
        for (const auto& clip : track.clips)
        {
            const auto& c = clip.mapping;
            if (!clip.source || !clip.source->current() || clip.source->sampleRate != rate
                || c.mediaGeneration != static_cast<Sample>(clip.source->generation) || c.timelineStartSample < previousEnd
                || c.sourceIn < 0 || c.lengthSamples <= 0 || c.sourceIn > clip.source->length
                || c.lengthSamples > clip.source->length - c.sourceIn || c.timelineStartSample > timelineEnd
                || c.lengthSamples > timelineEnd - c.timelineStartSample || c.sourceUnitsNumerator != 1 || c.sourceUnitsDenominator != 1
                || c.microfadeInSamples || c.microfadeOutSamples || !c.gaps.empty())
                throw std::invalid_argument("Invalid continuous WAV plan; rate conversion/edit fades require round 14");
            previousEnd = c.timelineStartSample + c.lengthSamples;
        }
    }
    tracks = std::move(plan); end = timelineEnd;
}
void TimelineAudioRenderer::renderAudio(Sample first, std::uint32_t frames, float* left, float* right)
{
    if (first < 0 || first > end || frames > static_cast<std::uint64_t>(end - first)) throw std::out_of_range("Audio range outside timeline");
    std::fill_n(left, frames, 0.0f); std::fill_n(right, frames, 0.0f);
    const bool anySolo = std::any_of(tracks.begin(), tracks.end(), [](const auto& t) { return t.solo; });
    std::size_t selected = 0;
    for (const auto& t : tracks) if (!t.mute && (!anySolo || t.solo)) ++selected;
    if (!selected) return;
    const float scale = 1.0f / (8388608.0f * static_cast<float>(selected));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(frames) * 3); // render worker only
    for (const auto& track : tracks)
    {
        if (track.mute || (anySolo && !track.solo)) continue;
        for (const auto& clip : track.clips)
        {
            if (!clip.source->current()) throw std::runtime_error("WAV generation replaced; prepare a new plan");
            const auto& c = clip.mapping;
            const auto begin = (std::max)(first, c.timelineStartSample), finish = (std::min)(first + frames, c.timelineStartSample + c.lengthSamples);
            if (begin >= finish) continue;
            const auto sourceBegin = c.sourceIn + begin - c.timelineStartSample, sourceEnd = sourceBegin + finish - begin;
            for (const auto& chunk : clip.source->chunks)
            {
                const auto a = (std::max)(sourceBegin, chunk.firstSample), b = (std::min)(sourceEnd, chunk.firstSample + chunk.validSamples);
                if (a >= b) continue;
                juce::FileInputStream input(chunk.file);
                const auto count = static_cast<int>((b - a) * 3);
                if (!input.openedOk() || !input.setPosition(static_cast<juce::int64>(chunk.dataOffset) + (a - chunk.firstSample) * 3)
                    || input.read(bytes.data(), count) != count) throw std::runtime_error("Short read inside committed WAV range");
                const auto output = static_cast<std::size_t>(begin - first + a - sourceBegin);
                for (std::size_t i = 0; i < static_cast<std::size_t>(b - a); ++i)
                {
                    const auto* p = bytes.data() + i * 3;
                    const auto raw = std::int32_t(p[0]) | (std::int32_t(p[1]) << 8) | (std::int32_t(p[2]) << 16);
                    const auto signedPcm = raw & 0x800000 ? raw - 0x1000000 : raw;
                    left[output + i] += static_cast<float>(signedPcm) * scale;
                }
            }
        }
    }
    std::copy_n(left, frames, right); // round-06 sources are mono; AAC is never decoded
}
void TimelineAudioRenderer::prepare(Sample sample, std::uint64_t gen)
{
    if (sample < 0 || sample > end || !gen) throw std::invalid_argument("Invalid audio prepare target");
    stopWorker(); pcmQueue.reset(); preparedAt = sample; stopping.store(false); failed.store(false);
    { std::lock_guard<std::mutex> lock(errorMutex); error.clear(); }
    worker = std::thread([this, sample, gen]
    {
        try
        {
            std::vector<float> l(pcmQueue.blockFrames), r(pcmQueue.blockFrames);
            auto at = sample;
            while (!stopping.load(std::memory_order_acquire) && at < end)
            {
                const auto count = static_cast<std::uint32_t>((std::min)(Sample(pcmQueue.blockFrames), end - at));
                renderAudio(at, count, l.data(), r.data());
                while (!stopping.load(std::memory_order_acquire) && !pcmQueue.push(at, gen, l.data(), r.data(), count))
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                at += count;
            }
        }
        catch (const std::exception& e)
        {
            { std::lock_guard<std::mutex> lock(errorMutex); error = juce::String::fromUTF8(e.what()); }
            failed.store(true, std::memory_order_release);
        }
    });
}
bool TimelineAudioRenderer::ready() const noexcept
{ return !failed.load(std::memory_order_acquire) && pcmQueue.queuedFrames() >= static_cast<std::uint64_t>((std::min)(end - preparedAt, Sample((rate + 3) / 4))); }
juce::Result TimelineAudioRenderer::status() const
{ std::lock_guard<std::mutex> lock(errorMutex); return error.isEmpty() ? juce::Result::ok() : juce::Result::fail(error); }
}
