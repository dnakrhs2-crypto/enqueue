#include "HardeningChecks.h"
#include "app/RecorderDocument.h"
#include "model/TakeStackEdits.h"
#include "support/Platform.h"
#include <array>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace gocue::recorder;
namespace
{
Id fixedId(int n){return juce::String::toHexString(n).paddedLeft('0',32);}
void need(bool b,const char* m){if(!b)throw std::runtime_error(m);}
void ok(const juce::Result& r){if(r.failed())throw std::runtime_error(r.getErrorMessage().toStdString());}
juce::String hash(const RecorderProject& p){return RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(p));}
RecorderProject fixture()
{
    RecorderProject p;p.projectId=fixedId(1);p.name="property";auto media=std::make_shared<MediaRegistry>();Take take;take.takeId=fixedId(2);take.number=1;take.createdAt="2026-09-09T00:00:00Z";take.logicalLength=600000;take.state=TakeState::complete;
    TakeStack stack;stack.stackId=fixedId(3);stack.anchorSample=48000;stack.spanSamples=288000;TakeVersion active,previous;active.versionId=fixedId(4);previous.versionId=fixedId(5);stack.activeVersionId=active.versionId;
    LinkGroup group,prior;group.linkGroupId=fixedId(6);prior.linkGroupId=fixedId(7);
    for(int i=0;i<6;++i)
    {
        MediaAsset a;a.assetId=fixedId(10+i);a.kind=i<2?AssetKind::camera:i<4?AssetKind::mic:AssetKind::importAudio;a.logicalLength=600000;a.availableRanges={{0,600000}};a.relativePath="media/imports/"+a.assetId+".fixture";a.contentIdentity="fixed:"+a.assetId;
        if(i<2){a.originalFormat.codec="h264";a.originalFormat.width=1920;a.originalFormat.height=1080;a.sourceUnitsNumerator=30;a.sourceUnitsDenominator=p.Fs;(i==0?take.cam1AssetId:take.cam2AssetId)=a.assetId;}
        else{a.originalFormat.codec="pcm_s24le";a.originalFormat.sampleRate=p.Fs;a.originalFormat.channels=i<4?1:2;a.originalFormat.bitsPerSample=24;if(i<4){take.microphoneAssetIds.push_back(a.assetId);take.capture.physicalInputs.push_back(i-2);}}
        Track t;t.trackId=fixedId(20+i);t.kind=i==0?TrackKind::cam1:i==1?TrackKind::cam2:i<4?TrackKind::mic:TrackKind::importAudio;t.microphoneIndex=i>=2&&i<4?i-2:-1;
        Clip c;c.clipId=fixedId(30+i);c.trackId=t.trackId;c.assetId=a.assetId;c.sourceIn=48000;c.timelineStartSample=48000;c.lengthSamples=288000;
        if(i<4){c.linkGroupId=group.linkGroupId;c.takeStackId=stack.stackId;c.versionId=active.versionId;active.clipIds.push_back(c.clipId);group.clipIds.push_back(c.clipId);}
        t.clips.edit().push_back(c);
        if(i<4){c.clipId=fixedId(40+i);c.linkGroupId=prior.linkGroupId;c.versionId=previous.versionId;c.lengthSamples=240000;previous.clipIds.push_back(c.clipId);prior.clipIds.push_back(c.clipId);t.clips.edit().push_back(c);}
        p.tracks.push_back(t);media->assets.push_back(a);
    }
    media->takes.push_back(take);p.media=media;stack.versions={active,previous};p.takeStacks.push_back(stack);p.linkGroups={group,prior};
    for(int i=0;i<3;++i){Marker m;m.markerId=fixedId(50+i);m.sample=i*96000;m.name="marker";p.markers.push_back(m);}ok(p.validate());return p;
}
}
juce::var gocue::recorder::hardening::editProperty(std::uint64_t seed, int iterations)
{
    std::uint64_t accepted=0,rejected=0,noops=0,undoSteps=0;std::array<int,15> operations{}; std::array<int,15> commits{};
    try
    {
        std::mt19937_64 random(seed);RecorderDocument d;const auto initial=fixture();ok(d.adopt(initial,{},{}));d.setSelection({fixedId(30)});const auto initialHash=hash(initial);const auto registry=initial.media;
        const auto draw=[&](std::uint64_t limit){return random()%limit;};
        // At most 200 gestures per batch; five random attempts form one explicit gesture.
        // Thus a 1,000-attempt run can undo to its original state without bypassing the cap.
        for(int base=0;base<iterations;base+=1000)
        {
            const auto baseline=hash(d.getProject());const auto baselineSelection=d.getSelection();std::vector<juce::String> beforeHashes;std::vector<std::vector<Id>> beforeSelections;
            const int count=(std::min)(1000,iterations-base);
            for(int j=0;j<count;++j)
            {
                const int i=base+j;const auto before=d.snapshot();const auto beforeHash=hash(*before);const auto selection=d.getSelection();const auto depth=d.getHistory().undoDepth();
                std::vector<Id> ids;for(const auto& t:before->tracks)for(const auto& c:t.clips.items())if(before->isActive(c))ids.push_back(c.clipId);
                // Cover a real version switch before random cuts can detach the
                // stack. Subsequent targets/operations remain seed-driven.
                const int op=i==0?14:static_cast<int>(draw(15));++operations[static_cast<size_t>(op)];const auto chosen=ids.empty()?Id{}:ids[static_cast<size_t>(draw(ids.size()))];const auto other=ids.empty()?Id{}:ids[static_cast<size_t>(draw(ids.size()))];
                const auto* c=before->findClip(chosen);const auto position=c?c->timelineStartSample+static_cast<Sample>(draw(static_cast<std::uint64_t>(c->lengthSamples))):0;
                const auto delta=static_cast<Sample>(draw(12801))-6400;const SampleRange r{position,1+static_cast<Sample>(draw(8000))};
                const auto status=d.performEdit("property","property",[&](const RecorderProject& p)->ClipEditResult
                {
                    if(op==13||ids.empty())
                    {
                        auto q=p;const auto lane=static_cast<size_t>(draw(q.tracks.size()));auto& t=q.tracks[lane];Clip fresh;fresh.clipId=fixedId(10000+i);fresh.trackId=t.trackId;fresh.assetId=registry->assets[lane].assetId;fresh.timelineStartSample=q.activeTimelineEnd()+1600;fresh.sourceIn=32000;fresh.lengthSamples=16000;t.clips.edit().push_back(fresh);return q;
                    }
                    if(op==14)
                    {
                        if(p.takeStacks.empty()) return p;
                        const auto& stack=p.takeStacks[static_cast<size_t>(draw(p.takeStacks.size()))];
                        const auto version=i==0?stack.versions.back().versionId:stack.versions[static_cast<size_t>(draw(stack.versions.size()))].versionId;
                        return TakeStackEdits::useVersion(p,stack.stackId,version);
                    }
                    switch(op)
                    {
                        case 0:return ClipEdits::split(p,{chosen},position);
                        case 1:return ClipEdits::trimIn(p,{chosen},c->timelineStartSample+delta);
                        case 2:return ClipEdits::trimOut(p,{chosen},c->timelineEnd()+delta);
                        case 3:return ClipEdits::remove(p,{chosen},r);
                        case 4:return ClipEdits::remove(p,{chosen});
                        case 5:return ClipEdits::rippleDeleteAll(p,r);
                        case 6:return ClipEdits::rippleDeleteTracks(p,r,{c->trackId});
                        case 7:return ClipEdits::move(p,{chosen},delta);
                        case 8:return ClipEdits::reorder(p,{chosen},draw(2)?ClipEdits::Placement::before:ClipEdits::Placement::after,{other});
                        case 9:return ClipEdits::link(p,{chosen,other});
                        case 10:return ClipEdits::unlink(p,{chosen});
                        case 11:{auto q=p;auto& t=q.tracks[static_cast<size_t>(draw(q.tracks.size()))];t.mute=!t.mute;t.solo=draw(2)!=0;return q;}
                        default:{auto q=p;Marker m;m.markerId=fixedId(10000+i);m.sample=position;m.name=juce::String(i);q.markers.push_back(m);return q;}
                    }
                },{{},"gesture-"+juce::String(i/5),static_cast<double>(i)*1000});
                ok(d.getProject().validate());need(d.getProject().media==registry&&hash(initial)==initialHash,"media or immutable input changed");
                if(status.failed()){++rejected;need(d.snapshot()==before&&d.getHistory().undoDepth()==depth&&d.getSelection()==selection,"rejected edit mutated document");}
                else if(d.snapshot()==before)++noops;
                else
                {
                    ++accepted; ++commits[static_cast<size_t>(op)];need(d.getProject().editRevision==before->editRevision+1,"revision monotonicity");
                    const auto plan=d.renderPlanSnapshot();need(plan->editRevision==d.getProject().editRevision&&plan->timelineEnd==d.getProject().activeTimelineEnd(),"plan revision/end");
                    for(const auto& lane:plan->tracks){Sample cursor=0;for(const auto& span:lane.spans){need(span.timeline.start==cursor&&span.timeline.length>0,"render partition");cursor+=span.timeline.length;}need(cursor==plan->timelineEnd,"common track length");}
                    if(d.getHistory().undoDepth()>depth){beforeHashes.push_back(beforeHash);beforeSelections.push_back(selection);}
                }
            }
            const auto finalHash=hash(d.getProject());const auto finalSelection=d.getSelection();const auto steps=d.getHistory().undoDepth();need(steps==beforeHashes.size()&&steps<=200,"bounded gesture accounting");
            for(size_t n=steps;n>0;--n){ok(d.undo());++undoSteps;ok(d.getProject().validate());need(hash(d.getProject())==beforeHashes[n-1]&&d.getSelection()==beforeSelections[n-1],"undo intermediate hash/selection");need(d.getProject().media==registry,"undo registry invariant");}
            need(hash(d.getProject())==baseline&&d.getSelection()==baselineSelection,"all undo restores original hash/selection");
            for(size_t n=0;n<steps;++n){ok(d.redo());ok(d.getProject().validate());}need(hash(d.getProject())==finalHash&&d.getSelection()==finalSelection,"all redo restores final state");
            if(base+count<iterations){const auto state=d.getProject();ok(d.adopt(state,{},{}));}
        }
        need(commits[14]>0,"retake version switch was never committed");
        if(seed==909 && iterations>=1000) need(commits[8]>0 && commits[5]>0 && commits[6]>0,"required seeded reorder/ripple coverage missing");
        auto result=jsonObject(); jsonSet(result,"status","PASS"); jsonSet(result,"seed",jsonInt(seed));
        jsonSet(result,"firstOperation","restore inactive retake version before random destructive edits");
        jsonSet(result,"iterations",iterations); jsonSet(result,"committed",jsonInt(accepted)); jsonSet(result,"rejected",jsonInt(rejected));
        jsonSet(result,"noops",jsonInt(noops)); jsonSet(result,"undoSteps",jsonInt(undoSteps));
        juce::Array<juce::var> attempts, changes; for(auto n:operations) attempts.add(n); for(auto n:commits) changes.add(n);
        jsonSet(result,"operationAttempts",attempts); jsonSet(result,"operationCommits",changes);
        jsonSet(result,"operationOrder","split,trimIn,trimOut,removeRange,remove,rippleAll,rippleTracks,move,reorder,link,unlink,muteSolo,marker,append,retakeVersion");
        jsonSet(result,"invariantsAndUndoRedoHashes",true); return result;
    }
    catch(const std::exception& e){std::cerr<<"EditPropertyTests: FAIL seed="<<seed<<" iterations="<<iterations<<" completed="<<(accepted+rejected+noops)<<" error="<<e.what()<<'\n';throw;}
}
