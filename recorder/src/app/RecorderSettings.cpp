#include "RecorderSettings.h"
#include "model/SafeFileWrite.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <set>

namespace gocue::recorder
{
namespace
{
juce::String ko(const char* text) { return juce::String::fromUTF8(text); }
juce::String key(const char* prefix, size_t index) { return juce::String(prefix) + juce::String(static_cast<int>(index)); }
juce::String encode(const UserSettings& s)
{
    juce::PropertySet p; p.setValue("schemaVersion", 1); p.setValue("productId", ProductIdentity::internalId());
    p.setValue("asioDeviceId", s.asioDeviceId); p.setValue("bufferSize", s.bufferSize); p.setValue("preferredSampleRate", static_cast<juce::int64>(s.preferredSampleRate));
    for (size_t i = 0; i < 2; ++i)
    {
        p.setValue(key("cameraEnabled", i), s.cameraEnabled[i]); p.setValue(key("cameraDeviceId", i), s.cameraDeviceIds[i]); p.setValue(key("cameraMode", i), s.cameraModes[i]);
        p.setValue(key("calibrationCameraDeviceId", i), s.calibration.cameraDeviceIds[i]); p.setValue(key("calibrationCameraMode", i), s.calibration.cameraModes[i]);
        p.setValue(key("cameraOffsetSamples", i), static_cast<juce::int64>(s.calibration.cameraOffsetSamples[i]));
    }
    juce::Array<juce::var> inputs; for (const int input : s.physicalInputs) inputs.add(input);
    p.setValue("physicalInputs", juce::JSON::toString(inputs, true));
    p.setValue("outputMono", s.output.mono); p.setValue("outputLeft", s.output.left); p.setValue("outputRight", s.output.right); p.setValue("outputMonoChannel", s.output.monoChannel);
    p.setValue("inputOffsetSamples", static_cast<juce::int64>(s.calibration.inputOffsetSamples)); p.setValue("outputOffsetSamples", static_cast<juce::int64>(s.calibration.outputOffsetSamples));
    p.setValue("calibrationDate", s.calibration.calibrationDate); p.setValue("calibrationIdentity", s.calibration.calibrationIdentity); p.setValue("calibrationAsioDeviceId", s.calibration.asioDeviceId);
    juce::Array<juce::var> calibrationInputs; for (const int input : s.calibration.physicalInputs) calibrationInputs.add(input);
    p.setValue("calibrationPhysicalInputs", juce::JSON::toString(calibrationInputs, true));
    juce::Array<juce::var> recent; for (const auto& path : s.recentProjects) recent.add(path);
    p.setValue("recentProjects", juce::JSON::toString(recent, true)); p.setValue("windowState", s.windowState);
    return p.createXml("RECORDER_SETTINGS")->toString();
}
juce::Result decode(const juce::String& text, UserSettings& out)
{
    try
    {
    const auto xml = juce::parseXML(text);
    if (xml == nullptr || !xml->hasTagName("RECORDER_SETTINGS")) return juce::Result::fail(ko("설정 파일을 읽을 수 없습니다."));
    juce::PropertySet p; p.restoreFromXml(*xml);
    const auto number = [&](const juce::String& name, Sample fallback)
    {
        const auto value = p.getValue(name, juce::String(static_cast<juce::int64>(fallback))).toStdString(); Sample parsed = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) throw std::invalid_argument("설정의 정수 값이 잘못되었습니다.");
        return parsed;
    };
    const auto bounded = [&](const juce::String& name, Sample fallback, Sample lower, Sample upper)
    { const auto n = number(name, fallback); if (n < lower || n > upper) throw std::invalid_argument("설정 값의 범위를 벗어났습니다."); return n; };
    const auto integer = [&](const juce::String& name, int fallback)
    { return static_cast<int>(bounded(name, fallback, (std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)())); };
    const auto boolean = [&](const juce::String& name, bool fallback) { return bounded(name, fallback ? 1 : 0, 0, 1) != 0; };
    if (integer("schemaVersion", 0) != 1 || p.getValue("productId") != ProductIdentity::internalId()) return juce::Result::fail(ko("지원하지 않는 설정 파일입니다."));
    UserSettings s; s.asioDeviceId = p.getValue("asioDeviceId"); s.bufferSize = integer("bufferSize", 256);
    s.preferredSampleRate = static_cast<std::uint32_t>(bounded("preferredSampleRate", 48000, 1, (std::numeric_limits<std::uint32_t>::max)()));
    for (size_t i = 0; i < 2; ++i)
    {
        s.cameraEnabled[i] = boolean(key("cameraEnabled", i), i == 0); s.cameraDeviceIds[i] = p.getValue(key("cameraDeviceId", i)); s.cameraModes[i] = p.getValue(key("cameraMode", i));
        s.calibration.cameraDeviceIds[i] = p.getValue(key("calibrationCameraDeviceId", i)); s.calibration.cameraModes[i] = p.getValue(key("calibrationCameraMode", i));
        s.calibration.cameraOffsetSamples[i] = number(key("cameraOffsetSamples", i), 0);
    }
    const auto inputs = juce::JSON::parse(p.getValue("physicalInputs", "[]"));
    const auto calibrationInputs = juce::JSON::parse(p.getValue("calibrationPhysicalInputs", "[]"));
    const auto recent = juce::JSON::parse(p.getValue("recentProjects", "[]"));
    if (!inputs.isArray() || !calibrationInputs.isArray() || !recent.isArray()) return juce::Result::fail(ko("설정의 입력 또는 최근 프로젝트 목록이 잘못되었습니다."));
    for (const auto& input : *inputs.getArray()) { if (!input.isInt()) return juce::Result::fail(ko("물리 입력 번호가 잘못되었습니다.")); s.physicalInputs.push_back(static_cast<int>(input)); }
    for (const auto& input : *calibrationInputs.getArray()) { if (!input.isInt()) return juce::Result::fail(ko("보정 입력 번호가 잘못되었습니다.")); s.calibration.physicalInputs.push_back(static_cast<int>(input)); }
    for (const auto& path : *recent.getArray()) { if (!path.isString()) return juce::Result::fail(ko("최근 프로젝트 경로가 잘못되었습니다.")); s.recentProjects.add(path.toString()); }
    s.output.mono = boolean("outputMono", false); s.output.left = integer("outputLeft", -1); s.output.right = integer("outputRight", -1); s.output.monoChannel = integer("outputMonoChannel", -1);
    s.calibration.inputOffsetSamples = number("inputOffsetSamples", 0); s.calibration.outputOffsetSamples = number("outputOffsetSamples", 0);
    s.calibration.calibrationDate = p.getValue("calibrationDate"); s.calibration.calibrationIdentity = p.getValue("calibrationIdentity"); s.calibration.asioDeviceId = p.getValue("calibrationAsioDeviceId");
    s.windowState = p.getValue("windowState"); const auto valid = s.validate(); if (valid.wasOk()) out = std::move(s); return valid;
    }
    catch (const std::exception& e) { return juce::Result::fail(juce::String::fromUTF8(e.what())); }
}
}
juce::Result OutputMapping::validate() const
{
    if (left < -1 || right < -1 || monoChannel < -1) return juce::Result::fail(ko("재생 출력 채널 번호가 잘못되었습니다."));
    if (!mono && ((left == -1) != (right == -1) || (left >= 0 && left == right))) return juce::Result::fail(ko("재생 출력 왼쪽과 오른쪽은 서로 다른 채널을 선택하세요."));
    return juce::Result::ok();
}
juce::Result UserSettings::validate() const
{
    const auto mapping = output.validate(); if (mapping.failed()) return mapping;
    if (bufferSize <= 0 || preferredSampleRate == 0 || physicalInputs.size() > 8) return juce::Result::fail(ko("오디오 장치 설정이 잘못되었습니다."));
    std::set<int> seen; for (const int input : physicalInputs) if (input < 0 || !seen.insert(input).second) return juce::Result::fail(ko("물리 입력이 중복되었거나 잘못되었습니다."));
    if (cameraEnabled[0] && cameraEnabled[1] && cameraDeviceIds[0].isNotEmpty() && cameraDeviceIds[0] == cameraDeviceIds[1]) return juce::Result::fail(ko("같은 카메라를 두 번 선택할 수 없습니다."));
    for (const auto& path : recentProjects) if (!juce::File::isAbsolutePath(path)) return juce::Result::fail(ko("최근 프로젝트 위치가 잘못되었습니다."));
    return juce::Result::ok();
}
RecorderSettings::RecorderSettings(const juce::File& testRoot)
    : settingsFile(ProductIdentity::settingsDirectory(testRoot).getChildFile(ProductIdentity::settingsFileName())), worker([this] { run(); }) {}
