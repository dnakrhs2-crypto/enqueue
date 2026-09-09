#include "ExportJob.h"
#include "app/RecorderDocument.h"
#include "support/Platform.h"
#include <algorithm>
#include <limits>
#include <set>

namespace gocue::recorder
{
void exportCheck(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
void exportRequire(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void exportRename(const juce::File& from, const juce::File& to)
{
    if (!MoveFileExW(from.getFullPathName().toWideCharPointer(), to.getFullPathName().toWideCharPointer(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Publish without replacement failed (Win32 " + std::to_string(GetLastError()) + ")");
}
ExportRange ExportRange::expand(SampleRange requested, std::uint32_t Fs, FrameRate fps)
{
    const auto limit = (std::numeric_limits<Sample>::max)();
    exportRequire(Fs && Fs <= 768000 && fps.numerator && fps.denominator
        && std::uint64_t(Fs) * fps.denominator >= fps.numerator && requested.start >= 0
        && requested.length > 0 && requested.start <= limit - requested.length, "Invalid export range/timebase");
    const auto end = requested.start + requested.length;
    // Use the same rounded sample grid as ClipEdits/MediaIndex, including Fs/fps
    // combinations where an exact frame boundary lies between audio samples.
    auto first = sampleToFrame(requested.start, Fs, fps), last = sampleToFrame(end, Fs, fps);
    if (frameToSample(first, Fs, fps) > requested.start) --first;
    if (frameToSample(last, Fs, fps) < end) { exportRequire(last < limit, "Export frame end overflows"); ++last; }
    exportRequire(last > first, "Empty expanded export range");
    const auto start = frameToSample(first, Fs, fps), count = frameToSample(last - first, Fs, fps);
    exportRequire(count > 0 && start <= limit - count, "Export PCM range overflows");
    return {requested, first, last - first, start, count};
}
double ExportRange::startSeconds(std::uint32_t, FrameRate fps) const
{ return double(firstFrame) * fps.denominator / fps.numerator; }
double ExportRange::endSeconds(std::uint32_t, FrameRate fps) const
{ return double(endFrame()) * fps.denominator / fps.numerator; }
juce::var ExportRange::toJson(std::uint32_t Fs, FrameRate fps) const
{
    auto v = jsonObject(); jsonSet(v, "requestedStartSample", requested.start); jsonSet(v, "requestedEndSample", requested.start + requested.length);
    jsonSet(v, "firstFrame", firstFrame); jsonSet(v, "endFrameExclusive", endFrame()); jsonSet(v, "frameCount", frameCount);
    jsonSet(v, "startSample", startSample); jsonSet(v, "sampleCount", sampleCount);
    jsonSet(v, "displayStartSeconds", startSeconds(Fs, fps)); jsonSet(v, "displayEndSeconds", endSeconds(Fs, fps));
    jsonSet(v, "outputOrigin", 0); jsonSet(v, "tailPadSamples", (std::max)(Sample{0}, startSample + sampleCount - requested.start - requested.length));
    jsonSet(v, "rounding", "round(frameCount * Fs * fps.den / fps.num); absolute rounded frame grid, at most 0.5 sample duration error");
    return v;
}
bool ExportActivity::beginRecording()
{ auto expected = State::idle; return state.compare_exchange_strong(expected, State::recording, std::memory_order_acq_rel); }
void ExportActivity::endRecording()
{ auto expected = State::recording; state.compare_exchange_strong(expected, State::idle, std::memory_order_acq_rel); }
ExportActivity::Lease::Lease(ExportActivity& a) : activity(a)
{
    auto expected = State::idle;
    exportRequire(activity.state.compare_exchange_strong(expected, State::exporting, std::memory_order_acq_rel), "Recording/export is already active");
}
ExportActivity::Lease::~Lease() { activity.state.store(State::idle, std::memory_order_release); }
void ExportControl::checkpoint() const { if (cancelled.load(std::memory_order_acquire)) throw ExportCancelled(); }
namespace
{
RecorderProject freeze(const RecorderProject& p, bool recording)
{
    exportRequire(!recording, "Cannot export while the document is recording");
    exportCheck(p.validate());
    for (const auto& take : p.media->takes) exportRequire(take.state != TakeState::recording, "Cannot snapshot a recording take");
    auto copy = p; copy.media = std::make_shared<const MediaRegistry>(*p.media);
    return copy;
}
juce::File destination(const juce::File& project, juce::File requested, const Id& id)
{
    if (requested == juce::File()) return project.getChildFile("exports").getChildFile(id);
    if (requested.isDirectory()) return requested.getChildFile(id);
    exportRequire(!requested.exists(), "Export destination is an existing file");
    return requested;
}
}
ExportJob::ExportJob(const RecorderProject& p, juce::File directory, juce::File out, std::optional<SampleRange> selected, bool recording)
    : snapshot(freeze(p, recording)), audioPlan(compileAudioRenderPlan(snapshot)),
      range(ExportRange::expand(selected.value_or(SampleRange{0, snapshot.activeTimelineEnd()}), p.Fs, p.fps)),
      projectDirectory(std::move(directory)), outputDirectory(destination(projectDirectory, std::move(out), jobId)),
      partialDirectory(outputDirectory.getSiblingFile(outputDirectory.getFileName() + "." + jobId + ".partial")) {}
ExportJob ExportJob::fromDocument(const RecorderDocument& d, juce::File out, std::optional<SampleRange> range)
{ return ExportJob(d.getProject(), d.getFile().getParentDirectory(), std::move(out), range, d.isRecordingStructureLocked()); }
juce::var ExportJob::manifest(const juce::Array<juce::var>& files) const
{
    auto v = jsonObject(); jsonSet(v, "schemaVersion", 1); jsonSet(v, "jobId", jobId); jsonSet(v, "projectId", snapshot.projectId);
    jsonSet(v, "editRevision", plan().editRevision); jsonSet(v, "range", range.toJson(snapshot.Fs, snapshot.fps));
    jsonSet(v, "Fs", snapshot.Fs); jsonSet(v, "fpsNumerator", snapshot.fps.numerator); jsonSet(v, "fpsDenominator", snapshot.fps.denominator);
    jsonSet(v, "files", files); jsonSet(v, "recordingMayOverlap", mayOverlapRecording);
    jsonSet(v, "microfadePolicy", "Shared TimelineAudioRenderer; 3ms linear, <= half adjacent continuous run; internal cuts/gaps only; no new export-boundary or transport ramps");
    jsonSet(v, "tailPolicy", "Silence after requested end through common frame-aligned end; never shortest-source truncation");
    juce::Array<juce::var> gaps, sources, fades;
    std::set<Id> ids;
    for (const auto& t : plan().tracks) for (const auto& s : t.spans)
    {
        const auto a = (std::max)(s.timeline.start, range.startSample), b = (std::min)(s.timeline.start + s.timeline.length, range.startSample + range.sampleCount);
        if (a >= b) continue;
        if (!s.isGap()) { ids.insert(s.assetId); continue; }
        auto g = jsonObject(); jsonSet(g, "trackId", t.trackId); jsonSet(g, "startSample", a - range.startSample); jsonSet(g, "sampleCount", b - a); gaps.add(g);
    }
    // Compiler spans stop at the active timeline end. An explicit export may
    // extend farther; represent that absent tail for every lane in the manifest.
    const auto absentStart = (std::max)(range.startSample, plan().timelineEnd), outputEnd = range.startSample + range.sampleCount;
    if (absentStart < outputEnd) for (const auto& t : plan().tracks)
    {
        auto g = jsonObject(); jsonSet(g, "trackId", t.trackId); jsonSet(g, "startSample", absentStart - range.startSample);
        jsonSet(g, "sampleCount", outputEnd - absentStart); gaps.add(g);
    }
    auto padding = jsonObject(); const auto padStart = (std::min)(outputEnd, range.requested.start + range.requested.length);
    jsonSet(padding, "startSample", padStart - range.startSample); jsonSet(padding, "sampleCount", outputEnd - padStart);
    jsonSet(v, "audioFrameAlignmentPadding", padding);
    for (const auto& id : ids)
    {
        const auto& asset = *snapshot.media->findAsset(id); auto a = jsonObject(); jsonSet(a, "assetId", id);
        jsonSet(a, "contentIdentity", asset.contentIdentity); jsonSet(a, "mediaGeneration", asset.mediaGeneration); sources.add(a);
    }
    for (const auto& f : plan().microfadeBoundaries)
        if (f.timelineSample + f.afterSamples > range.startSample && f.timelineSample - f.beforeSamples < range.startSample + range.sampleCount)
    {
        auto a = jsonObject(); jsonSet(a, "trackId", f.trackId); jsonSet(a, "sample", f.timelineSample - range.startSample);
        jsonSet(a, "beforeSamples", f.beforeSamples); jsonSet(a, "afterSamples", f.afterSamples); fades.add(a);
    }
    jsonSet(v, "gaps", gaps); jsonSet(v, "originalAssets", sources); jsonSet(v, "microfades", fades); return v;
}
ExportPublication::ExportPublication(const ExportJob& j) : job(j)
{
    exportRequire(!job.outputDirectory.exists() && !job.partialDirectory.exists(), "Export output collision");
    exportCheck(job.partialDirectory.getParentDirectory().createDirectory());
    exportRequire(CreateDirectoryW(job.partialDirectory.getFullPathName().toWideCharPointer(), nullptr) != FALSE, "Cannot exclusively create export partial directory");
    owns = true;
}
ExportPublication::~ExportPublication()
{
    if (owns && !published && job.partialDirectory.getParentDirectory() == job.outputDirectory.getParentDirectory()
        && job.partialDirectory.getFileName().endsWith("." + job.jobId + ".partial")) job.partialDirectory.deleteRecursively();
}
juce::File ExportPublication::file(const juce::String& name) const
{
    exportRequire(isProjectRelativePath(name) && !name.containsChar('/') && !name.endsWith(".partial")
        && name != "export-manifest.json", "Invalid export output filename");
    return job.partialDirectory.getChildFile(name);
}
void ExportPublication::commit(const juce::Array<juce::var>& files, ExportControl& control, FileIoFaultAdapter* faults)
{
    exportRequire(owns && !published && !files.isEmpty(), "Cannot commit an empty/already published export"); control.checkpoint();
    std::set<juce::String> names;
    for (const auto& f : files)
    {
        const auto name = f["name"].toString(); const auto target = file(name);
        exportRequire(names.insert(name.toLowerCase()).second && f["verified"] == juce::var(true)
            && target.getSiblingFile(name + ".partial").existsAsFile() && !target.exists(), "Unverified/missing/duplicate export output");
    }
    const auto manifest = job.partialDirectory.getChildFile("export-manifest.json");
    DurableFile output(faults); exportCheck(output.open(manifest.getSiblingFile("export-manifest.json.partial"), DurableFile::OpenMode::createNew));
    const auto bytes = juce::JSON::toString(job.manifest(files), false).toUTF8();
    exportCheck(output.write(bytes.getAddress(), bytes.sizeInBytes() - 1)); exportCheck(output.flushData()); exportCheck(output.close());
    for (const auto& f : files) { const auto target = file(f["name"].toString()); exportRename(target.getSiblingFile(target.getFileName() + ".partial"), target); }
    exportRename(manifest.getSiblingFile("export-manifest.json.partial"), manifest);
    exportRename(job.partialDirectory, job.outputDirectory); published = true;
}
}
