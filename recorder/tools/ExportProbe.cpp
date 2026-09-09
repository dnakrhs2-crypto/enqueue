#include "export/FinalVideoExporter.h"
#include "export/WavExportWriter.h"
#include "export/ExportController.h"
#include "diagnostics/CaptureTelemetry.h"
#include "support/Platform.h"
#include "../tests/AudioRenderFixtures.h"
#include "model/ClipEdits.h"
#include "record/NvencEncoder.h"
#include "record/ReferenceMixWriter.h"
#include "FramePatternSource.h"
#include "model/RecorderSerializer.h"
extern "C"
{
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <cstring>
#include <chrono>

namespace gocue::recorder
{
namespace
{
using Args = std::map<juce::String,juce::String>;
juce::String required(const Args& args,const juce::String& key)
{ const auto it=args.find(key); exportRequire(it!=args.end()&&it->second.isNotEmpty(),("Missing "+key).toRawUTF8()); return it->second; }
juce::String option(const Args& args,const juce::String& key,const juce::String& fallback={})
{ const auto it=args.find(key); return it==args.end()?fallback:it->second; }
juce::File path(const juce::String& text) { return juce::File::getCurrentWorkingDirectory().getChildFile(text); }
juce::var audioProbe(const Args& args)
{
    recorder_audio_fixture::Fixture fixture; const auto hashes=fixture.hashes(); ExportActivity gate; ExportControl control(gate);
    auto root=path(required(args,"--out-dir")); if(root.exists()) root=root.getChildFile(newId()); exportCheck(root.createDirectory());
    juce::Array<juce::var> cases;
    for(unsigned example=1;example<=4;++example)
    {
        auto p=example<=3?fixture.example(example):fixture.project;
        if(example==4)
        { auto edit=ClipEdits::move(p,{p.tracks[3].clips.items()[0].clipId},1601,false);exportCheck(edit.status);p=std::move(edit.project);++p.editRevision; }
        p.tracks[3].mute=true;p.tracks[2].solo=true;
        ExportJob job(p,fixture.root,root.getChildFile("example-"+juce::String(example)));
        const auto manifest=TimelineExporter::audioMaterials(job,control); Sample compared=0;
        for(unsigned mic=0;mic<2;++mic)
        {
            const auto wav=job.outputDirectory.getChildFile(mic?"mic02.wav":"mic01.wav");const auto header=WavExportWriter::inspect(wav);
            exportRequire(header.sampleCount==std::uint64_t(job.range.sampleCount),"Material WAV lengths differ");
            juce::WavAudioFormat format;std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(wav.createInputStream().release(),true));
            exportRequire(reader&&reader->lengthInSamples==job.range.sampleCount&&reader->bitsPerSample==24&&reader->numChannels==1,"Independent JUCE material reader mismatch");
            std::vector<float> pcm(static_cast<std::size_t>(job.range.sampleCount));float* channel=pcm.data();exportRequire(reader->read(&channel,1,0,static_cast<int>(pcm.size())),"Read exported material PCM");
            const Sample Fs=p.Fs;
            for(Sample n=0;n<job.range.sampleCount;++n)
            {
                bool gap=n>=10*Fs,fade=false;Sample source=n;
                if(example==1&&mic==1)gap|=n>=2*Fs&&n<3*Fs;
                if(example==2&&mic==1){gap=n>=9*Fs;if(n>=2*Fs)source+=Fs;}
                if(example==3&&n>=2*Fs)source+=Fs;
                if(example==4&&mic==1){source=n-1601;gap=source<0||source>=10*Fs;}
                std::vector<Sample> boundaries;
                if(example==1&&mic==1)boundaries={2*Fs,3*Fs};
                if(example==2&&mic==1)boundaries={2*Fs,9*Fs};
                if(example==3)boundaries={2*Fs};
                if(example==4)boundaries=mic?std::vector<Sample>{1601,10*Fs+1601}:std::vector<Sample>{10*Fs};
                for(const auto boundary:boundaries)fade|=std::abs(n-boundary)<=144;
                if(fade)continue;
                exportRequire(pcm[std::size_t(n)]==(gap?0.0f:fixture.sample(mic,source)),"Export PCM differs from independent cut/gap oracle");++compared;
            }
        }
        auto c=jsonObject();jsonSet(c,"example",example);jsonSet(c,"manifest",manifest);jsonSet(c,"outputDirectory",job.outputDirectory.getFullPathName());
        jsonSet(c,"comparedOutsideMicrofadeSamples",compared);jsonSet(c,"oracleMatch",true);cases.add(c);
    }
    const auto header=WavExportWriter::makeHeader(48000,0x100000000ULL/3+101);
    const auto reread=WavExportWriter::readHeader(header.bytes.data(),header.bytes.size());
    juce::WavAudioFormat format;std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(new juce::MemoryInputStream(header.bytes.data(),header.bytes.size(),true),true));
    exportRequire(header.rf64&&reread.sampleCount==header.sampleCount&&reader&&reader->lengthInSamples==static_cast<Sample>(header.sampleCount),"RF64 virtual header reread failed");
    const auto headerFile=root.getChildFile("rf64-virtual-header.bin");DurableFile out;exportCheck(out.open(headerFile,DurableFile::OpenMode::createNew));
    exportCheck(out.write(header.bytes.data(),header.bytes.size()));exportCheck(out.flushData());exportCheck(out.close());
    exportRequire(fixture.hashes()==hashes,"Original SHA-256 changed");
    auto report=jsonObject();jsonSet(report,"cases",cases);jsonSet(report,"originalHashesUnchanged",true);jsonSet(report,"rf64HeaderVerified",true);
    jsonSet(report,"rf64VirtualDataBytes",header.dataBytes);jsonSet(report,"rf64PhysicalTestBytes",header.bytes.size());jsonSet(report,"outputDirectory",root.getFullPathName());
    jsonSet(report,"unverified","RF64 test writes only its header, not >4GiB PCM. GPU video, physical I/O duration and NLE import are not measured.");return report;
}
void syntheticCamera(const juce::File& file, unsigned camera, unsigned seconds)
{
    NvencEncoder encoder({60,"p5"});encoder.open();ReferenceMixWriter aac(48000);
    const auto partial=file.getSiblingFile(file.getFileName()+".partial");
    FinalMp4Writer mux(partial,encoder.context(),aac.context());
    probe::PatternId id{1,camera};VideoSurface surface;surface.prepare(1920,1080);
    auto frame=ffFrame();frame->format=AV_PIX_FMT_NV12;frame->width=1920;frame->height=1080;
    frame->colorspace=AVCOL_SPC_BT709;frame->color_range=AVCOL_RANGE_MPEG;frame->color_primaries=AVCOL_PRI_BT709;frame->color_trc=AVCOL_TRC_BT709;
    ffCheck(av_frame_get_buffer(frame.get(),32),"Allocate final probe source frame");
    const PacketSink videoSink=[&](const AVPacket& p){mux.video(p);},audioSink=[&](const AVPacket& p){mux.audio(p);};
    std::vector<float> silence(1600,0);
    for(unsigned n=0;n<seconds*60;++n)
    {
        id.frame=n+1;probe::paintPattern(surface,id);ffCheck(av_frame_make_writable(frame.get()),"Writable probe frame");
        for(int plane=0;plane<2;++plane)for(int y=0;y<(plane?540:1080);++y)
            std::memcpy(frame->data[plane]+std::size_t(y)*frame->linesize[plane],(plane?surface.uv():surface.y())+std::size_t(y)*1920,1920);
        encoder.submit(*frame,n,videoSink);aac.append(silence.data(),800,audioSink);
    }
    encoder.drain(videoSink);aac.finishInput(audioSink);mux.finish();exportRename(partial,file);
}
float tone(unsigned channel, Sample at, unsigned Fs)
{
    const double frequency=channel==0?440:channel==1?997:channel==2?659:1319;
    return float(.2*std::sin(6.283185307179586*frequency*double(at)/Fs));
}
Id buildFinalFixture(recorder_audio_fixture::Fixture& f, const Args& args, unsigned camera, unsigned seconds, std::vector<juce::File>& originals)
{
    auto& p=f.project;p.fps={60,1};p.editRevision=21;auto registry=std::make_shared<MediaRegistry>(*p.media);
    const auto videoFile=f.root.getChildFile(registry->assets[camera-1].relativePath);
    Sample length=Sample(seconds)*p.Fs;
    if(args.count("--source-mp4"))
    {
        const auto source=path(args.at("--source-mp4"));exportRequire(source.existsAsFile(),"Actual source MP4 is missing");
        exportCheck(videoFile.getParentDirectory().createDirectory());exportRequire(source.copyFileTo(videoFile),"Copy actual source MP4 into isolated probe fixture");originals.push_back(source);
    }
    else syntheticCamera(videoFile,camera,seconds);
    originals.push_back(videoFile);MediaIndex index;const auto source=index.openVideo(videoFile,p.Fs);
    length=(std::min)(length,source->length);length=frameToSample(sampleToFrame(length,p.Fs,p.fps),p.Fs,p.fps);
    exportRequire(length>=Sample(8)*p.Fs,"Edited camera fixture requires at least 8 seconds");
    struct Input{AVFormatContext* p=nullptr;~Input(){avformat_close_input(&p);}}input;
    ffCheck(avformat_open_input(&input.p,videoFile.getFullPathName().toRawUTF8(),nullptr,nullptr),"Inspect source fps");ffCheck(avformat_find_stream_info(input.p,nullptr),"Inspect source stream info");
    const auto rate=av_guess_frame_rate(input.p,input.p->streams[source->stream],nullptr);exportRequire(rate.num>0&&rate.den>0,"Source fps unavailable");
    for(unsigned cam=0;cam<2;++cam)
    {
        auto& asset=registry->assets[cam];asset.logicalLength=length;asset.availableRanges={{0,length}};
        asset.originalFormat.fps=cam==camera-1?FrameRate{unsigned(rate.num),unsigned(rate.den)}:FrameRate{60,1};
        asset.sourceUnitsNumerator=asset.originalFormat.fps.numerator;asset.sourceUnitsDenominator=std::uint64_t(p.Fs)*asset.originalFormat.fps.denominator;
        p.tracks[cam].clips.edit()[0].lengthSamples=length;
    }
    for(unsigned mic=0;mic<2;++mic)
    {
        auto& asset=registry->assets[mic+2];asset.logicalLength=length;asset.availableRanges={{0,length}};asset.chunks.clear();asset.relativePath="media/takes/final/mic0"+juce::String(mic+1)+".wav";
        auto& pcm=f.pcm[mic];pcm.resize(static_cast<std::size_t>(length));
        for(Sample n=0;n<length;++n)pcm[std::size_t(n)]=static_cast<std::int32_t>(std::llround(tone(mic,n,p.Fs)*8388608.0));
        const auto file=f.root.getChildFile(asset.relativePath);recorder_audio_fixture::writePcm24(file,p.Fs,pcm,0,length);originals.push_back(file);
        p.tracks[mic+2].clips.edit()[0].lengthSamples=length;
    }
    registry->takes[0].logicalLength=length;p.media=registry;
    auto& clips=p.tracks[camera-1].clips.edit();auto a=clips.front(),b=a;
    a.sourceIn=7*800;a.lengthSamples=137*800;b.clipId=newId();b.timelineStartSample=181*800;b.sourceIn=227*800;b.lengthSamples=length-b.sourceIn;clips={a,b};
    auto cuts=ClipEdits::remove(p,{p.tracks[3].clips.items()[0].clipId},{2*Sample(p.Fs),p.Fs});exportCheck(cuts.status);p=std::move(cuts.project);
    // Stable ID allows --audio import:<id> on a fresh synthetic probe invocation.
    const Id imported="00000000000000000000000000000021";const auto importPath="media/imports/"+imported+"/completed.wav";const auto importFile=f.root.getChildFile(importPath);
    exportCheck(importFile.getParentDirectory().createDirectory());
    {
        juce::WavAudioFormat format;auto output=importFile.createOutputStream();exportRequire(output!=nullptr,"Create completed audio fixture");
        auto writer=std::unique_ptr<juce::AudioFormatWriter>(format.createWriterFor(output.get(),p.Fs,2,24,{},0));exportRequire(writer!=nullptr,"Create stereo completed WAV");output.release();
        juce::AudioBuffer<float> buffer(2,4096);
        for(Sample at=0;at<length;at+=4096)
        {
            const int count=static_cast<int>((std::min)(Sample{4096},length-at));
            for(int ch=0;ch<2;++ch)for(int n=0;n<count;++n)buffer.setSample(ch,n,tone(unsigned(ch+2),at+n,p.Fs));
            exportRequire(writer->writeFromAudioSampleBuffer(buffer,0,count),"Write stereo completed fixture");
        }
        exportRequire(writer->flush(),"Flush stereo completed fixture");
    }
    AudioImportControl importControl;ImportedAudioInfo info;exportCheck(AudioImport::inspect(importFile,importControl,info));
    CaptureTelemetry::writeJson(importFile.getSiblingFile(".import-info.json"),info.toVar());originals.push_back(importFile);
    registry=std::make_shared<MediaRegistry>(*p.media);MediaAsset asset;asset.assetId=imported;asset.kind=AssetKind::importAudio;asset.relativePath=importPath;
    asset.contentIdentity=info.contentHash;asset.mediaGeneration=1;asset.logicalLength=length;asset.availableRanges={{0,length}};
    asset.originalFormat.codec=info.codec;asset.originalFormat.sampleRate=p.Fs;asset.originalFormat.channels=2;asset.originalFormat.bitsPerSample=24;registry->assets.push_back(asset);p.media=registry;
    Track track;track.kind=TrackKind::importAudio;track.solo=true;Clip clip;clip.trackId=track.trackId;clip.assetId=imported;
    clip.timelineStartSample=2*Sample(p.Fs);clip.sourceIn=Sample(p.Fs)+91;clip.lengthSamples=length-2*Sample(p.Fs);track.clips.edit().push_back(clip);p.tracks.push_back(track);
    cuts=ClipEdits::remove(p,{clip.clipId},{4*Sample(p.Fs),p.Fs});exportCheck(cuts.status);p=std::move(cuts.project);exportCheck(p.validate());return imported;
}
struct PcmOracle
{
    std::vector<float> interleaved;
    PcmOracle(const ExportJob& job,const AudioSourceMask& mask,ExportControl& control)
    {
        ExportAudioRenderer renderer(job,TimelineExporter::openSources(job,mask,control),mask);
        const auto inputCount=job.range.sampleCount,outputCount=rescaleRound(inputCount,48000,job.snapshot.Fs);
        std::vector<float> source(static_cast<std::size_t>(inputCount)*2),l(16384),r(l.size());
        for(Sample at=0;at<inputCount;)
        {
            const auto n=static_cast<unsigned>((std::min)(Sample(l.size()),inputCount-at));renderer.render(at,n,l.data(),r.data());
            for(unsigned i=0;i<n;++i){source[std::size_t(at+i)*2]=l[i];source[std::size_t(at+i)*2+1]=r[i];}at+=n;
        }
        if(job.snapshot.Fs==48000){interleaved=std::move(source);return;}
        struct Swr{SwrContext* p=nullptr;~Swr(){swr_free(&p);}}swr;AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
        ffCheck(swr_alloc_set_opts2(&swr.p,&stereo,AV_SAMPLE_FMT_FLT,48000,&stereo,AV_SAMPLE_FMT_FLT,int(job.snapshot.Fs),0,nullptr),"Prepare probe oracle resampler");ffCheck(swr_init(swr.p),"Open probe oracle resampler");
        interleaved.resize(static_cast<std::size_t>(outputCount+4096)*2);Sample at=0,produced=0;
        while(at<inputCount)
        {
            const int n=static_cast<int>((std::min)(Sample{16384},inputCount-at));const std::uint8_t* in[]{reinterpret_cast<const std::uint8_t*>(source.data()+at*2)};
            std::uint8_t* out[]{reinterpret_cast<std::uint8_t*>(interleaved.data()+produced*2)};
            const auto count=swr_convert(swr.p,out,int(outputCount+4096-produced),in,n);ffCheck(count,"Resample probe PCM oracle");at+=n;produced+=count;
        }
        for(;;)
        {std::uint8_t* out[]{reinterpret_cast<std::uint8_t*>(interleaved.data()+produced*2)};const auto n=swr_convert(swr.p,out,int(outputCount+4096-produced),nullptr,0);ffCheck(n,"Drain probe PCM oracle");if(!n)break;produced+=n;}
        exportRequire(produced>=outputCount&&produced-outputCount<=2,"Probe oracle resampling length mismatch");interleaved.resize(static_cast<std::size_t>(outputCount)*2);
    }
};
// CPU verification of actual source pixels. This is independent of the D3D11VA
// export decoder; physical captures without a barcode cannot claim frame-ID proof.
struct SourcePixelOracle
{
    struct Input{AVFormatContext* p=nullptr;~Input(){avformat_close_input(&p);}}input;
    std::shared_ptr<const VideoIndex> index;CodecPtr codec;FramePtr frame=ffFrame();PacketPtr packet=ffPacket();std::optional<std::size_t> last;bool draining=false;
    explicit SourcePixelOracle(std::shared_ptr<const VideoIndex> s):index(std::move(s))
    {
        ffCheck(avformat_open_input(&input.p,index->file.getFullPathName().toRawUTF8(),nullptr,nullptr),"Open independent source pixel oracle");ffCheck(avformat_find_stream_info(input.p,nullptr),"Read source pixel oracle");
        const auto* c=avcodec_find_decoder(AV_CODEC_ID_H264);codec.reset(avcodec_alloc_context3(c));exportRequire(codec!=nullptr,"Allocate independent source decoder");
        ffCheck(avcodec_parameters_to_context(codec.get(),input.p->streams[index->stream]->codecpar),"Copy source oracle codec");codec->thread_count=1;codec->err_recognition=AV_EF_EXPLODE;ffCheck(avcodec_open2(codec.get(),c,nullptr),"Open software source oracle");
    }
    const AVFrame& at(Sample sample)
    {
        const auto wanted=index->frameAt(sample);if(last&&*last==wanted)return *frame;
        if(!last||wanted<*last)
        {ffCheck(av_seek_frame(input.p,index->stream,index->packets[index->previousIdr(sample)].pts,AVSEEK_FLAG_BACKWARD),"Seek source oracle");avcodec_flush_buffers(codec.get());draining=false;}
        for(;;)
        {
            av_frame_unref(frame.get());const int got=avcodec_receive_frame(codec.get(),frame.get());
            if(!got)
            {exportRequire(frame->best_effort_timestamp<=index->packets[wanted].pts,"Source oracle passed frame");if(frame->best_effort_timestamp==index->packets[wanted].pts){last=wanted;return *frame;}continue;}
            exportRequire(got==AVERROR(EAGAIN)&&!draining,"Source oracle decode ended early");
            for(;;)
            {
                const auto read=av_read_frame(input.p,packet.get());if(read==AVERROR_EOF){ffCheck(avcodec_send_packet(codec.get(),nullptr),"Drain source oracle");draining=true;break;}ffCheck(read,"Read source oracle packet");
                if(packet->stream_index==index->stream){ffCheck(avcodec_send_packet(codec.get(),packet.get()),"Decode source oracle packet");av_packet_unref(packet.get());break;}av_packet_unref(packet.get());
            }
        }
    }
};
juce::var finalProbe(const Args& args)
{
    exportRequire(!(args.count("--source-mp4")&&args.count("--project")),"Choose --source-mp4 or --project");
    const auto videoChoice=option(args,"--video","cam1");exportRequire(videoChoice=="cam1"||videoChoice=="cam2","Video must be cam1 or cam2");const unsigned camera=videoChoice=="cam1"?1u:2u;
    const auto secondsText=option(args,"--seconds","60");exportRequire(secondsText.containsOnly("0123456789")&&secondsText.getIntValue()>=8&&secondsText.getIntValue()<=60,"Probe --seconds must be 8..60; product export has no duration cap");
    const auto seconds=static_cast<unsigned>(secondsText.getIntValue());
    auto root=path(required(args,"--out-dir"));if(root.exists())root=root.getChildFile(newId());exportCheck(root.createDirectory());
    recorder_audio_fixture::Fixture fixture;RecorderProject project;juce::File projectRoot;std::vector<juce::File> originals;Id imported;
    const bool synthetic=!args.count("--project")&&!args.count("--source-mp4");
    if(args.count("--project"))
    {
        const auto checkpoint=path(args.at("--project"));exportCheck(RecorderSerializer::readCheckpoint(checkpoint,project,nullptr,false));projectRoot=checkpoint.getParentDirectory();
        for(const auto& a:project.media->assets)
        {
            if(a.relativePath.isNotEmpty()&&projectRoot.getChildFile(a.relativePath).existsAsFile())originals.push_back(projectRoot.getChildFile(a.relativePath));
            for(const auto& chunk:a.chunks)if(projectRoot.getChildFile(chunk.relativePath).existsAsFile())originals.push_back(projectRoot.getChildFile(chunk.relativePath));
        }
    }
    else
    {
        const auto temporary=fixture.root;
        exportRequire(temporary.getParentDirectory()==juce::File::getSpecialLocation(juce::File::tempDirectory)
            &&temporary.getFileName().startsWith("recorder-r14-fixture-"),"Unexpected temporary fixture owner");
        exportRequire(temporary.deleteRecursively(),"Remove only this temporary seed fixture");
        fixture.root=root.getChildFile("fixture");exportCheck(fixture.root.createDirectory());
        imported=buildFinalFixture(fixture,args,camera,seconds,originals);project=fixture.project;projectRoot=fixture.root;
        exportCheck(RecorderSerializer::writeCheckpoint(projectRoot.getChildFile("project.recorder"),project));
    }
    AudioImportControl hashControl;std::vector<juce::String> hashes;for(const auto& file:originals)hashes.push_back(AudioImport::hashFile(file,hashControl));
    juce::StringArray choices;
    if(args.count("--audio-cases"))
    {
        exportRequire(args.at("--audio-cases")=="all"&&!args.count("--audio"),"Use --audio-cases all or --audio selection");choices.add("mix");
        for(const auto& t:project.tracks)if(t.kind==TrackKind::mic&&t.microphoneIndex==1)choices.add("mic:2");
        for(const auto& t:project.tracks)if(t.kind==TrackKind::importAudio)for(const auto& c:t.clips.items())if(project.isActive(c))choices.addIfNotAlreadyThere("import:"+c.assetId);
    }
    else choices.add(option(args,"--audio","mix"));
    ExportActivity activity;ExportControl control(activity);juce::Array<juce::var> cases;
    const auto length=(std::min)(project.activeTimelineEnd(),Sample(seconds)*project.Fs);
    exportRequire(length>0,"Project has no active export range");
    for(int caseIndex=0;caseIndex<choices.size();++caseIndex)
    {
        ExportJob job(project,projectRoot,root.getChildFile("case-"+juce::String(caseIndex+1)),SampleRange{0,length});
        const auto mask=FinalVideoExporter::audioSource(job,choices[caseIndex]);FinalVideoExporter::validateSelection(job,{camera==1?TrackKind::cam1:TrackKind::cam2,mask});
        PcmOracle pcm(job,mask,control);double error=0,tailError=0;Sample compared=0,tailCompared=0,checkedFrames=0,blackFrames=0;double lumaError=0,maxLumaMae=0;
        MediaIndex media;std::map<Id,std::unique_ptr<SourcePixelOracle>> pixelSources;
        ExportVerificationObserver observer;
        observer.audio=[&](Sample at,unsigned count,const float* l,const float* r)
        {
            for(unsigned n=0;n<count;++n)
            {
                const auto i=std::size_t(at+n)*2;exportRequire(i+1<pcm.interleaved.size(),"Decoded AAC passed valid oracle tail");
                const auto a=l[n]-pcm.interleaved[i],b=r[n]-pcm.interleaved[i+1];const auto e=double(a)*a+double(b)*b;error+=e;++compared;
                if(at+n>=Sample(pcm.interleaved.size()/2)-4096){tailError+=e;++tailCompared;}
            }
        };
        observer.video=[&](Sample n,const AVFrame& decoded)
        {
            ++checkedFrames;const auto t=frameToSample(n,project.Fs,project.fps);const Clip* clip=nullptr;const MediaAsset* asset=nullptr;Sample sourceSample=0;
            // Independent raw model oracle, not FinalVideoExporter::mappingAt.
            for(const auto& lane:project.tracks)if(lane.kind==(camera==1?TrackKind::cam1:TrackKind::cam2))for(const auto& c:lane.clips.items())
                if(project.isActive(c)&&t>=c.timelineStartSample&&t<c.timelineEnd())
            {
                const auto* a=project.media->findAsset(c.assetId);const auto u=c.sourceIn+t-c.timelineStartSample;
                for(const auto& available:a->availableRanges)if(u>=available.start&&u<available.start+available.length){clip=&c;asset=a;sourceSample=u;}
            }
            if(!clip)
            {
                ++blackFrames;for(int y=0;y<decoded.height;y+=16)for(int x=0;x<decoded.width;x+=16)
                    exportRequire(std::abs(int(decoded.data[0][y*decoded.linesize[0]+x])-16)<=3,"Selected camera gap was not black");return;
            }
            if(synthetic)
            {
                const auto id=probe::readPattern(decoded.data[0],decoded.linesize[0],decoded.width,decoded.height);
                const Sample expected=n<137?n+7:n+46;
                exportRequire(id&&id->camera==camera&&id->frame==std::uint64_t(expected+1),"Final pixel frame-ID/camera mapping mismatch");
            }
            auto& source=pixelSources[asset->assetId];if(!source)source=std::make_unique<SourcePixelOracle>(media.openVideo(projectRoot.getChildFile(asset->relativePath),project.Fs));
            const auto& expected=source->at(sourceSample);double mae=0;Sample pixels=0;
            for(int y=0;y<decoded.height;y+=16)for(int x=0;x<decoded.width;x+=16){mae+=std::abs(int(decoded.data[0][y*decoded.linesize[0]+x])-int(expected.data[0][y*expected.linesize[0]+x]));++pixels;}
            mae/=double(pixels);lumaError+=mae;maxLumaMae=(std::max)(maxLumaMae,mae);exportRequire(mae<=12,"Final decoded source luma comparison failed");
        };
        observer.finish=[&]
        {
            exportRequire(compared>0&&tailCompared>0&&compared==Sample(pcm.interleaved.size()/2)&&checkedFrames==job.range.frameCount
                &&std::sqrt(error/(2*double(compared)))<.04&&std::sqrt(tailError/(2*double(tailCompared)))<.04,"Final decoded AAC source/tail oracle mismatch");
        };
        auto result=FinalVideoExporter::run(job,{camera==1?TrackKind::cam1:TrackKind::cam2,mask},control,observer);
        const double rms=std::sqrt(error/(2*double(compared))),tailRms=std::sqrt(tailError/(2*double(tailCompared)));
        exportRequire(compared==Sample(pcm.interleaved.size()/2)&&checkedFrames==job.range.frameCount&&rms<.04&&tailRms<.04,"Final decoded AAC source/tail oracle mismatch");
        jsonSet(result,"outputDirectory",job.outputDirectory.getFullPathName());jsonSet(result,"audioSelection",choices[caseIndex]);jsonSet(result,"audioOracleRms",rms);jsonSet(result,"audioTail4096Rms",tailRms);
        jsonSet(result,"checkedFrames",checkedFrames);jsonSet(result,"blackFrames",blackFrames);jsonSet(result,"pixelFrameIdVerified",synthetic);
        jsonSet(result,"sourceLumaMeanAbsoluteError",lumaError/(double((std::max)(Sample{1},checkedFrames-blackFrames))));jsonSet(result,"sourceLumaMaximumFrameMae",maxLumaMae);
        jsonSet(result,"initial2xTargetMet",double(result["effectiveFps"])>=2.0*project.fps.numerator/project.fps.denominator);
        jsonSet(result,"speedIncludesProbeOracle",true);cases.add(result);
    }
    for(std::size_t n=0;n<originals.size();++n)exportRequire(AudioImport::hashFile(originals[n],hashControl)==hashes[n],"Original source hash changed during export");
    juce::Array<juce::var> sourceHashes;
    for(std::size_t n=0;n<originals.size();++n){auto row=jsonObject();jsonSet(row,"path",originals[n].getFullPathName());jsonSet(row,"sha256",hashes[n]);sourceHashes.add(row);}
    auto report=jsonObject();jsonSet(report,"cases",cases);jsonSet(report,"originalHashesUnchanged",true);jsonSet(report,"hashedOriginalCount",originals.size());jsonSet(report,"originalHashes",sourceHashes);jsonSet(report,"fixtureImportAssetId",imported);
    jsonSet(report,"outputDirectory",root.getFullPathName());jsonSet(report,"sourceMode",synthetic?"synthetic-pixel-IDs":args.count("--project")?"actual-checkpoint":"actual-MP4-with-synthetic-edited-audio");
    jsonSet(report,"unverified","Physical captures without the synthetic barcode cannot certify source frame IDs; decoded luma comparison is reported separately. Visual quality, physical sync and NLE import remain manual gates. Speed includes software validation/oracle work.");
    return report;
}
}
namespace
{
class ExportProbeUnavailable : public std::runtime_error
{ public: using std::runtime_error::runtime_error; };
struct ExportProbeSource
{
    RecorderProject project;
    juce::File directory;
    std::vector<juce::File> originals;
    juce::String description;
    bool pixelIds = false;
};
ExportProbeSource checkpointSource(const juce::File& checkpoint, const juce::File& destination)
{
    if (!checkpoint.existsAsFile()) throw ExportProbeUnavailable("Actual round-10 checkpoint is missing: " + checkpoint.getFullPathName().toStdString());
    ExportProbeSource source; exportCheck(RecorderSerializer::readCheckpoint(checkpoint, source.project, nullptr, false));
    source.directory = destination; source.description = "Actual saved checkpoint copied with original media; journal replay is not implied";
    source.originals.push_back(checkpoint); std::set<juce::String> paths;
    for (const auto& asset : source.project.media->assets)
    {
        if (asset.relativePath.isNotEmpty()) paths.insert(asset.relativePath);
        for (const auto& chunk : asset.chunks) paths.insert(chunk.relativePath);
        if (asset.kind == AssetKind::importAudio && asset.relativePath.isNotEmpty())
        {
            const auto metadata = checkpoint.getParentDirectory().getChildFile(asset.relativePath).getSiblingFile(".import-info.json");
            if (metadata.existsAsFile()) paths.insert(metadata.getRelativePathFrom(checkpoint.getParentDirectory()));
        }
    }
    for (const auto& relative : paths)
    {
        exportRequire(isProjectRelativePath(relative), "Unsafe fixture media path");
        const auto original = checkpoint.getParentDirectory().getChildFile(relative), copy = destination.getChildFile(relative);
        exportRequire(original.existsAsFile(), ("Checkpoint source is missing: " + original.getFullPathName()).toRawUTF8());
        exportCheck(copy.getParentDirectory().createDirectory()); exportRequire(!copy.exists() && original.copyFileTo(copy), "Copy checkpoint source into isolated fixture");
        source.originals.push_back(original); source.originals.push_back(copy);
    }
    exportCheck(RecorderSerializer::writeCheckpoint(destination.getChildFile("project.recorder"), source.project));
    source.originals.push_back(destination.getChildFile("project.recorder")); return source;
}
ExportProbeSource editedSource(const Args& args, const juce::File& destination, unsigned seconds, bool twoCameras)
{
    recorder_audio_fixture::Fixture fixture; const auto temporary = fixture.root;
    exportRequire(temporary.getParentDirectory() == juce::File::getSpecialLocation(juce::File::tempDirectory)
        && temporary.getFileName().startsWith("recorder-r14-fixture-"), "Unexpected temporary fixture owner");
    exportRequire(temporary.deleteRecursively(), "Remove only owned seed fixture"); fixture.root = destination;
    exportCheck(destination.createDirectory()); ExportProbeSource source; source.directory = destination;
    buildFinalFixture(fixture, args, 1, seconds, source.originals);
    auto& p = fixture.project;
    if (twoCameras)
    {
        const auto camera = destination.getChildFile(p.media->assets[1].relativePath); syntheticCamera(camera, 2, seconds); source.originals.push_back(camera);
        // Actual linked cut then independent one-sample/microphone ripple edits.
        std::vector<Id> linked; for (std::size_t i = 0; i < 4; ++i) linked.push_back(p.tracks[i].clips.items()[0].clipId);
        const auto apply = [&](ClipEditResult r) { exportCheck(r.status); p = std::move(r.project); ++p.editRevision; };
        apply(ClipEdits::link(p, linked)); apply(ClipEdits::remove(p, {p.tracks[0].clips.items()[0].clipId}, {Sample(p.Fs), Sample(p.Fs / 2)}));
        const auto ids = [](const Track& t) { std::vector<Id> result; for (const auto& c : t.clips.items()) result.push_back(c.clipId); return result; };
        apply(ClipEdits::unlink(p, ids(p.tracks[2]))); apply(ClipEdits::move(p, ids(p.tracks[2]), 1, false));
        apply(ClipEdits::unlink(p, ids(p.tracks[3]))); apply(ClipEdits::rippleDeleteTracks(p, {4 * Sample(p.Fs), p.Fs}, {p.tracks[3].trackId}));
        auto registry = std::make_shared<MediaRegistry>(*p.media); auto& cameraAsset = registry->assets[1];
        cameraAsset.availableRanges = {{0, 6 * Sample(p.Fs)}, {7 * Sample(p.Fs), cameraAsset.logicalLength - 7 * Sample(p.Fs)}};
        cameraAsset.gaps = {{6 * Sample(p.Fs), p.Fs}}; p.media = registry;
        std::vector<Id> versions; for (unsigned i = 0; i < 2; ++i) versions.push_back(p.tracks[i].clips.items()[0].clipId);
        apply(ClipEdits::unlink(p, versions)); TakeStack stack; stack.spanSamples = p.Fs; TakeVersion oldVersion, newVersion;
        for (unsigned i = 0; i < 2; ++i)
        {
            auto& track = p.tracks[i]; auto old = track.clips.items()[0], active = old;
            old.takeStackId = active.takeStackId = stack.stackId; old.versionId = oldVersion.versionId; active.versionId = newVersion.versionId;
            active.clipId = newId(); active.sourceIn += 800; oldVersion.clipIds.push_back(old.clipId); newVersion.clipIds.push_back(active.clipId);
            track.clips.edit()[0] = old; track.clips.edit().push_back(active);
        }
        stack.activeVersionId = newVersion.versionId; stack.versions = {oldVersion, newVersion}; p.takeStacks.push_back(stack);
        p.tracks[2].mute = true; p.tracks[3].solo = true; source.pixelIds = !args.count("--source-mp4");
    }
    else
    {
        // buildFinalFixture's second camera is metadata-only until explicitly
        // supplied. Remove it from both track and take usage for this real 1cam.
        p.tracks[1].clips.edit().clear(); auto registry = std::make_shared<MediaRegistry>(*p.media);
        registry->takes[0].cam2AssetId.clear(); registry->assets.erase(registry->assets.begin() + 1); p.media = registry;
    }
    exportCheck(p.validate()); source.project = p;
    exportCheck(RecorderSerializer::writeCheckpoint(destination.getChildFile("project.recorder"), p)); source.originals.push_back(destination.getChildFile("project.recorder"));
    source.description = args.count("--source-mp4") ? "Actual round-02 MP4; deterministic edited microphone/import tones are synthetic" : "Two synthetic frame-ID cameras and independent edited audio";
    return source;
}
// Streaming source pixel check and AAC presentation/tail check, run BEFORE each
// child publication. The shared production verifier owns demux/decode and EOF.
struct ExportSourceOracle
{
    const ExportJob& job;
    TrackKind camera;
    bool pixelIds;
    unsigned slot;
    PcmOracle pcm;
    MediaIndex media;
    std::map<Id, std::unique_ptr<SourcePixelOracle>> sources;
    double squared = 0, tailSquared = 0, luma = 0, maximumLuma = 0;
    Sample samples = 0, tailSamples = 0, frames = 0, black = 0;
    ExportSourceOracle(const ExportJob& j, const FinalExportSelection& selection, ExportControl& c, bool ids)
        : job(j), camera(selection.video), pixelIds(ids), slot(camera == TrackKind::cam1 ? 1 : 2), pcm(j, selection.audio, c) {}
    ExportVerificationObserver observer()
    {
        ExportVerificationObserver observer;
        observer.audio = [this](Sample at, unsigned count, const float* left, const float* right)
        {
            for (unsigned i = 0; i < count; ++i)
            {
                const auto n = std::size_t(at + i) * 2; exportRequire(n + 1 < pcm.interleaved.size(), "AAC exceeded common presentation range");
                const double a = left[i] - pcm.interleaved[n], b = right[i] - pcm.interleaved[n + 1], e = a * a + b * b;
                squared += e; ++samples; if (at + i >= Sample(pcm.interleaved.size() / 2) - 4096) { tailSquared += e; ++tailSamples; }
            }
        };
        observer.video = [this](Sample n, const AVFrame& frame)
        {
            ++frames; const auto at = frameToSample(job.range.firstFrame + n, job.snapshot.Fs, job.snapshot.fps);
            const MediaAsset* asset = nullptr; Sample sourceSample = 0;
            for (const auto& t : job.snapshot.tracks) if (t.kind == camera) for (const auto& c : t.clips.items())
                if (job.snapshot.isActive(c) && at >= c.timelineStartSample && at < c.timelineEnd())
                {
                    const auto* a = job.snapshot.media->findAsset(c.assetId); const auto u = c.sourceIn + (at - c.timelineStartSample);
                    for (const auto& range : a->availableRanges) if (u >= range.start && u < range.start + range.length) { asset = a; sourceSample = u; }
                }
            if (!asset)
            {
                ++black; for (int y = 0; y < frame.height; y += 16) for (int x = 0; x < frame.width; x += 16)
                    exportRequire(std::abs(int(frame.data[0][y * frame.linesize[0] + x]) - 16) <= 3, "Camera gap is not black");
                return;
            }
            auto& source = sources[asset->assetId]; if (!source) source = std::make_unique<SourcePixelOracle>(media.openVideo(job.projectDirectory.getChildFile(asset->relativePath), job.snapshot.Fs));
            const auto& expected = source->at(sourceSample);
            if (pixelIds)
            {
                const auto id = probe::readPattern(frame.data[0], frame.linesize[0], frame.width, frame.height);
                const auto expectedId = probe::readPattern(expected.data[0], expected.linesize[0], expected.width, expected.height);
                exportRequire(id && expectedId && id->camera == slot && id->frame == expectedId->frame, "Material camera/frame-ID mapping mismatch");
            }
            double error = 0; Sample pixels = 0;
            for (int y = 0; y < frame.height; y += 16) for (int x = 0; x < frame.width; x += 16)
            { error += std::abs(int(frame.data[0][y * frame.linesize[0] + x]) - int(expected.data[0][y * expected.linesize[0] + x])); ++pixels; }
            error /= double(pixels); luma += error; maximumLuma = (std::max)(maximumLuma, error); exportRequire(error <= 12, "Decoded source luma comparison failed");
        };
        observer.finish = [this]
        {
            exportRequire(frames == job.range.frameCount && samples == Sample(pcm.interleaved.size() / 2) && tailSamples > 0
                && std::sqrt(squared / (2 * double(samples))) < .04 && std::sqrt(tailSquared / (2 * double(tailSamples))) < .04,
                "Source audio/presentation/tail count oracle failed");
        };
        return observer;
    }
    juce::var report() const
    {
        auto row = jsonObject(); jsonSet(row, "camera", slot); jsonSet(row, "frames", frames); jsonSet(row, "blackFrames", black);
        jsonSet(row, "pixelFrameIdVerified", pixelIds); jsonSet(row, "audioRmsError", std::sqrt(squared / (2 * double(samples))));
        jsonSet(row, "audioTail4096RmsError", std::sqrt(tailSquared / (2 * double(tailSamples))));
        jsonSet(row, "meanSourceLumaError", luma / double((std::max)(Sample{1}, frames - black))); jsonSet(row, "maximumFrameLumaError", maximumLuma); return row;
    }
};
juce::var encodedQuality(const juce::File& file, const ExportJob& job)
{
    struct Input { AVFormatContext* p = nullptr; ~Input() { avformat_close_input(&p); } } input;
    ffCheck(avformat_open_input(&input.p, file.getFullPathName().toRawUTF8(), nullptr, nullptr), "Inspect delivered quality");
    ffCheck(avformat_find_stream_info(input.p, nullptr), "Inspect delivered streams");
    std::uint64_t videoBytes = 0, audioBytes = 0; Sample frames = 0; auto packet = ffPacket();
    for (;;)
    {
        const auto code = av_read_frame(input.p, packet.get()); if (code == AVERROR_EOF) break; ffCheck(code, "Read delivered packet sizes");
        if (input.p->streams[packet->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { videoBytes += packet->size; ++frames; }
        else audioBytes += packet->size; av_packet_unref(packet.get());
    }
    exportRequire(frames == job.range.frameCount, "Delivered packet frame count mismatch");
    const auto seconds = double(job.range.frameCount) * job.snapshot.fps.denominator / job.snapshot.fps.numerator;
    auto row = jsonObject(); jsonSet(row, "name", file.getFileName()); jsonSet(row, "fileBytes", file.getSize()); jsonSet(row, "frameCount", frames);
    jsonSet(row, "videoPacketBytes", videoBytes); jsonSet(row, "videoBitrateBps", videoBytes * 8.0 / seconds); jsonSet(row, "audioBitrateBps", audioBytes * 8.0 / seconds);
    jsonSet(row, "visualQualityGate", "Manual visual/NLE assessment remains unverified; bitrate is a measurement, not a quality verdict"); return row;
}
struct ProbeDiskFault final : FileIoFaultAdapter
{
    bool manifest = false;
    juce::String manifestParentPrefix;
    juce::Result beforeIo(FileIoOperation op, const juce::File& file, std::uint64_t, std::size_t) override
    {
        if (manifest ? op == FileIoOperation::flushData && file.getFileName().startsWith("export-manifest")
                         && file.getParentDirectory().getFileName().startsWith(manifestParentPrefix)
                     : op == FileIoOperation::append)
            return juce::Result::fail("Injected export disk failure");
        return juce::Result::ok();
    }
};
juce::var combinedProbe(const Args& args)
{
    const auto secondsText = option(args, "--seconds", "60");
    exportRequire(secondsText.containsOnly("0123456789") && secondsText.getIntValue() >= 8 && secondsText.getIntValue() <= 60,
        "Development probe --seconds must be 8..60; product export duration is unlimited");
    const auto seconds = static_cast<unsigned>(secondsText.getIntValue());
    const auto mode = required(args, "--mode"); exportRequire(mode == "materials" || mode == "both" || mode == "final", "Use materials, final or both");
    exportRequire(!args.count("--audio-cases"), "Use a single --audio selection for this combined benchmark");
    const auto root = ExportController::resolveDestination(path(required(args, "--out-dir"))); exportCheck(root.createDirectory());
    std::vector<ExportProbeSource> sources;
    if (required(args, "--fixture") == "long-form")
    {
        const auto evidence = args.count("--evidence-dir") ? path(args.at("--evidence-dir"))
            : juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("tools/claude_harness/recorder_validation");
        const auto mp4 = args.count("--source-mp4") ? path(args.at("--source-mp4")) : evidence.getChildFile("r02/encode60/cam1.mp4");
        const auto checkpoint = args.count("--project") ? path(args.at("--project")) : evidence.getChildFile("r10/project/project.recorder");
        if (!mp4.existsAsFile() || !checkpoint.existsAsFile()) throw ExportProbeUnavailable("long-form requires actual r02 MP4 and r10 project; supply --source-mp4 and --project or --evidence-dir");
        auto actual = args; actual["--source-mp4"] = mp4.getFullPathName();
        sources.push_back(editedSource(actual, root.getChildFile("r02-fixture"), seconds, false));
        sources.push_back(checkpointSource(checkpoint, root.getChildFile("r10-fixture")));
    }
    else if (args.count("--project")) sources.push_back(checkpointSource(path(args.at("--project")), root.getChildFile("fixture")));
    else sources.push_back(editedSource(args, root.getChildFile("fixture"), seconds, true));
    juce::Array<juce::var> cases, faults, hashes;
    AudioImportControl hashControl; std::map<juce::String, juce::String> protectedFiles;
    const auto protect = [&](const juce::File& file)
    { protectedFiles[file.getFullPathName()] = AudioImport::hashFile(file, hashControl); };
    const auto checkProtected = [&]
    { for (const auto& entry : protectedFiles) exportRequire(AudioImport::hashFile(juce::File(entry.first), hashControl) == entry.second, "Original or completed output changed during fault/cancel/retry"); };
    for (const auto& source : sources) for (const auto& file : source.originals) protect(file);
    for (std::size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex)
    {
        const auto& source = sources[sourceIndex]; const auto duration = (std::min)(source.project.activeTimelineEnd(), Sample(seconds) * source.project.Fs);
        for (auto exportMode : {ExportController::Mode::materials, ExportController::Mode::finalVideo})
        {
            if ((mode == "materials" && exportMode != ExportController::Mode::materials) || (mode == "final" && exportMode != ExportController::Mode::finalVideo)) continue;
            const juce::String name = exportMode == ExportController::Mode::materials ? "materials" : "final";
            ExportController::Request request; request.mode = exportMode; request.range = SampleRange{0, duration};
            request.destination = root.getChildFile("source-" + juce::String(sourceIndex + 1) + "-" + name); request.materials.includeImports = true;
            ExportJob plan(source.project, source.directory, request.destination, request.range);
            const auto camera = option(args, "--video", "cam1"); exportRequire(camera == "cam1" || camera == "cam2", "Video must be cam1 or cam2");
            request.finalSource = {camera == "cam1" ? TrackKind::cam1 : TrackKind::cam2, FinalVideoExporter::audioSource(plan, option(args, "--audio", "mix"))};
            if (args.count("--reference-audio")) request.materials.referenceAudio = FinalVideoExporter::audioSource(plan, args.at("--reference-audio"));
            juce::Array<juce::var> oracles;
            const MaterialExporter::CameraRenderer cameraRenderer = [&](const ExportJob& j, const FinalExportSelection& selection, ExportControl& control, FileIoFaultAdapter* fault)
            {
                ExportSourceOracle oracle(j, selection, control, source.pixelIds);
                auto result = FinalVideoExporter::run(j, selection, control, oracle.observer(), fault); oracles.add(oracle.report()); return result;
            };
            ExportController controller([&](const ExportJob& j, const ExportController::Request& r, ExportControl& control, FileIoFaultAdapter* fault)
            { return r.mode == ExportController::Mode::materials ? MaterialExporter::run(j, r.materials, control, fault, cameraRenderer) : cameraRenderer(j, r.finalSource, control, fault); });
            const auto started = std::chrono::steady_clock::now();
            exportCheck(controller.start(source.project, source.directory, request)); controller.wait(); const auto result = controller.status();
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            exportRequire(result.state == ExportController::State::completed, result.error.toRawUTF8());
            juce::Array<juce::var> quality;
            for (const auto& row : *result.manifest["files"].getArray())
            {
                const auto output = result.outputDirectory.getChildFile(row["name"].toString());
                if (output.hasFileExtension("mp4")) quality.add(encodedQuality(output, plan));
                else exportRequire(WavExportWriter::inspect(output).sampleCount == std::uint64_t(plan.range.sampleCount), "Material WAV common count failed");
                protect(output);
            }
            protect(result.outputDirectory.getChildFile("export-manifest.json"));
            auto record = result.manifest.clone(); jsonSet(record, "outputDirectory", result.outputDirectory.getFullPathName()); jsonSet(record, "sourceDescription", source.description);
            const auto renderedFrames = double(plan.range.frameCount) * (exportMode == ExportController::Mode::materials ? double(MaterialExporter::cameras(source.project).size()) : 1.0);
            jsonSet(record, "coreElapsedSeconds", record["elapsedSeconds"]); jsonSet(record, "elapsedSeconds", elapsed);
            jsonSet(record, "effectiveFps", renderedFrames / elapsed); jsonSet(record, "renderedVideoFrames", renderedFrames);
            jsonSet(record, "speedMeasurement", "Job creation, all preparation/render/encode/full verification/source oracles/durable publish; excludes fixture copying and separate bitrate scan");
            jsonSet(record, "mode", name); jsonSet(record, "quality", quality); jsonSet(record, "sourceOracles", oracles);
            jsonSet(record, "benchmarkRequested", args.count("--benchmark") != 0); jsonSet(record, "speedIncludesProbeOracle", true);
            if (exportMode == ExportController::Mode::finalVideo) jsonSet(record, "initial2xTargetMet", double(record["effectiveFps"]) >= 2.0 * source.project.fps.numerator / source.project.fps.denominator);
            cases.add(record);
            if (args.count("--fault-cases")) for (int faultCase = 0; faultCase < 3; ++faultCase)
            {
                auto faultRequest = request; faultRequest.range = SampleRange{0, (std::min)(duration, Sample(source.project.Fs) * 2)};
                faultRequest.destination = root.getChildFile("fault-" + juce::String(sourceIndex + 1) + "-" + name + "-" + juce::String(faultCase));
                ProbeDiskFault disk; disk.manifest = faultCase == 2; disk.manifestParentPrefix = faultRequest.destination.getFileName() + "."; bool cancelAtRender = faultCase == 0;
                ExportController failure([&](const ExportJob& j, const ExportController::Request& r, ExportControl& control, FileIoFaultAdapter* fault)
                {
                    auto progress = control.onProgress;
                    control.onProgress = [&, progress](const ExportProgress& p)
                    { if (progress) progress(p); if (cancelAtRender && (p.stage == "render" || p.stage.endsWith("/render"))) control.cancelled.store(true); };
                    return r.mode == ExportController::Mode::materials ? MaterialExporter::run(j, r.materials, control, fault) : FinalVideoExporter::run(j, r.finalSource, control, {}, fault);
                });
                exportCheck(failure.start(source.project, source.directory, faultRequest, false, faultCase ? &disk : nullptr)); failure.wait();
                const auto failed = failure.status();
                exportRequire(failed.state == (faultCase ? ExportController::State::failed : ExportController::State::cancelled)
                    && !faultRequest.destination.exists(), "Fault case incorrectly published/returned success");
                exportRequire(root.findChildFiles(juce::File::findFilesAndDirectories, true, "*.partial").isEmpty(), "Fault left partial output behind");
                checkProtected(); auto faultRow = jsonObject(); jsonSet(faultRow, "mode", name); jsonSet(faultRow, "sourceIndex", sourceIndex + 1);
                jsonSet(faultRow, "case", faultCase == 0 ? "cancel-during-render" : faultCase == 1 ? "disk-write" : "manifest-flush");
                jsonSet(faultRow, "originalsAndCompletedOutputsUnchanged", true); jsonSet(faultRow, "ownedPartialsRemoved", true);
                if (faultCase == 1)
                {
                    cancelAtRender = false; exportCheck(failure.retry()); failure.wait();
                    exportRequire(failure.status().state == ExportController::State::completed, "Disk failure retry did not complete"); checkProtected(); jsonSet(faultRow, "retrySucceeded", true);
                    for (const auto& file : failure.status().outputDirectory.findChildFiles(juce::File::findFiles, true)) protect(file);
                }
                faults.add(faultRow);
            }
        }
    }
    checkProtected();
    for (const auto& entry : protectedFiles) { auto row = jsonObject(); jsonSet(row, "path", entry.first); jsonSet(row, "sha256", entry.second); hashes.add(row); }
    auto report = jsonObject(); jsonSet(report, "cases", cases); jsonSet(report, "faultCases", faults); jsonSet(report, "protectedHashes", hashes);
    jsonSet(report, "outputDirectory", root.getFullPathName()); jsonSet(report, "originalsAndCompletedOutputsUnchanged", true);
    jsonSet(report, "developmentSecondsCap", seconds); jsonSet(report, "longDurationMeasured", false);
    jsonSet(report, "unverified", "1h/3h I/O, physical disk-full/power loss, visual quality, ASIO/GUI source-preview and Premiere/Resolve MP4/WAV/RF64/AAC import are separate manual gates. Default development run is at most 60 seconds per source, not a long-duration certification.");
    return report;
}
}
int runExportProbe(int argc,wchar_t** argv)
{
    Args args;auto report=jsonObject();
    try
    {
        const std::set<juce::String> allowed{"--fixture","--mode","--out-dir","--report","--video","--audio","--audio-cases","--source-mp4","--project","--seconds","--evidence-dir","--reference-audio"};
        const std::set<juce::String> flags{"--benchmark", "--fault-cases"};
        for(int i=2;i<argc;++i)
        { const juce::String key(argv[i]); exportRequire(!args.count(key), "Duplicate export option");
          if (flags.count(key)) args.emplace(key, "true");
          else { exportRequire(allowed.count(key)&&i+1<argc,"Unknown export option or missing value");args.emplace(key,juce::String(argv[++i])); } }
        required(args,"--report");const auto mode=required(args,"--mode"),fixture=required(args,"--fixture");
        if (!args.count("--out-dir")) args["--out-dir"] = path(args.at("--report")).getSiblingFile(path(args.at("--report")).getFileNameWithoutExtension() + "-output").getFullPathName();
        if(mode=="audio-materials"&&fixture=="audio-cuts-gaps-rf64")report=audioProbe(args);
        else if(mode=="final"&&fixture=="edited-one-camera")report=finalProbe(args);
        else if(fixture=="long-form" || fixture=="two-camera-independent-audio") report=combinedProbe(args);
        else throw std::runtime_error("Unsupported export fixture/mode");
        jsonSet(report,"result","PASS");CaptureTelemetry::writeJson(path(required(args,"--report")),report);std::cout<<"PASS: "<<required(args,"--report")<<'\n';return 0;
    }
    catch(const ExportProbeUnavailable& e)
    {
        jsonSet(report,"result","unavailable");jsonSet(report,"error",e.what());
        if(args.count("--report")) try { CaptureTelemetry::writeJson(path(args.at("--report")),report); } catch (...) {}
        std::cerr<<juce::JSON::toString(report,false)<<'\n';return 2;
    }
    catch(const std::exception& e)
    {
        jsonSet(report,"result","FAIL");jsonSet(report,"error",e.what());
        if(args.count("--report"))try{CaptureTelemetry::writeJson(path(args.at("--report")),report);}catch(const std::exception& write){std::cerr<<"Export report write failed: "<<write.what()<<'\n';}
        std::cerr<<juce::JSON::toString(report,false)<<'\n';return 1;
    }
}
}
