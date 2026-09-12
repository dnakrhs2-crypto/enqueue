#pragma once
#include "video/CaptureFrameDecoder.h"

namespace gocue::recorder
{
// Worker-only recovery boundary. Record sinks and source errors stay outside it.
class CaptureDecodeRecovery
{
public:
    static constexpr unsigned failureLimit = 30;
    template<class Decode> bool frame(CaptureFrameDecoder& decoder, CaptureTelemetry& telemetry, Decode&& decode)
    {
        try { decode(); consecutive = 0; return true; }
        catch (const std::exception& e)
        {
            telemetry.loss(LossReason::decoderError);
            decoder.flush();
            if (++consecutive >= failureLimit)
                throw std::runtime_error(std::to_string(failureLimit) + " consecutive capture decode failures: " + e.what());
            return false;
        }
    }
    unsigned consecutiveFailures() const noexcept { return consecutive; }
private:
    unsigned consecutive = 0;
};
inline void requireSameCaptureSignal(const CameraMode& expected, const CameraMode& current)
{
    if (!current.sameSignal(expected))
        throw std::runtime_error("SourceReader changed subtype/size/rational FPS; capture requires re-prepare");
}
}
