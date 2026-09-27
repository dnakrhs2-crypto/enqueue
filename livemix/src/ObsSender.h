#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>

struct lm_obs_ring_header;
struct lm_obs_readers;

namespace gocue::livemix
{
/** One audio producer. Mappings are opened only on the message thread, at the first enable, and retained until
    destruction (after the engine has joined its callback). The optional base name isolates tests from LiveMix. */
class ObsSender
{
public:
    explicit ObsSender (const juce::String& mappingName = {});
    ~ObsSender();

    void setEnabled (bool enabled); // message thread; may create mappings or wait for an in-flight copy
    bool isEnabled() const noexcept { return enabled.load (std::memory_order_acquire); }
    void deviceStarted (double rate); // message/device lifecycle thread; never creates a mapping
    void write (const float* left, const float* right, int frames) noexcept;
    void writeSilence (int frames) noexcept;

    enum class ReaderState { none, idle, connected };
    ReaderState readerState() const; // message thread; at most 16 process liveness checks
    /** Message thread: optional process-identity check for isolated test mappings. Empty restores the OS query. */
    void setReaderProcessCheck (std::function<bool (juce::uint32)> check) { readerProcessCheck = std::move (check); }
    juce::String getError() const; // control/UI only

private:
    bool openMappings();
    void stopWrites(); // owner only: closes the gate, then waits for the current copy
    void resetEpoch (lm_obs_ring_header& header);
    void writeBlock (const float* left, const float* right, int frames) noexcept;

    const juce::String ringName, readersName;
    void* ringHandle = nullptr;
    void* readersHandle = nullptr;
    lm_obs_readers* readers = nullptr;
    std::atomic<lm_obs_ring_header*> published { nullptr };
    std::atomic<bool> enabled { false };
    // Bit 0: one copy in flight; bit 1: owner resetting or disabled. Audio makes ONE CAS attempt, never waits.
    std::atomic<unsigned> writerState { 2 };
    mutable juce::CriticalSection ownerLock; // control methods only, never the audio callback
    juce::String lastError;
    double sampleRate = 48000.0;
    int64_t frequency = 0;
    std::function<bool (juce::uint32)> readerProcessCheck;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ObsSender)
};
}
