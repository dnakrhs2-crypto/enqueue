#include "app/RecorderDocument.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace gocue::recorder;
namespace
{
constexpr Sample S = 48000;
void need(bool b, const char* text) { if (!b) throw std::runtime_error(text); }
void ok(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
RecorderProject result(ClipEditResult r) { ok(r.status); ok(r.project.validate()); return std::move(r.project); }
juce::String hash(const RecorderProject& p) { return RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(p)); }
RecorderProject fixture()
{
    RecorderDocument d; Take take; take.logicalLength = 10*S; std::vector<MediaAsset> assets;
    for (int i = 0; i < 4; ++i)
    {
        MediaAsset a; a.kind = i < 2 ? AssetKind::camera : AssetKind::mic; a.logicalLength = 10*S;
        a.relativePath = "media/takes/" + a.assetId + ".fixture"; a.contentIdentity = a.assetId; a.availableRanges = {{0, 10*S}};
        auto& f = a.originalFormat; f.codec = i < 2 ? "h264" : "pcm_s24le";
        if (i < 2) { f.width = 1920; f.height = 1080; a.sourceUnitsNumerator = 30; a.sourceUnitsDenominator = S; (i == 0 ? take.cam1AssetId : take.cam2AssetId) = a.assetId; }
        else { f.sampleRate = static_cast<std::uint32_t>(S); f.channels = 1; f.bitsPerSample = 24; take.microphoneAssetIds.push_back(a.assetId); take.capture.physicalInputs.push_back(i); }
        assets.push_back(a);
    }
    ok(d.placeTake(take, assets)); return d.getProject();
}
const Clip& clip(const RecorderProject& p, size_t lane, size_t index = 0) { return p.tracks.at(lane).clips.items().at(index); }
void rejected(const RecorderProject& p, const ClipEditResult& r)
{ need(r.status.failed() && hash(r.project) == hash(p) && r.project.media == p.media, "rejection must be atomic"); }
std::vector<Id> append(RecorderProject& p, Sample start, Sample length)
{
    LinkGroup group;
    for (auto& t : p.tracks) { auto c = t.clips.items().front(); c.clipId = newId(); c.linkGroupId = group.linkGroupId; c.timelineStartSample = start; c.sourceIn = 0; c.lengthSamples = length; group.clipIds.push_back(c.clipId); t.clips.edit().push_back(c); }
    p.linkGroups.push_back(group); ok(p.validate()); return group.clipIds;
}
RecorderProject stacked()
{
    auto p = fixture(); TakeStack stack; stack.spanSamples = 10*S; TakeVersion active, previous; LinkGroup old;
    for (auto& t : p.tracks)
    {
        auto& c = t.clips.edit().front(); c.takeStackId = stack.stackId; c.versionId = active.versionId; active.clipIds.push_back(c.clipId);
        auto prior = c; prior.clipId = newId(); prior.versionId = previous.versionId; prior.linkGroupId = old.linkGroupId; prior.lengthSamples = 8*S;
        previous.clipIds.push_back(prior.clipId); old.clipIds.push_back(prior.clipId); t.clips.edit().push_back(prior);
    }
    stack.versions = {active, previous}; stack.activeVersionId = active.versionId; p.takeStacks.push_back(stack); p.linkGroups.push_back(old); ok(p.validate()); return p;
}
const RenderSpan& spanAt(const CompiledRenderPlan& p, size_t lane, Sample t)
{ for (const auto& s : p.tracks.at(lane).spans) if (t >= s.timeline.start && t - s.timeline.start < s.timeline.length) return s; throw std::runtime_error("span missing"); }
void partition(const CompiledRenderPlan& plan)
{
    for (const auto& lane : plan.tracks) { Sample cursor = 0; for (const auto& s : lane.spans) { need(s.timeline.start == cursor && s.timeline.length > 0, "render partition gap/overlap"); cursor += s.timeline.length; } need(cursor == plan.timelineEnd, "common render end"); }
}
}
int runClipEditTests()
{
    int passed = 0, failed = 0;
    const auto test = [&](const char* name, auto body) { try { body(); ++passed; std::cout << "PASS clip " << name << '\n'; } catch (const std::exception& e) { ++failed; std::cerr << "FAIL clip " << name << ": " << e.what() << '\n'; } };
    test("carve is deterministic, sample-exact and does not expand links or inactive versions", []
    {
        const auto p = stacked(); const std::vector<Id> lanes{p.tracks[0].trackId}; const SampleRange r{101, 203};
        const auto q = result(ClipEdits::carveOut(p, lanes, r));
        need(hash(q) == hash(result(ClipEdits::carveOut(p, lanes, r))) && q.media == p.media && q.editRevision == p.editRevision, "Pure deterministic carve");
        need(clip(q, 0).lengthSamples == 101 && clip(q, 0, 1).timelineStartSample == 304 && clip(q, 0, 1).sourceIn == 304, "Exact fragments");
        for (size_t lane = 1; lane < p.tracks.size(); ++lane) need(&q.tracks[lane].clips.items() == &p.tracks[lane].clips.items(), "Untargeted clip storage unchanged");
        need(q.findClip(clip(p, 0, 1).clipId)->lengthSamples == 8*S, "Inactive version unchanged");
        need(hash(result(ClipEdits::carveOut(p, {}, r))) == hash(p), "Empty target lanes are a no-op");
        rejected(p, ClipEdits::carveOut(p, {newId()}, r));
        rejected(p, ClipEdits::carveOut(p, lanes, {-1, 3}));
        rejected(p, ClipEdits::carveOut(p, lanes, {0, 0}));
        rejected(p, ClipEdits::carveOut(p, lanes, {(std::numeric_limits<Sample>::max)() - 2, 3}));
    });
    test("split snaps all linked members and preserves source continuity", []
    {
        const auto p = fixture(); const auto before = hash(p); const auto q = result(ClipEdits::split(p, {clip(p, 2).clipId}, 12017));
        for (size_t i = 0; i < 4; ++i) { const auto& a = clip(q, i); const auto& b = clip(q, i, 1); need(a.lengthSamples == 12800 && b.timelineStartSample == 12800 && b.sourceIn == 12800 && a.clipId != clip(p, i).clipId && b.clipId != clip(p, i).clipId, "split geometry/new IDs"); }
        need(q.linkGroups.size() == 2 && hash(p) == before && q.media == p.media && q.editRevision == p.editRevision, "pure split");
        need(hash(q) == hash(result(ClipEdits::split(p, {clip(p, 2).clipId}, 12017))), "deterministic result IDs");
    });
    test("split endpoints and outside are no-ops", [] { const auto p = fixture(); for (const auto t : {Sample{0}, 10*S, 20*S}) need(hash(result(ClipEdits::split(p, {clip(p, 0).clipId}, t))) == hash(p), "endpoint no-op"); });
    test("front trim preserves remaining absolute source position and extends handles", []
    {
        const auto p = fixture(); auto q = result(ClipEdits::trimIn(p, {clip(p, 0).clipId}, 2*S+1));
        for (size_t i = 0; i < 4; ++i) need(clip(q,i).timelineStartSample == 2*S && clip(q,i).sourceIn == 2*S && clip(q,i).timelineEnd() == 10*S, "front trim preserves absolute mapping");
        q = result(ClipEdits::trimIn(q, {clip(q,0).clipId}, S)); need(clip(q,0).sourceIn == S && clip(q,0).timelineStartSample == S, "source handle extension");
    });
    test("back trim and shared handle intersection reject atomically", []
    {
        auto p = fixture(); p.tracks[3].clips.edit()[0].sourceIn = S; p.tracks[3].clips.edit()[0].lengthSamples = 8*S; ok(p.validate());
        const auto q = result(ClipEdits::trimOut(p, {clip(p,0).clipId}, 9*S)); need(clip(q,0).lengthSamples == 9*S && clip(q,3).lengthSamples == 7*S, "same edge delta");
        rejected(q, ClipEdits::trimIn(q, {clip(q,3).clipId}, -1));
        rejected(q, ClipEdits::trimOut(q, {clip(q,0).clipId}, 11*S));
        rejected(q, ClipEdits::trimIn(q, {clip(q,0).clipId}, 8*S));
    });
    test("unlinked audio trims and moves by one sample", []
    {
        auto p = fixture(); const auto mic = clip(p,3).clipId; p = result(ClipEdits::unlink(p,{mic}));
        auto q = result(ClipEdits::trimIn(p,{mic},1)); q = result(ClipEdits::trimOut(q,{mic},10*S-1)); q = result(ClipEdits::move(q,{mic},1));
        need(clip(q,3).sourceIn == 1 && clip(q,3).timelineStartSample == 2 && clip(q,3).lengthSamples == 10*S-2 && clip(q,0).lengthSamples == 10*S, "audio sample independence");
    });
    test("remove clip and middle range preserve empty time", []
    {
        const auto p = fixture(); const auto q = result(ClipEdits::remove(p,{clip(p,0).clipId}, {2*S,S}));
        for (size_t i = 0; i < 4; ++i) need(clip(q,i).timelineEnd() == 2*S && clip(q,i,1).timelineStartSample == 3*S && clip(q,i,1).sourceIn == 3*S, "middle deletion");
        const auto empty = result(ClipEdits::remove(p,{clip(p,0).clipId})); need(empty.activeTimelineEnd() == 0 && empty.media == p.media && empty.linkGroups.empty(), "clip delete keeps media");
        const auto head = result(ClipEdits::remove(p, SampleRange{0,S})); need(clip(head,0).sourceIn == S && clip(head,0).timelineStartSample == S, "head range delete");
    });
    test("11.1 example 1 mic2 gap keeps both cameras at ten seconds", []
    {
        auto p = fixture(); const auto mic = clip(p,3).clipId; p = result(ClipEdits::unlink(p,{mic})); p = result(ClipEdits::remove(p,{mic},{2*S,S}));
        const auto plan = RenderPlanCompiler::compile(p); partition(*plan);
        need(plan->timelineEnd == 10*S && clip(p,0).lengthSamples == 10*S && clip(p,1).lengthSamples == 10*S && spanAt(*plan,3,2*S).isGap() && !spanAt(*plan,3,3*S).isGap(), "one second silence, ten second common range");
    });
    test("11.1 example 2 mic2 ripple and last second silence", []
    {
        auto p = fixture(); p = result(ClipEdits::unlink(p,{clip(p,3).clipId})); p = result(ClipEdits::rippleDeleteTracks(p,{2*S,S},{p.tracks[3].trackId}));
        const auto plan = RenderPlanCompiler::compile(p); partition(*plan);
        need(plan->timelineEnd == 10*S && spanAt(*plan,3,2*S).sourceIn == 3*S && spanAt(*plan,3,9*S).isGap() && clip(p,0).lengthSamples == 10*S, "independent ripple tail pad");
    });
    test("11.1 example 3 all tracks become nine seconds", []
    {
        auto p = fixture(); p = result(ClipEdits::unlink(p,{clip(p,3).clipId})); p = result(ClipEdits::rippleDeleteAll(p,{2*S,S}));
        const auto plan = RenderPlanCompiler::compile(p); partition(*plan); need(plan->timelineEnd == 9*S, "nine second duration");
        for (size_t i = 0; i < 4; ++i) need(clip(p,i,1).timelineStartSample == 2*S && clip(p,i,1).sourceIn == 3*S && clip(p,i,1).timelineEnd() == 9*S, "global mapping");
    });
    test("ripple marker half-open boundaries and selected markers remain", []
    {
        auto p = fixture(); for (const auto t : {S,2*S,3*S,4*S}) { Marker m; m.sample=t; p.markers.push_back(m); }
        const auto q = result(ClipEdits::rippleDeleteAll(p,{2*S,S})); need(q.markers.size()==3 && q.markers[0].sample==S && q.markers[1].sample==2*S && q.markers[2].sample==3*S,"global marker transform");
        p = result(ClipEdits::unlink(p,{clip(p,3).clipId})); const auto r=result(ClipEdits::rippleDeleteTracks(p,{2*S,S},{p.tracks[3].trackId})); need(r.markers[2].sample==3*S && r.markers.size()==4,"local markers untouched");
    });
    test("move uses one absolute video snap and refuses overlap", []
    {
        auto p = fixture(); for (auto& t:p.tracks) t.clips.edit()[0].timelineStartSample=17; ok(p.validate());
        const auto q=result(ClipEdits::move(p,{clip(p,0).clipId},1600)); for(size_t i=0;i<4;++i) need(clip(q,i).timelineStartSample==1600,"shared snapped delta");
        append(p,12*S,3*S); rejected(p,ClipEdits::move(p,{clip(p,0).clipId},4*S)); rejected(p,ClipEdits::move(p,{clip(p,0).clipId},-S));
        need(hash(result(ClipEdits::move(p,{clip(p,0).clipId},0)))==hash(p),"zero move preserves off-grid anchor");
    });
    test("reorder unequal bundles in both directions is one value transform", []
    {
        auto p=fixture(); const auto a=p.linkGroups[0].clipIds; const auto b=append(p,10*S,3*S); append(p,13*S,5*S);
        const auto q=result(ClipEdits::reorder(p,a,ClipEdits::Placement::after,b));
        need(q.findClip(a[0])->timelineStartSample==3*S && q.findClip(b[0])->timelineStartSample==0 && q.activeTimelineEnd()==18*S,"remove/insert length difference");
        need(hash(result(ClipEdits::reorder(q,a,ClipEdits::Placement::before,b)))==hash(p),"reverse reorder restores geometry and IDs");
        rejected(p,ClipEdits::reorder(p,a,ClipEdits::Placement::after,a));
        auto asym=p; asym.tracks[3].clips.edit()[0].timelineStartSample=1; asym.tracks[3].clips.edit()[0].lengthSamples-=1; ok(asym.validate()); rejected(asym,ClipEdits::reorder(asym,a,ClipEdits::Placement::after,b));
    });
    test("stack split trim ripple move include all versions", []
    {
        const auto p=stacked(); const auto q=result(ClipEdits::split(p,{clip(p,0).clipId},3*S));
        for(const auto& t:q.tracks) need(t.clips.items().size()==4,"split every version"); need(q.takeStacks[0].versions[1].clipIds.size()==8,"version refs remapped");
        const auto trimmed=result(ClipEdits::trimOut(p,{clip(p,0).clipId},9*S)); need(clip(trimmed,0).lengthSamples==9*S && clip(trimmed,0,1).lengthSamples==7*S && trimmed.takeStacks[0].spanSamples==9*S,"shorter previous handles");
        const auto rippled=result(ClipEdits::rippleDeleteAll(p,{2*S,S})); need(rippled.takeStacks[0].spanSamples==9*S && rippled.activeTimelineEnd()==9*S,"stack time ripple");
        const auto moved=result(ClipEdits::move(p,{clip(p,0).clipId},S)); need(moved.takeStacks[0].anchorSample==S && clip(moved,0,1).timelineStartSample==S,"all version move");
        const auto plan=RenderPlanCompiler::compile(p); need(plan->activeClips.size()==4 && plan->timelineEnd==10*S,"inactive versions excluded");
    });
    test("stack trim after split touches only corresponding pieces", []
    {
        auto p=stacked(); p=result(ClipEdits::split(p,{clip(p,0).clipId},3*S));
        const auto q=result(ClipEdits::trimIn(p,{clip(p,0,1).clipId},4*S));
        need(clip(q,0).lengthSamples==3*S && clip(q,0,2).lengthSamples==3*S && clip(q,0,1).sourceIn==4*S && clip(q,0,3).sourceIn==4*S,"corresponding version edge only");
    });
    test("unlinked stack mic edits active placement and retains previous", []
    {
        auto p=stacked(); const auto mic=clip(p,3).clipId; const auto previous=clip(p,3,1).clipId; p=result(ClipEdits::unlink(p,{mic}));
        const auto q=result(ClipEdits::trimIn(p,{mic},1)); need(q.findClip(previous)->sourceIn==0 && q.findClip(mic)->sourceIn==1,"previous optional mic preserved");
        const auto all=result(ClipEdits::trimIn(p,{clip(p,0).clipId},S));need(all.findClip(mic)->sourceIn==S&&all.findClip(previous)->sourceIn==S,"video stack trim transforms each version including detached mic");
        const auto r=result(ClipEdits::rippleDeleteTracks(p,{2*S,S},{p.tracks[3].trackId})); need(r.findClip(previous)->lengthSamples==8*S,"local ripple previous version preserved");
        p=result(ClipEdits::move(p,{mic},12*S)); p=result(ClipEdits::rippleDeleteAll(p,{0,10*S})); need(p.takeStacks.size()==1 && p.findClip(mic)->timelineStartSample==2*S && p.findClip(mic)->versionId.isNotEmpty(),"surviving moved mic retains version");
    });
    test("integer source oracle stays identical across video split", []
    {
        auto p=fixture(); for(auto& t:p.tracks)t.clips.edit()[0].timelineStartSample=17;
        const auto before=RenderPlanCompiler::compile(p); const auto q=result(ClipEdits::split(p,{clip(p,0).clipId},2*S+3)); const auto after=RenderPlanCompiler::compile(q);
        for(Sample t=1600;t<10*S;t+=1600) need(RenderPlanCompiler::sourceUnitAt(spanAt(*before,0,t),t,true)==RenderPlanCompiler::sourceUnitAt(spanAt(*after,0,t),t,true),"split source frame oracle");
        need(RenderPlanCompiler::sourceUnitAt(spanAt(*after,0,1600),1600,true)==0,"floor PTS, never nearest frame");
    });
    test("stack links cannot cross active versions and deletion removes empty stack", []
    {
        const auto p=stacked();rejected(p,ClipEdits::link(p,{clip(p,0).clipId,clip(p,0,1).clipId}));
        const auto removed=result(ClipEdits::remove(p,{clip(p,0).clipId}));need(removed.takeStacks.empty()&&removed.linkGroups.empty()&&removed.activeTimelineEnd()==0&&removed.media==p.media,"whole stack removal keeps registry");
    });
    test("unchanged trims preserve off-grid anchors and unrelated empty stacks", []
    {
        auto p=fixture();for(auto& t:p.tracks)t.clips.edit()[0].timelineStartSample=17;
        TakeStack empty;empty.anchorSample=20*S;empty.spanSamples=S;TakeVersion version;empty.versions={version};empty.activeVersionId=version.versionId;p.takeStacks.push_back(empty);ok(p.validate());
        need(hash(result(ClipEdits::trimIn(p,{clip(p,0).clipId},17)))==hash(p),"unchanged front edge");
        need(hash(result(ClipEdits::trimOut(p,{clip(p,0).clipId},10*S+17)))==hash(p),"unchanged back edge and empty stack");
        const auto unchanged=result(ClipEdits::split(p,{clip(p,0).clipId},0));need(unchanged.tracks[0].clips.items().data()==p.tracks[0].clips.items().data(),"no-op retains shared clip storage");
    });
    test("render explicit source gaps, microfades and no fade on continuous split", []
    {
        auto p=fixture(); auto registry=std::make_shared<MediaRegistry>(*p.media); auto& a=registry->assets[3]; a.availableRanges={{0,2*S},{3*S,7*S}}; a.gaps={{2*S,S}}; p.media=registry;
        const auto plan=RenderPlanCompiler::compile(p); partition(*plan); need(spanAt(*plan,3,2*S).isGap(),"source gap rendered");
        need(std::any_of(plan->microfadeBoundaries.begin(),plan->microfadeBoundaries.end(),[&](const MicrofadeBoundary& b){return b.trackId==p.tracks[3].trackId&&b.timelineSample==2*S&&b.beforeSamples==144&&b.afterSamples==0;}),"3ms gap fade");
        p=result(ClipEdits::split(p,{clip(p,0).clipId},S)); const auto split=RenderPlanCompiler::compile(p);
        need(std::none_of(split->microfadeBoundaries.begin(),split->microfadeBoundaries.end(),[](const MicrofadeBoundary& b){return b.timelineSample==S;}),"simple split has no microfade");
    });
    test("mute solo K includes empty selected lanes and excludes camera solo", []
    {
        auto p=fixture(); p.tracks[0].solo=true; p.tracks[2].mute=true; auto plan=RenderPlanCompiler::compile(p); need(plan->audibleTrackCount==1,"audio selection");
        p=result(ClipEdits::unlink(p,{clip(p,3).clipId})); p=result(ClipEdits::remove(p,{clip(p,3).clipId})); p.tracks[3].solo=true;
        plan=RenderPlanCompiler::compile(p); need(plan->audibleTrackCount==1&&plan->tracks[3].audible&&spanAt(*plan,3,S).isGap(),"fixed K in empty solo lane");
        p.tracks[3].mute=true; need(RenderPlanCompiler::compile(p)->audibleTrackCount==0,"muted solo yields silence");
    });
    test("tiny source pieces limit fades to half length and audio mapping rounds at origin", []
    {
        auto p=fixture(); const auto id=clip(p,3).clipId; p=result(ClipEdits::unlink(p,{id})); p=result(ClipEdits::trimOut(p,{id},5));
        p=result(ClipEdits::move(p,{id},100)); // Both edges are internal; project endpoints do not receive a fade.
        const auto plan=RenderPlanCompiler::compile(p); need(plan->activeClips.back().microfadeInSamples==2&&plan->activeClips.back().microfadeOutSamples==2,"half length cap");
        RenderSpan s{{100,1000},newId(),newId(),101,0,147,160}; need(RenderPlanCompiler::sourceUnitAt(s,100,false)==93,"absolute source rescale");
    });
    test("ripple discontinuity fades both sides while a shorter linked handle limits extension", []
    {
        auto p=fixture();const auto q=result(ClipEdits::rippleDeleteAll(p,{2*S,S}));const auto plan=RenderPlanCompiler::compile(q);
        need(std::any_of(plan->microfadeBoundaries.begin(),plan->microfadeBoundaries.end(),[&](const MicrofadeBoundary& b){return b.trackId==p.tracks[3].trackId&&b.timelineSample==2*S&&b.beforeSamples==144&&b.afterSamples==144;}),"source jump fades both sides");
        for(auto& t:p.tracks){t.clips.edit()[0].lengthSamples=8*S;t.clips.edit()[0].sourceIn=S;}p.tracks[3].clips.edit()[0].sourceIn=2*S;ok(p.validate());
        rejected(p,ClipEdits::trimOut(p,{clip(p,0).clipId},9*S));
    });
    test("invalid identifiers ranges and overflow return errors", []
    {
        const auto p=fixture(); rejected(p,ClipEdits::split(p,{},S)); rejected(p,ClipEdits::remove(p,{newId()})); rejected(p,ClipEdits::rippleDeleteAll(p,{S,0}));
        rejected(p,ClipEdits::rippleDeleteAll(p,{1,(std::numeric_limits<Sample>::max)()})); rejected(p,ClipEdits::move(p,{clip(p,0).clipId},(std::numeric_limits<Sample>::min)()));
        rejected(p,ClipEdits::rippleDeleteTracks(p,{S,S},{p.tracks[0].trackId}));
    });
    std::cout<<"ClipEditTests: "<<passed<<" passed, "<<failed<<" failed\n"; return failed?1:0;
}
