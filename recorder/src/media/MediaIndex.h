#pragma once
#include "model/RecorderModel.h"
#include <atomic>

namespace gocue::recorder
{
// File identity is independent of a transport seek generation. Invalidate before
// replacing a file; old readers retain their storage but may no longer publish.
struct MediaEpoch { std::atomic<std::uint64_t> value{1}; };
struct IndexedSource
{
    std::shared_ptr<MediaEpoch> epoch;
    std::uint64_t generation = 1;
    bool current() const noexcept { return epoch && epoch->value.load(std::memory_order_acquire) == generation; }
};
struct VideoPacket
{
    std::int64_t pts = 0, dts = 0, offset = 0, duration = 0;
    int bytes = 0;
    bool keyframe = false, idr = false;
    Sample sample = 0, endSample = 0;
};
struct VideoIndex : IndexedSource
{
    juce::File file;
    std::uint32_t sampleRate = 48000;
    int stream = 0, timeBaseNum = 1, timeBaseDen = 60, width = 0, height = 0;
    std::int64_t startPts = 0;
    Sample length = 0;
    std::vector<VideoPacket> packets;
    std::vector<std::size_t> idrs;
    // Sample containment (floor), never nearest-frame rounding across a cut.
    std::size_t frameAt(Sample) const;
    std::size_t previousIdr(Sample) const;
    std::pair<std::size_t, std::size_t> gopAt(Sample) const; // packet interval [IDR, next IDR)
    void validateAndBuild(); // also used by device-free synthetic indexes
};
struct WavChunk
{
    juce::File file;
    Sample firstSample = 0, validSamples = 0;
    std::uint64_t dataOffset = 44, validBytes = 0;
};
struct WavSource : IndexedSource
{
    Id trackId;
    std::uint32_t sampleRate = 48000;
    unsigned channels = 1;
    Sample length = 0;
    std::vector<WavChunk> chunks; // gaps are silence; chunk boundaries are hidden
};
class MediaIndex
{
public:
    std::uint64_t generation() const noexcept { return epoch->value.load(std::memory_order_acquire); }
    void invalidate() noexcept { epoch->value.fetch_add(1, std::memory_order_acq_rel); }
    // Worker only. Scans final MP4 packets once; does not read a growing file or AAC.
    std::shared_ptr<const VideoIndex> openVideo(const juce::File&, std::uint32_t Fs) const;
    // Schema-1 fixture/standalone manifest: state="complete", sampleRate,
    // audioTracks:[{trackId, chunks:[{path,firstSample,validSamples,validBytes}]}].
    // Paths are relative to mediaDirectory. No unspecified take.json schema guessed.
    std::vector<std::shared_ptr<const WavSource>> openWavManifest(const juce::File& mediaDirectory) const;
    // Round-06 committed journal; only a TakeFinalized source can be opened here.
    std::vector<std::shared_ptr<const WavSource>> openWavJournal(const juce::File& projectDirectory,
                                                               const juce::Uuid& take) const;
    static void validateWav(WavSource&); // validates PCM24 mono/stereo header and durable limits
    // Finalized/recovered microphone assets; shared by app playback and export.
    // Empty availability is logical silence; a single-file fallback needs a durable prefix.
    static std::shared_ptr<const WavSource> recordedAudio(const MediaAsset&, const juce::File& folder,
                                                         unsigned Fs, const Id& track);
private:
    std::shared_ptr<MediaEpoch> epoch = std::make_shared<MediaEpoch>();
};
}
