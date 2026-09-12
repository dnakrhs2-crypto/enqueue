#include "CalibrationProfile.h"
#include <charconv>
#include <cmath>
#include <set>
#include <stdexcept>

namespace gocue::recorder
{
namespace
{
const char* qualityName(CalibrationQuality q)
{
    switch (q) { case CalibrationQuality::unmeasured: return "unmeasured"; case CalibrationQuality::softwareEstimated: return "software-estimated";
        case CalibrationQuality::physicalMeasured: return "physical-measured"; default: throw std::invalid_argument("Invalid calibration quality"); }
}
void validate(const CalibrationProfile& p)
{
    const auto& k = p.key;
    if (k.cameraId.empty() || k.nativeMode.empty() || k.exposure.empty() || k.asioDriver.empty()
        || !k.fps.numerator || !k.fps.denominator || k.fps.numerator > 1000000 || k.fps.denominator > 1000000
        || k.sampleRate < 8000 || k.sampleRate > 768000 || !k.bufferSamples || k.bufferSamples > 262144
        || k.outputMapping.empty() || k.outputMapping.size() > 64 || !std::isfinite(p.residualRmsSamples) || p.residualRmsSamples < 0
        || std::abs(static_cast<double>(p.cameraResidualLatency100ns)) > 100000000
        || std::abs(static_cast<double>(p.inputResidualLatencySamples)) > 10.0 * k.sampleRate
        || std::abs(static_cast<double>(p.outputResidualLatencySamples)) > 10.0 * k.sampleRate)
        throw std::invalid_argument("Invalid calibration key/value");
    std::set<int> outputs;
    for (const auto c : k.outputMapping) if (c < 0 || c > 1023 || !outputs.insert(c).second) throw std::invalid_argument("Invalid calibration output mapping");
    if (k.inputMapping.size() > 24 || k.inputMapping.size() % 3) throw std::invalid_argument("Invalid calibration input mapping");
    std::set<int> inputs, slots;
    for (std::size_t i = 0; i < k.inputMapping.size(); i += 3)
    {
        const auto mic = k.inputMapping[i], left = k.inputMapping[i + 1], right = k.inputMapping[i + 2];
        if (mic < 1 || mic > 8 || !slots.insert(mic).second || left < 0 || left > 255 || !inputs.insert(left).second
            || (right != -1 && (right != left + 1 || right > 255 || !inputs.insert(right).second)))
            throw std::invalid_argument("Invalid calibration stereo input pair");
    }
    qualityName(p.quality);
    if (p.quality != CalibrationQuality::unmeasured
        && (p.measuredUtc.size() < 20 || p.measuredUtc[10] != 'T' || p.measuredUtc.back() != 'Z' || p.method.empty() || !p.measurementCount))
        throw std::invalid_argument("Measured calibration requires UTC date, method and observations");
}
const juce::var& field(const juce::var& v, const char* name)
{
    const auto* object = v.getDynamicObject();
    if (!object || !object->hasProperty(name)) throw std::invalid_argument(std::string("Missing calibration field: ") + name);
    return object->getProperty(name);
}
std::string stringField(const juce::var& v, const char* name)
{
    const auto& x = field(v, name);
    if (!x.isString()) throw std::invalid_argument(std::string("Calibration field must be string: ") + name);
    return x.toString().toStdString();
}
std::int64_t exactInteger(const juce::var& v)
{
    if (v.isInt() || v.isInt64()) return static_cast<juce::int64>(v);
    if (v.isString())
    {
        const auto text = v.toString().toStdString(); std::int64_t value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec == std::errc{} && result.ptr == text.data() + text.size()) return value;
    }
    throw std::invalid_argument("Calibration integer must be exact, not floating point");
}
std::uint32_t unsignedField(const juce::var& v, const char* name)
{
    const auto value = exactInteger(field(v, name));
    if (value < 0 || value > UINT32_MAX) throw std::invalid_argument("Calibration uint32 overflow");
    return static_cast<std::uint32_t>(value);
}
}
bool CalibrationKey::operator==(const CalibrationKey& b) const noexcept
{
    return cameraId == b.cameraId && nativeMode == b.nativeMode && fps == b.fps && exposure == b.exposure
        && asioDriver == b.asioDriver && sampleRate == b.sampleRate && bufferSamples == b.bufferSamples && outputMapping == b.outputMapping && inputMapping == b.inputMapping;
}
CalibrationKey calibrationKey(const std::string& cameraId, const CameraMode& mode, const std::string& exposure,
                              const std::string& asioDriver, unsigned Fs, unsigned buffer, const std::vector<int>& outputs, const std::vector<int>& inputs)
{ return {cameraId, mode.text(), exposure, asioDriver, mode.fps, Fs, buffer, outputs, inputs}; }
void CalibrationProfile::requireMatch(const CalibrationKey& expected) const
{
    validate(*this);
    if (!(key == expected)) throw std::invalid_argument("Calibration profile does not match camera/native mode/fps/exposure/ASIO/Fs/buffer/output mapping");
}
juce::var CalibrationProfile::toJson() const
{
    validate(*this);
    auto root = jsonObject(), k = jsonObject(), f = jsonObject();
    jsonSet(root, "schemaVersion", schemaVersion);
    jsonSet(k, "cameraId", key.cameraId); jsonSet(k, "nativeMode", key.nativeMode); jsonSet(k, "exposure", key.exposure);
    jsonSet(k, "asioDriver", key.asioDriver); jsonSet(k, "sampleRate", key.sampleRate); jsonSet(k, "bufferSamples", key.bufferSamples);
    jsonSet(f, "numerator", key.fps.numerator); jsonSet(f, "denominator", key.fps.denominator); jsonSet(k, "fps", f);
    juce::Array<juce::var> outputs; for (const auto c : key.outputMapping) outputs.add(c); jsonSet(k, "outputMapping", outputs);
    if (!key.inputMapping.empty()) { juce::Array<juce::var> inputs; for (const auto c : key.inputMapping) inputs.add(c); jsonSet(k, "inputMapping", inputs); }
    jsonSet(root, "key", k);
    jsonSet(root, "cameraResidualLatency100ns", std::to_string(cameraResidualLatency100ns));
    jsonSet(root, "inputResidualLatencySamples", std::to_string(inputResidualLatencySamples));
    jsonSet(root, "outputResidualLatencySamples", std::to_string(outputResidualLatencySamples));
    jsonSet(root, "measuredUtc", measuredUtc); jsonSet(root, "method", method); jsonSet(root, "quality", qualityName(quality));
    jsonSet(root, "measurementCount", measurementCount); jsonSet(root, "residualRmsSamples", residualRmsSamples);
    return root;
}
CalibrationProfile CalibrationProfile::fromJson(const juce::var& root)
{
    if (exactInteger(field(root, "schemaVersion")) != schemaVersion) throw std::invalid_argument("Unsupported calibration schemaVersion");
    CalibrationProfile p; const auto& k = field(root, "key"); const auto& f = field(k, "fps");
    p.key.cameraId = stringField(k, "cameraId"); p.key.nativeMode = stringField(k, "nativeMode"); p.key.exposure = stringField(k, "exposure");
    p.key.asioDriver = stringField(k, "asioDriver"); p.key.sampleRate = unsignedField(k, "sampleRate"); p.key.bufferSamples = unsignedField(k, "bufferSamples");
    p.key.fps = {unsignedField(f, "numerator"), unsignedField(f, "denominator")};
    const auto* outputs = field(k, "outputMapping").getArray();
    if (!outputs || outputs->size() > 64) throw std::invalid_argument("Invalid calibration outputs array");
    for (const auto& item : *outputs)
    {
        const auto c = exactInteger(item); if (c < 0 || c > 1023) throw std::invalid_argument("Invalid physical output");
        p.key.outputMapping.push_back(static_cast<int>(c));
    }
    if (k.hasProperty("inputMapping"))
    {
        const auto* inputs = field(k, "inputMapping").getArray();
        if (!inputs || inputs->size() > 24) throw std::invalid_argument("Invalid calibration inputs array");
        for (const auto& item : *inputs) { const auto c = exactInteger(item); if (c < -1 || c > 255) throw std::invalid_argument("Invalid calibration input"); p.key.inputMapping.push_back(int(c)); }
    }
    p.cameraResidualLatency100ns = exactInteger(field(root, "cameraResidualLatency100ns"));
    p.inputResidualLatencySamples = exactInteger(field(root, "inputResidualLatencySamples"));
    p.outputResidualLatencySamples = exactInteger(field(root, "outputResidualLatencySamples"));
    p.measuredUtc = stringField(root, "measuredUtc"); p.method = stringField(root, "method");
    const auto quality = stringField(root, "quality");
    if (quality == "unmeasured") p.quality = CalibrationQuality::unmeasured;
    else if (quality == "software-estimated") p.quality = CalibrationQuality::softwareEstimated;
    else if (quality == "physical-measured") p.quality = CalibrationQuality::physicalMeasured;
    else throw std::invalid_argument("Unknown calibration quality");
    p.measurementCount = unsignedField(root, "measurementCount");
    const auto& rms = field(root, "residualRmsSamples");
    if (!rms.isDouble() && !rms.isInt() && !rms.isInt64()) throw std::invalid_argument("Calibration RMS must be numeric");
    p.residualRmsSamples = static_cast<double>(rms); validate(p); return p;
}
std::string CalibrationProfile::serialize() const { return juce::JSON::toString(toJson(), false).toStdString(); }
CalibrationProfile CalibrationProfile::deserialize(const std::string& text)
{
    juce::var root;
    const auto result = juce::JSON::parse(juce::String::fromUTF8(text.c_str()), root);
    if (result.failed()) throw std::invalid_argument(result.getErrorMessage().toStdString());
    return fromJson(root);
}
}
