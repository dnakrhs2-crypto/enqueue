#pragma once
#include <juce_core/juce_core.h>

namespace gocue::recorder
{
// Unhandled exceptions / std::terminate: write <settings>/crash/Recorder-<version>-<time>.dmp (+ .txt with the stack)
// so a crash on the CEO's PC leaves evidence that can be sent back. Nothing is uploaded.
struct CrashHandler
{
    static void install();
    static juce::File directory();
    static juce::File latestUnseenReport(); // newest *.txt not yet shown at startup; empty when none
    static juce::File markSeen(const juce::File& report); // renames to *.seen.txt; returns the file to reveal
};
}
