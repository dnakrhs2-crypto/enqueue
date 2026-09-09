#pragma once
#include "RecorderLookAndFeel.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include "UiState.h"

namespace gocue::recorder
{
class RecordView : public juce::Component
{
public:
    RecordView();
    void update(const RecorderUiState&, const RecorderProject&, const UserSettings&, const juce::String& status,
                const juce::String& banner, Sample elapsed, juce::int64 remainingBytes, bool timelineMode);
    void updateMeters(const std::array<float, 8>&);
    void setCamera(unsigned, const juce::String& caption, const juce::String& placeholder, bool visible);
    std::array<void*, 2> nativeHosts();
    juce::Rectangle<int> timelineBounds() const { return lowerBounds; }
    void paint(juce::Graphics&) override;
    void resized() override;
    juce::TextButton projectButton {ko("프로젝트")}, recordTab {ko("녹화")}, timelineTab {ko("타임라인")};
    juce::TextButton normalButton {ko("일반")}, dubButton {ko("더빙")}, settingsButton {ko("설정")}, exportButton {ko("내보내기")};
    juce::TextButton startButton {ko("녹화 시작")}, stopButton {ko("정지")}, markerButton {ko("마커 추가")}, latestButton {ko("방금 테이크 재생")};
    std::function<void(unsigned, bool)> onArm, onMonitor;
    std::function<void(unsigned, juce::String)> onName;
private:
    class CameraCard : public juce::Component
    {
    public:
        CameraCard();
        void ensureHost();
        void paint(juce::Graphics&) override;
        void resized() override;
        juce::HWNDComponent host;
        juce::String caption, placeholder;
        bool showVideo = false;
    };
    class Microphone : public juce::Component
    {
    public:
        Microphone();
        void paint(juce::Graphics&) override;
        void resized() override;
        juce::Label name, physical;
        juce::ToggleButton arm {ko("녹음")}, monitor {ko("입력 소리 듣기")};
        float peak = 0;
    };
    std::array<CameraCard, 2> cameras;
    std::array<Microphone, 8> microphones;
    juce::Viewport microphoneViewport;
    juce::Component strips;
    juce::Label projectName, statusLabel, errorLabel, noMicrophones;
    juce::Rectangle<int> lowerBounds;
    unsigned stripCount = 0;
    bool timeline = false;
};
}
