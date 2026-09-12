#include "TimelineExporter.h"
#include "WavExportWriter.h"
#include "playback/ImportedAudioCache.h"
#include "support/Platform.h"
#include <algorithm>
#include <chrono>
#include <set>

namespace gocue::recorder
{
ExportAudioRenderer::ExportAudioRenderer(const ExportJob& j, std::vector<AudioSourceBinding> b, AudioSourceMask mask)
    : job(j), renderer(j.snapshot.Fs, 4096)
{ renderer.setPlan(j.audioPlan->timeline, std::move(b), std::move(mask)); }
void ExportAudioRenderer::render(Sample offset, unsigned frames, float* l, float* r) const
{
    exportRequire(offset >= 0 && offset <= job.range.sampleCount && frames <= std::uint64_t(job.range.sampleCount - offset), "PCM output range outside export");
    exportRequire(l && r && l != r, "Separate export stereo buffers required");
    const auto valid = (std::max)(Sample{0}, (std::min)(Sample(frames), job.range.requested.start + job.range.requested.length - job.range.startSample - offset));
    if (valid) renderer.renderAudio(job.range.startSample + offset, static_cast<unsigned>(valid), l, r);
    std::fill(l + valid, l + frames, 0.0f); std::fill(r + valid, r + frames, 0.0f);
}
juce::Array<juce::var> TimelineExporter::assetIds(const ExportJob& j, const AudioSourceMask& mask)
{
    using K = AudioSourceMask::Kind; std::set<Id> ids;
    bool solo = false;
    if (mask.kind == K::microphoneMix) for (const auto& t : j.plan().tracks) solo |= t.kind == TrackKind::mic && t.solo;
    for (const auto& t : j.plan().tracks)
    {
        const bool selected = mask.kind == K::microphoneMix ? t.kind == TrackKind::mic && !t.mute && (!solo || t.solo)
            : t.trackId == mask.trackId && (mask.kind == K::materialTrack || !t.mute);
        if (!selected) continue;
        for (const auto& c : j.plan().activeClips)
            if (c.trackId == t.trackId && (mask.kind != K::completedAudio || c.assetId == mask.assetId)) ids.insert(c.assetId);
    }
    juce::Array<juce::var> result; for (const auto& id : ids) result.add(id); return result;
}
std::vector<AudioSourceBinding> TimelineExporter::openSources(const ExportJob& j, const AudioSourceMask& mask, ExportControl& control)
{
    control.checkpoint(); auto used = assetIds(j, mask);
    // An explicitly selected muted source still has to pass the file/format/
    // length preflight. Its renderer mask, and therefore its output, stays muted.
    if (mask.kind == AudioSourceMask::Kind::microphone || mask.kind == AudioSourceMask::Kind::completedAudio)
        for (const auto& c : j.plan().activeClips)
            if (c.trackId == mask.trackId && (mask.kind != AudioSourceMask::Kind::completedAudio || c.assetId == mask.assetId)
                && !used.contains(juce::var(c.assetId))) used.add(c.assetId);
    AudioRenderPlan prepared; prepared.timeline = j.audioPlan->timeline;
    for (const auto& a : j.audioPlan->sources) if (used.contains(juce::var(a.assetId))) prepared.sources.push_back(a);
    std::vector<CachedImportedAudio> caches; caches.reserve(j.audioPlan->sources.size()); std::vector<ImportedAudioBinding> imports;
    for (const auto& a : j.audioPlan->sources) if (a.kind == AssetKind::importAudio && used.contains(juce::var(a.assetId)))
    {
        exportRequire(a.originalFormat.channels >= 1 && a.originalFormat.channels <= 2, "Completed audio supports mono/stereo only; 3+ channels cannot be downmixed");
        AudioImportControl cacheControl;
        cacheControl.onProgress = [&](AudioImportControl::Stage, double) { control.checkpoint(); };
        caches.emplace_back(); const auto info = AudioImport::loadInfo(j.projectDirectory, a);
        const auto built = ImportedAudioCache::build(j.projectDirectory, a, info, j.snapshot.Fs, cacheControl, caches.back());
        control.checkpoint(); exportCheck(built);
        imports.push_back({a.assetId, a.mediaGeneration, &caches.back()}); control.checkpoint();
    }
    return openAudioSources(prepared, j.projectDirectory, imports);
}
juce::var TimelineExporter::audioMaterials(const ExportJob& j, ExportControl& control, bool includeImports, FileIoFaultAdapter* faults)
{
    ExportActivity::Lease lease(control.activity); control.checkpoint();
    struct Material { const Track* track; AudioSourceMask mask; std::vector<AudioSourceBinding> sources; int channels; };
    std::vector<Material> materials;
    for (const auto& t : j.snapshot.tracks) if (t.kind == TrackKind::mic || (includeImports && t.kind == TrackKind::importAudio))
    {
        AudioSourceMask mask{AudioSourceMask::Kind::materialTrack, t.trackId};
        auto sources = openSources(j, mask, control); int channels = 1;
        for (const auto& b : sources) channels = (std::max)(channels, b.source->channels);
        materials.push_back({&t, mask, std::move(sources), channels});
    }
    exportRequire(!materials.empty(), "No audio material tracks");
    ExportPublication output(j); juce::Array<juce::var> files;
    const auto began = std::chrono::steady_clock::now(); std::vector<float> l(16384), r(l.size());
    std::size_t trackNumber = 0;
    for (auto& m : materials)
    {
        const auto stem = m.track->kind == TrackKind::mic ? "mic" + juce::String(m.track->microphoneIndex + 1).paddedLeft('0', 2)
            : "import-" + m.track->trackId;
        const auto fileChannels = m.track->kind == TrackKind::mic ? m.channels : 1;
        const auto fileCount = m.channels / fileChannels;
        std::vector<std::unique_ptr<WavExportWriter>> writers;
        for (int ch = 0; ch < fileCount; ++ch)
            writers.push_back(std::make_unique<WavExportWriter>(output.file(stem + (fileCount == 2 ? (ch ? "-R" : "-L") : "") + ".wav"), j.snapshot.Fs, j.range.sampleCount, faults, unsigned(fileChannels)));
        ExportAudioRenderer renderer(j, std::move(m.sources), m.mask);
        for (Sample at = 0; at < j.range.sampleCount;)
        {
            control.checkpoint(); const auto count = static_cast<unsigned>((std::min)(Sample(l.size()), j.range.sampleCount - at));
            renderer.render(at, count, l.data(), r.data());
            if (fileChannels == 2) writers[0]->appendStereo(l.data(), r.data(), count);
            else { writers[0]->append(l.data(), count); if (fileCount == 2) writers[1]->append(r.data(), count); }
            at += count;
            if (control.onProgress)
            {
                const double fraction = (trackNumber + double(at) / j.range.sampleCount) / materials.size();
                const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                control.onProgress({"audio", fraction * .95, seconds, fraction > 0 ? std::optional<double>(seconds * (1 - fraction) / fraction) : std::nullopt});
            }
        }
        for (int ch = 0; ch < fileCount; ++ch)
        {
            auto& w = *writers[std::size_t(ch)]; control.checkpoint(); w.finish(); auto f = jsonObject();
            jsonSet(f, "name", w.partialFile().getFileName().dropLastCharacters(8)); jsonSet(f, "verified", true);
            jsonSet(f, "trackId", m.track->trackId); jsonSet(f, "sourceChannel", fileChannels == 2 ? -1 : ch); jsonSet(f, "assetIds", assetIds(j, m.mask));
            jsonSet(f, "sampleCount", j.range.sampleCount); jsonSet(f, "frameCount", j.range.frameCount);
            jsonSet(f, "Fs", j.snapshot.Fs); jsonSet(f, "channels", fileChannels); jsonSet(f, "bitsPerSample", 24); jsonSet(f, "rf64", w.header().rf64); files.add(f);
        }
        ++trackNumber;
    }
    control.checkpoint(); output.commit(files, control, faults);
    if (control.onProgress) try
    {
        control.onProgress({"complete", 1, std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count(), 0.0});
    }
    catch (...) {} // completed publication cannot become a failed/cancelled job
    return j.manifest(files);
}
}
