#pragma once
#include <juce_core/juce_core.h>
#include <exception>
#include <functional>

namespace gocue::recorder
{
// Unhandled exceptions / std::terminate: write <settings>/crash/Recorder-<version>-<time>.dmp (+ .txt with the stack)
// so a crash on the CEO's PC leaves evidence that can be sent back. Nothing is uploaded.
struct CrashHandler
{
    static void install();
    static juce::File directory();
    static juce::File latestUnseenReport(); // newest *.txt not yet shown at startup; empty when none
    static void markSeen(const juce::File& report); // writes <report>.seen next to it; the report and its .dmp keep their names
    // Empty on a reporting failure. The optional directory keeps device-free tests isolated.
    static juce::File writeExceptionReport(const juce::String& what, const juce::String& file, int line,
                                           const juce::File& reportDirectory = {}) noexcept;
    static void handleException(const std::exception*, const juce::String& file, int line,
                                const std::function<void(const juce::File&)>& notify,
                                const juce::File& reportDirectory = {}) noexcept;
};
}
