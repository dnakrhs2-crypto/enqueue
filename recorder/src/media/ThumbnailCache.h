#pragma once
#include <juce_core/juce_core.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <thread>

namespace gocue::recorder
{
struct ThumbnailFrame { std::int64_t sample = 0; int width = 160, height = 90; std::vector<std::uint8_t> rgb; };
// Derived work only. Bounded, deduplicated and suspended during capture.
// A job receives a cooperative checkpoint: wait during recording, true on shutdown.
class ThumbnailCache
{
public:
    using Job = std::function<void(const std::function<bool()>&)>;
    static constexpr std::size_t maximumPending = 32;
    ThumbnailCache();
    ~ThumbnailCache();
    bool enqueue(const juce::String& key, Job);
    void setRecording(bool);
    bool mayRun() const;
    std::size_t pending() const;
    static std::vector<ThumbnailFrame> decode(const juce::File&, unsigned Fs, const std::function<bool()>& yield);
private:
    void run();
    struct Request { juce::String key; Job job; };
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Request> queue;
    std::set<juce::String> keys;
    bool recording = false, stopping = false;
    std::thread worker;
};
}
