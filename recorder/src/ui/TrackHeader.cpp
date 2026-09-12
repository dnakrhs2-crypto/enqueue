#include "TrackHeader.h"

namespace gocue::recorder
{
TrackHeader::TrackHeader(TimelineEditController& controller, Id track) : edits(controller), id(std::move(track))
{
    addAndMakeVisible(name); name.setInterceptsMouseClicks(false, false);
    name.setFont(recorderFont(13.5f, juce::Font::bold));
    for (auto* b : {&mute, &solo}) { addAndMakeVisible(b); b->setWantsKeyboardFocus(false); }
    for (auto* b : {&mute, &solo})
    {
        b->getProperties().set("recorderFontSize", 11.5f);
        b->setColour(juce::TextButton::buttonColourId, Palette::card);
        b->setColour(juce::TextButton::textColourOffId, Palette::dimText);
    }
    mute.setColour(juce::TextButton::buttonOnColourId, Palette::muteOn);
    mute.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    solo.setColour(juce::TextButton::buttonOnColourId, Palette::soloOn);
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
void TrackHeader::paint(juce::Graphics& g)
{
    g.fillAll(Palette::bar); g.setColour(Palette::line);
    g.fillRect(getWidth() - 1, 0, 1, getHeight()); g.fillRect(0, getHeight() - 1, getWidth(), 1);
}
juce::PopupMenu TrackHeader::createContextMenu() const
{
    juce::PopupMenu menu;
    if (isEnabled()) menu.addItem(int(TimelineAction::hideTrack) + 1, ko("트랙 숨기기"), !edits.isLocked());
    return menu;
}
void TrackHeader::mouseDown(const juce::MouseEvent& e)
{
    if (!isEnabled() || !e.mods.isPopupMenu()) return;
    const auto base = edits.document.snapshot();
    juce::Component::SafePointer<TrackHeader> safe(this);
    createContextMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [safe, base](int result)
    {
        if (safe) safe->handleContextMenuResult(result, base);
    });
}
void TrackHeader::handleContextMenuResult(int result, const RecorderDocument::Snapshot& base)
{
    if (!isEnabled() || result != int(TimelineAction::hideTrack) + 1) return;
    // Hiding a row destroys its header when onEdit refreshes the view.
    const auto callback = onEdit;
    const auto r = edits.document.snapshot() != base
        ? juce::Result::fail(ko("편집 대상이 바뀌었습니다. 메뉴를 다시 여세요."))
        : edits.setTrackHidden(id, true);
    if (callback) callback(r);
}
void TrackHeader::resized()
{
    name.setBounds(8, 2, getWidth() - 16, 28);
    auto buttons = getLocalBounds().withY(getHeight() - 30).withHeight(24).reduced(6, 0);
    mute.setBounds(buttons.removeFromLeft((buttons.getWidth() - 6) / 2));
    buttons.removeFromLeft(6); solo.setBounds(buttons);
}
}
