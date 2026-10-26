#pragma once
#include "RecordingTypes.h"
#include <atomic>
#include <limits>
#include <memory>

namespace gocue::livemix::recording
{
class Recorder
{
public:
    // Explicit probe seams; production uses the default constructor and contract constants.
    struct TestHooks
    {
        std::uint64_t wavLimitBytes = wavSplitBytes;
        std::uint64_t failAfterBytes = std::numeric_limits<std::uint64_t>::max();
        const std::atomic<bool>* writerGate = nullptr;
        bool encoderUnavailable = false;
    };
    Recorder();
    explicit Recorder(TestHooks);
    ~Recorder();
    bool prepareFormat(const DeviceFormat&); // device callbacks MUST be stopped
    bool start(const StartRequest&);
    void requestStop() noexcept;
    void push(collab::contracts::AudioSourceView master, collab::contracts::AudioSourceView remote1,
              collab::contracts::AudioSourceView remote2, int n) noexcept;
    void noteSeatOccupant(unsigned slot, const TrackIdentity&);
    Snapshot snapshot() const;
    void finish(); // control thread: stop accepting, wait for callback, drain, close, join
    double writerCpuSeconds() const noexcept; // diagnostic: current/last take's writer thread CPU
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
