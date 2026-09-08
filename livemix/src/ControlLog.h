#pragma once

#include <juce_core/juce_core.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace gocue::livemix
{

/** Bounded background log, never called by the audio thread. Producers only copy short diagnostic values;
    the worker does all disk work without holding the queue mutex. No global JUCE logger is installed. */
class ControlLog
{
public:
    static constexpr juce::int64 maxFileBytes = 2 * 1024 * 1024;
    static constexpr int failureIntervalMs = 30000;

    /** Empty -> %APPDATA%/LiveMix/logs. Keeps control.log, control.1.log and control.2.log. */
    explicit ControlLog (juce::File directory = {}, std::function<double()> clock = juce::Time::getMillisecondCounterHiRes);
    ~ControlLog();
    /** Only fixed event/code strings, app/protocol versions, ports and counts belong here. Never user data. */
    void info (const juce::String& event);
    void failure (const juce::String& code);

private:
    struct Failure { uint64_t count = 0; double nextWrite = 0; };
    void run();
    bool write (const juce::String& line);

    juce::File directory;
    std::function<double()> now;   // monotonic milliseconds; tests may advance a thread-safe clock
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<juce::String> pending;
    std::map<juce::String, Failure> failures;
    bool stopping = false;
    std::thread worker;
};

} // namespace gocue::livemix
