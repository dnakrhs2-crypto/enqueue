#pragma once
#include "EditHistory.h"
#include "../model/RecorderSerializer.h"
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
};
class RecorderDocument
{
public:
    using Snapshot = std::shared_ptr<const RecorderProject>;
    RecorderDocument();
    Snapshot snapshot() const { return project; }
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
    juce::Result performEdit(const juce::String& name, const std::function<void(EditState&)>&, const EditOptions& = {});
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
    std::function<void()> onChanged;
    // Recorder coordinator lock; owner thread only. Registry finalization remains
    // available, while timeline/timebase/document replacement is blocked.
    void setRecordingStructureLock(bool locked) { assertOwner(); recordingStructureLock = locked; }
    bool isRecordingStructureLocked() const { return recordingStructureLock; }
    juce::Result placeTake(Take, std::vector<MediaAsset>, const std::vector<int>& logicalMicrophoneIndices);
    const Id& lastEditTransaction() const { return lastTransaction; }
private:
    void assertOwner() const;
    juce::Result fail(const juce::String&);
    void notify();
    juce::Result publishEdit(RecorderProject, const juce::String&, const EditOptions&, bool addHistory, const std::vector<Id>& nextSelection);
    juce::Result place(RecorderProject, const Take&, Sample placement);
    EditSnapshot editSnapshot() const;
    Snapshot project;
    EditHistory history;
    std::vector<Id> selection;
    juce::File file;
    juce::String error, recovery;
    bool dirty = true, checkpointRequired = true, editing = false;
    Sample savedRevision = 0;
    std::map<Sample, Id> journalTransactions;
    IEditJournalSink* journal = nullptr; // caller-owned; detach before destroying
    const std::thread::id owner = std::this_thread::get_id();
    bool recordingStructureLock = false;
    std::vector<int> placementMicrophones;
    Id lastTransaction;
};
}
