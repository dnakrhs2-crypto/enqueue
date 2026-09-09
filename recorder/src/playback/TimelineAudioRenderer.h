#pragma once
#include "media/MediaIndex.h"
#include "playback/RenderPlanCompiler.h"
#include "playback/PlaybackBlockQueue.h"
#include <thread>
#include <mutex>

namespace gocue::recorder
{
struct CachedImportedAudio;
struct AudioSourceMask
{
    enum class Kind { listeningMix, microphoneMix, microphone, completedAudio, materialTrack };
    Kind kind = Kind::listeningMix;
    Id trackId, assetId;
};
// Reader state is separate from the immutable plan. Coordinates are always at
// project Fs, including resampled imports. Calls are worker/offline only.
struct PlaybackAudioSource : IndexedSource
{
    std::uint32_t sampleRate = 0;
    int channels = 1;
    Sample length = 0;
    virtual ~PlaybackAudioSource() = default;
    virtual void read(Sample first, unsigned frames, float* left, float* right) const = 0;
};
struct AudioSourceBinding { Id assetId; std::shared_ptr<const PlaybackAudioSource> source; };
struct ImportedAudioBinding { Id assetId; Sample mediaGeneration = 0; const CachedImportedAudio* cache = nullptr; };
// Snapshots paths/chunks and verifies project-Fs PCM; never changes originals.
std::vector<AudioSourceBinding> openAudioSources(const AudioRenderPlan&, const juce::File& projectDirectory,
                                               const std::vector<ImportedAudioBinding>& imports = {},
                                               const AudioSourceMask* selection = nullptr); // null prepares all masks
std::shared_ptr<const PlaybackAudioSource> wavAudioSource(std::shared_ptr<const WavSource>, bool allowGaps = false);
std::shared_ptr<const PlaybackAudioSource> importedAudioSource(const MediaAsset&, const CachedImportedAudio&,
                                                             std::shared_ptr<MediaEpoch> = {});

struct PlaybackAudioClip { RenderClip mapping; std::shared_ptr<const WavSource> source; };
struct PlaybackAudioTrack
{
    Id trackId;
    bool mute = false, solo = false;
    std::vector<PlaybackAudioClip> clips;
};
class TimelineAudioRenderer
{
public:
    TimelineAudioRenderer(std::uint32_t Fs, std::uint32_t blockFrames);
    ~TimelineAudioRenderer();
    // Legacy precompiled round-11 mappings retain their explicitly supplied fades.
    void setPlan(std::vector<PlaybackAudioTrack>, Sample timelineEnd);
    void setPlan(std::shared_ptr<const CompiledRenderPlan>, std::vector<AudioSourceBinding>, AudioSourceMask = {});
    void prepare(Sample, std::uint64_t generation); // >=250ms, quiescent callback
    // Same transport seek generation; edit revision is independently fenced.
    // Caller chooses a future callback boundary and pumps TimelineTransport::service
    // to recover buffering if preparation misses it. No model/GUI access here.
    void replacePlan(std::shared_ptr<const CompiledRenderPlan>, std::vector<AudioSourceBinding>,
                     Sample blockBoundary, std::uint64_t generation, AudioSourceMask = {});
    void stopWorker();
    bool ready() const noexcept;
    juce::Result status() const;
    Sample length() const noexcept { return plan ? plan->timelineEnd : 0; }
    PlaybackPcmQueue& queue() noexcept { return pcmQueue; }
    void renderAudio(Sample first, std::uint32_t frames, float* left, float* right) const;
    // Reusable by export. Explicit range may extend past timeline end (silence pad).
    // Fades are in absolute timeline coordinates, so arbitrary block sizes are identical.
    void renderAudio(const CompiledRenderPlan&, SampleRange, const AudioSourceMask&, float*, float*) const;
private:
    void startWorker(Sample, std::uint64_t, bool replacement);
    void validate(const CompiledRenderPlan&, const std::vector<AudioSourceBinding>&, const AudioSourceMask&) const;
    const std::uint32_t rate;
    PlaybackPcmQueue pcmQueue;
    std::shared_ptr<const CompiledRenderPlan> plan;
    std::vector<AudioSourceBinding> sources;
    AudioSourceMask mask;
    std::thread worker;
    std::atomic<bool> stopping{false};
    mutable std::mutex errorMutex;
    juce::String error;
};
}
