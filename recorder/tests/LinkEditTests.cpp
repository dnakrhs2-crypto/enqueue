#include "model/ClipEdits.h"
#include "model/RecorderSerializer.h"
#include <iostream>
#include <stdexcept>

using namespace gocue::recorder;
namespace
{
void need(bool b,const char* m){if(!b)throw std::runtime_error(m);}
juce::String hash(const RecorderProject& p){return RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(p));}
RecorderProject result(ClipEditResult r){if(r.status.failed())throw std::runtime_error(r.status.getErrorMessage().toStdString());need(r.project.validate().wasOk(),"valid result");return std::move(r.project);}
RecorderProject fixture()
{
    RecorderProject p; auto media=std::make_shared<MediaRegistry>();
    for(int i=0;i<4;++i)
    {
        MediaAsset a; a.kind=AssetKind::importAudio; a.logicalLength=1000; a.availableRanges={{0,1000}}; a.relativePath="media/imports/"+a.assetId+".wav"; a.contentIdentity=a.assetId; a.originalFormat.codec="pcm_s24le"; a.originalFormat.sampleRate=p.Fs; a.originalFormat.channels=2; a.originalFormat.bitsPerSample=24;
        Track t; t.kind=TrackKind::importAudio; Clip c; c.trackId=t.trackId; c.assetId=a.assetId; c.timelineStartSample=i*10; c.sourceIn=i; c.lengthSamples=100;
        t.clips.edit().push_back(c); p.tracks.push_back(t); media->assets.push_back(a);
    }
    p.media=media;need(p.validate().wasOk(),"fixture");return p;
}
Id id(const RecorderProject& p,size_t t){return p.tracks[t].clips.items()[0].clipId;}
void rejected(const RecorderProject& p,const ClipEditResult& r){need(r.status.failed()&&hash(r.project)==hash(p)&&r.project.media==p.media,"atomic rejection");}
}
int runLinkEditTests()
{
    int passed=0,failed=0;
    const auto test=[&](const char* name,auto body){try{body();++passed;std::cout<<"PASS link "<<name<<'\n';}catch(const std::exception& e){++failed;std::cerr<<"FAIL link "<<name<<": "<<e.what()<<'\n';}};
    test("cross-source link unlink relink preserves offsets and source metadata",[]
    {
        const auto p=fixture(); auto q=result(ClipEdits::link(p,{id(p,0),id(p,1),id(p,2)})); const auto group=q.linkGroups[0].linkGroupId;
        q=result(ClipEdits::unlink(q,{id(q,1)})); need(q.linkGroups.size()==1&&q.linkGroups[0].clipIds.size()==2&&q.linkGroups[0].linkGroupId==group,"detach just requested member");
        q=result(ClipEdits::move(q,{id(q,1)},7)); q=result(ClipEdits::link(q,{id(q,0),id(q,1)})); need(q.linkGroups[0].clipIds.size()==3,"merge complete existing group");
        for(size_t i=0;i<4;++i){const auto& a=*p.findClip(id(p,i));const auto& b=*q.findClip(id(q,i));need(b.sourceIn==a.sourceIn&&b.lengthSamples==a.lengthSamples&&b.assetId==a.assetId&&b.timelineStartSample==a.timelineStartSample+(i==1?7:0),"relink never aligns source");}
        need(q.media==p.media&&q.editRevision==p.editRevision,"metadata only");
    });
    test("merges two existing groups without losing members",[]
    {
        auto p=fixture();p=result(ClipEdits::link(p,{id(p,0),id(p,1)}));p=result(ClipEdits::link(p,{id(p,2),id(p,3)}));
        p=result(ClipEdits::link(p,{id(p,0),id(p,2)}));need(p.linkGroups.size()==1&&p.linkGroups[0].clipIds.size()==4,"group union");
        const auto same=result(ClipEdits::link(p,{id(p,0),id(p,1)}));need(hash(same)==hash(p),"repeated link no-op");
    });
    test("split preserves two outside members and creates fresh side groups",[]
    {
        auto p=fixture();p.tracks[1].clips.edit()[0].timelineStartSample=0;p.tracks[2].clips.edit()[0].timelineStartSample=200;p.tracks[3].clips.edit()[0].timelineStartSample=300;
        p=result(ClipEdits::link(p,{id(p,0),id(p,1),id(p,2),id(p,3)}));const auto old=p.linkGroups[0].linkGroupId;
        const auto q=result(ClipEdits::split(p,{id(p,0)},50));need(q.linkGroups.size()==3,"left right and outside group");
        for(size_t i=2;i<4;++i)need(q.findClip(id(p,i))->linkGroupId==old&&q.findClip(id(p,i))->timelineStartSample==p.findClip(id(p,i))->timelineStartSample,"outside placement and link preserved");
        need(q.tracks[0].clips.items()[0].linkGroupId!=old&&q.tracks[0].clips.items()[1].linkGroupId!=old&&q.tracks[0].clips.items()[0].linkGroupId!=q.tracks[0].clips.items()[1].linkGroupId,"fresh groups");
    });
    test("single outside survivor keeps identity and becomes unlinked",[]
    {
        auto p=fixture();p.tracks[2].clips.edit()[0].timelineStartSample=200;p=result(ClipEdits::link(p,{id(p,0),id(p,1),id(p,2)}));
        const auto q=result(ClipEdits::split(p,{id(p,0)},50));const auto& outside=*q.findClip(id(p,2));need(outside.linkGroupId.isEmpty()&&outside.sourceIn==2&&outside.timelineStartSample==200,"no invalid singleton");
    });
    test("selected-track ripple refuses outside links even when linked clip is later",[]
    {
        auto p=fixture();p=result(ClipEdits::link(p,{id(p,0),id(p,1)}));rejected(p,ClipEdits::rippleDeleteTracks(p,{20,10},{p.tracks[0].trackId}));
        rejected(p,ClipEdits::rippleDeleteTracks(p,{0,5},{p.tracks[1].trackId}));
        const auto q=result(ClipEdits::rippleDeleteTracks(p,{20,10},{p.tracks[0].trackId,p.tracks[1].trackId}));need(q.activeTimelineEnd()==130,"other lanes remain");
        p=result(ClipEdits::unlink(p,{id(p,0)}));const auto r=result(ClipEdits::rippleDeleteTracks(p,{20,10},{p.tracks[0].trackId}));need(r.findClip(id(p,1))->timelineStartSample==10,"explicit unlink permits local ripple");
    });
    test("linked move preserves offset and rejects collisions without partial moves",[]
    {
        auto p=fixture();p=result(ClipEdits::link(p,{id(p,0),id(p,1)}));const auto q=result(ClipEdits::move(p,{id(p,1)},13));need(q.findClip(id(p,0))->timelineStartSample==13&&q.findClip(id(p,1))->timelineStartSample==23,"common sample delta");
        auto extra=p.tracks[0].clips.items()[0];extra.clipId=newId();extra.linkGroupId.clear();extra.timelineStartSample=150;p.tracks[0].clips.edit().push_back(extra);need(p.validate().wasOk(),"collision fixture");rejected(p,ClipEdits::move(p,{id(p,1)},100));
    });
    test("middle removal preserves outside members and no hidden unlink of target tracks",[]
    {
        auto p=fixture();p.tracks[2].clips.edit()[0].timelineStartSample=200;p.tracks[3].clips.edit()[0].timelineStartSample=300;p=result(ClipEdits::link(p,{id(p,0),id(p,1),id(p,2),id(p,3)}));
        const auto q=result(ClipEdits::remove(p,{id(p,0)},{40,10}));need(q.linkGroups.size()==3&&q.findClip(id(p,2))->timelineStartSample==200,"outside group remains after range removal");
        rejected(p,ClipEdits::rippleDeleteTracks(p,{40,10},{p.tracks[0].trackId,p.tracks[1].trackId}));
    });
    test("invalid or singleton explicit selection rejected",[]
    {const auto p=fixture();rejected(p,ClipEdits::link(p,{id(p,0),id(p,0)}));rejected(p,ClipEdits::unlink(p,{newId()}));rejected(p,ClipEdits::link(p,{}));});
    std::cout<<"LinkEditTests: "<<passed<<" passed, "<<failed<<" failed\n";return failed?1:0;
}
