#pragma once
#include "VideoSurfacePool.h"
#include <memory>

namespace gocue::recorder
{
class PreviewPresenter
{
public:
    PreviewPresenter(HWND, VideoSurfacePool&, std::shared_ptr<CaptureTelemetry>, std::uint32_t width, std::uint32_t height);
    ~PreviewPresenter();
    void start();
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
