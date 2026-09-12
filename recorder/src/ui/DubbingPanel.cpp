#include "DubbingPanel.h"

namespace gocue::recorder
{
DubbingPanel::DubbingPanel(RecorderDocument& d, DubbingController& c) : document(d), controller(c), importer(d)
{
    for (juce::Component* item : std::array<juce::Component*, 8>{&importer, &audioTracks, &target, &status, &microphone, &record, &again, &stopButton}) addAndMakeVisible(item);
    microphone.setToggleState(false, juce::dontSendNotification);
    audioTracks.onChange = [this]
    {
        const auto i = audioTracks.getSelectedItemIndex(); if (i >= 0 && size_t(i) < trackIds.size()) config.audioTrackId = trackIds[size_t(i)];
        target.setText(juce::String::fromUTF8("대상 파일: ") + audioTracks.getText(), juce::dontSendNotification);
    };
    importer.onImported = [this](const MediaAsset& asset, const CachedImportedAudio&)
    {
        for (const auto& t : document.getProject().tracks) for (const auto& clip : t.clips.items()) if (clip.assetId == asset.assetId) config.audioTrackId = t.trackId;
        refresh();
    };
    record.onClick = [this]
    {
        config.takeId = juce::Uuid(); config.retakeStack.clear(); config.recordMicrophones = microphone.getToggleState();
        const auto result = controller.prepare(config); autoStart = result.wasOk();
        if (result.failed()) status.setText(result.getErrorMessage(), juce::dontSendNotification); refresh();
    };
    again.onClick = [this]
    {
        const auto result = controller.retake(microphone.getToggleState()); autoStart = result.wasOk();
        if (result.wasOk()) config.Pstart = controller.placement().Pstart;
        else status.setText(result.getErrorMessage(), juce::dontSendNotification); refresh();
    };
    stopButton.onClick = [this]
    {
        autoStart = false;
        if (controller.state() == DubbingController::State::preparing || controller.state() == DubbingController::State::armed) controller.abort();
        else { const auto r = controller.stop(); if (r.failed()) status.setText(r.getErrorMessage(), juce::dontSendNotification); }
    };
    startTimer(10); refresh();
}
DubbingPanel::~DubbingPanel() { stopTimer(); }
void DubbingPanel::setContext(DubbingController::Config c)
{ if (!controller.locked()) { config = std::move(c); microphone.setToggleState(config.recordMicrophones, juce::dontSendNotification); refresh(); } }
void DubbingPanel::setPlayhead(Sample p) { if (!controller.locked()) { config.Pstart = p; importer.setImportContext(config.projectDirectory, p); } }
void DubbingPanel::refresh()
{
    const bool locked = controller.locked(); importer.setRecordingActive(locked);
    importer.setImportContext(config.projectDirectory, config.Pstart);
    audioTracks.clear(juce::dontSendNotification); trackIds.clear();
    for (const auto& t : document.getProject().tracks) if (t.kind == TrackKind::importAudio)
    {
        trackIds.push_back(t.trackId); audioTracks.addItem(t.name, int(trackIds.size()));
        if (t.trackId == config.audioTrackId) audioTracks.setSelectedId(int(trackIds.size()), juce::dontSendNotification);
    }
    target.setText(juce::String::fromUTF8("대상 파일: ") + audioTracks.getText(), juce::dontSendNotification);
    audioTracks.setEnabled(!locked); microphone.setEnabled(!locked); record.setEnabled(!locked && config.audioTrackId.isNotEmpty());
    again.setEnabled(!locked && controller.placement().stackId.isNotEmpty()); stopButton.setEnabled(locked && controller.state() != DubbingController::State::finalizing);
}
void DubbingPanel::timerCallback()
{
    controller.tick();
    if (autoStart && controller.state() == DubbingController::State::armed)
    { autoStart = false; const auto r = controller.start(); if (r.failed()) status.setText(r.getErrorMessage(), juce::dontSendNotification); }
    if (previous != controller.state())
    {
        previous = controller.state(); status.setText(controller.statusText() + (controller.error().isEmpty() ? "" : juce::String::fromUTF8(" · ") + controller.error()), juce::dontSendNotification);
        refresh(); if (onTakeChanged) onTakeChanged();
    }
}
void DubbingPanel::resized()
{
    auto r = getLocalBounds().reduced(8); importer.setBounds(r.removeFromTop(100));
    audioTracks.setBounds(r.removeFromTop(30)); target.setBounds(r.removeFromTop(28)); microphone.setBounds(r.removeFromTop(30));
    auto buttons = r.removeFromTop(34); record.setBounds(buttons.removeFromLeft(180)); again.setBounds(buttons.removeFromLeft(180)); stopButton.setBounds(buttons.removeFromLeft(64));
    status.setBounds(r.removeFromTop(44));
}
}
