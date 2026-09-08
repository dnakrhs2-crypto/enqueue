#pragma once

#include <juce_core/juce_core.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace gocue::livemix
{

/** Owns only status values, never a document. All directory, JSON and file operations run on its worker.
    An empty directory selects %APPDATA%/LiveMix/control; an injected directory is used verbatim. No ACL edits. */
class ControlDiscovery
{
public:
    enum class State { starting, ready, disabled, error, stopped };
    struct Status
    {
        juce::String appVersion;
        juce::Uuid instanceId;
        State state = State::disabled;
        int port = 0;
        juce::String token, errorCode;
        uint64_t generation = 0;   // callback routing only, never written to discovery
    };
    using Published = std::function<void (const Status&, bool success)>; // called on the worker

    ControlDiscovery (juce::File directory, Status initial, Published);
    ~ControlDiscovery();
    void publish (Status);        // latest immutable value wins; heartbeat continues even when disabled
    void stop (Status tombstone);  // writes the final value, then joins; no thread kill

    /** 32 bytes from the OS CSPRNG, encoded as unpadded base64url. Empty means entropy acquisition failed. */
    static juce::String createToken();
    static bool tokensEqual (const juce::String& expected, const juce::String& supplied) noexcept;

private:
    void run();
    bool write (const Status&);

    juce::File directory;
    Published published;
    std::mutex mutex;
    std::condition_variable wake;
    Status latest;
    bool dirty = true, stopping = false;
    juce::int64 heartbeat = 0;
    std::thread worker;
};

} // namespace gocue::livemix
