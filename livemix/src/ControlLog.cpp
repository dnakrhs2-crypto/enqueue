#include "ControlLog.h"

#include <chrono>

namespace gocue::livemix
{

ControlLog::ControlLog (juce::File folder, std::function<double()> clock)
    : directory (std::move (folder)), now (std::move (clock)), worker (&ControlLog::run, this) {}

ControlLog::~ControlLog()
{
    { std::lock_guard<std::mutex> lock (mutex); stopping = true; }
    wake.notify_one();
    if (worker.joinable()) worker.join();
}

void ControlLog::info (const juce::String& event)
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (stopping || pending.size() >= 256) return;   // disk stalls cannot grow the queue
        pending.push_back (event.substring (0, 256));
    }
    wake.notify_one();
}

void ControlLog::failure (const juce::String& code)
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (stopping || (failures.size() >= 64 && failures.find (code) == failures.end())) return;
        ++failures[code].count;   // count every identical failure, even when it arrives faster than the worker
    }
    wake.notify_one();
}

void ControlLog::run()
{
    if (directory == juce::File())
        directory = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("LiveMix/logs");

    double retryAt = 0;
    for (;;)
    {
        std::deque<juce::String> batch;
        bool final;
        {
            std::unique_lock<std::mutex> lock (mutex);
            // The periodic wake also emits a trailing failure count after its 30-second quiet window.
            wake.wait_for (lock, std::chrono::milliseconds (250));
            final = stopping;
            batch.swap (pending);
            const auto time = now();
            for (auto& entry : failures)
            {
                auto& failure = entry.second;
                if (failure.count == 0 || time < failure.nextWrite) continue;
                batch.push_back ("error code=" + entry.first + " count=" + juce::String ((juce::int64) failure.count));
                failure.count = 0;
                failure.nextWrite = time + failureIntervalMs;
            }
        }

        // A failed disk gets a quiet retry later; log loss never changes control/discovery/audio status.
        if (now() >= retryAt)
            for (const auto& event : batch)
            {
                bool success = false;
                try { success = write (juce::Time::getCurrentTime().toISO8601 (true) + " INFO " + event + "\n"); }
                catch (...) {}   // no exception text: it might contain a path
                if (! success)
                {
                    retryAt = now() + failureIntervalMs;
                    break;
                }
            }
        if (final) return;
    }
}

bool ControlLog::write (const juce::String& line)
{
    if (directory.createDirectory().failed()) return false;
    const auto file = directory.getChildFile ("control.log");
    if (file.getSize() + (juce::int64) line.getNumBytesAsUTF8() > maxFileBytes)
    {
        const auto previous = directory.getChildFile ("control.1.log"), oldest = directory.getChildFile ("control.2.log");
        if (! oldest.deleteFile()) return false;
        if (previous.existsAsFile() && ! previous.moveFileTo (oldest)) return false;
        if (! file.moveFileTo (previous)) return false;
    }
    juce::FileOutputStream output (file);
    if (! output.openedOk() || ! output.write (line.toRawUTF8(), line.getNumBytesAsUTF8())) return false;
    output.flush();
    return output.getStatus().wasOk();
}

} // namespace gocue::livemix
