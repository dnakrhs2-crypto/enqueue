#pragma once
#include "model/RecorderModel.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <map>
#include <memory>

namespace gocue::recorder
{
struct ThumbnailFrame { std::int64_t sample = 0, endSample = 0; int width = 160, height = 90; std::vector<std::uint8_t> rgb; };
// Derived work only. Bounded, deduplicated and suspended during capture.
// A job receives a cooperative checkpoint: wait during recording, true on shutdown.
class ThumbnailCache
{
public:
    using Job = std::function<void(const std::function<bool()>&)>;
    using Decoder = std::function<std::vector<ThumbnailFrame>(const juce::File&, unsigned,
        const std::vector<std::int64_t>&, const std::function<bool()>&)>;
    static constexpr std::size_t maximumPending = 32;
    static constexpr std::size_t maximumBytes = 16 * 1024 * 1024;
    explicit ThumbnailCache(Decoder = {});
    ~ThumbnailCache();
    bool enqueue(const juce::String& key, Job);
    void setRecording(bool);
    bool mayRun() const;
    std::size_t pending() const;
    // Key includes project/asset/media generation. Each point publishes separately;
    // caller requests only visible strip points, with drag target given priority.
    bool request(const juce::String& key, const juce::File&, unsigned Fs, std::int64_t sample, bool priority = false);
    std::shared_ptr<const ThumbnailFrame> nearest(const juce::String& key, std::int64_t sample);
    // Exact containment for the timeline/scrub view. A nearby cached image is
    // not evidence of the requested frame; show pending until it is available.
    std::shared_ptr<const ThumbnailFrame> at(const juce::String& key, Sample sample);
    static Sample frameSample(Sample, unsigned Fs, FrameRate);
    void invalidate(); // rejects queued/in-flight old-generation results
    struct Stats { std::size_t bytes = 0, peakBytes = 0, frames = 0, pending = 0; std::uint64_t hits = 0, misses = 0, evictions = 0, stale = 0; };
    Stats stats() const;
    static std::vector<ThumbnailFrame> decode(const juce::File&, unsigned Fs, const std::function<bool()>& yield);
    static std::vector<ThumbnailFrame> decodePoints(const juce::File&, unsigned Fs,
        const std::vector<std::int64_t>& samples, const std::function<bool()>& yield);
private:
    void run();
    struct Request { juce::String key; Job job; };
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Request> queue;
    std::set<juce::String> keys;
    bool recording = false, stopping = false;
    struct Cached { juce::String sourceKey; std::int64_t requested; std::shared_ptr<const ThumbnailFrame> frame; std::uint64_t used; };
    std::map<juce::String, Cached> cache;
    Stats counters;
    std::uint64_t generation = 1, access = 0;
    Decoder decoder;
    std::thread worker;
};
}
