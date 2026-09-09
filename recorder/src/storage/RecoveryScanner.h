#pragma once
#include "RecoverySupport.h"
#include "RecordingJournal.h"
#include "app/RecorderDocument.h"

namespace gocue::recorder
{
struct RecoveryReport
{
    RecorderProject project;
    juce::File checkpoint, attempt;
    Sample lastSavedEditRevision = 0;
    std::uint64_t changedTakes = 0, addedClips = 0, duplicateTransactions = 0;
    bool ignoredEditTail = false, ignoredTakeTail = false, usedBackup = false;
    juce::StringArray messages, warnings, orphans;
    juce::Array<juce::var> takes;
    juce::var toJson() const;
};
class RecoveryScanner
{
public:
    struct Options { FileIoFaultAdapter* faults = nullptr; recovery::Hook hook; };
    RecoveryScanner() = default;
    explicit RecoveryScanner(Options o) : options(std::move(o)) {}
    // Owns project and legacy take-writer locks through commit and optional
    // document adoption. No callback/RT use. Every output is CREATE_NEW.
    juce::Result run(const juce::File& projectRoot, RecoveryReport&, RecorderDocument* = nullptr);
    // Worker integration for round 08 IEditJournalSink. Caller owns project lock;
    // enqueue itself must remain asynchronous. result contains registered media.
    static void appendEdit(const juce::File& log, const EditDelta&, const RecorderProject& result,
                           FileIoFaultAdapter* = nullptr, const recovery::Hook& = {});
    static void writeCheckpoint(const juce::File&, const RecorderProject&,
                                FileIoFaultAdapter* = nullptr, const recovery::Hook& = {});
    static void writeTakeManifest(const juce::File& root, const RecorderProject&, const Id& take);
    static std::uint64_t wavSamples(std::uint64_t actualBytes, const JournalFilePosition&);
private:
    Options options;
};
}
