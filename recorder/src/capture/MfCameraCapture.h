#pragma once
#include "video/CaptureFrameDecoder.h"
#include <thread>
#include <functional>
#include <future>

namespace gocue::recorder
{
struct CaptureOpenInfo
{
    CameraMode nativeMode, outputMode;
    bool mfDecoder = false;
    int decoderThreads = 0;
    std::string decoderName;
};
class MfCameraCapture
{
public:
    MfCameraCapture(std::shared_ptr<CaptureTelemetry>, VideoSurfacePool&, std::function<void(const VideoSurface&)> recordSink = {});
    ~MfCameraCapture();
    CaptureOpenInfo start(const std::string& symbolicLink, CameraMode, bool mfMjpegDecoder, int decoderThreads = 1,
                          std::shared_future<void> measurementStart = {});
    void stop();
    bool finished() const noexcept;
    // After stop only.
    const std::string& error() const noexcept;
    const std::string& colourDecision() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state;
};
}
