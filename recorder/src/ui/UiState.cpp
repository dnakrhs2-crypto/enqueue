#include "UiState.h"

namespace gocue::recorder
{
namespace { juce::String k(const char* s) { return juce::String::fromUTF8(s); } }
RecorderUiState mapUiState(const RecorderProject& p, const UserSettings& settings, TakeController::State state,
                          bool locked, bool devicesBusy, bool audio, bool cam)
{
    using S = TakeController::State; RecorderUiState ui;
    ui.live = state == S::preparing || state == S::armed || state == S::recording || state == S::stopping;
    ui.structureLocked = locked || ui.live || devicesBusy || state == S::finalizing;
    ui.canStop = state == S::recording || state == S::armed;
    ui.canRecord = !ui.structureLocked && audio && cam;
    ui.canTransport = !ui.live && !devicesBusy && p.activeTimelineEnd() > 0;
    ui.camera2Off = !settings.cameraEnabled[1]; ui.timebaseFixed = !p.media->assets.empty();
    for (std::size_t i = 0; i < settings.physicalInputs.size(); ++i)
        if (settings.physicalInputs[i] >= 0 && settings.microphoneArmed[i]) ++ui.armedMicrophones;
    if (!ui.armedMicrophones) ui.warning = k("녹음 중인 마이크가 없습니다");
    ui.takeStatus = state == S::recording ? k("녹화 중") : state == S::stopping ? k("정지 중")
        : state == S::finalizing ? k("마무리 중") : state == S::done ? k("완료")
        : state == S::partialFailure ? k("일부 자료 저장 · 확인 필요") : ui.live ? k("준비 중") : k("대기");
    return ui;
}
juce::Result validateAudioSettings(const UserSettings& s, const RecorderAudioEngine::DeviceInfo& device, const RecorderProject& p)
{
    const auto basic = s.validate(device.name == s.asioDeviceId && device.sampleRate ? device.physicalInputs : 256); if (basic.failed()) return basic;
    if (s.asioDeviceId.isEmpty()) return juce::Result::fail(k("오디오 장치를 선택하세요."));
    if (s.output.mono ? s.output.monoChannel < 0 : s.output.left < 0 || s.output.right < 0)
        return juce::Result::fail(k("재생 출력 채널을 선택하세요."));
    if (device.name != s.asioDeviceId || !device.sampleRate) return juce::Result::ok(); // inspect actual limits after open
    for (auto input : s.physicalInputs) if (input >= device.physicalInputs) return juce::Result::fail(k("선택한 물리 입력이 없습니다."));
    if (s.output.mono ? s.output.monoChannel >= device.physicalOutputs : s.output.left >= device.physicalOutputs || s.output.right >= device.physicalOutputs)
        return juce::Result::fail(k("선택한 재생 출력 채널이 없습니다."));
    // A fixed project whose device opened at another rate is not an input error: 적용 must reach configure() so the
    // device is reopened at the project rate; recording/playback are blocked by the session with both numbers shown.
    return juce::Result::ok();
}
juce::Result validateCameraSettings(const UserSettings& s, const std::vector<CameraDevice>& cameras)
{
    const auto basic = s.validate(); if (basic.failed()) return basic;
    for (std::size_t i = 0; i < 2; ++i) if (s.cameraEnabled[i])
    {
        bool found = false;
        for (const auto& camera : cameras) if (camera.symbolicLink == s.cameraDeviceIds[i].toStdString())
            for (const auto& mode : camera.modes)
                if (mode.width == 1920 && mode.height == 1080 && mode.text() == s.cameraModes[i].toStdString()) found = true;
        if (!found) return juce::Result::fail(k(i == 0 ? "캠1 장치와 입력 모드를 선택하세요." : "캠2 장치와 입력 모드를 선택하세요."));
    }
    return juce::Result::ok();
}
CalibrationKey calibrationKey(const UserSettings& s, unsigned camera)
{
    if (camera >= s.cameraModes.size() || s.validate().failed()) throw std::invalid_argument("Invalid calibration settings");
    std::vector<int> inputs;
    for (std::size_t i = 0; i < s.physicalInputs.size(); ++i)
        if (s.physicalInputs[i] >= 0 && s.microphoneArmed[i])
            inputs.insert(inputs.end(), {int(i + 1), s.physicalInputs[i], s.stereoSlots[i] ? s.physicalInputs[i] + 1 : -1});
    return calibrationKey(s.cameraDeviceIds[camera].toStdString(), CameraMode::parse(s.cameraModes[camera].toStdString()),
        "uncontrolled", s.asioDeviceId.toStdString(), s.preferredSampleRate, unsigned(s.bufferSize),
        s.output.mono ? std::vector<int>{s.output.monoChannel} : std::vector<int>{s.output.left, s.output.right}, inputs);
}
CalibrationMatch calibrationMatches(const UserSettings& s, const std::vector<CalibrationProfile>& profiles)
{
    const bool measured = std::any_of(profiles.begin(), profiles.end(), [](const auto& p) { return p.quality != CalibrationQuality::unmeasured; });
    if (!measured) return s.calibration.calibrationDate.isEmpty() ? CalibrationMatch::unmeasured : CalibrationMatch::inputMappingUnverified;
    bool enabled = false, legacy = false;
    try
    {
        for (unsigned i = 0; i < s.cameraEnabled.size(); ++i) if (s.cameraEnabled[i])
        {
            enabled = true; const auto key = calibrationKey(s, i); bool matched = false, missingMapping = false;
            for (const auto& profile : profiles) if (profile.quality != CalibrationQuality::unmeasured)
            {
                if (profile.key == key && !profile.key.inputMapping.empty()) { profile.requireMatch(key); matched = true; break; }
                auto legacyKey = key; legacyKey.inputMapping.clear();
                if (profile.key.inputMapping.empty() && profile.key == legacyKey) missingMapping = true;
            }
            if (!matched && !missingMapping) return CalibrationMatch::settingsChanged;
            legacy = legacy || !matched;
        }
    }
    catch (const std::exception&) { return CalibrationMatch::settingsChanged; }
    return !enabled ? CalibrationMatch::settingsChanged : legacy ? CalibrationMatch::inputMappingUnverified : CalibrationMatch::matched;
}
juce::String calibrationStatusText(CalibrationMatch match)
{
    return k("동기 보정 · ") + (match == CalibrationMatch::matched ? k("측정됨")
        : match == CalibrationMatch::inputMappingUnverified ? k("입력 매핑 확인 전 · 재측정 필요")
        : match == CalibrationMatch::settingsChanged ? k("설정 변경으로 재측정 필요") : k("보정 결과 없음"));
}
juce::String formatRecorderTime(Sample sample, unsigned Fs)
{
    const auto nonnegative = (std::max)(Sample{0}, sample);
    const auto seconds = Fs ? nonnegative / Fs : 0;
    const auto millis = Fs ? (nonnegative % Fs) * 1000 / Fs : 0;
    return juce::String::formatted("%02lld:%02lld:%02lld.%03lld", seconds / 3600, seconds / 60 % 60, seconds % 60, millis);
}
}
