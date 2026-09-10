#pragma once
#include "capture/CameraCatalog.h"
#include <string>
#include <vector>

namespace gocue::recorder
{
enum class CalibrationQuality { unmeasured, softwareEstimated, physicalMeasured };
struct CalibrationKey
{
    std::string cameraId, nativeMode, exposure, asioDriver;
    Rational fps;
    std::uint32_t sampleRate = 0, bufferSamples = 0;
    std::vector<int> outputMapping; // zero-based physical outputs, ORDER is significant
    std::vector<int> inputMapping; // logical/L/R triples; absent in legacy profiles
    bool operator==(const CalibrationKey&) const noexcept;
};
struct CalibrationProfile
{
    static constexpr int schemaVersion = 1;
    CalibrationKey key;
    std::int64_t cameraResidualLatency100ns = 0;
    std::int64_t inputResidualLatencySamples = 0, outputResidualLatencySamples = 0;
    std::string measuredUtc, method;
    CalibrationQuality quality = CalibrationQuality::unmeasured;
    std::uint32_t measurementCount = 0;
    double residualRmsSamples = 0;
    // Residuals exclude device-reported latencies. Lcam > 0 moves the timestamp
    // earlier; output residual > 0 moves O0 later. Input residual is applied by
    // the input-coordinate adapter ONCE, never by projectInputSample().
    // Pure value/serialization only; no RecorderSettings or filesystem side effects.
    juce::var toJson() const;
    static CalibrationProfile fromJson(const juce::var&); // strict schema/types/ranges
    std::string serialize() const;
    static CalibrationProfile deserialize(const std::string&);
    // Full-key match is required before using any residual. Throws on mismatch.
    void requireMatch(const CalibrationKey&) const;
};
CalibrationKey calibrationKey(const std::string& cameraId, const CameraMode&, const std::string& exposure,
                              const std::string& asioDriver, unsigned Fs, unsigned buffer,
                              const std::vector<int>& outputMapping, const std::vector<int>& inputMapping = {});
}
