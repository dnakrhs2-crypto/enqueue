#pragma once
#include "VideoSurfacePool.h"
#include "GpuColourConverter.h"
#include <memory>

namespace gocue::recorder
{
enum class CaptureDecodeOrigin { native, mfMjpeg };
class CaptureFrameDecoder
{
public:
    CaptureFrameDecoder(const CameraMode& actualOutput, int mjpegThreads, ColourDevice = ColourDevice::hardware,
                        CaptureDecodeOrigin = CaptureDecodeOrigin::native);
    ~CaptureFrameDecoder();
    CaptureFrameDecoder(const CaptureFrameDecoder&) = delete;
    CaptureFrameDecoder& operator=(const CaptureFrameDecoder&) = delete;
    // Worker copies out of driver storage and releases IMFSample before CPU decode.
    void copySample(IMFSample*);
    void decodeCopied(VideoSurface&, FrameStamp&);
    // Same production decoder/normalisation for deterministic memory fixtures.
    void decodeBytes(const std::uint8_t*, size_t bytes, VideoSurface&, FrameStamp&);
    void flush() noexcept;
    void updateOutputMode(const CameraMode&); // same signal, refreshed stride/metadata
    int effectiveThreads() const noexcept;
    const std::string& colourDecision() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state;
};
}
