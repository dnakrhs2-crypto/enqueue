#include "RecorderDocument.h"
#include "model/TakeStackEdits.h"
#include <algorithm>
#include <limits>
#include <map>

namespace gocue::recorder
{
namespace
{
juce::String json(const juce::var& v) { return juce::JSON::toString(v, true); }
EditDelta deltaFor(const RecorderProject& before, const RecorderProject& after, const juce::String& name)
{
    EditDelta delta; delta.projectId = after.projectId; delta.baseRevision = before.editRevision; delta.revision = after.editRevision; delta.name = name;
    const auto a = RecorderSerializer::editStateToVar(before), b = RecorderSerializer::editStateToVar(after);
    delta.validationHash = RecorderSerializer::fingerprint(b);
    if (before.name != after.name) delta.entities.push_back({"project", after.projectId, false, b.getProperty("name", {})});
    const std::pair<const char*, const char*> fields[] { {"tracks", "trackId"}, {"markers", "markerId"}, {"linkGroups", "linkGroupId"}, {"takeStacks", "stackId"} };
    for (const auto& field : fields)
    {
        std::map<Id, juce::var> old;
        for (const auto& entity : *a.getProperty(field.first, {}).getArray()) old.emplace(entity.getProperty(field.second, {}).toString(), entity);
        std::vector<Id> oldOrder, newOrder;
        for (const auto& entity : *a.getProperty(field.first, {}).getArray()) oldOrder.push_back(entity.getProperty(field.second, {}).toString());
        for (const auto& entity : *b.getProperty(field.first, {}).getArray())
        {
            const auto id = entity.getProperty(field.second, {}).toString(); newOrder.push_back(id);
            const auto it = old.find(id);
            if (it == old.end() || json(it->second) != json(entity)) delta.entities.push_back({field.first, id, false, entity});
            if (it != old.end()) old.erase(it);
        }
        for (const auto& removed : old) delta.entities.push_back({field.first, removed.first, true, {}});
        if (oldOrder != newOrder)
        {
            juce::Array<juce::var> order; for (const auto& id : newOrder) order.add(id);
            delta.entities.push_back({juce::String(field.first) + "Order", after.projectId, false, order});
        }
    }
    return delta;
}
}
RecorderDocument::RecorderDocument() : project(std::make_shared<const RecorderProject>()), renderPlan(RenderPlanCompiler::compile(*project)) {}
juce::Result RecorderDocument::preparePlan(const RecorderProject& next, std::shared_ptr<const CompiledRenderPlan>& plan)
{
    try
    {
        plan = RenderPlanCompiler::compile(next);
        if (renderConsumer) return renderConsumer->prepareRenderPlan(plan);
        return juce::Result::ok();
    }
    catch (const std::exception& e) { return juce::Result::fail(juce::String::fromUTF8(e.what())); }
}
juce::Result RecorderDocument::replaceProject(RecorderProject next)
{
    std::shared_ptr<const CompiledRenderPlan> plan;
    const auto ready = preparePlan(next, plan); if (ready.failed()) return fail(ready.getErrorMessage());
    project = std::make_shared<const RecorderProject>(std::move(next)); renderPlan = std::move(plan);
    if (renderConsumer) renderConsumer->publishPreparedPlan(project->projectId, project->editRevision);
    return juce::Result::ok();
}
juce::Result RecorderDocument::setRenderPlanConsumer(IRenderPlanConsumer* consumer)
{
    assertOwner(); if (editing) return juce::Result::fail(juce::String::fromUTF8("편집 작업 중입니다."));
    const juce::ScopedValueSetter<bool> guard(editing, true);
    if (consumer)
    {
        try { const auto ready = consumer->prepareRenderPlan(renderPlan); if (ready.failed()) return fail(ready.getErrorMessage()); }
        catch (const std::exception& e) { return fail(juce::String::fromUTF8(e.what())); }
    }
    renderConsumer = consumer;
    if (consumer) consumer->publishPreparedPlan(project->projectId, project->editRevision);
    return juce::Result::ok();
}
void RecorderDocument::assertOwner() const { jassert(owner == std::this_thread::get_id()); }
void RecorderDocument::notify() { if (onChanged) onChanged(); }
juce::Result RecorderDocument::fail(const juce::String& message) { error = message; notify(); return juce::Result::fail(message); }
juce::String RecorderDocument::getStatusText() const
{
    if (error.isNotEmpty()) return juce::String::fromUTF8("확인 필요");
    if (dirty) return juce::String::fromUTF8("저장 중");
    return juce::String::fromUTF8("저장됨");
}
void RecorderDocument::newProject(const juce::String& name, std::uint32_t Fs, FrameRate fps)
{
    if (recordingStructureLock) { fail(juce::String::fromUTF8("녹화 중에는 프로젝트를 바꿀 수 없습니다.")); return; }
    assertOwner(); if (editing) return;
    const juce::ScopedValueSetter<bool> guard(editing, true);
    RecorderProject next; next.name = name; next.Fs = Fs; next.fps = fps;
    const auto valid = next.validate(); if (valid.failed()) { fail(valid.getErrorMessage()); return; }
    if (replaceProject(std::move(next)).failed()) return;
    history.clear(); selection.clear(); file = {}; error.clear(); recovery.clear(); dirty = checkpointRequired = true; savedRevision = 0; journalTransactions.clear(); notify();
}
juce::Result RecorderDocument::adopt(RecorderProject next, const juce::File& source, const CheckpointInfo& info)
{
    if (recordingStructureLock) return fail(juce::String::fromUTF8("녹화 중에는 프로젝트를 바꿀 수 없습니다."));
    assertOwner(); if (editing) return juce::Result::fail(juce::String::fromUTF8("편집 작업 중입니다."));
    const juce::ScopedValueSetter<bool> guard(editing, true);
    const auto valid = next.validate(); if (valid.failed()) return fail(valid.getErrorMessage());
    const auto replaced = replaceProject(std::move(next)); if (replaced.failed()) return replaced;
    history.clear(); selection.clear(); file = source;
    savedRevision = project->editRevision; dirty = checkpointRequired = false; journalTransactions.clear(); error.clear(); recovery = info.recoveryMessage; notify(); return juce::Result::ok();
}
juce::Result RecorderDocument::openCheckpoint(const juce::File& source)
{
    assertOwner(); RecorderProject next; CheckpointInfo info; const auto result = RecorderSerializer::readCheckpoint(source, next, &info);
    return result.wasOk() ? adopt(std::move(next), source, info) : fail(result.getErrorMessage());
}
juce::Result RecorderDocument::saveCheckpoint(const juce::File& target)
{
    assertOwner(); const auto written = snapshot(); const auto result = RecorderSerializer::writeCheckpoint(target, *written);
    checkpointFinished(written, target, result); return result;
}
void RecorderDocument::checkpointFinished(Snapshot written, const juce::File& target, const juce::Result& result)
{
    assertOwner(); if (written->projectId != project->projectId) return;
    if (result.failed()) { fail(juce::String::fromUTF8("프로젝트를 저장할 수 없습니다: ") + result.getErrorMessage()); return; }
    file = target; savedRevision = (std::max)(savedRevision, written->editRevision);
    if (written->media == project->media && written->Fs == project->Fs && written->fps == project->fps) checkpointRequired = false;
    dirty = checkpointRequired || savedRevision < project->editRevision;
    if (!dirty) error.clear();
    notify();
}
juce::Result RecorderDocument::setTimebase(std::uint32_t Fs, FrameRate fps)
{
    if (recordingStructureLock) return fail(juce::String::fromUTF8("녹화 중에는 시간 기준을 바꿀 수 없습니다."));
    assertOwner(); if (editing) return juce::Result::fail(juce::String::fromUTF8("편집 작업 중입니다."));
    const juce::ScopedValueSetter<bool> guard(editing, true);
    if (!project->media->assets.empty() && (project->Fs != Fs || project->fps != fps)) return fail(juce::String::fromUTF8("첫 미디어 이후에는 프로젝트 샘플레이트와 프레임레이트를 바꿀 수 없습니다."));
    auto next = *project; next.Fs = Fs; next.fps = fps; const auto valid = next.validate(); if (valid.failed()) return fail(valid.getErrorMessage());
    if (Fs != project->Fs || fps != project->fps) { const auto replaced = replaceProject(std::move(next)); if (replaced.failed()) return replaced; dirty = checkpointRequired = true; history.clear(); error.clear(); notify(); }
    return juce::Result::ok();
}
EditSnapshot RecorderDocument::editSnapshot() const { return {static_cast<const EditState&>(*project), selection}; }
void RecorderDocument::setSelection(std::vector<Id> ids)
{
    assertOwner(); if (editing) return; std::vector<Id> valid;
    for (const auto& id : ids) if (project->findClip(id) != nullptr && std::find(valid.begin(), valid.end(), id) == valid.end()) valid.push_back(id);
    if (selection != valid) history.breakCoalescing();
    selection = std::move(valid); notify();
}
juce::Result RecorderDocument::performEdit(const juce::String& name, const std::function<void(EditState&)>& edit, const EditOptions& options)
{
    assertOwner();
    if (editing) return juce::Result::fail(juce::String::fromUTF8("편집 작업 중에는 다른 편집을 시작할 수 없습니다."));
    if (recordingStructureLock) return fail(juce::String::fromUTF8("녹화 중에는 구조를 편집할 수 없습니다. 마커 추가는 가능합니다."));
    const juce::ScopedValueSetter<bool> guard(editing, true);
    auto next = *project;
    try { edit(static_cast<EditState&>(next)); }
    catch (const std::exception& e) { return fail(juce::String::fromUTF8(e.what())); }
    auto nextSelection = selection;
    nextSelection.erase(std::remove_if(nextSelection.begin(), nextSelection.end(), [&](const auto& id) { return next.findClip(id) == nullptr; }), nextSelection.end());
    return publishEdit(std::move(next), name, options, true, nextSelection);
}
juce::Result RecorderDocument::addMarker(Marker marker)
{
    assertOwner(); if (editing) return juce::Result::fail(juce::String::fromUTF8("편집 작업 중입니다."));
    const juce::ScopedValueSetter<bool> guard(editing, true);
    auto next = *project; next.markers.push_back(std::move(marker));
    return publishEdit(std::move(next), juce::String::fromUTF8("마커 추가"), {}, true, selection, EditOrigin::markerAppend);
}
juce::Result RecorderDocument::publishEdit(RecorderProject next, const juce::String& name, const EditOptions& options,
    bool addHistory, const std::vector<Id>& nextSelection, EditOrigin origin)
{
    if (recordingStructureLock && origin != EditOrigin::coordinator)
    {
        // Coordinator authority is local to this publication, never a temporary
        // unlock that notifications or other user edits could inherit.
        auto structural = next; structural.markers = project->markers;
        const bool prefix = next.markers.size() > project->markers.size()
            && std::equal(project->markers.begin(), project->markers.end(), next.markers.begin(), [](const Marker& a, const Marker& b)
            { return a.markerId == b.markerId && a.sample == b.sample && a.name == b.name && a.colour == b.colour; });
        if (origin != EditOrigin::markerAppend || !prefix || next.media != project->media
            || json(RecorderSerializer::editStateToVar(structural)) != json(RecorderSerializer::editStateToVar(*project)))
            return fail(juce::String::fromUTF8("녹화 중에는 구조를 편집할 수 없습니다. 마커 추가는 가능합니다."));
    }
    const auto valid = next.validate(); if (valid.failed()) return fail(valid.getErrorMessage());
    // A coalesced gesture can return to its starting geometry but change selection.
    // Undo/redo still restores that snapshot and emits a fresh journal revision.
    if (addHistory && json(RecorderSerializer::editStateToVar(next)) == json(RecorderSerializer::editStateToVar(*project)) && next.media == project->media) return juce::Result::ok();
    if (project->editRevision == (std::numeric_limits<Sample>::max)()) return fail(juce::String::fromUTF8("편집 이력 번호 범위를 초과했습니다."));
    next.editRevision = project->editRevision + 1;
    const auto delta = deltaFor(*project, next, name);
    std::shared_ptr<const CompiledRenderPlan> plan;
    const auto ready = preparePlan(next, plan); if (ready.failed()) return fail(ready.getErrorMessage());
    lastTransaction = delta.transactionId;
    if (addHistory) history.push(editSnapshot(), {static_cast<const EditState&>(next), nextSelection}, name, options);
    if (next.media != project->media) checkpointRequired = true;
    project = std::make_shared<const RecorderProject>(std::move(next)); selection = nextSelection; dirty = true; error.clear();
    renderPlan = std::move(plan);
    if (renderConsumer) renderConsumer->publishPreparedPlan(project->projectId, project->editRevision);
    // Publish first, then queue the resulting delta. A failed queue leaves the edit visible and unsaved.
    if (journal != nullptr)
    {
        journalTransactions[delta.revision] = delta.transactionId;
        try { const auto accepted = journal->enqueue(delta); if (accepted.failed()) error = accepted.getErrorMessage(); }
        catch (const std::exception& e) { error = juce::String::fromUTF8(e.what()); }
    }
    if (addHistory) notify();
    return juce::Result::ok();
}
juce::Result RecorderDocument::performEdit(const juce::String& name, const juce::String& key,
    const std::function<ClipEditResult(const RecorderProject&)>& edit, const EditOptions& supplied)
{
    assertOwner(); if (editing) return juce::Result::fail(juce::String::fromUTF8("편집 작업 중입니다."));
    if (recordingStructureLock) return fail(juce::String::fromUTF8("녹화 중에는 구조를 편집할 수 없습니다. 마커 추가는 가능합니다."));
    const juce::ScopedValueSetter<bool> guard(editing, true);
    try
    {
        auto result = edit(*project); if (result.status.failed()) return fail(result.status.getErrorMessage());
        auto& next = result.project;
        if (next.projectId != project->projectId || next.schemaVersion != project->schemaVersion || next.Fs != project->Fs
            || next.fps != project->fps || next.editRevision != project->editRevision || next.media != project->media)
            return fail(juce::String::fromUTF8("편집은 프로젝트 식별자·시간 기준·미디어 registry를 변경할 수 없습니다."));
        auto selected = result.selection.value_or(selection); std::vector<Id> filtered;
        for (const auto& id : selected) if (const auto* c = next.findClip(id); c && next.isActive(*c) && std::find(filtered.begin(), filtered.end(), id) == filtered.end()) filtered.push_back(id);
        auto options = supplied; options.mergeKey = key;
        return publishEdit(std::move(next), name, options, true, filtered);
    }
    catch (const std::exception& e) { return fail(juce::String::fromUTF8(e.what())); }
}

juce::Result RecorderDocument::placeDubbingTake(Take take, std::vector<MediaAsset> assets,
    const std::vector<int>& microphones, SampleRange range, const Id& retakeStack)
{
    assertOwner(); if (editing) return fail(juce::String::fromUTF8("편집 작업 중입니다."));
    const juce::ScopedValueSetter<bool> guard(editing, true);
    auto next = *project; auto registry = std::make_shared<MediaRegistry>(*project->media);
    int last = 0; for (const auto& previous : registry->takes) last = (std::max)(last, previous.number);
    if (last == (std::numeric_limits<int>::max)()) return fail(juce::String::fromUTF8("테이크 번호 범위를 초과했습니다."));
    take.number = last + 1; take.mode = TakeMode::dub;
    if (take.createdAt.isEmpty()) take.createdAt = juce::Time::getCurrentTime().toISO8601(true);
    if (take.name.isEmpty()) take.name = juce::String::fromUTF8("더빙 ") + juce::String(take.number);
    registry->assets.insert(registry->assets.end(), assets.begin(), assets.end()); registry->takes.push_back(take); next.media = registry;
    auto result = TakeStackEdits::addTake(next, take.takeId, range, microphones, retakeStack);
    if (result.status.failed()) return fail(result.status.getErrorMessage());
    std::vector<Id> selected;
    for (const auto& t : result.project.tracks) for (const auto& c : t.clips.items())
        if (result.project.isActive(c) && (c.assetId == take.cam1AssetId || c.assetId == take.cam2AssetId)) selected.push_back(c.clipId);
    return publishEdit(std::move(result.project), juce::String::fromUTF8("더빙 테이크 배치"), {}, true, selected, EditOrigin::coordinator);
}
juce::Result RecorderDocument::useTakeVersion(const Id& stack, const Id& version)
{
    return performEdit(juce::String::fromUTF8("이 테이크 사용"), {}, [&](const RecorderProject& p) { return TakeStackEdits::useVersion(p, stack, version); });
}
juce::Result RecorderDocument::undo()
{
    if (recordingStructureLock) return fail(juce::String::fromUTF8("녹화 중에는 실행취소할 수 없습니다."));
    assertOwner(); if (editing) return juce::Result::fail(juce::String::fromUTF8("편집 작업 중입니다."));
    const juce::ScopedValueSetter<bool> guard(editing, true);
    const auto* entry = history.undoEntry(); if (entry == nullptr) return juce::Result::ok();
    const auto current = editSnapshot();
    auto next = *project; static_cast<EditState&>(next) = entry->before.state;
    const auto result = publishEdit(std::move(next), juce::String::fromUTF8("실행취소: ") + entry->name, {}, false, entry->before.selection);
    if (result.wasOk()) { history.commitUndo(current); notify(); } return result;
}
juce::Result RecorderDocument::redo()
{
    if (recordingStructureLock) return fail(juce::String::fromUTF8("녹화 중에는 다시실행할 수 없습니다."));
    assertOwner(); if (editing) return juce::Result::fail(juce::String::fromUTF8("편집 작업 중입니다."));
    const juce::ScopedValueSetter<bool> guard(editing, true);
    const auto* entry = history.redoEntry(); if (entry == nullptr) return juce::Result::ok();
    const auto current = editSnapshot();
    auto next = *project; static_cast<EditState&>(next) = entry->after.state;
    const auto result = publishEdit(std::move(next), juce::String::fromUTF8("다시실행: ") + entry->name, {}, false, entry->after.selection);
    if (result.wasOk()) { history.commitRedo(current); notify(); } return result;
}
juce::Result RecorderDocument::registerMedia(std::vector<MediaAsset> assets, std::vector<Take> takes)
{
    assertOwner(); if (editing) return juce::Result::fail(juce::String::fromUTF8("편집 작업 중입니다."));
    const juce::ScopedValueSetter<bool> guard(editing, true);
    auto next = *project; auto registry = std::make_shared<MediaRegistry>(*project->media);
    registry->assets.insert(registry->assets.end(), assets.begin(), assets.end()); registry->takes.insert(registry->takes.end(), takes.begin(), takes.end()); next.media = registry;
    const auto valid = next.validate(); if (valid.failed()) return fail(valid.getErrorMessage());
    const auto replaced = replaceProject(std::move(next)); if (replaced.failed()) return replaced;
    dirty = checkpointRequired = true; error.clear(); notify(); return juce::Result::ok();
}
juce::Result RecorderDocument::updateMediaAsset(MediaAsset asset)
{
    assertOwner(); if (editing) return juce::Result::fail(juce::String::fromUTF8("편집 작업 중입니다."));
    const juce::ScopedValueSetter<bool> guard(editing, true);
    auto registry = std::make_shared<MediaRegistry>(*project->media);
    auto found = std::find_if(registry->assets.begin(), registry->assets.end(), [&](const auto& a) { return a.assetId == asset.assetId; });
    if (found == registry->assets.end()) return fail(juce::String::fromUTF8("원본 미디어를 찾을 수 없습니다."));
    const auto& a = found->originalFormat; const auto& b = asset.originalFormat;
    if (asset.mediaGeneration <= found->mediaGeneration || asset.kind != found->kind || asset.contentIdentity != found->contentIdentity
        || a.codec != b.codec || a.sampleRate != b.sampleRate || a.channels != b.channels || a.bitsPerSample != b.bitsPerSample
        || a.width != b.width || a.height != b.height || a.fps != b.fps)
        return fail(juce::String::fromUTF8("원본 식별자와 포맷을 보존한 새 미디어 세대가 필요합니다."));
    *found = std::move(asset); auto next = *project; next.media = registry;
    const auto valid = next.validate(); if (valid.failed()) return fail(valid.getErrorMessage());
    const auto replaced = replaceProject(std::move(next)); if (replaced.failed()) return replaced;
    dirty = checkpointRequired = true; error.clear(); notify(); return juce::Result::ok();
}
juce::Result RecorderDocument::updateTakeState(const Id& id, TakeState state)
{
    assertOwner(); if (editing) return juce::Result::fail(juce::String::fromUTF8("편집 작업 중입니다."));
    const juce::ScopedValueSetter<bool> guard(editing, true);
    auto registry = std::make_shared<MediaRegistry>(*project->media);
    auto found = std::find_if(registry->takes.begin(), registry->takes.end(), [&](const auto& t) { return t.takeId == id; });
    if (found == registry->takes.end()) return fail(juce::String::fromUTF8("테이크를 찾을 수 없습니다."));
    found->state = state; auto next = *project; next.media = registry;
    const auto valid = next.validate(); if (valid.failed()) return fail(valid.getErrorMessage());
    const auto replaced = replaceProject(std::move(next)); if (replaced.failed()) return replaced;
    dirty = checkpointRequired = true; error.clear(); notify(); return juce::Result::ok();
}
juce::Result RecorderDocument::placeTake(Take take, std::vector<MediaAsset> assets)
{
    return placeNewTake(std::move(take), std::move(assets), EditOrigin::user);
}
juce::Result RecorderDocument::placeNewTake(Take take, std::vector<MediaAsset> assets, EditOrigin origin)
{
    if (recordingStructureLock && origin != EditOrigin::coordinator) return fail(juce::String::fromUTF8("녹화 중에는 테이크를 배치할 수 없습니다."));
    assertOwner(); if (editing) return juce::Result::fail(juce::String::fromUTF8("편집 작업 중입니다."));
    const juce::ScopedValueSetter<bool> guard(editing, true);
    auto next = *project; auto registry = std::make_shared<MediaRegistry>(*project->media);
    if (take.number == 0)
    {
        int last = 0; for (const auto& previous : registry->takes) last = (std::max)(last, previous.number);
        if (last == (std::numeric_limits<int>::max)()) return fail(juce::String::fromUTF8("테이크 번호 범위를 초과했습니다."));
        take.number = last + 1;
    }
    if (take.mode == TakeMode::normal) take.placementSample = project->activeTimelineEnd();
    if (take.createdAt.isEmpty()) take.createdAt = juce::Time::getCurrentTime().toISO8601(true);
    if (take.name.isEmpty()) take.name = juce::String::fromUTF8("테이크 ") + juce::String(take.number).paddedLeft('0', 3) + " · " + take.createdAt;
    registry->assets.insert(registry->assets.end(), assets.begin(), assets.end()); registry->takes.push_back(take); next.media = registry;
    return place(std::move(next), take, take.placementSample, origin);
}
juce::Result RecorderDocument::placeTake(const Id& id)
{
    if (recordingStructureLock) return fail(juce::String::fromUTF8("녹화 중에는 테이크를 배치할 수 없습니다."));
    assertOwner(); if (editing) return juce::Result::fail(juce::String::fromUTF8("편집 작업 중입니다."));
    const juce::ScopedValueSetter<bool> guard(editing, true);
    const auto* take = project->media->findTake(id); if (take == nullptr) return fail(juce::String::fromUTF8("테이크를 찾을 수 없습니다."));
    return place(*project, *take, take->mode == TakeMode::normal ? project->activeTimelineEnd() : take->placementSample);
}
juce::Result RecorderDocument::place(RecorderProject next, const Take& take, Sample placement, EditOrigin origin)
{
    LinkGroup group;
    const auto addClip = [&](const Id& assetId, TrackKind kind, int micIndex, const juce::String& name)
    {
        if (assetId.isEmpty()) return;
        const auto* source = next.media->findAsset(assetId); if (source == nullptr) return;
        auto found = std::find_if(next.tracks.begin(), next.tracks.end(), [&](const auto& t) { return t.kind == kind && t.microphoneIndex == micIndex; });
        if (found == next.tracks.end()) { Track t; t.kind = kind; t.microphoneIndex = micIndex; t.name = name; next.tracks.push_back(t); found = std::prev(next.tracks.end()); }
        Clip c; c.trackId = found->trackId; c.assetId = assetId; c.timelineStartSample = placement; c.lengthSamples = source->logicalLength; c.linkGroupId = group.linkGroupId;
        group.clipIds.push_back(c.clipId); found->clips.edit().push_back(c);
    };
    addClip(take.cam1AssetId, TrackKind::cam1, -1, juce::String::fromUTF8("캠1"));
    addClip(take.cam2AssetId, TrackKind::cam2, -1, juce::String::fromUTF8("캠2"));
    for (size_t i = 0; i < take.microphoneAssetIds.size(); ++i)
    {
        const int logical = placementMicrophones.empty() ? static_cast<int>(i) : placementMicrophones[i];
        addClip(take.microphoneAssetIds[i], TrackKind::mic, logical, juce::String::fromUTF8("마이크 ") + juce::String(logical + 1));
    }
    if (group.clipIds.size() >= 2) next.linkGroups.push_back(group);
    else for (auto& t : next.tracks) for (auto& c : t.clips.edit()) if (c.linkGroupId == group.linkGroupId) c.linkGroupId.clear();
    return publishEdit(std::move(next), juce::String::fromUTF8("테이크 배치"), {}, true, group.clipIds, origin);
}
void RecorderDocument::acknowledgeJournal(const EditDelta& ticket, const juce::Result& result)
{
    assertOwner(); const auto revision = ticket.revision; const auto pending = journalTransactions.find(revision);
    if (ticket.projectId != project->projectId || pending == journalTransactions.end() || pending->second != ticket.transactionId
        || revision < savedRevision || revision > project->editRevision) return;
    if (result.failed()) { fail(result.getErrorMessage()); return; }
    savedRevision = revision;
    journalTransactions.erase(journalTransactions.begin(), journalTransactions.upper_bound(revision));
    dirty = checkpointRequired || savedRevision < project->editRevision;
    if (!dirty) error.clear();
    // Registry/timebase durability still requires a checkpoint; a pure edit can be saved by its journal.
    notify();
}
juce::Result RecorderDocument::placeRecordedTake(Take take, std::vector<MediaAsset> assets, const std::vector<int>& microphones)
{
    assertOwner();
    if (microphones.size() != take.microphoneAssetIds.size() || !placementMicrophones.empty())
        return fail("Invalid recorded microphone placement");
    std::vector<int> seen;
    for (auto mic : microphones)
    {
        if (mic < 0 || mic >= 8 || std::find(seen.begin(), seen.end(), mic) != seen.end()) return fail("Invalid logical microphone lane");
        seen.push_back(mic);
    }
    const juce::ScopedValueSetter<std::vector<int>> mapping(placementMicrophones, microphones);
    return placeNewTake(std::move(take), std::move(assets), EditOrigin::coordinator);
}
}