RecorderSettings::~RecorderSettings()
{
    { const std::lock_guard<std::mutex> lock(mutex); stopping = true; }
    wake.notify_one(); worker.join();
}
juce::Result RecorderSettings::load()
{ return settingsFile.existsAsFile() ? decode(settingsFile.loadFileAsString(), state) : settingsFile.exists() ? juce::Result::fail(ko("설정 경로가 파일이 아닙니다.")) : juce::Result::ok(); }
juce::Result RecorderSettings::set(UserSettings next)
{ const auto valid = next.validate(); if (valid.wasOk()) state = std::move(next); return valid; }
void RecorderSettings::rememberProject(const juce::File& file)
{
    state.recentProjects.removeString(file.getFullPathName()); state.recentProjects.insert(0, file.getFullPathName());
    while (state.recentProjects.size() > 20) state.recentProjects.remove(state.recentProjects.size() - 1);
}
std::future<juce::Result> RecorderSettings::save()
{
    Request request; auto future = request.completion.get_future(); const auto valid = state.validate();
    if (valid.failed()) { request.completion.set_value(valid); return future; }
    request.xml = encode(state);
    { const std::lock_guard<std::mutex> lock(mutex); pending.push_back(std::move(request)); }
    wake.notify_one(); return future;
}
void RecorderSettings::run()
{
    for (;;)
    {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex); wake.wait(lock, [&] { return stopping || !pending.empty(); });
            if (pending.empty() && stopping) return;
            request = std::move(pending.front()); pending.pop_front();
        }
        try { request.completion.set_value(gocue::SafeFileWrite::writeTextVerified(settingsFile, request.xml, [](const auto& text) { UserSettings verified; return decode(text, verified); })); }
        catch (const std::exception& e) { request.completion.set_value(juce::Result::fail(juce::String::fromUTF8(e.what()))); }
    }
}
}
