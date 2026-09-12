#pragma once
#include "EditHistory.h"
#include "../model/RecorderSerializer.h"
#include "../model/ClipEdits.h"
#include "../model/RenderPlanCompiler.h"
#include <functional>
#include <map>
#include <thread>

namespace gocue::recorder
{
struct EntityDelta
{
    juce::String collection;
    Id entityId;
    bool removed = false;
    juce::var value; // complete resulting entity; tracks include their clip list
};
// Entity-level difference between two projects (name, tracks, markers, link groups, take stacks); shared by edits and
// the journal's registry record, which carries the markers a time-base change rescaled.
struct EditDelta;
EditDelta deltaFor(const RecorderProject& before, const RecorderProject& after, const juce::String& name);
struct EditDelta
{
    Id transactionId = newId(), projectId;
    Sample baseRevision = 0, revision = 0;
    juce::String name, validationHash;
    std::vector<EntityDelta> entities;
};
class IEditJournalSink
{
public:
    virtual ~IEditJournalSink() = default;
    // Must only enqueue a copy; no disk I/O here. Completion returns on the document owner thread.
    // Queue acceptance does NOT mean durable storage. Call acknowledgeJournal after append + flush.
    virtual juce::Result enqueue(const EditDelta&) = 0;
    virtual juce::Result enqueue(const EditDelta& delta, std::shared_ptr<const RecorderProject>) { return enqueue(delta); }
    virtual juce::Result enqueueRegistry(std::shared_ptr<const RecorderProject>) { return juce::Result::ok(); }
    virtual bool ownsCheckpoint(const juce::File&) const { return false; }
    virtual juce::Result checkpointAndWait() { return juce::Result::fail("No checkpoint worker"); }
};
class IRenderPlanConsumer
{
public:
    virtual ~IRenderPlanConsumer() = default;
    // Owner thread, before publication. Accept a new immutable plan and prepare/queue
    // prefetch. Failure rejects the edit without changing history or the current plan.
    virtual juce::Result prepareRenderPlan(std::shared_ptr<const CompiledRenderPlan>) = 0;
    // Called only after model publication. TimelineTransport must swap the matching
    // prepared plan at a block boundary with a ramp. While prefetch is pending it must
    // pause/report preparing; it must never label old audio as the new revision.
    // No callbacks into the document, exceptions, I/O or blocking in this notification.
    virtual void publishPreparedPlan(const Id& projectId, Sample revision) noexcept = 0;
};
class RecorderDocument
{
public:
    using Snapshot = std::shared_ptr<const RecorderProject>;
    RecorderDocument();
    Snapshot snapshot() const { return project; }
    std::shared_ptr<const CompiledRenderPlan> renderPlanSnapshot() const { return renderPlan; }
    juce::Result setRenderPlanConsumer(IRenderPlanConsumer*); // caller-owned; detach before destruction
    const RecorderProject& getProject() const { return *project; }
    const EditHistory& getHistory() const { return history; }
    const juce::File& getFile() const { return file; }
    bool isDirty() const { return dirty; }
    Sample durableRevision() const { return savedRevision; }
    juce::String getStatusText() const;
    const juce::String& getError() const { return error; }
    const juce::String& getRecoveryMessage() const { return recovery; }
    void newProject(const juce::String& name, std::uint32_t Fs = 48000, FrameRate fps = {});
    // Complete the read on a worker, then adopt on the owner thread. No device selection/engine calls.
    juce::Result adopt(RecorderProject, const juce::File&, const CheckpointInfo&);
    juce::Result openCheckpoint(const juce::File&); // synchronous CLI/test convenience
    juce::Result saveCheckpoint(const juce::File&); // synchronous CLI/test convenience
    void checkpointFinished(Snapshot written, const juce::File&, const juce::Result&);
    juce::Result setTimebase(std::uint32_t Fs, FrameRate);
    // A project without media runs at the open device's rate: replaces Fs, rescales marker coordinates so their time is kept,
    // clears undo history (old-rate coordinates) and leaves the dirty state alone (the saved file re-adopts on the next open;
    // the first take fixes the rate for good).
    juce::Result adoptProvisionalTimebase(std::uint32_t Fs);
    juce::Result performEdit(const juce::String& name, const std::function<void(EditState&)>&, const EditOptions& = {});
    // Pure Project/ClipEdits adapter. A returned Project implicitly converts to ClipEditResult.
    juce::Result performEdit(const juce::String& name, const juce::String& coalesceKey,
                             const std::function<ClipEditResult(const RecorderProject&)>&,
                             const EditOptions& = {});
    // The only user edit allowed during capture; no arbitrary edit callback runs.
    juce::Result addMarker(Marker);
    juce::Result undo();
    juce::Result redo();
    void endGesture() { history.endGesture(); }
    void setSelection(std::vector<Id>);
    const std::vector<Id>& getSelection() const { return selection; }
    // Assets/takes persist through undo. Registration is atomic and creates no undo step.
    juce::Result registerMedia(std::vector<MediaAsset>, std::vector<Take> = {});
    juce::Result updateMediaAsset(MediaAsset); // higher generation, same source identity/format
    juce::Result updateTakeState(const Id&, TakeState); // finalisation/availability is outside undo
    juce::Result placeTake(Take, std::vector<MediaAsset>);
    juce::Result placeTake(const Id& registeredTakeId);
    void setJournalSink(IEditJournalSink* sink) { journal = sink; }
    void acknowledgeJournal(const EditDelta& ticket, const juce::Result&);
    // Called on the owner thread with exactly the immutable state whose media
    // registry was flushed. Finalization does not add an undo entry/revision.
    void acknowledgeJournalState(Snapshot, const juce::Result&);
    std::function<void()> onChanged;
    // Recorder coordinator lock; owner thread only. Registry finalization remains
    // available, while user timeline/timebase/document replacement is blocked.
    void setRecordingStructureLock(bool locked) { assertOwner(); recordingStructureLock = locked; }
    bool isRecordingStructureLocked() const { return recordingStructureLock; }
    // Coordinator-only placement keeps the structure lock held through publication.
    juce::Result placeRecordedTake(Take, std::vector<MediaAsset>, const std::vector<int>& logicalMicrophoneIndices,
                                  const std::function<void(EditState&)>& metadata = {});
    const Id& lastEditTransaction() const { return lastTransaction; }
    // Coordinator transaction: register captured originals and replace the complete
    // dubbing version in one publication/undo step, including while capture is locked.
    juce::Result placeDubbingTake(Take, std::vector<MediaAsset>, const std::vector<int>& microphoneLanes,
                                 SampleRange recordingRange, const Id& retakeStack = {});
    juce::Result useTakeVersion(const Id& stackId, const Id& versionId);
private:
    enum class EditOrigin { user, markerAppend, coordinator };
    void assertOwner() const;
    juce::Result fail(const juce::String&);
    void notify();
    juce::Result publishEdit(RecorderProject, const juce::String&, const EditOptions&, bool addHistory,
                             const std::vector<Id>& nextSelection, EditOrigin = EditOrigin::user);
    juce::Result placeNewTake(Take, std::vector<MediaAsset>, EditOrigin);
    juce::Result place(RecorderProject, const Take&, Sample placement, EditOrigin = EditOrigin::user);
    EditSnapshot editSnapshot() const;
    juce::Result preparePlan(const RecorderProject&, std::shared_ptr<const CompiledRenderPlan>&);
    juce::Result replaceProject(RecorderProject);
    Snapshot project;
    std::shared_ptr<const CompiledRenderPlan> renderPlan;
    EditHistory history;
    std::vector<Id> selection;
    juce::File file;
    juce::String error, recovery;
    bool dirty = true, checkpointRequired = true, editing = false;
    Sample savedRevision = 0;
    std::map<Sample, Id> journalTransactions;
    IEditJournalSink* journal = nullptr; // caller-owned; detach before destroying
    IRenderPlanConsumer* renderConsumer = nullptr;
    const std::thread::id owner = std::this_thread::get_id();
    bool recordingStructureLock = false;
    std::vector<int> placementMicrophones;
    std::function<void(EditState&)> placementMetadata;
    Id lastTransaction;
    void enqueueRegistry();
};
}
