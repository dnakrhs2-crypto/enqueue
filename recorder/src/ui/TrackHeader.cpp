#include "TrackHeader.h"

namespace gocue::recorder
{
TrackHeader::TrackHeader(TimelineEditController& controller, Id track) : edits(controller), id(std::move(track))
{
    addAndMakeVisible(name); name.setInterceptsMouseClicks(false, false);
    name.setFont(juce::Font(juce::FontOptions(16, juce::Font::bold)));
    for (auto* b : {&mute, &solo}) { addAndMakeVisible(b); b->setWantsKeyboardFocus(false); }
    mute.setColour(juce::TextButton::buttonOnColourId, Palette::danger);
    mute.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    solo.setColour(juce::TextButton::buttonOnColourId, Palette::meterYellow);
    solo.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    mute.onClick = [this] { const auto r = edits.setTrackListening(id, false); if (onEdit) onEdit(r); };
    solo.onClick = [this] { const auto r = edits.setTrackListening(id, true); if (onEdit) onEdit(r); };
}
void TrackHeader::refresh(const Track& t)
{
    name.setText(t.name, juce::dontSendNotification); name.setTooltip(t.name);
    const bool isAudio = t.kind == TrackKind::mic || t.kind == TrackKind::importAudio;
    for (auto* b : {&mute, &solo}) b->setVisible(isAudio);
    mute.setToggleState(t.mute, juce::dontSendNotification); solo.setToggleState(t.solo, juce::dontSendNotification);
    mute.setEnabled(edits.enabled(TimelineAction::mute)); solo.setEnabled(edits.enabled(TimelineAction::solo));
    mute.setTooltip(edits.isLocked() ? ko("녹화 중에는 청취 상태를 바꿀 수 없습니다.") : ko("음소거"));
    solo.setTooltip(edits.isLocked() ? ko("녹화 중에는 청취 상태를 바꿀 수 없습니다.") : ko("솔로"));
}
void TrackHeader::resized()
{
    name.setBounds(8, 2, getWidth() - 16, 28);
    auto buttons = getLocalBounds().withY(35).withHeight(28).reduced(6, 0);
    mute.setBounds(buttons.removeFromLeft((buttons.getWidth() - 6) / 2));
    buttons.removeFromLeft(6); solo.setBounds(buttons);
}
}
