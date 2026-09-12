#pragma once
#include "RecorderLookAndFeel.h"
#include "UiState.h"

namespace gocue::recorder
{
class RecorderTransportBar : public juce::Component
{
public:
    RecorderTransportBar();
    void setState(bool enabled, bool playing, Sample, unsigned Fs);
    void setWaveformScale(unsigned);
    void resized() override;
    juce::TextButton play {ko("재생")}, stop {ko("정지")}, beginning {ko("처음으로")};
    juce::TextButton zoomOut {ko("−")}, zoomIn {"+"}, fit {ko("전체")};
    juce::TextButton waveOut {ko("−")}, waveIn {"+"};
private:
    juce::Label time, zoom, wave, waveValue;
};
}
