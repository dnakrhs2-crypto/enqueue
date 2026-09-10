#include "TimelineAudioRenderer.h"
#include "ImportedAudioCache.h"
#include "audio/MicroFade.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <set>

namespace gocue::recorder
{
namespace
{
void need(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool audio(TrackKind kind) { return kind == TrackKind::mic || kind == TrackKind::importAudio; }
const PlaybackAudioSource& sourceFor(const std::vector<AudioSourceBinding>& sources, const Id& id)
{
    for (const auto& s : sources) if (s.assetId == id && s.source) return *s.source;
    throw std::runtime_error("Audio source is not prepared for this asset");
}
bool eligible(const RenderTrackPlan& t, const AudioSourceMask& mask)
{
    using K = AudioSourceMask::Kind;
    switch (mask.kind)
    {
        case K::listeningMix: return audio(t.kind);
        case K::microphoneMix: return t.kind == TrackKind::mic;
        case K::microphone: return t.kind == TrackKind::mic && t.trackId == mask.trackId;
        case K::completedAudio: return t.kind == TrackKind::importAudio && t.trackId == mask.trackId;
        case K::materialTrack: return audio(t.kind) && t.trackId == mask.trackId;
    }
    return false;
}
std::vector<const RenderTrackPlan*> selectedTracks(const CompiledRenderPlan& plan, const AudioSourceMask& mask)
{
    using K = AudioSourceMask::Kind;
    const bool mix = mask.kind == K::listeningMix || mask.kind == K::microphoneMix;
    const bool solo = mix && std::any_of(plan.tracks.begin(), plan.tracks.end(), [&](const auto& t) { return eligible(t, mask) && t.solo; });
    std::vector<const RenderTrackPlan*> result;
    bool found = mix;
    for (const auto& t : plan.tracks) if (eligible(t, mask))
    {
        found = true;
        if (mask.kind == K::materialTrack || (!t.mute && (!solo || t.solo))) result.push_back(&t);
    }
    need(found, "Selected audio track does not exist or has the wrong kind");
    if (mask.kind == K::completedAudio)
    {
        need(mask.assetId.isNotEmpty(), "Completed audio requires an asset selection");
        need(std::any_of(plan.activeClips.begin(), plan.activeClips.end(), [&](const auto& c)
            { return c.trackId == mask.trackId && c.assetId == mask.assetId; }), "Selected completed audio has no active timeline clips");
    }
    return result;
}
std::set<Id> neededAssets(const CompiledRenderPlan& plan, const AudioSourceMask& mask)
{
    std::set<Id> result;
    for (const auto* track : selectedTracks(plan, mask)) for (const auto& span : track->spans)
        if (!span.isGap() && (mask.kind != AudioSourceMask::Kind::completedAudio || span.assetId == mask.assetId)) result.insert(span.assetId);
    return result;
}
struct WavReader final : PlaybackAudioSource
{
    WavSource wav;
    bool allowGaps;
    WavReader(const WavSource& s, bool gaps) : wav(s), allowGaps(gaps)
    { epoch = s.epoch; generation = s.generation; sampleRate = s.sampleRate; channels = int(s.channels); length = s.length; }
    void read(Sample first, unsigned frames, float* l, float* r) const override
    {
        need(current() && first >= 0 && first <= length && frames <= static_cast<std::uint64_t>(length - first), "Stale WAV generation or invalid source range");
        std::fill_n(l, frames, 0.0f); std::fill_n(r, frames, 0.0f);
        const auto align = unsigned(channels) * 3;
        std::vector<std::uint8_t> bytes(std::size_t(frames) * align);
        Sample covered = 0;
        for (const auto& c : wav.chunks)
        {
            const auto a = (std::max)(first, c.firstSample), b = (std::min)(first + frames, c.firstSample + c.validSamples);
            if (a >= b) continue;
            const auto count = static_cast<int>((b - a) * align);
            juce::FileInputStream input(c.file);
            need(c.dataOffset + static_cast<std::uint64_t>(b - c.firstSample) * align <= c.validBytes
                && input.openedOk() && input.setPosition(static_cast<juce::int64>(c.dataOffset) + (a - c.firstSample) * align)
                && input.read(bytes.data(), count) == count, "Short read inside committed WAV range");
            for (Sample i = 0; i < b - a; ++i)
                for (int ch = 0; ch < channels; ++ch)
                {
                    const auto* p = bytes.data() + (i * channels + ch) * 3;
                    const auto raw = std::int32_t(p[0]) | (std::int32_t(p[1]) << 8) | (std::int32_t(p[2]) << 16);
                    (ch ? r : l)[a - first + i] = static_cast<float>(raw & 0x800000 ? raw - 0x1000000 : raw) / 8388608.0f;
                }
            covered += b - a;
        }
        need(allowGaps || covered == frames, "Available audio span is missing a source chunk");
        if (channels == 1) std::copy_n(l, frames, r);
        need(current(), "WAV generation changed during read");
    }
};
struct ImportedReader final : PlaybackAudioSource
{
    juce::File file;
    std::unique_ptr<juce::AudioFormatReader> open() const
    {
        juce::WavAudioFormat format;
        auto stream = file.createInputStream(); need(stream != nullptr, "Imported PCM cache is missing");
        auto result = std::unique_ptr<juce::AudioFormatReader>(format.createReaderFor(stream.release(), true));
        need(result && result->sampleRate == sampleRate && result->numChannels == static_cast<unsigned>(channels)
            && result->lengthInSamples == length && result->usesFloatingPointData && result->bitsPerSample == 32,
            "Imported cache format/length changed; rebuild the cache");
        return result;
    }
    void read(Sample first, unsigned frames, float* l, float* r) const override
    {
        need(current() && first >= 0 && first <= length && frames <= static_cast<std::uint64_t>(length - first), "Stale import generation or invalid source range");
        auto reader = open(); float* channelsOut[]{l, r};
        need(reader->read(channelsOut, channels, first, static_cast<int>(frames)), "Cannot read imported PCM");
        if (channels == 1) std::copy_n(l, frames, r);
        need(current(), "Import generation changed during read");
    }
};
}
std::shared_ptr<const PlaybackAudioSource> wavAudioSource(std::shared_ptr<const WavSource> source, bool allowGaps)
{
    need(source && source->current(), "Missing/current WAV source required");
    return std::make_shared<WavReader>(*source, allowGaps);
}
std::shared_ptr<const PlaybackAudioSource> importedAudioSource(const MediaAsset& asset, const CachedImportedAudio& cache, std::shared_ptr<MediaEpoch> epoch)
{
    need(asset.kind == AssetKind::importAudio && cache.sampleRate > 0 && cache.samples == asset.logicalLength
        && cache.channels >= 1 && cache.channels <= 2 && cache.channels == asset.originalFormat.channels
        && cache.key.startsWith(asset.contentIdentity + "-" + juce::String(cache.sampleRate) + "-v1-"), "Imported cache does not match source identity/length/channels");
    auto reader = std::make_shared<ImportedReader>(); reader->file = cache.pcmFile;
    reader->sampleRate = cache.sampleRate; reader->channels = cache.channels; reader->length = cache.samples;
    reader->generation = static_cast<std::uint64_t>(asset.mediaGeneration);
    if (!epoch) { epoch = std::make_shared<MediaEpoch>(); epoch->value = reader->generation; }
    reader->epoch = std::move(epoch); need(reader->current(), "Imported cache generation is stale"); reader->open();
    return reader;
}
std::vector<AudioSourceBinding> openAudioSources(const AudioRenderPlan& plan, const juce::File& directory,
                                                const std::vector<ImportedAudioBinding>& imports, const AudioSourceMask* selection)
{
    need(plan.timeline != nullptr, "Missing immutable timeline plan");
    const auto selected = selection ? neededAssets(*plan.timeline, *selection) : std::set<Id>{};
    std::vector<AudioSourceBinding> result;
    for (const auto& asset : plan.sources)
    {
        if (selection && !selected.count(asset.assetId)) continue;
        if (asset.kind == AssetKind::importAudio)
        {
            const auto it = std::find_if(imports.begin(), imports.end(), [&](const auto& b) { return b.assetId == asset.assetId; });
            need(it != imports.end() && it->cache && it->mediaGeneration == asset.mediaGeneration, "Import cache for this generation has not been prepared");
            need(it->cache->sampleRate == plan.timeline->Fs, "Import cache must use project Fs");
            result.push_back({asset.assetId, importedAudioSource(asset, *it->cache)});
        }
        else
        {
            auto source = std::make_shared<WavSource>(); source->sampleRate = plan.timeline->Fs; source->channels = unsigned(asset.originalFormat.channels); source->trackId = asset.assetId;
            source->generation = static_cast<std::uint64_t>(asset.mediaGeneration); source->epoch = std::make_shared<MediaEpoch>(); source->epoch->value = source->generation;
            const auto add = [&](const juce::String& path, SampleRange range)
            {
                need(isProjectRelativePath(path) && range.length > 0 && range.length <= ((std::numeric_limits<Sample>::max)() - 44) / (source->channels * 3), "Invalid virtual WAV chunk");
                source->chunks.push_back({directory.getChildFile(path), range.start, range.length, 44, 44 + static_cast<std::uint64_t>(range.length) * source->channels * 3});
            };
            if (asset.chunks.empty()) add(asset.relativePath, {0, asset.logicalLength});
            else for (const auto& chunk : asset.chunks) add(chunk.relativePath, chunk.sourceRange);
            MediaIndex::validateWav(*source); source->length = asset.logicalLength;
            result.push_back({asset.assetId, wavAudioSource(source)});
        }
    }
    return result;
}
TimelineAudioRenderer::TimelineAudioRenderer(std::uint32_t Fs, std::uint32_t frames) : rate(Fs), pcmQueue(Fs, frames) {}
TimelineAudioRenderer::~TimelineAudioRenderer() { stopWorker(); }
void TimelineAudioRenderer::stopWorker()
{ stopping.store(true, std::memory_order_release); pcmQueue.cancelProducer(); if (worker.joinable()) worker.join(); }
void TimelineAudioRenderer::validate(const CompiledRenderPlan& p, const std::vector<AudioSourceBinding>& bindings, const AudioSourceMask& selection) const
{
    need(p.Fs == rate && p.timelineEnd >= 0, "Plan timebase differs from playback device");
    const auto selected = selectedTracks(p, selection);
    std::set<Id> ids;
    for (const auto& b : bindings) need(b.source && ids.insert(b.assetId).second, "Missing or duplicate audio source binding");
    for (const auto* t : selected) for (const auto& s : t->spans)
        if (!s.isGap() && (selection.kind != AudioSourceMask::Kind::completedAudio || s.assetId == selection.assetId))
    {
        const auto& source = sourceFor(bindings, s.assetId);
        need(source.current() && source.sampleRate == rate && source.channels >= 1 && source.channels <= 2
            && s.mediaGeneration == static_cast<Sample>(source.generation) && s.sourceIn >= 0 && s.timeline.length > 0
            && s.sourceIn <= source.length && s.timeline.length <= source.length - s.sourceIn,
            "Audio mapping/rate/generation does not match prepared PCM");
    }
}
void TimelineAudioRenderer::setPlan(std::shared_ptr<const CompiledRenderPlan> next, std::vector<AudioSourceBinding> bindings, AudioSourceMask selection)
{
    need(next != nullptr, "Missing render plan"); validate(*next, bindings, selection); stopWorker();
    plan = std::move(next); sources = std::move(bindings); mask = std::move(selection);
}
void TimelineAudioRenderer::setPlan(std::vector<PlaybackAudioTrack> legacy, Sample end)
{
    need(end >= 0, "Negative timeline duration");
    RecorderProject project; project.Fs = rate;
    std::vector<RenderClip> clips;
    std::vector<RenderTrackPlan> tracks;
    std::vector<AudioSourceBinding> bindings;
    std::vector<MicrofadeBoundary> fades;
    for (auto& t : legacy)
    {
        RenderTrackPlan track{t.trackId, TrackKind::mic, t.mute, t.solo, false, {}};
        std::sort(t.clips.begin(), t.clips.end(), [](const auto& a, const auto& b) { return a.mapping.timelineStartSample < b.mapping.timelineStartSample; });
        Sample cursor = 0;
        for (auto& item : t.clips)
        {
            auto c = item.mapping; c.trackId = t.trackId;
            need(item.source && c.timelineStartSample >= cursor && c.timelineStartSample <= end && c.lengthSamples > 0
                && c.lengthSamples <= end - c.timelineStartSample && c.sourceUnitsNumerator == c.sourceUnitsDenominator, "Invalid legacy PCM mapping");
            // Legacy callers may supply only a WAV source and sample mapping.
            // Empty asset IDs denote gaps in the compiled plan, never anonymous PCM.
            if (c.assetId.isEmpty()) c.assetId = c.clipId.isNotEmpty() ? c.clipId : newId();
            if (std::none_of(bindings.begin(), bindings.end(), [&](const auto& b) { return b.assetId == c.assetId; }))
                bindings.push_back({c.assetId, wavAudioSource(item.source, true)});
            if (cursor < c.timelineStartSample) track.spans.push_back({{cursor, c.timelineStartSample - cursor}});
            Sample offset = 0;
            for (const auto& gap : c.gaps)
            {
                need(gap.start >= offset && gap.start <= c.lengthSamples && gap.length > 0 && gap.length <= c.lengthSamples - gap.start, "Invalid clip gap");
                if (gap.start > offset) track.spans.push_back({{c.timelineStartSample + offset, gap.start - offset}, c.clipId, c.assetId, c.sourceIn + offset, c.mediaGeneration});
                track.spans.push_back({{c.timelineStartSample + gap.start, gap.length}}); offset = gap.start + gap.length;
            }
            if (offset < c.lengthSamples) track.spans.push_back({{c.timelineStartSample + offset, c.lengthSamples - offset}, c.clipId, c.assetId, c.sourceIn + offset, c.mediaGeneration});
            if (c.microfadeInSamples) fades.push_back({t.trackId, c.timelineStartSample, 0, MicroFade::clampLength(c.microfadeInSamples, c.lengthSamples)});
            if (c.microfadeOutSamples) fades.push_back({t.trackId, c.timelineStartSample + c.lengthSamples, MicroFade::clampLength(c.microfadeOutSamples, c.lengthSamples), 0});
            clips.push_back(c); cursor = c.timelineStartSample + c.lengthSamples;
        }
        if (cursor < end) track.spans.push_back({{cursor, end - cursor}});
        tracks.push_back(std::move(track));
    }
    auto compiled = std::make_shared<CompiledRenderPlan>(project, std::move(clips)); compiled->timelineEnd = end;
    compiled->tracks = std::move(tracks); compiled->microfadeBoundaries = std::move(fades);
    setPlan(std::move(compiled), std::move(bindings));
}
void TimelineAudioRenderer::renderAudio(Sample first, std::uint32_t frames, float* l, float* r) const
{ need(plan != nullptr, "No render plan"); renderAudio(*plan, {first, frames}, mask, l, r); }
void TimelineAudioRenderer::renderAudio(const CompiledRenderPlan& p, SampleRange range, const AudioSourceMask& selection, float* l, float* r) const
{
    need(p.Fs == rate && range.start >= 0 && range.length >= 0 && range.length <= (std::numeric_limits<int>::max)() / 3
        && range.start <= (std::numeric_limits<Sample>::max)() - range.length, "Invalid audio render range");
    if (!range.length) return;
    need(l && r && l != r, "Separate stereo output buffers required");
    const auto frames = static_cast<unsigned>(range.length);
    std::fill_n(l, frames, 0.0f); std::fill_n(r, frames, 0.0f);
    const auto selected = selectedTracks(p, selection); if (selected.empty()) return;
    const float scale = 1.0f / static_cast<float>(selected.size());
    std::vector<float> a(frames), b(frames);
    for (const auto* track : selected)
    {
        std::fill(a.begin(), a.end(), 0.0f); std::fill(b.begin(), b.end(), 0.0f);
        for (const auto& span : track->spans)
        {
            if (span.isGap() || (selection.kind == AudioSourceMask::Kind::completedAudio && span.assetId != selection.assetId)) continue;
            const auto begin = (std::max)(range.start, span.timeline.start);
            const auto finish = (std::min)(range.start + range.length, span.timeline.start + span.timeline.length);
            if (begin >= finish) continue;
            const auto& source = sourceFor(sources, span.assetId);
            need(source.current() && source.sampleRate == rate && static_cast<Sample>(source.generation) == span.mediaGeneration, "Audio source generation changed; prepare a new plan");
            // Import resampling is done once by round 16. Logical sourceIn must not
            // be converted a second time using the original-file rational mapping.
            source.read(span.sourceIn + begin - span.timeline.start, static_cast<unsigned>(finish - begin), a.data() + begin - range.start, b.data() + begin - range.start);
            need(source.current(), "Audio source generation changed during render");
        }
        for (const auto& fade : p.microfadeBoundaries) if (fade.trackId == track->trackId)
        {
            const auto begin = (std::max)(range.start, fade.timelineSample - fade.beforeSamples);
            const auto finish = (std::min)(range.start + range.length, fade.timelineSample + fade.afterSamples);
            for (auto at = begin; at < finish; ++at)
            { const auto gain = MicroFade::gain(fade, at); a[static_cast<std::size_t>(at - range.start)] *= gain; b[static_cast<std::size_t>(at - range.start)] *= gain; }
        }
        for (unsigned i = 0; i < frames; ++i) { l[i] += a[i] * scale; r[i] += b[i] * scale; }
    }
}
void TimelineAudioRenderer::prepare(Sample at, std::uint64_t gen)
{ stopWorker(); startWorker(at, gen, false); }
void TimelineAudioRenderer::replacePlan(std::shared_ptr<const CompiledRenderPlan> next, std::vector<AudioSourceBinding> bindings,
                                      Sample boundary, std::uint64_t gen, AudioSourceMask selection)
{
    need(next && plan && next->projectId == plan->projectId && next->editRevision >= plan->editRevision, "Replacement plan project/revision is stale");
    validate(*next, bindings, selection);
    need(boundary >= pcmQueue.submittedEnd() && boundary <= next->timelineEnd, "Replacement boundary is outside the new timeline");
    stopWorker(); plan = std::move(next); sources = std::move(bindings); mask = std::move(selection); startWorker(boundary, gen, true);
}
void TimelineAudioRenderer::startWorker(Sample sample, std::uint64_t gen, bool replacement)
{
    need(plan && sample >= 0 && sample <= length() && gen, "Invalid audio prepare target");
    stopping = false; { std::lock_guard<std::mutex> lock(errorMutex); error.clear(); }
    const auto ramp = replacement ? static_cast<unsigned>(MicroFade::clampLength(MicroFade::defaultLength(rate), length() - sample)) : 0;
    std::vector<PlaybackGenerationGuard> guards;
    std::vector<const PlaybackAudioSource*> checkedSources;
    const auto used = neededAssets(*plan, mask);
    for (const auto& binding : sources) if (used.count(binding.assetId))
    {
        guards.push_back({std::shared_ptr<const std::atomic<std::uint64_t>>(binding.source->epoch, &binding.source->epoch->value), binding.source->generation});
        checkedSources.push_back(binding.source.get());
    }
    const auto ticket = pcmQueue.begin(sample, gen, plan->editRevision, length(), (rate + 3ull) / 4, ramp, replacement, std::move(guards));
    worker = std::thread([this, sample, ticket, checkedSources = std::move(checkedSources)]
    {
        try
        {
            std::vector<float> l(pcmQueue.blockFrames), r(pcmQueue.blockFrames);
            for (auto at = sample; !stopping.load(std::memory_order_acquire) && at < length();)
            {
                const auto count = static_cast<unsigned>((std::min)(Sample(pcmQueue.blockFrames), length() - at));
                renderAudio(at, count, l.data(), r.data());
                while (!stopping.load(std::memory_order_acquire))
                {
                    for (const auto* source : checkedSources) need(source->current(), "Audio source generation invalidated during prefetch");
                    if (pcmQueue.pushPrepared(ticket, at, l.data(), r.data(), count)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                at += count;
            }
        }
        catch (const std::exception& e)
        {
            if (!stopping.load(std::memory_order_acquire))
            { { std::lock_guard<std::mutex> lock(errorMutex); error = juce::String::fromUTF8(e.what()); } pcmQueue.fail(ticket); }
        }
        catch (...) { { std::lock_guard<std::mutex> lock(errorMutex); error = "Unknown audio render worker error"; } pcmQueue.fail(ticket); }
    });
}
bool TimelineAudioRenderer::ready() const noexcept { return pcmQueue.ready(); }
juce::Result TimelineAudioRenderer::status() const
{
    if (plan) for (const auto& id : neededAssets(*plan, mask))
        if (!sourceFor(sources, id).current()) return juce::Result::fail("Audio source generation is stale");
    std::lock_guard<std::mutex> lock(errorMutex); return error.isEmpty() ? juce::Result::ok() : juce::Result::fail(error);
}
}
