#pragma once
#include "RecorderModel.h"

namespace gocue::recorder
{
struct CheckpointInfo
{
    Sample checkpointRevision = 0;
    bool usedBackup = false;
    juce::String recoveryMessage;
};
class RecorderSerializer
{
public:
    // Canonical UTF-8 JSON; every 64-bit sample remains an integer, never a double.
    static juce::String toJson(const RecorderProject&);
    static juce::Result fromJson(const juce::String&, RecorderProject&, CheckpointInfo* = nullptr);
    // Synchronous worker-side entry points. Never read or modify media payloads.
    static juce::Result readCheckpoint(const juce::File&, RecorderProject&, CheckpointInfo* = nullptr, bool allowBackup = true);
    static juce::Result writeCheckpoint(const juce::File&, const RecorderProject&);
    static juce::var editStateToVar(const EditState&);
    static juce::String fingerprint(const juce::var&); // FNV-1a-64 corruption check, not authentication
};
}
