#pragma once
#include "RecordingJournal.h"
#include "RecoverySupport.h"
#include "app/RecorderDocument.h"
#include <juce_events/juce_events.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>

namespace gocue::recorder
{
struct EditJournalReplay
{
    JournalReplay framing;
    std::set<Id> transactions;
    std::uint64_t appliedEdits = 0, registryCommits = 0;
    bool legacy = false;
};

// Synchronous storage owner. The document and RT callbacks never call these
// file operations. Legacy RCV1 logs are read by RecoveryScanner, never extended.
class EditJournal
{
public:
    struct Options { FileIoFaultAdapter* faults = nullptr; recovery::Hook hook; std::uint64_t rotationBytes = 8 * 1024 * 1024; };
    EditJournal();
    explicit EditJournal(Options);
    ~EditJournal();
    juce::Result open(const juce::File& root, const RecorderProject& durable, const CheckpointInfo& = {});
    juce::Result append(const EditDelta&, const RecorderProject&, JournalKind = JournalKind::EditTransaction);
    juce::Result appendRegistry(const RecorderProject&);
    juce::Result checkpoint();
    juce::Result close();
    const RecorderProject& durableProject() const { return current; }
    const CheckpointInfo& checkpointInfo() const { return cursor; }
    std::uint64_t durableSequence() const { return journal.durableSequence(); }
    std::uint64_t collectedSegments() const { return collected; }
    static juce::Result replay(const juce::File& root, RecorderProject&, const CheckpointInfo&, EditJournalReplay&);
    static juce::var payload(const RecorderProject& before, const EditDelta&, const RecorderProject& after);
    static RecorderProject apply(const RecorderProject&, const juce::var&, bool registryOnly = false);
private:
    Options options;
    RecordingJournal journal;
    std::unique_ptr<recovery::WriterLock> lock;
    juce::File root, directory;
    RecorderProject current;
    CheckpointInfo cursor;
    std::map<Sample, juce::String> committedGenerations;
    std::uint64_t collected = 0;
};

// One worker per document/project. attach/detach/drain are owner-thread calls.
// Timer dispatches flush acknowledgements on JUCE's message thread; CLI/tests
// explicitly drain. The host owns this object and shuts it down before document
// replacement/destruction. No callback captures a document on the worker.
class EditJournalWorker final : public IEditJournalSink, private juce::Timer
{
public:
    struct Options
    {
        EditJournal::Options journal;
        std::chrono::milliseconds checkpointInterval{10000};
        std::size_t maxPending = 256;
    };
    EditJournalWorker(const juce::File&, RecorderDocument::Snapshot, CheckpointInfo = {});
    EditJournalWorker(const juce::File&, RecorderDocument::Snapshot, CheckpointInfo, Options);
    ~EditJournalWorker() override;
    void attach(RecorderDocument&);
    void detach();
    juce::Result enqueue(const EditDelta&) override;
    juce::Result enqueue(const EditDelta&, RecorderDocument::Snapshot) override;
    juce::Result enqueueRegistry(RecorderDocument::Snapshot) override;
    juce::Result requestCheckpoint(); // explicit Save; returns queue acceptance
    bool ownsCheckpoint(const juce::File& file) const override { return file == root.getChildFile("project.recorder"); }
    juce::Result checkpointAndWait() override;
    juce::Result waitUntilIdle(std::chrono::milliseconds timeout = std::chrono::seconds(30));
    juce::Result shutdown(); // drains, checkpoints latest durable state, joins; errors are observable
    void drain();
private:
    struct Work { EditDelta delta; RecorderDocument::Snapshot state; bool registry = false, checkpoint = false; };
    struct Completion { Work work; juce::Result result; };
    juce::Result push(Work);
    void run();
    void timerCallback() override { drain(); }
    Options options;
    juce::File root;
    RecorderDocument::Snapshot initial;
    CheckpointInfo cursor;
    RecorderDocument* document = nullptr;
    const std::thread::id owner = std::this_thread::get_id();
    std::mutex mutex;
    std::condition_variable wake, idle;
    std::deque<Work> pending;
    std::deque<Completion> completions;
    bool stopping = false, busy = true, finished = false;
    juce::Result error = juce::Result::ok();
    std::thread worker;
};
}
