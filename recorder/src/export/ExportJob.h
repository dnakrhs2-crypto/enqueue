#pragma once
#include "playback/TimelineAudioRenderer.h"
#include "storage/DurableFile.h"
#include <functional>
#include <optional>

namespace gocue::recorder
{
class RecorderDocument;
struct ExportRange
{
    SampleRange requested;
    Sample firstFrame = 0, frameCount = 0, startSample = 0, sampleCount = 0;
    Sample endFrame() const { return firstFrame + frameCount; }
    // Display endpoints retain the rational video grid, independent of PCM rounding.
    double startSeconds(std::uint32_t Fs, FrameRate) const;
    double endSeconds(std::uint32_t Fs, FrameRate) const;
    static ExportRange expand(SampleRange, std::uint32_t Fs, FrameRate);
    juce::var toJson(std::uint32_t Fs, FrameRate) const;
};

// The round-23 coordinator must share this gate with recording. Acquiring either
// activity is atomic; checking a bool and subsequently starting is insufficient.
class ExportActivity
{
public:
    enum class State { idle, recording, exporting };
    bool beginRecording();
    void endRecording();
    State current() const { return state.load(std::memory_order_acquire); }
    class Lease
    {
    public:
        explicit Lease(ExportActivity&);
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
    private:
        ExportActivity& activity;
    };
private:
    std::atomic<State> state{State::idle};
};
struct ExportProgress
{
    juce::String stage;
    double fraction = 0, elapsedSeconds = 0;
    std::optional<double> etaSeconds; // measured estimate, never a throughput promise
};
struct ExportControl
{
    explicit ExportControl(ExportActivity& gate) : activity(gate) {}
    ExportActivity& activity;
    std::atomic<bool> cancelled{false};
    std::function<void(const ExportProgress&)> onProgress; // worker only, install before run
    void checkpoint() const;
};
class ExportCancelled : public std::runtime_error
{
public:
    ExportCancelled() : std::runtime_error("Export cancelled") {}
};

class ExportJob
{
public:
    static constexpr bool mayOverlapRecording = false;
    // No I/O/handles in the snapshot. Existing output directories resolve to a new
    // UUID child; existing completed files are never replaced.
    ExportJob(const RecorderProject&, juce::File projectDirectory, juce::File outDirectory = {},
              std::optional<SampleRange> = {}, bool recordingActive = false);
    static ExportJob fromDocument(const RecorderDocument&, juce::File outDirectory = {},
                                  std::optional<SampleRange> = {});
    const Id jobId = newId();
    const RecorderProject snapshot;
    const std::shared_ptr<const AudioRenderPlan> audioPlan;
    const ExportRange range;
    const juce::File projectDirectory, outputDirectory, partialDirectory;
    const CompiledRenderPlan& plan() const { return *audioPlan->timeline; }
    juce::var manifest(const juce::Array<juce::var>& files) const;
};

// Whole-job commit: all verified *.partial files -> durable manifest -> rename
// inside owned *.partial directory -> atomic directory publish, without replace.
// Cancellation before commit removes only this owned partial directory. Once
// commit starts, cancellation is deferred until the publication is complete.
class ExportPublication
{
public:
    explicit ExportPublication(const ExportJob&);
    ~ExportPublication();
    ExportPublication(const ExportPublication&) = delete;
    juce::File file(const juce::String& finalName) const;
    void commit(const juce::Array<juce::var>& verifiedFiles, ExportControl&, FileIoFaultAdapter* = nullptr);
private:
    const ExportJob& job;
    bool owns = false, published = false;
};
void exportCheck(const juce::Result&);
void exportRequire(bool, const char*);
void exportRename(const juce::File& from, const juce::File& to);
}
