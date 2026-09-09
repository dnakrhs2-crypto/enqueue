#include "RecorderTransportBar.h"

namespace gocue::recorder
{
RecorderTransportBar::RecorderTransportBar()
{
    for (auto* b : {&play, &pause, &stop, &beginning, &zoomOut, &zoomIn, &fit}) { addAndMakeVisible(b); b->setWantsKeyboardFocus(false); }
    for (auto* l : {&time, &zoom, &snap}) { addAndMakeVisible(l); l->setFont(juce::Font(juce::FontOptions(17))); }
    zoom.setText(ko("확대"), juce::dontSendNotification); snap.setText(ko("스냅 · 준비 전"), juce::dontSendNotification); snap.setColour(juce::Label::textColourId, Palette::dimText);
    play.setTooltip(ko("재생 / 일시정지 · Space")); beginning.setTooltip(ko("프로젝트 처음으로 이동")); stop.setTooltip(ko("현재 위치에서 정지"));
}
void RecorderTransportBar::setState(bool enabled, bool playing, Sample at, unsigned Fs)
{
    play.setEnabled(enabled && !playing); pause.setEnabled(enabled && playing); stop.setEnabled(enabled); beginning.setEnabled(enabled);
    time.setText(formatRecorderTime(at, Fs), juce::dontSendNotification);
}
void RecorderTransportBar::resized()
{
    auto row = getLocalBounds().reduced(0, 2);
    beginning.setBounds(row.removeFromLeft(92).reduced(2)); play.setBounds(row.removeFromLeft(72).reduced(2)); pause.setBounds(row.removeFromLeft(92).reduced(2)); stop.setBounds(row.removeFromLeft(70).reduced(2));
    snap.setBounds(row.removeFromRight(125)); fit.setBounds(row.removeFromRight(58).reduced(2)); zoomIn.setBounds(row.removeFromRight(36).reduced(2)); zoomOut.setBounds(row.removeFromRight(36).reduced(2)); zoom.setBounds(row.removeFromRight(50)); time.setBounds(row.reduced(4, 0));
}
}
