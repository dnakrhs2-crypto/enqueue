#include "app/RecorderDocument.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace gocue::recorder;
namespace
{
void need(bool b,const char* m){if(!b)throw std::runtime_error(m);}
void ok(const juce::Result& r){if(r.failed())throw std::runtime_error(r.getErrorMessage().toStdString());}
juce::String hash(const RecorderProject& p){return RecorderSerializer::fingerprint(RecorderSerializer::editStateToVar(p));}
RecorderProject fixture()
{
    RecorderProject p;MediaAsset a;a.kind=AssetKind::importAudio;a.logicalLength=100000;a.availableRanges={{0,100000}};a.relativePath="media/imports/"+a.assetId+".wav";a.contentIdentity=a.assetId;a.originalFormat.codec="pcm_s24le";a.originalFormat.sampleRate=p.Fs;a.originalFormat.channels=2;a.originalFormat.bitsPerSample=24;
    auto media=std::make_shared<MediaRegistry>();media->assets.push_back(a);p.media=media;Track t;t.kind=TrackKind::importAudio;Clip c;c.trackId=t.trackId;c.assetId=a.assetId;c.lengthSamples=10000;t.clips.edit().push_back(c);p.tracks.push_back(t);ok(p.validate());return p;
}
struct Journal:IEditJournalSink
{
    RecorderDocument* document=nullptr;std::vector<EditDelta> deltas;bool reject=false;
    juce::Result enqueue(const EditDelta& d)override{need(document->getProject().editRevision==d.revision,"journal after publication");need(hash(document->getProject())==d.validationHash,"delta result hash");deltas.push_back(d);return reject?juce::Result::fail("queue full"):juce::Result::ok();}
};
struct Consumer:IRenderPlanConsumer
{
    RecorderDocument* document=nullptr;bool reject=false,order=true;int prepares=0,publishes=0;
    std::shared_ptr<const CompiledRenderPlan> prepared,queued,playing;
    juce::Result prepareRenderPlan(std::shared_ptr<const CompiledRenderPlan> p)override
    {++prepares;if(reject)return juce::Result::fail("prefetch rejected");prepared=std::move(p);return juce::Result::ok();}
    void publishPreparedPlan(const Id& id,Sample revision)noexcept override
    {++publishes;order=order&&prepared&&prepared->projectId==id&&prepared->editRevision==revision&&document->getProject().editRevision==revision;queued=prepared;}
    void boundary(){playing=queued;}
};
void replay(juce::var& state,const EditDelta& delta)
{
    for(const auto& d:delta.entities)
    {
        if(d.collection=="project"){state.getDynamicObject()->setProperty("name",d.value);continue;}
        const bool order=d.collection.endsWith("Order");const auto collection=order?d.collection.dropLastCharacters(5):d.collection;
        auto values=state.getProperty(collection,{});auto& list=*values.getArray();const auto field=collection=="tracks"?"trackId":collection=="markers"?"markerId":collection=="linkGroups"?"linkGroupId":"stackId";
        if(order){juce::Array<juce::var> sorted;for(const auto& id:*d.value.getArray())for(const auto& v:list)if(v.getProperty(field,{})==id)sorted.add(v);list=sorted;}
        else{int index=-1;for(int i=0;i<list.size();++i)if(list[i].getProperty(field,{}).toString()==d.entityId)index=i;if(d.removed){need(index>=0,"delta remove exists");list.remove(index);}else if(index>=0)list.set(index,d.value);else list.add(d.value);}
        state.getDynamicObject()->setProperty(collection,values);
    }
    need(RecorderSerializer::fingerprint(state)==delta.validationHash,"entity delta replay hash");
}
}
int runEditHistoryTests()
{
    int passed=0,failed=0;const auto test=[&](const char* name,auto body){try{body();++passed;std::cout<<"PASS history "<<name<<'\n';}catch(const std::exception& e){++failed;std::cerr<<"FAIL history "<<name<<": "<<e.what()<<'\n';}};
    test("pure edit undo redo restores hash selection and publishes monotonic deltas",[]
    {
        RecorderDocument d;const auto p=fixture();ok(d.adopt(p,{},{}));const auto id=p.tracks[0].clips.items()[0].clipId;d.setSelection({id});Journal journal;journal.document=&d;d.setJournalSink(&journal);
        ok(d.performEdit("split","",[&](const RecorderProject& q){return ClipEdits::split(q,{id},3000);}));const auto after=hash(d.getProject());const auto selected=d.getSelection();need(selected.size()==2&&d.getHistory().undoDepth()==1,"split result selection");
        ok(d.undo());need(hash(d.getProject())==hash(p)&&d.getSelection()==std::vector<Id>{id},"undo state selection");ok(d.redo());need(hash(d.getProject())==after&&d.getSelection()==selected,"redo state selection");
        auto replayed=RecorderSerializer::editStateToVar(p);for(const auto& delta:journal.deltas)replay(replayed,delta);need(journal.deltas.size()==3&&journal.deltas[2].revision==3&&journal.deltas[2].baseRevision==2,"undo redo journaled as revisions");
    });
    test("700ms inclusive key window and context boundaries",[]
    {
        RecorderDocument d;ok(d.adopt(fixture(),{},{}));
        const auto rename=[&](const char* value,const char* key,double now,const char* gesture=""){ok(d.performEdit("rename",key,[&](const RecorderProject& p){auto q=p;q.name=value;return q;},{ {},gesture,now}));};
        rename("a","name",0);rename("b","name",700);need(d.getHistory().undoDepth()==1,"700ms inclusive");rename("c","name",1401);need(d.getHistory().undoDepth()==2,"701ms new entry");
        rename("d","different",1402);rename("e","different",1300);need(d.getHistory().undoDepth()==4,"different key and backwards clock");
        rename("f","drag",2000,"gesture");rename("g","drag",20000,"gesture");need(d.getHistory().undoDepth()==5,"long drag one step");rename("h","other",20001,"gesture");need(d.getHistory().undoDepth()==6,"same gesture different coalesce key distinct");
        d.endGesture();rename("i","other",20002,"gesture");need(d.getHistory().undoDepth()==7,"gesture end");
        d.setSelection({d.getProject().tracks[0].clips.items()[0].clipId});rename("j","other",20003,"gesture");need(d.getHistory().undoDepth()==8,"selection context breaks merge");
    });
    test("coalesced return to original geometry still restores selection",[]
    {
        RecorderDocument d;const auto p=fixture();ok(d.adopt(p,{},{}));const auto id=p.tracks[0].clips.items()[0].clipId;
        ok(d.performEdit("move","move",[&](const RecorderProject& q){return ClipEdits::move(q,{id},1);},{{},"drag",0}));
        ok(d.performEdit("move","move",[&](const RecorderProject& q){return ClipEdits::move(q,{id},-1);},{{},"drag",100}));
        need(hash(d.getProject())==hash(p)&&d.getSelection()==std::vector<Id>{id}&&d.getHistory().undoDepth()==1,"cancelled geometry, changed selection");
        ok(d.undo());need(d.getSelection().empty()&&d.getProject().editRevision==3,"undo selection-only difference is journalable");ok(d.redo());need(d.getSelection()==std::vector<Id>{id}&&d.getProject().editRevision==4,"redo selection-only difference");
    });
    test("200 step bound and redo invalidation",[]
    {
        RecorderDocument d;ok(d.adopt(fixture(),{},{}));for(int i=1;i<=205;++i)ok(d.performEdit("name","",[&](const RecorderProject& p){auto q=p;q.name=juce::String(i);return q;}));
        need(d.getHistory().undoDepth()==200,"bounded history");for(int i=0;i<200;++i)ok(d.undo());need(d.getProject().name=="5"&&d.getHistory().undoDepth()==0&&d.getHistory().redoDepth()==200,"oldest five evicted");
        for(int i=0;i<200;++i)ok(d.redo());need(d.getProject().name=="205","redo complete");ok(d.undo());ok(d.performEdit("branch","",[](const RecorderProject& p){auto q=p;q.name="branch";return q;}));need(d.getHistory().redoDepth()==0,"new edit invalidates redo");
    });
    test("registry generation append and cache identity survive every undo",[]
    {
        RecorderDocument d;const auto p=fixture();ok(d.adopt(p,{},{}));const auto id=p.tracks[0].clips.items()[0].clipId;const auto held=d.snapshot();
        ok(d.performEdit("trim","",[&](const RecorderProject& q){return ClipEdits::trimIn(q,{id},1);}));
        auto a=d.getProject().media->assets[0];a.mediaGeneration=1;ok(d.updateMediaAsset(a));auto extra=a;extra.assetId=newId();extra.relativePath="media/imports/"+extra.assetId+".wav";ok(d.registerMedia({extra}));const auto registry=d.getProject().media;
        ok(d.undo());need(d.getProject().media==registry&&registry->assets.size()==2&&registry->assets[0].mediaGeneration==1,"registry outside snapshot");ok(d.redo());need(d.getProject().media==registry,"redo registry retained");
        need(held->media==p.media&&held->findClip(id)->sourceIn==0&&d.renderPlanSnapshot()->activeClips[0].mediaGeneration==1,"held immutable snapshot and current plan generation");
    });
    test("no-op and failed edits preserve revision redo and publication",[]
    {
        RecorderDocument d;ok(d.adopt(fixture(),{},{}));const auto id=d.getProject().tracks[0].clips.items()[0].clipId;ok(d.performEdit("trim","",[&](const RecorderProject& p){return ClipEdits::trimIn(p,{id},1);}));ok(d.undo());
        const auto before=d.snapshot();const auto plan=d.renderPlanSnapshot();ok(d.performEdit("noop","",[&](const RecorderProject& p){return ClipEdits::split(p,{id},0);}));
        need(d.performEdit("bad","",[&](const RecorderProject& p){return ClipEdits::move(p,{id},-1);}).failed(),"invalid edit rejected");
        need(d.snapshot()==before&&d.renderPlanSnapshot()==plan&&d.getHistory().redoDepth()==1,"no-op/rejection keeps undo redo and plan");
    });
    test("pure adapter rejects registry timebase identity and revision mutations",[]
    {
        RecorderDocument d;ok(d.adopt(fixture(),{},{}));const auto before=d.snapshot();
        for(int mode=0;mode<4;++mode)need(d.performEdit("invalid","",[&](const RecorderProject& p){auto q=p;if(mode==0)q.media=std::make_shared<MediaRegistry>(*p.media);if(mode==1)q.Fs=44100;if(mode==2)q.projectId=newId();if(mode==3)++q.editRevision;return q;}).failed(),"metadata boundary enforced");
        need(d.snapshot()==before&&d.getHistory().undoDepth()==0,"no partial publish");
    });
    test("prepared plan publication and simulated block boundary including undo",[]
    {
        RecorderDocument d;ok(d.adopt(fixture(),{},{}));Consumer consumer;consumer.document=&d;ok(d.setRenderPlanConsumer(&consumer));consumer.boundary();const auto playing=consumer.playing;
        const auto id=d.getProject().tracks[0].clips.items()[0].clipId;ok(d.performEdit("cut","",[&](const RecorderProject& p){return ClipEdits::remove(p,{id},{2000,1000});}));
        need(consumer.playing==playing&&consumer.queued==d.renderPlanSnapshot()&&consumer.queued->editRevision==1&&consumer.order,"old plan held until boundary");consumer.boundary();need(consumer.playing->editRevision==1,"boundary replaces plan");
        consumer.reject=true;const auto before=d.snapshot();need(d.undo().failed()&&d.snapshot()==before&&d.getHistory().undoDepth()==1,"prepare failure preserves history cursor");consumer.reject=false;ok(d.undo());consumer.boundary();need(consumer.playing->editRevision==2&&consumer.order,"undo also prepares new revision");ok(d.setRenderPlanConsumer(nullptr));
    });
    test("journal queue failure leaves published edit visible and unsaved",[]
    {
        RecorderDocument d;ok(d.adopt(fixture(),{},{}));Journal journal;journal.document=&d;journal.reject=true;d.setJournalSink(&journal);ok(d.performEdit("name","",[](const RecorderProject& p){auto q=p;q.name="visible";return q;}));
        need(d.getProject().name=="visible"&&d.isDirty()&&d.getError()=="queue full"&&d.getHistory().undoDepth()==1,"published edit survives queue failure");
    });
    test("revision overflow and reentrant edits cannot publish",[]
    {
        RecorderDocument d;auto p=fixture();p.editRevision=(std::numeric_limits<Sample>::max)();ok(d.adopt(p,{},{}));need(d.performEdit("name","",[](const RecorderProject& q){auto r=q;r.name="overflow";return r;}).failed(),"revision overflow");need(d.getProject().editRevision==p.editRevision&&d.getHistory().undoDepth()==0,"overflow atomic");
        d.newProject("new");ok(d.performEdit("outer","",[&](const RecorderProject& q){need(d.performEdit("inner",[](EditState& e){e.name="inner";}).failed(),"reentrant edit rejected");auto r=q;r.name="outer";return r;}));need(d.getProject().name=="outer"&&d.getHistory().undoDepth()==1,"one transaction");
    });
    std::cout<<"EditHistoryTests: "<<passed<<" passed, "<<failed<<" failed\n";return failed?1:0;
}
