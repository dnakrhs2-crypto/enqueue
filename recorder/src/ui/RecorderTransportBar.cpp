#include "RecorderTransportBar.h"

namespace gocue::recorder
{
RecorderTransportBar::RecorderTransportBar()
{
    for (auto* b : {&play, &stop, &beginning, &zoomOut, &zoomIn, &fit, &waveOut, &waveIn}) { addAndMakeVisible(b); b->setWantsKeyboardFocus(false); }
    for (auto* l : {&time, &zoom, &wave, &waveValue}) { addAndMakeVisible(l); l->setFont(juce::Font(juce::FontOptions(17))); }
    zoom.setText(ko("확대"), juce::dontSendNotification); wave.setText(ko("파형"), juce::dontSendNotification);
    waveValue.setJustificationType(juce::Justification::centred);
    waveOut.setTooltip(ko("파형 높이 줄이기")); waveIn.setTooltip(ko("파형 높이 키우기"));
    setWaveformScale(1);
    play.setTooltip(ko("재생 / 정지 · Space")); beginning.setTooltip(ko("프로젝트 처음으로 이동")); stop.setTooltip(ko("현재 위치에서 정지"));
}
void RecorderTransportBar::setWaveformScale(unsigned scale)
{
    waveValue.setText(ko("×") + juce::String(scale), juce::dontSendNotification);
    waveOut.setEnabled(scale > 1); waveIn.setEnabled(scale < 16);
}
void RecorderTransportBar::setState(bool enabled, bool playing, Sample at, unsigned Fs)
{
    play.setEnabled(enabled && !playing); stop.setEnabled(enabled); beginning.setEnabled(enabled);
    time.setText(formatRecorderTime(at, Fs), juce::dontSendNotification);
}
void RecorderTransportBar::resized()
{
    auto row = getLocalBounds().reduced(0, 2);
    beginning.setBounds(row.removeFromLeft(92).reduced(2)); play.setBounds(row.removeFromLeft(72).reduced(2)); stop.setBounds(row.removeFromLeft(70).reduced(2));
    waveIn.setBounds(row.removeFromRight(36).reduced(2)); waveValue.setBounds(row.removeFromRight(44)); waveOut.setBounds(row.removeFromRight(36).reduced(2)); wave.setBounds(row.removeFromRight(46));
    fit.setBounds(row.removeFromRight(58).reduced(2)); zoomIn.setBounds(row.removeFromRight(36).reduced(2)); zoomOut.setBounds(row.removeFromRight(36).reduced(2)); zoom.setBounds(row.removeFromRight(50)); time.setBounds(row.reduced(4, 0));
}
}
