#include "MaterialExporter.h"
#include "WavExportWriter.h"
#include "support/Platform.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <set>

namespace gocue::recorder
{
namespace
{
juce::String cameraName(TrackKind camera) { return camera == TrackKind::cam1 ? "cam1" : "cam2"; }
class CancellationRelay
{
public:
    CancellationRelay(const ExportControl& parent, ExportControl& child)
        : worker([this, &parent, &child]
        {
            std::unique_lock<std::mutex> lock(mutex);
            while (!stopping)
            {
                if (parent.cancelled.load(std::memory_order_acquire))
                { child.cancelled.store(true, std::memory_order_release); return; }
                wake.wait_for(lock, std::chrono::milliseconds(10), [this] { return stopping; });
            }
        }) {}
    ~CancellationRelay()
    {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        wake.notify_one(); worker.join();
    }
private:
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread worker;
};
struct RelayIo final : FileIoFaultAdapter
{
    ExportControl& parent;
    FileIoFaultAdapter* next;
    RelayIo(ExportControl& c, FileIoFaultAdapter* f) : parent(c), next(f) {}
    juce::Result beforeIo(FileIoOperation op, const juce::File& path, std::uint64_t offset, std::size_t bytes) override
    {
        // A child is permitted to finish its own atomic commit, but cancellation
        // still prevents the outer job from publishing any of those files.
        parent.checkpoint();
        return next ? next->beforeIo(op, path, offset, bytes) : juce::Result::ok();
    }
};
}
std::vector<TrackKind> MaterialExporter::cameras(const RecorderProject& p)
{
    std::vector<TrackKind> result;
    for (auto kind : {TrackKind::cam1, TrackKind::cam2})
    {
        bool used = false;
        for (const auto& t : p.tracks) if (t.kind == kind)
            for (const auto& c : t.clips.items())
            {
                const auto* a = p.media->findAsset(c.assetId);
                used |= a && a->kind == AssetKind::camera;
            }
        for (const auto& take : p.media->takes)
            used |= (kind == TrackKind::cam1 ? take.cam1AssetId : take.cam2AssetId).isNotEmpty();
        if (used) result.push_back(kind);
    }
    return result;
}
AudioSourceMask MaterialExporter::referenceAudio(const ExportJob& j, const MaterialExportOptions& options)
{
    bool dub = false;
    for (const auto& take : j.snapshot.media->takes) dub |= take.mode == TakeMode::dub;
    if (options.referenceAudio)
    {
        const auto& mask = *options.referenceAudio;
        exportRequire(mask.kind == (dub ? AudioSourceMask::Kind::completedAudio : AudioSourceMask::Kind::microphoneMix),
                      "Material reference uses completed audio for dubbing and the microphone mix for a normal project");
        if (mask.kind == AudioSourceMask::Kind::microphoneMix)
            exportRequire(mask.trackId.isEmpty() && mask.assetId.isEmpty(), "Microphone mix cannot select another track/asset");
        else
        {
            const auto* asset = j.snapshot.media->findAsset(mask.assetId); bool found = false;
            for (const auto& t : j.snapshot.tracks) if (t.kind == TrackKind::importAudio && t.trackId == mask.trackId)
                for (const auto& c : t.clips.items()) found |= j.snapshot.isActive(c) && c.assetId == mask.assetId;
            exportRequire(found && asset && asset->kind == AssetKind::importAudio && asset->originalFormat.channels >= 1 && asset->originalFormat.channels <= 2,
                "Choose an active mono/stereo completed audio reference track and asset");
        }
        return mask;
    }
    if (!dub) return {AudioSourceMask::Kind::microphoneMix};
    std::set<std::pair<Id, Id>> imports;
    for (const auto& t : j.snapshot.tracks) if (t.kind == TrackKind::importAudio)
        for (const auto& c : t.clips.items()) if (j.snapshot.isActive(c)) imports.insert({t.trackId, c.assetId});
    exportRequire(imports.size() == 1, "Choose the completed audio reference source for this dubbing project");
    return referenceAudio(j, {false, AudioSourceMask{AudioSourceMask::Kind::completedAudio, imports.begin()->first, imports.begin()->second}});
}
std::vector<MaterialOutput> MaterialExporter::outputs(const ExportJob& j, const MaterialExportOptions& options)
{
    std::vector<MaterialOutput> result;
    const auto cams = cameras(j.snapshot);
    const auto reference = cams.empty() ? AudioSourceMask{AudioSourceMask::Kind::microphoneMix} : referenceAudio(j, options);
    std::set<juce::String> names;
    for (auto cam : cams)
    {
        const auto name = cameraName(cam) + ".mp4";
        result.push_back({name, cam, reference, 0}); names.insert(name);
    }
    for (const auto& t : j.snapshot.tracks)
    {
        if (t.kind != TrackKind::mic && !(options.includeImports && t.kind == TrackKind::importAudio)) continue;
        AudioSourceMask mask{AudioSourceMask::Kind::materialTrack, t.trackId};
        int channels = 1;
        juce::String sourceName;
        for (const auto& c : j.plan().activeClips) if (c.trackId == t.trackId)
        {
            const auto* asset = j.snapshot.media->findAsset(c.assetId);
            exportRequire(asset != nullptr, "Missing material audio asset");
            if (t.kind == TrackKind::mic) channels = (std::max)(channels, asset->originalFormat.channels);
            if (t.kind == TrackKind::importAudio)
            {
                exportRequire(asset->originalFormat.channels >= 1 && asset->originalFormat.channels <= 2,
                              "Imported WAV materials support mono/stereo only; automatic downmix is unsupported");
                channels = (std::max)(channels, asset->originalFormat.channels);
                if (sourceName.isEmpty()) sourceName = j.projectDirectory.getChildFile(asset->relativePath).getFileNameWithoutExtension();
            }
        }
        auto stem = t.kind == TrackKind::mic ? "mic" + juce::String(t.microphoneIndex + 1).paddedLeft('0', 2)
            : "import_" + juce::File::createLegalFileName(t.name.isNotEmpty() ? t.name : sourceName.isNotEmpty() ? sourceName : "audio").substring(0, 100);
        const int fileChannels = t.kind == TrackKind::mic ? channels : 1;
        const int fileCount = channels / fileChannels;
        // Reserve all outputs together, including case-insensitive Windows collisions.
        juce::String unique = stem;
        for (unsigned suffix = 2;; ++suffix)
        {
            bool collision = false;
            for (int ch = 0; ch < fileCount; ++ch)
                collision |= names.count((unique + (fileCount == 2 ? (ch ? "-R" : "-L") : "") + ".wav").toLowerCase()) != 0;
            if (!collision) break;
            unique = stem + " (" + juce::String(suffix) + ")";
        }
        for (int ch = 0; ch < fileCount; ++ch)
        {
            const auto name = unique + (fileCount == 2 ? (ch ? "-R" : "-L") : "") + ".wav";
            names.insert(name.toLowerCase()); result.push_back({name, {}, mask, fileChannels == 2 ? -1 : ch, unsigned(fileChannels)});
        }
    }
    exportRequire(!result.empty(), "No camera or audio material tracks");
    return result;
}
juce::var MaterialExporter::run(const ExportJob& j, const MaterialExportOptions& options, ExportControl& control,
                                FileIoFaultAdapter* faults, const CameraRenderer& renderCamera)
{
    ExportActivity::Lease lease(control.activity); control.checkpoint();
    const auto list = outputs(j, options);
    ExportPublication publication(j);
    juce::Array<juce::var> files;
    const auto began = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count(); };
    const auto progress = [&](const juce::String& stage, std::size_t index, double fraction)
    {
        control.checkpoint();
        const auto done = (double(index) + fraction) / double(list.size()) * .98;
        const auto seconds = elapsed();
        if (control.onProgress) control.onProgress({stage, done, seconds,
            done > 0 ? std::optional<double>(seconds * (1 - done) / done) : std::nullopt});
    };
    std::size_t cameraCount = 0;
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        const auto& item = list[i]; control.checkpoint();
        juce::var row;
        if (item.camera)
        {
            // Legacy run() owns a publication. Keep it entirely inside the
            // outer owned .partial tree until every camera and WAV is verified.
            ExportJob child(j.snapshot, j.projectDirectory, j.partialDirectory.getChildFile(cameraName(*item.camera) + "-work"), j.range.requested);
            ExportActivity childGate; ExportControl childControl(childGate); RelayIo relay(control, faults);
            // Legacy ExportControl owns its atomic flag. Forward cancellation
            // even while import cache/index preparation emits no render events.
            // This is cooperative cancellation, not a deadline for blocking I/O.
            CancellationRelay cancellation(control, childControl);
            childControl.onProgress = [&](const ExportProgress& p) { progress(cameraName(*item.camera) + "/" + p.stage, i, p.fraction); };
            auto manifest = renderCamera ? renderCamera(child, {*item.camera, item.audio}, childControl, &relay)
                : FinalVideoExporter::run(child, {*item.camera, item.audio}, childControl, {}, &relay);
            control.checkpoint();
            const auto* entries = manifest["files"].getArray();
            exportRequire(entries && entries->size() == 1, "Camera child did not verify exactly one MP4");
            row = (*entries)[0].clone();
            exportRequire(row["verified"] == juce::var(true) && Sample(row["frameCount"]) == j.range.frameCount
                && Sample(row["renderedPcmSampleCount"]) == j.range.sampleCount, "Camera material common count mismatch");
            exportRename(child.outputDirectory.getChildFile("final.mp4"), publication.file(item.name).getSiblingFile(item.name + ".partial"));
            // Explicit ownership and containment before recursive cleanup.
            exportRequire(child.outputDirectory.getParentDirectory() == j.partialDirectory && child.outputDirectory.getFileName().endsWith("-work"), "Unexpected child publication owner");
            exportRequire(child.outputDirectory.deleteRecursively(), "Cannot clean camera work directory");
            ++cameraCount;
        }
        else
        {
            auto bindings = TimelineExporter::openSources(j, item.audio, control);
            ExportAudioRenderer renderer(j, std::move(bindings), item.audio);
            WavExportWriter writer(publication.file(item.name), j.snapshot.Fs, j.range.sampleCount, faults, item.channels);
            std::vector<float> left(16384), right(left.size());
            for (Sample at = 0; at < j.range.sampleCount;)
            {
                control.checkpoint();
                const auto count = static_cast<unsigned>((std::min)(Sample(left.size()), j.range.sampleCount - at));
                renderer.render(at, count, left.data(), right.data());
                if (item.channels == 2) writer.appendStereo(left.data(), right.data(), count);
                else writer.append(item.sourceChannel ? right.data() : left.data(), count);
                at += count;
                progress(item.name + "/audio", i, double(at) / j.range.sampleCount * .97);
            }
            control.checkpoint(); writer.finish();
            const auto header = WavExportWriter::inspect(writer.partialFile());
            exportRequire(header.sampleCount == std::uint64_t(j.range.sampleCount), "Material WAV common count mismatch");
            row = jsonObject(); jsonSet(row, "verified", true); jsonSet(row, "trackId", item.audio.trackId);
            jsonSet(row, "assetIds", TimelineExporter::assetIds(j, item.audio)); jsonSet(row, "sourceChannel", item.sourceChannel);
            jsonSet(row, "sampleCount", j.range.sampleCount); jsonSet(row, "frameCount", j.range.frameCount);
            jsonSet(row, "Fs", j.snapshot.Fs); jsonSet(row, "channels", item.channels); jsonSet(row, "bitsPerSample", 24);
            jsonSet(row, "rf64", header.rf64); jsonSet(row, "ignoresMuteSolo", true);
        }
        jsonSet(row, "name", item.name); jsonSet(row, "outputOrigin", 0);
        jsonSet(row, "commonFrameCount", j.range.frameCount); jsonSet(row, "commonPcmSampleCount", j.range.sampleCount);
        jsonSet(row, "commonPcmFs", j.snapshot.Fs); files.add(row); progress(item.name + "/verified", i, 1);
    }
    control.checkpoint(); publication.commit(files, control, faults);
    auto result = j.manifest(files); const auto seconds = elapsed();
    jsonSet(result, "mode", "materials"); jsonSet(result, "elapsedSeconds", seconds);
    jsonSet(result, "renderedVideoFrames", double(cameraCount) * double(j.range.frameCount));
    jsonSet(result, "effectiveFps", seconds > 0 ? double(cameraCount) * double(j.range.frameCount) / seconds : 0);
    jsonSet(result, "speedMeasurement", "Sequential cameras + audio files + full verification + durable publication");
    if (control.onProgress) try { control.onProgress({"complete", 1, seconds, 0.0}); } catch (...) {}
    return result;
}
}
