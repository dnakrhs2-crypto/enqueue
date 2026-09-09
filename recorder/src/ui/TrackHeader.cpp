#include "TrackHeader.h"

namespace gocue::recorder
{
TrackHeader::TrackHeader(TimelineEditController& controller, Id track) : edits(controller), id(std::move(track))
{
    addAndMakeVisible(name); name.setInterceptsMouseClicks(false, false);
    name.setFont(juce::Font(juce::FontOptions(16, juce::Font::bold)));
    for (auto* b : {&target, &mute, &solo}) { addAndMakeVisible(b); b->setWantsKeyboardFocus(false); }
    target.setTooltip(ko("오디오만 당길 대상 트랙 · 여러 트랙 선택 가능"));
    target.onClick = [this] { edits.selectTrack(id, true); if (onSelection) onSelection(); };
    mute.onClick = [this] { const auto r = edits.setTrackListening(id, false); if (onEdit) onEdit(r); };
    solo.onClick = [this] { const auto r = edits.setTrackListening(id, true); if (onEdit) onEdit(r); };
}
void TrackHeader::refresh(const Track& t)
{
    name.setText(t.name, juce::dontSendNotification); name.setTooltip(t.name);
    const bool isAudio = t.kind == TrackKind::mic || t.kind == TrackKind::importAudio;
    for (auto* b : {&target, &mute, &solo}) b->setVisible(isAudio);
    target.setToggleState(std::find(edits.selectedTracks().begin(), edits.selectedTracks().end(), id) != edits.selectedTracks().end(), juce::dontSendNotification);
    mute.setToggleState(t.mute, juce::dontSendNotification); solo.setToggleState(t.solo, juce::dontSendNotification);
    mute.setEnabled(edits.enabled(TimelineAction::mute)); solo.setEnabled(edits.enabled(TimelineAction::solo));
    mute.setTooltip(edits.isLocked() ? ko("녹화 중에는 청취 상태를 바꿀 수 없습니다.") : ko("음소거"));
    solo.setTooltip(edits.isLocked() ? ko("녹화 중에는 청취 상태를 바꿀 수 없습니다.") : ko("솔로"));
}
void TrackHeader::mouseDown(const juce::MouseEvent& e)
{ edits.selectTrack(id, e.mods.isCtrlDown() || e.mods.isShiftDown()); if (onSelection) onSelection(); }
void TrackHeader::resized()
{ name.setBounds(8, 2, getWidth() - 16, 28); target.setBounds(6, 35, 47, 28); mute.setBounds(57, 35, 70, 28); solo.setBounds(131, 35, 62, 28); }
}
