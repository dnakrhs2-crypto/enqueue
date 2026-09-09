#pragma once
#include "MaterialExporter.h"
#include <mutex>
#include <thread>

namespace gocue::recorder
{
class ExportController
{
public:
    enum class Mode { materials, finalVideo };
    enum class State { idle, running, cancelling, completed, cancelled, failed };
    struct Request
    {
        Mode mode = Mode::materials;
        juce::File destination; // exact desired result folder; collisions get (2)
        std::optional<SampleRange> range;
        MaterialExportOptions materials;
        FinalExportSelection finalSource;
    };
    struct Status
    {
        State state = State::idle;
        Mode mode = Mode::materials;
        Id projectId;
        Sample editRevision = 0;
        ExportProgress progress;
        double estimatedEffectiveFps = 0;
        juce::String error, notice;
        juce::File outputDirectory;
        juce::var manifest;
    };
    using Runner = std::function<juce::var(const ExportJob&, const Request&, ExportControl&, FileIoFaultAdapter*)>;
    explicit ExportController(Runner = {}); // injection is for lifecycle tests only
    ~ExportController();
    ExportController(const ExportController&) = delete;
    ExportController& operator=(const ExportController&) = delete;
    static juce::File resolveDestination(const juce::File& desired);
    juce::Result start(const RecorderDocument&, Request, FileIoFaultAdapter* = nullptr);
    juce::Result start(const RecorderProject&, const juce::File& projectDirectory, Request,
                       bool recording = false, FileIoFaultAdapter* = nullptr);
    juce::Result retry(std::optional<juce::File> destination = {}, FileIoFaultAdapter* = nullptr);
    void cancel();
    bool busy() const noexcept { return active.load(std::memory_order_acquire); }
    Status status() const;
    // Owner thread polls after requesting cancellation. True is returned only
    // after the worker has released every export handle/lease, then recording is
    // atomically reserved. endRecording also handles a rejected recording start.
    bool beforeRecording();
    void endRecording();
    ExportActivity::State activityState() const { return activity.current(); }
    void wait(); // shutdown/test only; never used to block the GUI cancel button
private:
    Runner runner;
    ExportActivity activity;
    std::atomic<bool> active{false};
    std::shared_ptr<ExportControl> control;
    std::thread worker;
    mutable std::mutex mutex;
    Status current;
    std::shared_ptr<const RecorderProject> lastProject;
    juce::File lastDirectory;
    Request lastRequest;
};
}
