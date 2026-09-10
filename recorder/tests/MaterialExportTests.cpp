#include "export/MaterialExporter.h"
#include "export/WavExportWriter.h"
#include "record/ReferenceMixWriter.h"
#include "diagnostics/CaptureTelemetry.h"
#include "AudioRenderFixtures.h"
#include "RecordedGapFixtures.h"
#include "TestSupport.h"
#include "support/Platform.h"
#include "model/RecorderSerializer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
// Independent raw-clip/source-availability oracle. Camera pixels are encoded by
// the CPU adapter below; this does not claim a D3D11VA/NVENC product run.
Sample sourceAt(const ExportJob& j, TrackKind kind, Sample frame)
{
    const auto at = frameToSample(j.range.firstFrame + frame, j.snapshot.Fs, j.snapshot.fps);
    for (const auto& t : j.snapshot.tracks) if (t.kind == kind)
        for (const auto& c : t.clips.items()) if (j.snapshot.isActive(c) && at >= c.timelineStartSample && at < c.timelineEnd())
        {
            const auto source = c.sourceIn + (at - c.timelineStartSample); const auto* a = j.snapshot.media->findAsset(c.assetId);
            for (const auto& range : a->availableRanges) if (source >= range.start && source < range.start + range.length) return source;
        }
    return -1;
}
int pixel(Sample sample, TrackKind camera)
{ return sample < 0 ? 16 : 40 + int((sample / 1600) % 60) + (camera == TrackKind::cam2 ? 90 : 0); }
juce::var cpuCamera(const ExportJob& j, const FinalExportSelection& selection, ExportControl& control, FileIoFaultAdapter* faults)
{
    ExportActivity::Lease lease(control.activity); ExportPublication publication(j);
    const auto* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4); require(codec != nullptr, "CPU camera fixture encoder");
    CodecPtr video(avcodec_alloc_context3(codec)); video->width = video->height = 16; video->pix_fmt = AV_PIX_FMT_YUV420P;
    video->time_base = {int(j.snapshot.fps.denominator), int(j.snapshot.fps.numerator)}; video->framerate = av_inv_q(video->time_base);
    video->gop_size = 30; video->max_b_frames = 0; video->thread_count = 1; video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ffCheck(avcodec_open2(video.get(), codec, nullptr), "Open material CPU fixture codec");
    ReferenceMixWriter audio(j.snapshot.Fs);
    ExportAudioRenderer pcm(j, TimelineExporter::openSources(j, selection.audio, control), selection.audio);
    const auto partial = publication.file("final.mp4").getSiblingFile("final.mp4.partial");
    Sample at = 0, blacks = 0;
    {
        FinalMp4Writer mux(partial, *video, audio.context(), faults);
        auto frame = ffFrame(); frame->width = frame->height = 16; frame->format = AV_PIX_FMT_YUV420P;
        ffCheck(av_frame_get_buffer(frame.get(), 32), "Allocate CPU camera frame");
        auto packet = ffPacket();
        const auto receive = [&]
        {
            for (;;)
            {
                const auto code = avcodec_receive_packet(video.get(), packet.get()); if (code == AVERROR_EOF || code == AVERROR(EAGAIN)) return;
                ffCheck(code, "Receive CPU camera packet"); packet->duration = 1; mux.video(*packet); av_packet_unref(packet.get());
            }
        };
        const PacketSink sink = [&](const AVPacket& p) { mux.audio(p); }; std::vector<float> left(4096), right(left.size()), stereo(left.size() * 2);
        for (Sample n = 0; n < j.range.frameCount; ++n)
        {
            control.checkpoint(); const auto mapped = FinalVideoExporter::mappingAt(j, selection.video, n);
            const auto expected = sourceAt(j, selection.video, n);
            require(mapped.black() == (expected < 0) && (expected < 0 || mapped.sourceSample == expected), "Compiled camera mapping vs independent clip oracle");
            blacks += mapped.black() ? 1 : 0; ffCheck(av_frame_make_writable(frame.get()), "Writable CPU frame");
            for (int p = 0; p < 3; ++p) for (int y = 0; y < (p ? 8 : 16); ++y)
                std::memset(frame->data[p] + y * frame->linesize[p], p ? 128 : pixel(mapped.black() ? -1 : mapped.sourceSample, selection.video), p ? 8 : 16);
            frame->pts = n; ffCheck(avcodec_send_frame(video.get(), frame.get()), "Encode CPU material frame"); receive();
            const auto end = frameToSample(n + 1, j.snapshot.Fs, j.snapshot.fps);
            while (at < end)
            {
                const auto count = static_cast<unsigned>((std::min)(Sample(left.size()), end - at)); pcm.render(at, count, left.data(), right.data());
                for (unsigned i = 0; i < count; ++i) { stereo[i * 2] = left[i]; stereo[i * 2 + 1] = right[i]; }
                audio.append(stereo.data(), count, sink); at += count;
            }
            if (control.onProgress) control.onProgress({"render", .8 * double(n + 1) / j.range.frameCount, .1, {}});
        }
        ffCheck(avcodec_send_frame(video.get(), nullptr), "Drain CPU camera"); receive(); audio.finishInput(sink); mux.finish();
    }
    Sample decoded = 0; ExportVerificationObserver observer;
    observer.video = [&](Sample n, const AVFrame& frame)
    { ++decoded; require(std::abs(int(frame.data[0][0]) - pixel(sourceAt(j, selection.video, n), selection.video)) <= 3, "Decoded MP4 camera/source/black pixel oracle"); };
    auto row = FinalVideoExporter::verify(partial, j.range.frameCount, j.range.sampleCount, j.snapshot.Fs, j.snapshot.fps, control, observer, AV_CODEC_ID_MPEG4);
    require(decoded == j.range.frameCount && at == j.range.sampleCount, "Full CPU decode/render counts");
    jsonSet(row, "name", "final.mp4"); jsonSet(row, "blackFrames", blacks); jsonSet(row, "testCodec", "software-mpeg4");
    jsonSet(row, "audioAssetIds", TimelineExporter::assetIds(j, selection.audio));
    juce::Array<juce::var> files{row}; publication.commit(files, control, faults); return j.manifest(files);
}
std::vector<float> readMono(const juce::File& path)
{
    juce::WavAudioFormat format; std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(path.createInputStream().release(), true));
    require(reader && reader->bitsPerSample == 24 && reader->numChannels == 1, "Independent PCM24 WAV reader");
    std::vector<float> pcm(static_cast<std::size_t>(reader->lengthInSamples)); float* out = pcm.data();
    require(reader->read(&out, 1, 0, int(pcm.size())), "Read material WAV"); return pcm;
}
Id addImport(recorder_audio_fixture::Fixture& f)
{
    auto& p = f.project; MediaAsset a; a.kind = AssetKind::importAudio; a.logicalLength = p.Fs * 10; a.mediaGeneration = 1;
    a.relativePath = "media/imports/" + a.assetId + "/completed.wav"; const auto file = f.root.getChildFile(a.relativePath);
    recorder_audio_fixture::writePcm24(file, p.Fs, f.pcm[0], 0, a.logicalLength); f.originals.push_back(file);
    AudioImportControl control; ImportedAudioInfo info; exportCheck(AudioImport::inspect(file, control, info));
    CaptureTelemetry::writeJson(file.getSiblingFile(".import-info.json"), info.toVar());
    a.contentIdentity = info.contentHash; a.availableRanges = {{0, a.logicalLength}};
    a.originalFormat.codec = info.codec; a.originalFormat.sampleRate = p.Fs; a.originalFormat.channels = 1; a.originalFormat.bitsPerSample = 24;
    auto registry = std::make_shared<MediaRegistry>(*p.media); registry->assets.push_back(a); p.media = registry;
    Track track; track.kind = TrackKind::importAudio; track.name = "completed"; track.mute = track.solo = true;
    Clip c; c.trackId = track.trackId; c.assetId = a.assetId; c.sourceIn = 17; c.lengthSamples = p.Fs * 3;
    track.clips.edit().push_back(c); p.tracks.push_back(track); exportCheck(p.validate()); return a.assetId;
}
void edit(RecorderProject& p, ClipEditResult next) { exportCheck(next.status); p = std::move(next.project); ++p.editRevision; }
std::vector<Id> clips(const Track& t) { std::vector<Id> ids; for (const auto& c : t.clips.items()) ids.push_back(c.clipId); return ids; }
}
int runMaterialExportTests()
{
    Suite s;
    for (unsigned channels : {1u, 2u}) for (const auto gap : recorder_audio_fixture::recordedGaps)
    {
        const auto name = "Material export with healthy take: " + std::to_string(channels) + " channels, " + recorder_audio_fixture::gapName(gap);
        s.test(name.c_str(), [=]
        {
            recorder_audio_fixture::RecordedGapFixture f(channels, gap);
            ExportActivity gate; ExportControl control(gate); ExportJob job(f.project, f.root);
            const auto manifest = MaterialExporter::run(job, {}, control, nullptr, cpuCamera);
            require(manifest["files"].size() == 2, "Publish camera reference and microphone material together");
            const auto file = job.outputDirectory.getChildFile("mic01.wav");
            const auto header = WavExportWriter::inspect(file);
            require(header.channels == channels && header.sampleCount == f.totalFrames, "Material channel layout and both take lengths");
            juce::WavAudioFormat format;
            std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(file.createInputStream().release(), true));
            std::vector<float> l(f.totalFrames), r(f.totalFrames); float* dst[]{l.data(), r.data()};
            require(reader && reader->read(dst, int(channels), 0, int(f.totalFrames)), "Independently decode complete material WAV");
            if (channels == 1) r = l;
            f.verifyPcm(l, r);
        });
    }
    s.test("Material export emits one interleaved stereo mic WAV alongside mono and camera files", []
    {
        recorder_audio_fixture::Fixture f(48000, true); ExportActivity gate; ExportControl control(gate);
        ExportJob job(f.project, f.root, {}, SampleRange{0,4800}); const auto outputs = MaterialExporter::outputs(job, {});
        require(outputs.size() == 4, "Two cameras plus two slots");
        const auto manifest = MaterialExporter::run(job, {}, control, nullptr, cpuCamera);
        require(manifest["files"].size() == 4, "Published complete material set");
        const auto file = job.outputDirectory.getChildFile("mic01.wav"); const auto header = WavExportWriter::inspect(file);
        require(header.channels == 2 && header.sampleCount == 4800, "Stereo material frame count");
        require(WavExportWriter::inspect(job.outputDirectory.getChildFile("mic02.wav")).channels == 1, "Mono material unchanged");
        juce::WavAudioFormat format; std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(file.createInputStream().release(),true));
        float l[1]{},r[1]{}; float* dst[]{l,r}; require(reader && reader->read(dst,2,333,1), "Stereo material independent reread");
        require(l[0] == f.sample(0,333) && r[0] == float(-f.pcm[0][333]/2)/8388608.0f, "Material export L/R oracle");
    });
    s.test("Two cameras linked cut, unlinked one-sample move, mic ripple, camera gap and edited import share exact end", []
    {
        recorder_audio_fixture::Fixture f; auto& p = f.project; std::vector<Id> linked;
        for (const auto& t : p.tracks) linked.push_back(t.clips.items()[0].clipId);
        edit(p, ClipEdits::link(p, linked)); edit(p, ClipEdits::remove(p, {p.tracks[0].clips.items()[0].clipId}, {2 * Sample(p.Fs), p.Fs}));
        edit(p, ClipEdits::unlink(p, clips(p.tracks[2]))); edit(p, ClipEdits::move(p, clips(p.tracks[2]), 1, false));
        edit(p, ClipEdits::unlink(p, clips(p.tracks[3]))); edit(p, ClipEdits::rippleDeleteTracks(p, {4 * Sample(p.Fs), p.Fs}, {p.tracks[3].trackId}));
        auto registry = std::make_shared<MediaRegistry>(*p.media); registry->assets[1].availableRanges = {{0, 6 * Sample(p.Fs)}, {7 * Sample(p.Fs), 3 * Sample(p.Fs)}};
        registry->assets[1].gaps = {{6 * Sample(p.Fs), p.Fs}}; p.media = registry; p.tracks[2].mute = true; p.tracks[3].solo = true;
        addImport(f); const auto hashes = f.hashes(); const auto fingerprint = RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(p));
        ExportJob job(p, f.root, f.root.getChildFile("materials")); ExportActivity gate; ExportControl control(gate);
        const auto result = MaterialExporter::run(job, {true, {}}, control, nullptr, cpuCamera);
        require(job.range.frameCount == 301 && job.range.sampleCount == 481600 && result["files"].size() == 5, "One-sample move expands all outputs by one common frame");
        const auto a = readMono(job.outputDirectory.getChildFile("mic01.wav")), b = readMono(job.outputDirectory.getChildFile("mic02.wav")), imported = readMono(job.outputDirectory.getChildFile("import_completed.wav"));
        require(a.size() == 481600 && b.size() == a.size() && imported.size() == a.size(), "WAV common sample lengths");
        for (Sample n : {Sample{1001}, Sample{48001}, Sample{192501}, Sample{384001}})
        {
            require(a[std::size_t(n)] == f.sample(0, n - 1), "One-sample mic edit retained despite camera cuts/mute");
            const auto source = n >= 4 * Sample(p.Fs) ? n + p.Fs : n;
            require(b[std::size_t(n)] == f.sample(1, source), "Mic-only ripple retains independent mapping despite solo");
        }
        require(a[0] == 0 && b[Sample(p.Fs) * 2 + 500] == 0 && b[Sample(p.Fs) * 9 + 500] == 0 && a.back() == 0, "Gaps and padded tails are silent");
        require(imported[1001] == f.sample(0, 1018) && imported[Sample(p.Fs) * 4] == 0, "Edited imported WAV ignores mute/solo and retains sourceIn");
        Sample compared = 0;
        for (Sample n = 0; n < job.range.sampleCount; ++n)
        {
            const Sample Fs = p.Fs;
            const auto outsideFade = [n](std::initializer_list<Sample> boundaries)
            { for (auto boundary : boundaries) if (std::abs(n - boundary) <= 144) return false; return true; };
            if (outsideFade({0, 1, 2 * Fs + 1, 3 * Fs + 1, 10 * Fs + 1}))
            {
                const auto u = n - 1; const bool gap = u < 0 || u >= 10 * Fs || (u >= 2 * Fs && u < 3 * Fs);
                require(a[std::size_t(n)] == (gap ? 0.0f : f.sample(0, u)), "Every material mic1 sample outside fades matches independent edits"); ++compared;
            }
            if (outsideFade({0, 2 * Fs, 3 * Fs, 4 * Fs, 9 * Fs}))
            {
                const bool gap = n >= 9 * Fs || (n >= 2 * Fs && n < 3 * Fs); const auto u = n >= 4 * Fs ? n + Fs : n;
                require(b[std::size_t(n)] == (gap ? 0.0f : f.sample(1, u)), "Every material mic2 sample outside fades matches independent ripple"); ++compared;
            }
            if (outsideFade({0, 3 * Fs}))
            { require(imported[std::size_t(n)] == (n >= 3 * Fs ? 0.0f : f.sample(0, n + 17)), "Every imported material sample outside fades matches sourceIn"); ++compared; }
        }
        require(compared > 1400000, "Whole-output PCM oracle coverage");
        const auto manifest = juce::JSON::parse(job.outputDirectory.getChildFile("export-manifest.json"));
        require(manifest["gaps"].size() > 0 && manifest["microfades"].size() > 0 && manifest["microfadePolicy"].isString(), "Gaps and shared fade policy in committed manifest");
        for (const auto& file : *manifest["files"].getArray())
            require(Sample(file["commonFrameCount"]) == 301 && Sample(file["commonPcmSampleCount"]) == 481600 && int(file["outputOrigin"]) == 0, "Each manifest row uses common origin/counts");
        require(Sample(manifest["files"][1]["blackFrames"]) > Sample(manifest["files"][0]["blackFrames"]), "Cam2 gap remains black independently");
        require(f.hashes() == hashes && RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(p)) == fingerprint, "Original media and edit snapshot unchanged");
    });
    s.test("Active retake changes its camera mapping; inactive long version never extends export", []
    {
        recorder_audio_fixture::Fixture f; auto& p = f.project; TakeStack stack; stack.anchorSample = 0; stack.spanSamples = p.Fs * 10;
        TakeVersion previous, active;
        for (unsigned cam = 0; cam < 2; ++cam)
        {
            auto old = p.tracks[cam].clips.items()[0], current = old; old.takeStackId = current.takeStackId = stack.stackId;
            old.versionId = previous.versionId; current.versionId = active.versionId; current.clipId = newId(); current.sourceIn = p.Fs * 4; current.lengthSamples = p.Fs * 2;
            previous.clipIds.push_back(old.clipId); active.clipIds.push_back(current.clipId); p.tracks[cam].clips.edit() = {old, current};
        }
        p.tracks.erase(p.tracks.begin() + 2, p.tracks.end()); stack.versions = {previous, active}; stack.activeVersionId = active.versionId; p.takeStacks.push_back(stack); exportCheck(p.validate());
        ExportJob job(p, f.root, f.root.getChildFile("retake")); require(job.range.frameCount == 60, "Inactive version excluded from common end");
        ExportActivity gate; ExportControl control(gate); const auto result = MaterialExporter::run(job, {}, control, nullptr, cpuCamera);
        require(result["files"].size() == 2 && FinalVideoExporter::mappingAt(job, TrackKind::cam2, 0).sourceSample == p.Fs * 4, "Both cams use active version");
    });
    s.test("Unused cam2 omitted, used cam2 with no active clip remains a black lane", []
    {
        recorder_audio_fixture::Fixture f; auto p = f.project; auto registry = std::make_shared<MediaRegistry>(*p.media);
        registry->takes[0].cam2AssetId.clear(); registry->assets.erase(registry->assets.begin() + 1); p.media = registry; p.tracks[1].clips.edit().clear();
        ExportJob omitted(p, f.root); const auto list = MaterialExporter::outputs(omitted, {});
        require(list.size() == 3 && list[0].name == "cam1.mp4", "Never-used cam2 omitted");
        p.media = f.project.media; ExportJob used(p, f.root); const auto all = MaterialExporter::outputs(used, {});
        require(all.size() == 4 && all[1].name == "cam2.mp4" && FinalVideoExporter::mappingAt(used, TrackKind::cam2, 0).black(), "Used but absent lane is black");
    });
    s.test("Dubbing reference chooses completed audio; ambiguous imports need explicit choice", []
    {
        recorder_audio_fixture::Fixture f; const auto id = addImport(f); auto registry = std::make_shared<MediaRegistry>(*f.project.media);
        registry->takes[0].mode = TakeMode::dub; f.project.media = registry; ExportJob job(f.project, f.root);
        const auto reference = MaterialExporter::referenceAudio(job, {});
        require(reference.kind == AudioSourceMask::Kind::completedAudio && reference.assetId == id, "Dubbing reference is completed source");
        addImport(f); ExportJob ambiguous(f.project, f.root); rejects([&] { MaterialExporter::referenceAudio(ambiguous, {}); });
        const auto list = MaterialExporter::outputs(ambiguous, {true, reference});
        require(list[list.size() - 2].name == "import_completed.wav" && list.back().name == "import_completed (2).wav", "Duplicate import names resolve without dropping tracks");
    });
    s.test("Cancellation after verified cam1 preserves no partial material set", []
    {
        recorder_audio_fixture::Fixture f; const auto hashes = f.hashes(); ExportActivity gate; ExportControl control(gate);
        ExportJob job(f.project, f.root, f.root.getChildFile("cancelled"), SampleRange{17, 5000});
        control.onProgress = [&](const ExportProgress& p) { if (p.stage == "cam1.mp4/verified") control.cancelled.store(true); };
        rejects([&] { MaterialExporter::run(job, {}, control, nullptr, cpuCamera); });
        require(!job.outputDirectory.exists() && !job.partialDirectory.exists() && f.hashes() == hashes, "Cancelled multi-file job cleans only own partials");
        require(gate.current() == ExportActivity::State::idle, "Material lease released on cancellation");
    });
    s.test("Parent cancellation reaches child preparation checkpoints without progress or file callbacks", []
    {
        recorder_audio_fixture::Fixture f; ExportActivity gate; ExportControl control(gate);
        ExportJob job(f.project, f.root, f.root.getChildFile("cancel-prepare"), SampleRange{0, 1600});
        bool observed = false;
        const MaterialExporter::CameraRenderer preparing = [&](const ExportJob&, const FinalExportSelection&, ExportControl& child, FileIoFaultAdapter*) -> juce::var
        {
            control.cancelled.store(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline)
            {
                try { child.checkpoint(); }
                catch (const ExportCancelled&) { observed = true; throw; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            throw std::runtime_error("Child preparation never observed parent cancellation");
        };
        rejects([&] { MaterialExporter::run(job, {}, control, nullptr, preparing); });
        require(observed && !job.outputDirectory.exists() && !job.partialDirectory.exists() && gate.current() == ExportActivity::State::idle,
            "Prepare cancellation propagates and all worker/partial ownership unwinds");
    });
    return s.result("MaterialExportTests");
}
