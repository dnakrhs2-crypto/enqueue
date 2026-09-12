#pragma once
#include <future>
#include "VideoSurfacePool.h"
#include <memory>

namespace gocue::recorder
{
class PreviewPresenter
{
public:
    // Additive round-11 view, implemented in playback/VideoPlaybackEngine.cpp.
    // Reuses a live view's HWND after that live presenter has stopped.
    class PlaybackView;
    PreviewPresenter(HWND, VideoSurfacePool&, std::shared_ptr<CaptureTelemetry>, std::uint32_t width, std::uint32_t height);
    ~PreviewPresenter();
    void start(std::shared_future<void> measurementStart = {});
    void stop();
    bool finished() const noexcept;
    // Read after stop only.
    const std::string& error() const noexcept;
    juce::var adapterJson() const;
private:
    struct State;
    std::unique_ptr<State> state;
};
}
