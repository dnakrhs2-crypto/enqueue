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
    const auto basic = s.validate(); if (basic.failed()) return basic;
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
juce::String formatRecorderTime(Sample sample, unsigned Fs)
{
    const auto ms = Fs ? (std::max)(Sample{0}, sample) * 1000 / Fs : 0;
    return juce::String::formatted("%02lld:%02lld:%02lld.%03lld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
}
}
