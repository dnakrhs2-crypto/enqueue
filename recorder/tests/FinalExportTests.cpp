#include "export/FinalVideoExporter.h"
#include "record/NvencEncoder.h"
#include "record/ReferenceMixWriter.h"
#include "playback/VideoPlaybackEngine.h"
#include "AudioRenderFixtures.h"
#include "RecordedGapFixtures.h"
#include "TimelineGapFixtures.h"
#include "TestSupport.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
struct MemoryPcm final : PlaybackAudioSource
{
    float l = .7f, r = -.3f;
    MemoryPcm(Sample samples, int ch = 2) { length = samples; channels = ch; sampleRate = 48000; generation = 1; epoch = std::make_shared<MediaEpoch>(); }
    void read(Sample at, unsigned count, float* left, float* right) const override
    { require(at >= 0 && at + count <= length, "Fixture source bounds"); std::fill_n(left, count, l); std::fill_n(right, count, channels == 1 ? l : r); }
};
Id addImport(RecorderProject& p, Track* existing = nullptr, int channels = 2)
{
    auto registry = std::make_shared<MediaRegistry>(*p.media); MediaAsset a; a.kind = AssetKind::importAudio; a.mediaGeneration = 1;
    a.relativePath = "media/imports/" + a.assetId + "/audio.wav"; a.contentIdentity = "test-import"; a.logicalLength = Sample(p.Fs) * 10;
    a.availableRanges = {{0, a.logicalLength}}; a.originalFormat.codec = "pcm_f32le"; a.originalFormat.sampleRate = p.Fs; a.originalFormat.channels = channels; a.originalFormat.bitsPerSample = 32;
    registry->assets.push_back(a); p.media = registry;
    if (!existing) { Track t; t.kind = TrackKind::importAudio; p.tracks.push_back(t); existing = &p.tracks.back(); }
    Clip c; c.assetId = a.assetId; c.trackId = existing->trackId; c.timelineStartSample = p.Fs; c.sourceIn = p.Fs * 2; c.lengthSamples = p.Fs * 2;
    existing->clips.edit().push_back(c); return a.assetId;
}
std::pair<float,float> pcmAt(const ExportJob& j, const AudioSourceMask& mask, std::vector<AudioSourceBinding> b, Sample at)
{
    ExportAudioRenderer renderer(j, std::move(b), mask); float l = 0, r = 0; renderer.render(at, 1, &l, &r); return {l,r};
}
CodecPtr softwareVideo()
{
    const auto* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4); require(codec != nullptr, "Software mux test codec");
    CodecPtr c(avcodec_alloc_context3(codec)); require(bool(c), "Software codec allocation");
    c->width = c->height = 16; c->pix_fmt = AV_PIX_FMT_YUV420P; c->time_base = {1,30}; c->framerate = {30,1};
    c->gop_size = 30; c->max_b_frames = 0; c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; c->thread_count = 1;
    ffCheck(avcodec_open2(c.get(), codec, nullptr), "Open CPU mux fixture"); return c;
}
void writeMuxFixture(const juce::File& partial, unsigned Fs, Sample frameCount, FileIoFaultAdapter* fault = nullptr,
                     ExportAudioRenderer* rendered = nullptr, const ExportJob* mapped = nullptr)
{
    auto video = softwareVideo(); ReferenceMixWriter audio(Fs); FinalMp4Writer mux(partial, *video, audio.context(), fault);
    auto f = ffFrame(); f->width = f->height = 16; f->format = AV_PIX_FMT_YUV420P; ffCheck(av_frame_get_buffer(f.get(),32), "Allocate CPU fixture frame");
    for (int p = 0; p < 3; ++p) for (int y = 0; y < (p ? 8 : 16); ++y) std::memset(f->data[p] + y * f->linesize[p], p ? 128 : 64, p ? 8 : 16);
    auto packet = ffPacket(); const auto receive = [&]
    {
        for (;;)
        {
            const auto code = avcodec_receive_packet(video.get(), packet.get()); if (code == AVERROR_EOF || code == AVERROR(EAGAIN)) return;
            ffCheck(code, "Receive CPU fixture"); packet->duration = 1; mux.video(*packet); av_packet_unref(packet.get());
        }
    };
    Sample at = 0; std::vector<float> stereo(8192); const PacketSink audioSink = [&](const AVPacket& p) { mux.audio(p); };
    for (Sample n = 0; n < frameCount; ++n)
    {
        if (mapped)
        {
            ffCheck(av_frame_make_writable(f.get()), "Writable mapped CPU fixture frame");
            const auto black = FinalVideoExporter::mappingAt(*mapped, TrackKind::cam1, n).black();
            for (int p = 0; p < 3; ++p) for (int y = 0; y < (p ? 8 : 16); ++y)
                std::memset(f->data[p] + y * f->linesize[p], p ? 128 : black ? 16 : 64, p ? 8 : 16);
        }
        f->pts = n; ffCheck(avcodec_send_frame(video.get(), f.get()), "Encode CPU fixture"); receive();
        const auto end = frameToSample(n + 1, Fs, {30,1});
        while (at < end)
        {
            const auto count = static_cast<unsigned>((std::min)(Sample{4096}, end - at));
            for (unsigned i = 0; i < count; ++i)
            { stereo[std::size_t(i) * 2] = .2f * float(std::sin(6.283185307179586 * 440 * (at + i) / Fs)); stereo[std::size_t(i) * 2 + 1] = -.1f * float(std::sin(6.283185307179586 * 660 * (at + i) / Fs)); }
            if (rendered)
            {
                std::vector<float> l(count), r(count); rendered->render(at, count, l.data(), r.data());
                for (unsigned i = 0; i < count; ++i) { stereo[i*2] = l[i]; stereo[i*2+1] = r[i]; }
            }
            audio.append(stereo.data(), count, audioSink); at += count;
        }
    }
    ffCheck(avcodec_send_frame(video.get(), nullptr), "Drain CPU fixture"); receive(); audio.finishInput(audioSink); mux.finish();
}
}
int runFinalExportTests()
{
    Suite s;
    for (const bool leading : {false, true}) s.test(leading ? "Final MP4: first clip at 15 seconds retains black/silent leading range"
        : "Final MP4: 5-to-15 second empty interval is black video and silent AAC", [leading]
    {
        recorder_audio_fixture::Fixture f; const auto p = recorder_timeline_gap::project(f, leading);
        ExportJob job(p, f.root); ExportActivity gate; ExportControl control(gate);
        require(job.range.requested.start == 0 && job.range.sampleCount == Sample(leading ? 25 : 20) * p.Fs,
            "Default final range includes all leading and middle time");
        const auto mask = FinalVideoExporter::audioSource(job, "mix");
        ExportAudioRenderer audio(job, TimelineExporter::openSources(job, mask, control), mask);
        const auto file = f.root.getChildFile("timeline-gap.mp4.partial"); writeMuxFixture(file, p.Fs, job.range.frameCount, nullptr, &audio, &job);
        Sample frames = 0, blackFrames = 0, samples = 0, silentSamples = 0;
        ExportVerificationObserver observer;
        observer.video = [&](Sample n, const AVFrame& picture)
        {
            const bool gap = recorder_timeline_gap::sourceAt(frameToSample(n, p.Fs, p.fps), p.Fs, leading) < 0;
            require(std::abs(int(picture.data[0][0]) - (gap ? 16 : 64)) <= 3, "Decoded video matches independent black-frame oracle");
            ++frames; if (gap) ++blackFrames;
        };
        observer.audio = [&](Sample first, unsigned count, const float* l, const float* r)
        {
            samples += count;
            for (unsigned i = 0; i < count; ++i) if (recorder_timeline_gap::silentInterior(first + i, p.Fs, leading))
            { require(std::abs(l[i]) < .003f && std::abs(r[i]) < .003f, "Decoded AAC gap is silent within codec tolerance"); ++silentSamples; }
        };
        FinalVideoExporter::verify(file, job.range.frameCount, job.range.sampleCount, p.Fs, p.fps, control, observer, AV_CODEC_ID_MPEG4);
        require(frames == job.range.frameCount && blackFrames == (leading ? 450 : 300) && samples == job.range.sampleCount,
            "Exact decoded video/black/AAC counts match common range");
        require(silentSamples > Sample(leading ? 14 : 9) * p.Fs, "AAC silence checked throughout the long empty interval");
    });
    for (unsigned channels : {1u, 2u}) for (const auto gap : recorder_audio_fixture::recordedGaps)
    {
        const auto name = "Final export with healthy take: " + std::to_string(channels) + " channels, " + recorder_audio_fixture::gapName(gap);
        s.test(name.c_str(), [=]
        {
            recorder_audio_fixture::RecordedGapFixture f(channels, gap);
            ExportActivity gate; ExportControl control(gate); ExportJob job(f.project, f.root);
            const auto mix = FinalVideoExporter::audioSource(job, "mix"), mic = FinalVideoExporter::audioSource(job, "mic:1");
            std::vector<float> l(f.totalFrames), r(f.totalFrames);
            for (const auto& mask : {mix, mic})
            {
                FinalVideoExporter::validateSelection(job, {TrackKind::cam1, mask});
                const auto sources = TimelineExporter::openSources(job, mask, control);
                require(sources.size() == 2, "Final preflight retains both damaged and healthy take sources");
                ExportAudioRenderer renderer(job, sources, mask);
                renderer.render(0, unsigned(f.totalFrames), l.data(), r.data()); f.verifyPcm(l, r);
            }
            ExportAudioRenderer renderer(job, TimelineExporter::openSources(job, mix, control), mix);
            ExportPublication publication(job);
            const auto partial = publication.file("final.mp4").getSiblingFile("final.mp4.partial");
            writeMuxFixture(partial, f.project.Fs, job.range.frameCount, nullptr, &renderer);
            double error = 0; Sample samples = 0; ExportVerificationObserver observer;
            observer.audio = [&](Sample first, unsigned count, const float* left, const float* right)
            {
                for (unsigned i = 0; i < count; ++i)
                {
                    const auto at = std::size_t(first + i);
                    error += (left[i] - l[at]) * (left[i] - l[at]) + (right[i] - r[at]) * (right[i] - r[at]);
                    if (first + i >= 512 && first + i + 512 < f.totalFrames
                        && f.silent(first + i - 512) && f.silent(first + i + 512))
                        require(std::abs(left[i]) < .003f && std::abs(right[i]) < .003f, "Decoded AAC gap interior remains silent within codec tolerance");
                }
                samples += count;
            };
            auto row = FinalVideoExporter::verify(partial, job.range.frameCount, f.totalFrames, f.project.Fs,
                f.project.fps, control, observer, AV_CODEC_ID_MPEG4);
            require(samples == f.totalFrames && std::sqrt(error / (samples * 2)) < .015, "Decode all final AAC samples against verified PCM");
            jsonSet(row, "name", "final.mp4"); publication.commit(juce::Array<juce::var>{row}, control);
            require(job.outputDirectory.getChildFile("final.mp4").existsAsFile()
                && job.outputDirectory.getChildFile("export-manifest.json").existsAsFile(), "Publish verified final output");
        });
    }
    s.test("Stereo microphone final selection, mix and decoded AAC retain channel separation", []
    {
        recorder_audio_fixture::Fixture f(48000, true);
        for (Sample i = 0; i < Sample(f.pcm[0].size()); ++i) f.pcm[0][i] = std::int32_t(2000000 * std::sin(6.283185307179586 * 440 * i / 48000));
        const auto* asset = f.project.media->findAsset(f.project.tracks[2].clips.items()[0].assetId);
        for (const auto& chunk : asset->chunks)
        {
            const auto file = f.root.getChildFile(chunk.relativePath); require(file.deleteFile(), "Replace isolated stereo source with tone");
            recorder_audio_fixture::writePcm24(file,48000,f.pcm[0],chunk.sourceRange.start,chunk.sourceRange.length,2);
        }
        ExportActivity gate; ExportControl control(gate); ExportJob job(f.project,f.root,{},SampleRange{0,48000});
        auto bindings = TimelineExporter::openSources(job,{AudioSourceMask::Kind::microphoneMix},control);
        const auto mic = FinalVideoExporter::audioSource(job,"mic:1"), mix = FinalVideoExporter::audioSource(job,"mix");
        for (const auto& mask : {mic,mix}) FinalVideoExporter::validateSelection(job,{TrackKind::cam1,mask});
        const auto one = pcmAt(job,mic,bindings,337), mixed = pcmAt(job,mix,bindings,337);
        const auto expectedL = f.sample(0,337), expectedR = float(-f.pcm[0][337]/2)/8388608.0f;
        require(one.first == expectedL && one.second == expectedR, "Selected stereo mic PCM");
        require(mixed.first == (expectedL + f.sample(1,337)) * .5f && mixed.second == (expectedR + f.sample(1,337)) * .5f, "Mixed mono/stereo slot mean");
        ExportAudioRenderer renderer(job,bindings,mic); const auto path = f.root.getChildFile("stereo-final.mp4.partial");
        writeMuxFixture(path,48000,30,nullptr,&renderer);
        double error = 0; Sample samples = 0; ExportVerificationObserver observer;
        observer.audio = [&](Sample first,unsigned count,const float* l,const float* r)
        {
            for (unsigned i = 0; i < count; ++i)
            {
                const auto expected = .0 + f.sample(0,first+i), right = double(float(-f.pcm[0][first+i]/2)/8388608.0f);
                error += (l[i]-expected)*(l[i]-expected)+(r[i]-right)*(r[i]-right);
            }
            samples += count;
        };
        FinalVideoExporter::verify(path,30,48000,48000,{30,1},control,observer,AV_CODEC_ID_MPEG4);
        require(samples == 48000 && std::sqrt(error / (samples * 2)) < .015, "Decoded stereo AAC PCM oracle");
    });
    s.test("three final masks isolate microphones/import and scope solo correctly", []
    {
        recorder_audio_fixture::Fixture f; auto p = f.project; const auto imported = addImport(p); p.tracks.back().solo = true;
        ExportJob job(p, f.root); ExportActivity gate; ExportControl control(gate);
        auto bindings = openAudioSources(*compileAudioRenderPlan(f.project), f.root); bindings.push_back({imported, std::make_shared<MemoryPcm>(p.Fs * 10)});
        const Sample at = p.Fs + 777;
        const auto mix = FinalVideoExporter::audioSource(job, "mix"), mic = FinalVideoExporter::audioSource(job, "mic:2"), imp = FinalVideoExporter::audioSource(job, "import:" + imported);
        for (const auto& mask : {mix, mic, imp}) FinalVideoExporter::validateSelection(job, {TrackKind::cam1, mask});
        const auto mixed = pcmAt(job, mix, bindings, at), one = pcmAt(job, mic, bindings, at), complete = pcmAt(job, imp, bindings, at);
        require(mixed.first == (f.sample(0, at) + f.sample(1, at)) * .5f && mixed.first == mixed.second, "Mic-only fixed mean; import solo irrelevant");
        require(one.first == f.sample(1, at) && one.first == one.second, "Individual mic duplicated L/R");
        require(complete.first == .7f && complete.second == -.3f, "Completed stereo preserved; microphone PCM excluded");
        require(pcmAt(job, imp, bindings, 500).first == 0, "Import's timeline leading gap retained");
        require(pcmAt(job, imp, bindings, p.Fs * 4).first == 0, "Import's trim retained, original whole file not appended");
        p.tracks[2].solo = true; p.tracks[3].mute = true; ExportJob muted(p, f.root);
        require(pcmAt(muted, mic, bindings, at).first == 0, "Selected mic own mute applies even with another solo");
        require(pcmAt(muted, mix, bindings, at).first == f.sample(0, at), "Solo scope selects one microphone");
        p.tracks.back().mute = true; ExportJob mutedImport(p, f.root); require(pcmAt(mutedImport, imp, bindings, at).first == 0, "Import own mute applies");
    });
    s.test("import asset filter, mono duplicate and 3+ channel error even if muted", []
    {
        recorder_audio_fixture::Fixture f; auto p = f.project; const auto first = addImport(p, nullptr, 1);
        const auto second = addImport(p, &p.tracks.back()); auto& clips = p.tracks.back().clips.edit(); clips.back().timelineStartSample = p.Fs * 5;
        ExportJob job(p, f.root); auto mask = FinalVideoExporter::audioSource(job, "import:" + first);
        std::vector<AudioSourceBinding> b{{first, std::make_shared<MemoryPcm>(p.Fs * 10,1)}};
        const auto mono = pcmAt(job, mask, b, p.Fs + 500); require(mono.first == mono.second && mono.first == .7f, "Mono import duplicate");
        require(pcmAt(job, mask, b, p.Fs * 5 + 500).first == 0, "Other asset on same lane excluded without binding it");
        auto registry = std::make_shared<MediaRegistry>(*p.media); for (auto& a : registry->assets) if (a.assetId == first) a.originalFormat.channels = 3;
        p.media = registry; p.tracks.back().mute = true; ExportJob multi(p, f.root);
        rejects([&] { FinalVideoExporter::validateSelection(multi, {TrackKind::cam1, mask}); });
        rejects([&] { FinalVideoExporter::audioSource(job, "import:" + newId()); });
        (void)second;
    });
    s.test("empty mic selection is continuous silence and material mask cannot leak into final", []
    {
        recorder_audio_fixture::Fixture f; auto p = f.project; p.tracks.erase(p.tracks.begin() + 2, p.tracks.end()); ExportJob job(p,f.root);
        const auto mask = FinalVideoExporter::audioSource(job,"mix"); const auto silence = pcmAt(job,mask,{},1000);
        require(silence.first == 0 && silence.second == 0, "Zero microphone mix is silence");
        rejects([&] { FinalVideoExporter::audioSource(job,"mic:2"); });
        rejects([&] { FinalVideoExporter::validateSelection(job,{TrackKind::cam1,{AudioSourceMask::Kind::materialTrack,p.tracks[0].trackId}}); });
    });
    s.test("final source preparation builds and validates real stereo import cache at project Fs", []
    {
        recorder_audio_fixture::Fixture f; const auto source = f.root.getChildFile("external-stereo.wav");
        {
            juce::WavAudioFormat format; auto output = source.createOutputStream(); require(output != nullptr, "Import source output");
            std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(output.get(), 44100, 2, 24, {}, 0)); require(writer != nullptr, "Stereo import writer"); output.release();
            juce::AudioBuffer<float> buffer(2, 44100);
            for (int n = 0; n < 44100; ++n) { buffer.setSample(0,n,.2f*float(std::sin(6.283185307179586*440*n/44100))); buffer.setSample(1,n,.1f*float(std::cos(6.283185307179586*660*n/44100))); }
            require(writer->writeFromAudioSampleBuffer(buffer,0,44100) && writer->flush(), "Write original stereo PCM");
        }
        AudioImportControl importing; const auto originalHash = AudioImport::hashFile(source, importing);
        AudioImportRequest request; request.source=source; request.projectDirectory=f.root; request.projectId=f.project.projectId; request.projectFs=f.project.Fs; request.playhead=1000;
        std::unique_ptr<PreparedAudioImport> prepared; exportCheck(AudioImport::prepare(request,importing,prepared));
        auto p=f.project; auto registry=std::make_shared<MediaRegistry>(*p.media); registry->assets.push_back(prepared->asset()); p.media=registry;
        auto t=prepared->track(); auto& c=t.clips.edit()[0]; c.sourceIn=401; c.lengthSamples=3000; p.tracks.push_back(t);
        ExportJob job(p,f.root); const auto mask=FinalVideoExporter::audioSource(job,"import:"+prepared->asset().assetId); ExportActivity gate; ExportControl control(gate);
        const auto bindings=TimelineExporter::openSources(job,mask,control); const auto pcm=pcmAt(job,mask,bindings,1500);
        require(bindings.size()==1 && bindings[0].source->sampleRate==48000 && bindings[0].source->channels==2, "Automatic selected import cache at project rate");
        require(std::abs(pcm.first-.2*std::sin(6.283185307179586*440*901/48000))<.002
            && std::abs(pcm.second-.1*std::cos(6.283185307179586*660*901/48000))<.002, "Independent stereo sourceIn/resampling oracle");
        require(AudioImport::hashFile(source,importing)==originalHash && AudioImport::hashFile(prepared->originalFile(),importing)==originalHash, "External/copied originals unchanged");
        p.tracks.back().mute=true; ExportJob muted(p,f.root);
        require(!TimelineExporter::openSources(muted,mask,control).empty(), "Explicit muted selection still validates its file");
        require(prepared->originalFile().deleteFile(), "Remove only owned test import to exercise missing-file preflight");
        rejects([&] { TimelineExporter::openSources(muted,mask,control); });
    });
    s.test("arbitrary P-frame cuts use selected lane mapping, IDR decode plan and never stream-copy", []
    {
        recorder_audio_fixture::Fixture f; auto p = f.project; auto& clips = p.tracks[0].clips.edit();
        auto first = clips.front(); first.sourceIn = 7 * 1600; first.lengthSamples = 53 * 1600;
        auto next = first; next.clipId = newId(); next.timelineStartSample = 60 * 1600; next.sourceIn = 91 * 1600; next.lengthSamples = 13 * 1600; clips = {first,next};
        ExportJob job(p, f.root);
        for (Sample n = 0; n < job.range.frameCount; ++n)
        {
            const auto m = FinalVideoExporter::mappingAt(job,TrackKind::cam1,n), other = FinalVideoExporter::mappingAt(job,TrackKind::cam2,n);
            const bool black = n >= 53 && n < 60 || n >= 73;
            require(m.black() == black && !other.black(), "Missing selected lane stays black, never falls back to cam2");
            if (!black) require(m.assetId == first.assetId && m.sourceFrame == (n < 53 ? n + 7 : n + 31), "Independent cut frame mapping");
        }
        VideoIndex index; index.sampleRate = 48000; index.timeBaseDen = 30;
        for (Sample n = 0; n < 300; ++n) index.packets.push_back({n,n,n*100,1,100,n%30==0,n%30==0}); index.validateAndBuild();
        const auto decode = playbackDecodePlan(index,7); require(decode.fromIdr && decode.firstPacket == 0 && decode.decodeOnlyFrames == 7, "P-frame cut needs IDR dependency decoding");
        const auto encoder = NvencEncoder::configuredContext({30,"p5"}); require(encoder->codec_id == AV_CODEC_ID_H264 && encoder->pix_fmt == AV_PIX_FMT_NV12 && !encoder->max_b_frames, "Product always reencodes NV12 H.264, B=0");
        ExportJob range(p,f.root,{},SampleRange{60*1600+1,12*1600-2});
        require(FinalVideoExporter::mappingAt(range,TrackKind::cam1,0).sourceFrame == 91, "Selected range rebases output frame zero");
    });
    s.test("ordinary MP4 full decode validates presentation zero, AAC priming and unaligned tail at project rates", []
    {
        recorder_audio_fixture::Fixture f; ExportActivity gate; ExportControl control(gate);
        for (const auto Fs : {44100u,48000u,48001u,96000u})
        {
            const Sample frames = 31, samples = frameToSample(frames,Fs,{30,1}); const auto path = f.root.getChildFile(juce::String(Fs) + ".mp4.partial");
            writeMuxFixture(path,Fs,frames); double error = 0; Sample count = 0; ExportVerificationObserver observer;
            observer.audio = [&](Sample at, unsigned n, const float* l, const float* r)
            {
                for (unsigned i=0;i<n;++i)
                {
                    const auto a = .2 * std::sin(6.283185307179586 * 440 * (at+i) / 48000), b = -.1 * std::sin(6.283185307179586 * 660 * (at+i) / 48000);
                    error += (l[i]-a)*(l[i]-a)+(r[i]-b)*(r[i]-b); ++count;
                }
            };
            const auto result = FinalVideoExporter::verify(path,frames,samples,Fs,{30,1},control,observer,AV_CODEC_ID_MPEG4);
            require(bool(result["verified"]) && static_cast<juce::int64>(result["aacPresentationSamples"]) == 49600
                && static_cast<juce::int64>(result["aacInitialPadding"]) == 1024 && static_cast<juce::int64>(result["aacDecodedTailPadding"]) == 576, "AAC priming, valid length and physical padding distinguished");
            require(static_cast<juce::int64>(result["sampleCount"]) == 49600 && static_cast<int>(result["Fs"]) == 48000
                && static_cast<juce::int64>(result["renderedPcmSampleCount"]) == samples && static_cast<int>(result["renderedPcmFs"]) == int(Fs), "Manifest distinguishes file AAC samples from project-rate PCM");
            require(count == 49600 && std::sqrt(error / (count*2)) < .015, "Decoded stereo source/tail oracle");
            rejects([&] { FinalVideoExporter::verify(path,frames,samples-4,Fs,{30,1},control,{},AV_CODEC_ID_MPEG4); });
        }
    });
    s.test("large rebased source positions do not overflow intermediate timeline addition", []
    {
        recorder_audio_fixture::Fixture f; auto p=f.project; const Sample large=5000000000000000000LL;
        auto registry=std::make_shared<MediaRegistry>(*p.media); auto& asset=registry->assets[0];
        asset.logicalLength=large+48000; asset.availableRanges={{0,asset.logicalLength}}; registry->takes[0].logicalLength=asset.logicalLength; p.media=registry;
        auto& clip=p.tracks[0].clips.edit()[0]; clip.timelineStartSample=large; clip.sourceIn=large; clip.lengthSamples=48000;
        ExportJob job(p,f.root,{},SampleRange{large,48000}); const auto m=FinalVideoExporter::mappingAt(job,TrackKind::cam1,1);
        require(m.sourceSample==large+1600 && m.sourceFrame==large/1600+1, "Subtract timeline origin before adding large sourceIn");
    });
    s.test("final cancellation before hardware and during full verification preserves completed outputs", []
    {
        recorder_audio_fixture::Fixture f; const auto hashes = f.hashes(); ExportJob job(f.project,f.root); ExportActivity gate; ExportControl control(gate); control.cancelled = true;
        rejects([&] { FinalVideoExporter::run(job,{},control); }); require(!job.partialDirectory.exists() && !job.outputDirectory.exists(), "Early cancellation has no output/device work");
        control.cancelled = false; const auto complete = f.root.getChildFile("previous.mp4"); require(complete.replaceWithText("previous"), "Completed sentinel");
        {
            ExportPublication transaction(job); const auto partial = transaction.file("final.mp4").getSiblingFile("final.mp4.partial"); writeMuxFixture(partial,48000,31);
            ExportVerificationObserver observer; observer.video = [&](Sample n,const AVFrame&) { if (n==3) control.cancelled=true; };
            rejects([&] { FinalVideoExporter::verify(partial,31,49600,48000,{30,1},control,observer,AV_CODEC_ID_MPEG4); });
        }
        require(!job.partialDirectory.exists() && !job.outputDirectory.exists() && complete.loadFileAsString()=="previous" && f.hashes()==hashes, "Cancel cleans only owned partial, keeps originals/completed");
        require(gate.current()==ExportActivity::State::idle, "Cancelled final export releases activity gate");
        control.cancelled=false;
        {
            ExportPublication transaction(job); const auto partial=transaction.file("final.mp4").getSiblingFile("final.mp4.partial"); writeMuxFixture(partial,48000,1);
            ExportVerificationObserver observer; observer.finish=[] { throw std::runtime_error("Independent oracle rejected the completed decode"); };
            rejects([&] { FinalVideoExporter::verify(partial,1,1600,48000,{30,1},control,observer,AV_CODEC_ID_MPEG4); });
        }
        require(!job.outputDirectory.exists()&&!job.partialDirectory.exists(),"Aggregate oracle verdict occurs before publish");
    });
    s.test("MP4 DurableFile flush failure and abandoned mux never publish", []
    {
        recorder_audio_fixture::Fixture f;
        struct Fault final : FileIoFaultAdapter
        {
            bool hit=false;
            juce::Result beforeIo(FileIoOperation op,const juce::File&,std::uint64_t,std::size_t) override
            { if (op==FileIoOperation::flushData) {hit=true;return juce::Result::fail("MP4 flush fault");} return juce::Result::ok(); }
        } fault;
        const auto file=f.root.getChildFile("failed.mp4.partial"); rejects([&] { writeMuxFixture(file,48000,2,&fault); }); require(fault.hit&&!file.exists(),"MP4 flush failure propagates and removes owned partial");
        const auto abandoned=f.root.getChildFile("abandoned.mp4.partial");
        { auto codec=softwareVideo(); ReferenceMixWriter aac(48000); FinalMp4Writer writer(abandoned,*codec,aac.context()); }
        require(!abandoned.exists(),"No destructor trailer/publish");
    });
    return s.result("FinalExportTests");
}
