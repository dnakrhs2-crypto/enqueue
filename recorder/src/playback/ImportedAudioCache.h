#pragma once
#include "media/AudioImport.h"
#include "media/PeakCache.h"
#include <array>
#include <thread>

namespace gocue::recorder
{
struct ImportedPeak
{
    std::array<float, 2> minimum{}, maximum{};
};
struct CachedImportedAudio
{
    juce::File pcmFile, peaksFile;
    juce::String key;
    std::uint32_t sampleRate = 0;
    int channels = 0, samplesPerPeak = 256;
    Sample samples = 0;
    bool reused = false;
    // Project-Fs min/max bins. Round 09 PeakCache is absent in this branch;
    // its adapter may consume these bins or the verified PCM without changing the asset.
    std::vector<ImportedPeak> peaks;
};

class ImportedAudioCache
{
public:
    static juce::String keyFor(const ImportedAudioInfo&, std::uint32_t projectFs);
    // Rechecks the original SHA-256, validates cached bytes, and publishes a new generation
    // through an atomic manifest only after WAV and peaks are complete. Never opens a device.
    static juce::Result build(const juce::File& projectDirectory, const MediaAsset&,
                             const ImportedAudioInfo&, std::uint32_t projectFs,
                             AudioImportControl&, CachedImportedAudio&);
    static Sample sourceSampleFor(Sample projectSourceSample, const ImportedAudioInfo&, std::uint32_t projectFs);
    static PeakSnapshot peakSnapshot(const CachedImportedAudio&); // bounded L/R envelope for one timeline lane

    // One import/cache job, below-normal CPU and background I/O priority on Windows.
    // The document is deliberately excluded: take the prepared value on its owner thread.
    class Worker
    {
    public:
        explicit Worker(AudioImportRequest, bool recording = false);
        ~Worker();
        void cancel() { control.cancelled.store(true); }
        void setRecordingActive(bool active) { control.recordingActive.store(active); }
        bool finished() const { return done.load(std::memory_order_acquire); }
        AudioImportControl control;
        juce::Result takeResult(std::unique_ptr<PreparedAudioImport>&, CachedImportedAudio&);
    private:
        std::thread thread;
        std::atomic<bool> done{false};
        bool taken = false;
        juce::Result result = juce::Result::ok();
        std::unique_ptr<PreparedAudioImport> prepared;
        CachedImportedAudio cached;
    };
};
}
