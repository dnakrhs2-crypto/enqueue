#pragma once
#include "RecorderModel.h"

namespace gocue::recorder
{
class FileIoFaultAdapter;
struct CheckpointInfo
{
    Sample checkpointRevision = 0;
    bool usedBackup = false;
    juce::String recoveryMessage;
    Sample generation = 0;
    juce::String journalPath = "journal";
    unsigned journalSegment = 1;
    std::uint64_t journalSequence = 0;
    juce::File sourceFile; // read/write provenance, not serialized into the project
};
class RecorderSerializer
{
public:
    // Canonical UTF-8 JSON; every 64-bit sample remains an integer, never a double.
    static juce::String toJson(const RecorderProject&);
    static juce::String toJson(const RecorderProject&, const CheckpointInfo&);
    static juce::Result fromJson(const juce::String&, RecorderProject&, CheckpointInfo* = nullptr);
    // Synchronous worker-side entry points. Never read or modify media payloads.
    static juce::Result readCheckpoint(const juce::File&, RecorderProject&, CheckpointInfo* = nullptr, bool allowBackup = true);
    static juce::Result writeCheckpoint(const juce::File&, const RecorderProject&);
    static juce::Result writeCheckpoint(const juce::File&, const RecorderProject&, CheckpointInfo& written,
                                        FileIoFaultAdapter* = nullptr, const std::function<void(const char*)>& = {});
    static juce::var editStateToVar(const EditState&);
    static juce::String fingerprint(const juce::var&); // FNV-1a-64 corruption check, not authentication
};
}
