#include "ui/PlaybackFields.h"

#include "ui/UiUtils.h"
#include "ui/WaveformView.h"

namespace gocue
{
PlaybackFields::PlaybackFields (ProjectDocument& doc, AudioEngine& e) : document (doc), engine (e) {}

void PlaybackFields::addTo (juce::Component& parent)
{
    const auto label = [&parent] (juce::Label& l, const char* text)
    {
        l.setText (ko (text), juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, Palette::dimText);
        l.setFont (Palette::font (Palette::fieldLabelSize));
        parent.addAndMakeVisible (l);
    };
    label (startLabel, "시작");
    label (endLabel, "끝");
    label (lengthLabel, "");
    label (countLabel, "재생 횟수");
    label (rateLabel, "속도");
    label (fadeInLabel, "페이드 인");

    const auto editor = [this, &parent] (CommitEditor& field, const char* allowed, int maxLength, void (PlaybackFields::*commit)())
    {
        field.setInputRestrictions (maxLength, allowed);
        field.setFont (Palette::monoFont (Palette::fieldValueSize));
        field.setJustification (juce::Justification::centredRight);
        field.setSelectAllWhenFocused (true);
        field.onReturnKey = [this, &field, commit] { (this->*commit)(); field.giveAwayKeyboardFocus(); };
        field.onFocusLost = [this, commit] { (this->*commit)(); };
        field.onEscapeKey = [this] { cancelEdit(); };
        parent.addAndMakeVisible (field);
    };
    editor (startEditor, "0123456789:.", 12, &PlaybackFields::commitStart);
    editor (endEditor, "0123456789:.", 12, &PlaybackFields::commitEnd);
    editor (countEditor, "0123456789", 4, &PlaybackFields::commitPlayCount);
    editor (rateEditor, "0123456789.", 6, &PlaybackFields::commitRate);
    editor (fadeInEditor, "0123456789:.", 12, &PlaybackFields::commitFadeIn);

    const auto toggle = [&parent] (juce::ToggleButton& t, const char* text)
    {
        t.setButtonText (ko (text));
        t.setColour (juce::ToggleButton::textColourId, Palette::text);
        t.setColour (juce::ToggleButton::tickColourId, Palette::standby);
        t.setWantsKeyboardFocus (false);
        parent.addAndMakeVisible (t);
    };
    toggle (infiniteToggle, "무한 루프");
    infiniteToggle.onClick = [this]
    {
        const bool on = infiniteToggle.getToggleState();
        if (const auto* cue = shownCue(); cue != nullptr && on != cue->audio.infiniteLoop)
            updateShown (on ? ko ("무한 루프 켜기") : ko ("무한 루프 끄기"),
                         [on] (Cue& c) { c.audio.infiniteLoop = on; }, {}, LiveApply::loop);
    };
    toggle (pitchToggle, "피치 유지");
    pitchToggle.setTooltip (ko ("타임스트레치(Signalsmith Stretch): 속도를 바꿔도 음높이가 그대로 (0.25~4배 범위). 다음 재생부터 적용"));
    pitchToggle.onClick = [this]
    {
        const bool on = pitchToggle.getToggleState();
        if (const auto* cue = shownCue(); cue != nullptr && on != cue->audio.preservePitch)
            updateShown (ko ("피치 유지"), [on] (Cue& c) { c.audio.preservePitch = on; }, {}, LiveApply::none);
    };
}

std::array<juce::Component*, 13> PlaybackFields::fields()
{
    return { &startLabel, &startEditor, &endLabel, &endEditor, &lengthLabel, &fadeInLabel, &fadeInEditor,
             &countLabel, &countEditor, &infiniteToggle, &rateLabel, &rateEditor, &pitchToggle };
}

const Cue* PlaybackFields::shownCue() const { return document.cues.findById (shownId); }

bool PlaybackFields::canEdit() const
{
    const auto* cue = shownCue();
    return ! refreshing && ! cancellingEdit && ! document.isReplacingModel() && editable
           && startEditor.isEnabled() && cue != nullptr && cue->isAudio();
}

bool PlaybackFields::takeEdit (CommitEditor& editor)
{
    return canEdit() && editor.isEnabled() && editor.takePendingEdit();
}

void PlaybackFields::commitPending()
{
    commitStart();
    commitEnd();
    commitPlayCount();
    commitRate();
    commitFadeIn();
}

void PlaybackFields::refresh (bool shouldBeEditable)
{
    editable = shouldBeEditable;
    if (refreshing || committingPending) return;

    const bool modelReplaced = document.isReplacingModel();
    const auto* selected = document.cues.getSelected();
    if (! modelReplaced && ! shownId.isNull() && (selected == nullptr || selected->id != shownId))
    {
        const juce::ScopedValueSetter<bool> committing (committingPending, true);
        commitPending();
    }

    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    const auto* cue = document.cues.getSelected();
    if (cue != nullptr && ! cue->isAudio()) cue = nullptr;
    const auto id = cue != nullptr ? cue->id : juce::Uuid::null();
    const bool selectionChanged = id != shownId;
    shownId = id;
    const bool enabled = editable && cue != nullptr;
    for (auto* c : std::initializer_list<juce::Component*> { &startEditor, &endEditor, &countEditor, &rateEditor,
                                                           &fadeInEditor, &infiniteToggle, &pitchToggle })
        c->setEnabled (enabled);

    if (cue == nullptr)
    {
        for (auto* e : { &startEditor, &endEditor, &countEditor, &rateEditor, &fadeInEditor }) e->syncText ({});
        lengthLabel.setText ({}, juce::dontSendNotification);
        infiniteToggle.setToggleState (false, juce::dontSendNotification);
        pitchToggle.setToggleState (false, juce::dontSendNotification);
        return;
    }

    const auto setIfIdle = [this, modelReplaced, selectionChanged] (CommitEditor& e, const juce::String& text)
    {
        // As on the basic tab, undo/redo drops stale typing even while the field has focus.
        if (modelReplaced || selectionChanged || cancellingEdit || ! e.hasKeyboardFocus (true)) e.syncText (text);
    };
    setIfIdle (startEditor, formatTimeMs (cue->regionStart()));
    setIfIdle (endEditor, formatTimeMs (cue->regionEnd()));
    setIfIdle (countEditor, juce::String (cue->audio.playCount));
    setIfIdle (rateEditor, juce::String (cue->audio.rate, 2));
    setIfIdle (fadeInEditor, formatTimeMs (cue->audio.envelope.fadeInSeconds (cue->regionLength())));
    countEditor.setEnabled (enabled && ! cue->audio.infiniteLoop);
    fadeInEditor.setEnabled (enabled && cue->regionLength() > 0.0);
    infiniteToggle.setToggleState (cue->audio.infiniteLoop, juce::dontSendNotification);
    pitchToggle.setToggleState (cue->audio.preservePitch, juce::dontSendNotification);

    juce::String length;
    length << ko ("구간 ") << formatTimeMs (cue->regionLength());
    const double effective = cue->effectiveLength();
    if (effective < 0.0)
        length << (cue->audio.infiniteLoop ? ko ("  · 무한 반복") : ko ("  · 무한 슬라이스"));
    else if (! juce::approximatelyEqual (effective, cue->regionLength()))
        length << ko ("  · 전체 ") << formatTimeMs (effective);
    lengthLabel.setText (length, juce::dontSendNotification);
}

void PlaybackFields::cancelEdit()
{
    const juce::ScopedValueSetter<bool> guard (cancellingEdit, true);
    refresh (editable);
    if (onCancelEdit) onCancelEdit();
    else if (auto* parent = startEditor.getParentComponent()) parent->giveAwayKeyboardFocus();
}

void PlaybackFields::updateShown (const juce::String& name, const std::function<void (Cue&)>& mutator,
                                  const juce::String& coalesceKey, LiveApply apply)
{
    if (! canEdit()) return;
    const auto id = shownId;
    const int index = document.cues.indexOf (id);
    document.perform (name, [this, index, mutator] { document.cues.update (index, mutator); }, { coalesceKey, false });

    // A focus-loss edit belongs to the displayed cue even if a different row is now selected.
    if (const auto* cue = document.cues.findById (id); cue != nullptr && apply != LiveApply::none && engine.isPlaying (id))
    {
        if (apply == LiveApply::region || apply == LiveApply::regionAndRate)
            engine.setLiveRegion (id, cue->audio.startSeconds, cue->audio.endSeconds);
        if (apply == LiveApply::rate || apply == LiveApply::regionAndRate)
            engine.setLiveRate (id, cue->audio.rate);
        if (apply == LiveApply::envelope)
            engine.setLiveEnvelope (id, cue->audio.envelope);
        if (apply == LiveApply::loop)
            engine.setLivePlayCount (id, cue->audio.playCount, cue->audio.infiniteLoop);
    }
    refresh (editable);
}

void PlaybackFields::commitStart()
{
    if (! takeEdit (startEditor)) return;
    const auto* cue = shownCue();
    const double value = parseTimeText (startEditor.getText());
    const auto currentText = formatTimeMs (cue->regionStart());
    if (value < 0.0 || formatTimeMs (value) == currentText)
    {
        startEditor.syncText (currentText);
        return;
    }
    const double start = juce::jlimit (0.0, juce::jmax (0.0, cue->regionEnd() - WaveformView::minRegionSeconds), value);
    startEditor.syncText (formatTimeMs (start));
    if (formatTimeMs (start) == currentText) return;
    updateShown (ko ("시작 시간"), [start] (Cue& c) { c.audio.startSeconds = start; }, {}, LiveApply::region);
}

void PlaybackFields::commitEnd()
{
    if (! takeEdit (endEditor)) return;
    const auto* cue = shownCue();
    const double value = parseTimeText (endEditor.getText());
    const auto currentText = formatTimeMs (cue->regionEnd());
    if (value < 0.0 || formatTimeMs (value) == currentText)
    {
        endEditor.syncText (currentText);
        return;
    }
    const double length = cue->durationSeconds;
    double end = juce::jmax (cue->regionStart() + WaveformView::minRegionSeconds, value);
    if (length > 0.0 && end >= length - 1e-6) end = -1.0;
    const auto text = formatTimeMs (end < 0.0 ? length : end);
    endEditor.syncText (text);
    if (text == currentText) return;
    updateShown (ko ("끝 시간"), [end] (Cue& c) { c.audio.endSeconds = end; }, {}, LiveApply::region);
}

void PlaybackFields::commitPlayCount()
{
    if (! takeEdit (countEditor)) return;
    const auto* cue = shownCue();
    const int value = juce::jlimit (1, AudioCueData::maxPlayCount, countEditor.getText().getIntValue());
    if (value == cue->audio.playCount || countEditor.getText().trim().isEmpty())
    {
        countEditor.syncText (juce::String (cue->audio.playCount));
        return;
    }
    countEditor.syncText (juce::String (value));
    updateShown (ko ("재생 횟수"), [value] (Cue& c) { c.audio.playCount = value; }, {}, LiveApply::loop);
}

void PlaybackFields::commitRate()
{
    if (! takeEdit (rateEditor)) return;
    const auto* cue = shownCue();
    const auto text = rateEditor.getText().trim();
    const double value = juce::jlimit (AudioCueData::minRate, AudioCueData::maxRate, text.getDoubleValue());
    const auto currentText = juce::String (cue->audio.rate, 2);
    if (text.isEmpty() || juce::String (value, 2) == currentText)
    {
        rateEditor.syncText (currentText);
        return;
    }
    rateEditor.syncText (juce::String (value, 2));
    updateShown (ko ("속도"), [value] (Cue& c) { c.audio.rate = value; }, {}, LiveApply::rate);
}

void PlaybackFields::commitFadeIn()
{
    if (! takeEdit (fadeInEditor)) return;
    const auto* cue = shownCue();
    const double length = cue->regionLength();
    const auto currentText = formatTimeMs (cue->audio.envelope.fadeInSeconds (length));
    const double value = parseTimeText (fadeInEditor.getText());
    if (value < 0.0 || length <= 0.0 || formatTimeMs (value) == currentText)
    {
        fadeInEditor.syncText (currentText);
        return;
    }
    const double seconds = juce::jlimit (0.0, length, value);
    const auto text = formatTimeMs (seconds);
    fadeInEditor.syncText (text);
    if (text == currentText) return;
    updateShown (ko ("페이드 인 시간"), [seconds] (Cue& c)
    {
        c.audio.envelope.setFadeInSeconds (seconds, c.regionLength());
    }, {}, LiveApply::envelope);
}
}
