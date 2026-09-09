#pragma once
#include "app/RecorderDocument.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <functional>

namespace gocue::recorder
{
class FileIoFaultAdapter;

// Worker-owned operations only. The UI/record controller may update these atomics.
struct AudioImportControl
{
    enum class Stage { idle, copying, verifying, cache, pausedForRecording, ready, failed, cancelled };
    std::atomic<bool> cancelled{false}, recordingActive{false};
    std::atomic<double> progress{0};
    std::atomic<Stage> stage{Stage::idle};
    // Optional worker callback; install before starting work. Never access UI/document here.
    std::function<void(Stage, double)> onProgress;
    void checkpoint(Stage, double fraction); // throws on cancellation; yields during recording
};

struct ImportedAudioInfo
{
    juce::String codec, reader, contentHash, primingEvidence;
    std::uint32_t sampleRate = 0;
    int channels = 0, bitsPerSample = 0;
    Sample readerReportedSamples = 0;
    Sample rawDecodedSamples = 0; // full validation decoder output, before skip/padding
    Sample readerDecodedSamples = 0; // actually counted JUCE/MF PCM, before presentation tail trim
    Sample readerPrimingDiscardedSamples = 0; // MF duplicate timestamp buffers, independently counted
    Sample decodedSamples = 0;    // valid presentation samples at original Fs
    Sample leadingSkipSamples = 0, trailingSkipSamples = 0;
    Sample readerStartSample = 0; // JUCE MP3 needs skip; MF handles timestamp priming itself
    bool primingKnown = false;
    juce::var toVar() const;
    static ImportedAudioInfo fromVar(const juce::var&);
};

struct AudioImportRequest
{
    juce::File source, projectDirectory;
    Id projectId;
    std::uint32_t projectFs = 48000;
    Sample playhead = 0; // captured when the user chooses import, never append-to-end
    FileIoFaultAdapter* copyFaults = nullptr; // optional bounded I/O fault injection
};

// Owns the copied directory until commit. Destruction after any failure/cancel removes it.
// Keep on its worker until fully prepared; transfer to the document owner to commit.
class PreparedAudioImport
{
public:
    ~PreparedAudioImport();
    const MediaAsset& asset() const { return mediaAsset; }
    const Clip& clip() const { return importedClip; }
    const Track& track() const { return importedTrack; }
    const ImportedAudioInfo& info() const { return sourceInfo; }
    const juce::File& originalFile() const { return copiedFile; }
    const juce::File& projectDirectory() const { return request.projectDirectory; }
private:
    PreparedAudioImport() = default;
    AudioImportRequest request;
    ImportedAudioInfo sourceInfo;
    MediaAsset mediaAsset;
    Clip importedClip;
    Track importedTrack;
    juce::File ownedDirectory, copiedFile;
    bool committed = false;
    friend class AudioImport;
    friend juce::Result commitImportedAudio(RecorderDocument&, PreparedAudioImport&, AudioImportControl&);
};

class AudioImport
{
public:
    // Synchronous worker operations. Result is cleared on failure; no document is touched.
    static juce::Result prepare(const AudioImportRequest&, AudioImportControl&,
                                std::unique_ptr<PreparedAudioImport>&);
    static std::unique_ptr<juce::AudioFormatReader> openReader(const juce::File&);
    static juce::Result inspect(const juce::File&, AudioImportControl&, ImportedAudioInfo&);
    static juce::String hashFile(const juce::File&, AudioImportControl&,
                                 AudioImportControl::Stage = AudioImportControl::Stage::verifying);
    static ImportedAudioInfo loadInfo(const juce::File& projectDirectory, const MediaAsset&);
};

// Owner-thread only. Exactly one publication/revision; registry remains append-only on undo.
// Coupled to round 08: performEdit passes the EditState base of a RecorderProject working copy.
// Keep this bridge here until RecorderDocument exposes a general registry+edit transaction.
juce::Result commitImportedAudio(RecorderDocument&, PreparedAudioImport&, AudioImportControl&);
}
