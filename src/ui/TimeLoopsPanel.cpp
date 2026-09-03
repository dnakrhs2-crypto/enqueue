#include "ui/TimeLoopsPanel.h"

#include "ui/UiUtils.h"

namespace gocue
{

TimeLoopsPanel::TimeLoopsPanel (ProjectDocument& doc, AudioEngine& e, juce::AudioThumbnailCache& cache)
    : document (doc), engine (e), waveform (e.getFormatManager(), cache)
{
    auto setupLabel = [this] (juce::Label& label, const char* text)
    {
        label.setText (ko (text), juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, Palette::dimText);
        label.setFont (juce::Font (juce::FontOptions (15.0f)));
        addAndMakeVisible (label);
    };

    setupLabel (startLabel, "시작");
    setupLabel (endLabel, "끝");
    setupLabel (lengthLabel, "");
    setupLabel (countLabel, "재생 횟수");
    setupLabel (rateLabel, "속도");
    setupLabel (envelopeLabel, "페이드 엔벨로프");
    setupLabel (zoomLabel, "확대");
    setupLabel (sizeLabel, "크기");

    setupEditor (startEditor, "0123456789:.", 12);
    startEditor.onReturnKey = [this] { commitStart(); startEditor.giveAwayKeyboardFocus(); };
    startEditor.onFocusLost = [this] { commitStart(); };

    setupEditor (endEditor, "0123456789:.", 12);
    endEditor.onReturnKey = [this] { commitEnd(); endEditor.giveAwayKeyboardFocus(); };
    endEditor.onFocusLost = [this] { commitEnd(); };

    setupEditor (countEditor, "0123456789", 4);
    countEditor.onReturnKey = [this] { commitPlayCount(); countEditor.giveAwayKeyboardFocus(); };
    countEditor.onFocusLost = [this] { commitPlayCount(); };

    setupEditor (rateEditor, "0123456789.", 6);
    rateEditor.onReturnKey = [this] { commitRate(); rateEditor.giveAwayKeyboardFocus(); };
    rateEditor.onFocusLost = [this] { commitRate(); };

    setupToggle (infiniteToggle, "무한 루프");
    infiniteToggle.onClick = [this]
    {
        const bool on = infiniteToggle.getToggleState();
        updateSelected (on ? ko ("무한 루프 켜기") : ko ("무한 루프 끄기"), [on] (Cue& c) { c.audio.infiniteLoop = on; }, {}, LiveApply::loop);   // heard at once
    };

    setupToggle (envelopeToggle, "사용");
    envelopeToggle.onClick = [this]
    {
        const bool on = envelopeToggle.getToggleState();
        updateSelected (on ? ko ("엔벨로프 켜기") : ko ("엔벨로프 끄기"), [on] (Cue& c)
        {
            c.audio.envelope.enabled = on;

            if (on && c.audio.envelope.points.empty())
            {
                c.audio.envelope.lockToTrim = true;
                c.audio.envelope.points = { { 0.0, 1.0 }, { 1.0, 1.0 } };   // a flat line to click on
            }
        }, {}, LiveApply::envelope);
    };

    setupToggle (linearToggle, "직선 (끄면 곡선)");
    linearToggle.onClick = [this]
    {
        const bool on = linearToggle.getToggleState();
        updateSelected (ko ("엔벨로프 모양"), [on] (Cue& c) { c.audio.envelope.linear = on; }, {}, LiveApply::envelope);
    };

    setupToggle (lockToggle, "시작/끝에 잠금");
    lockToggle.setTooltip (ko ("켜면 트림을 바꿀 때 엔벨로프가 구간에 맞춰 늘어나고, 끄면 초 단위로 고정됩니다"));
    lockToggle.onClick = [this]
    {
        const bool on = lockToggle.getToggleState();
        const auto* cue = selected();

        if (cue != nullptr && cue->regionLength() <= 0.0)
        {
            // the conversion needs a known length; without one every point would collapse to 0
            lockToggle.setToggleState (! on, juce::dontSendNotification);
            return;
        }

        updateSelected (ko ("엔벨로프 잠금"), [on] (Cue& c) { c.audio.envelope.setLockToTrim (on, c.regionLength()); }, {}, LiveApply::envelope);
    };

    setupToggle (pitchToggle, "피치 유지");
    pitchToggle.setTooltip (ko ("타임스트레치(Signalsmith Stretch): 속도를 바꿔도 음높이가 그대로 (0.25~4배 범위). 다음 재생부터 적용"));
    pitchToggle.onClick = [this]
    {
        const bool on = pitchToggle.getToggleState();
        updateSelected (ko ("피치 유지"), [on] (Cue& c) { c.audio.preservePitch = on; }, {}, LiveApply::none);   // next start
    };

    auto setupButton = [this] (juce::TextButton& button, const char* text, std::function<void()> action)
    {
        button.setButtonText (ko (text));
        button.setWantsKeyboardFocus (false);
        button.onClick = std::move (action);
        addAndMakeVisible (button);
    };

    setupButton (resetButton, "리셋", [this] { if (onReset) onReset(); });
    resetButton.setTooltip (ko ("이 큐를 정지하고 처음 상태로 되돌립니다"));
    setupButton (zoomInButton, "+", [this] { waveform.zoomIn(); });
    zoomInButton.setTooltip (ko ("시간축 확대 (Ctrl+휠도 됩니다). 전체 보기 / 구간에 맞춰 줌은 파형 우클릭"));
    setupButton (zoomOutButton, "-", [this] { waveform.zoomOut(); });
    zoomOutButton.setTooltip (ko ("시간축 축소 (Ctrl+휠도 됩니다)"));
    setupButton (sizeUpButton, "+", [this] { waveform.zoomVertical (1.5f); });
    sizeUpButton.setTooltip (ko ("파형을 세로로 크게 (작은 소리를 보기 쉽게)"));
    setupButton (sizeDownButton, "-", [this] { waveform.zoomVertical (1.0f / 1.5f); });
    sizeDownButton.setTooltip (ko ("파형을 세로로 작게"));

    waveform.onTrimChanged = [this] (double start, double end, bool finished) { commitTrim (start, end, finished); };
    waveform.onEnvelopeChanged = [this] (const Envelope& envelope, bool finished) { commitEnvelope (envelope, finished); };
    waveform.onContextMenu = [this] (juce::Point<int> screenPosition) { showContextMenu (screenPosition); };
    waveform.onSlicesChanged = [this] (const std::vector<Slice>& slices, int firstCount, bool finished) { commitSlices (slices, firstCount, finished); };
    waveform.onSeekPlay = [this] (double fileSeconds) { if (onSeekPlay) onSeekPlay (fileSeconds); };
    addAndMakeVisible (waveform);

    refresh();
}

TimeLoopsPanel::~TimeLoopsPanel() = default;

void TimeLoopsPanel::setupEditor (juce::TextEditor& editor, const juce::String& allowed, int maxLength)
{
    editor.setInputRestrictions (maxLength, allowed);
    editor.setJustification (juce::Justification::centredRight);
    editor.setSelectAllWhenFocused (true);
    editor.onEscapeKey = [this] { cancelEditAndPanic(); };
    addAndMakeVisible (editor);
}

void TimeLoopsPanel::setupToggle (juce::ToggleButton& toggle, const char* text)
{
    toggle.setButtonText (ko (text));
    toggle.setColour (juce::ToggleButton::textColourId, Palette::text);
    toggle.setColour (juce::ToggleButton::tickColourId, Palette::standby);
    toggle.setWantsKeyboardFocus (false);
    addAndMakeVisible (toggle);
}

void TimeLoopsPanel::cancelEditAndPanic()
{
    {
        const juce::ScopedValueSetter<bool> guard (cancellingEdit, true);

        if (onPanic)
            onPanic();
        else
            giveAwayKeyboardFocus();
    }

    refresh();
}

//==============================================================================
void TimeLoopsPanel::refresh()
{
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    const auto* cue = selected();
    const bool enabled = cue != nullptr;

    for (auto* c : { static_cast<juce::Component*> (&startEditor), static_cast<juce::Component*> (&endEditor),
                     static_cast<juce::Component*> (&countEditor), static_cast<juce::Component*> (&rateEditor),
                     static_cast<juce::Component*> (&infiniteToggle), static_cast<juce::Component*> (&envelopeToggle),
                     static_cast<juce::Component*> (&linearToggle), static_cast<juce::Component*> (&lockToggle),
                     static_cast<juce::Component*> (&resetButton) })
        c->setEnabled (enabled);

    waveform.setCue (cue);

    if (cue == nullptr)
    {
        startEditor.setText ("", false);
        endEditor.setText ("", false);
        countEditor.setText ("", false);
        rateEditor.setText ("", false);
        lengthLabel.setText ("", juce::dontSendNotification);
        return;
    }

    {
        bool editing = false;

        for (auto* e : { &startEditor, &endEditor, &countEditor, &rateEditor })
            editing = editing || e->hasKeyboardFocus (true);

        if (editing && ! shownId.isNull() && shownId != cue->id && document.cues.indexOf (shownId) >= 0)
            return;   // keep showing the cue being edited until its commit lands

        shownId = cue->id;
    }

    if (! startEditor.hasKeyboardFocus (true))
        startEditor.setText (formatTimeMs (cue->regionStart()), false);

    if (! endEditor.hasKeyboardFocus (true))
        endEditor.setText (formatTimeMs (cue->regionEnd()), false);

    if (! countEditor.hasKeyboardFocus (true))
        countEditor.setText (juce::String (cue->audio.playCount), false);

    countEditor.setEnabled (! cue->audio.infiniteLoop);

    if (! rateEditor.hasKeyboardFocus (true))
        rateEditor.setText (juce::String (cue->audio.rate, 2), false);

    infiniteToggle.setToggleState (cue->audio.infiniteLoop, juce::dontSendNotification);
    envelopeToggle.setToggleState (cue->audio.envelope.enabled, juce::dontSendNotification);
    linearToggle.setToggleState (cue->audio.envelope.linear, juce::dontSendNotification);
    lockToggle.setToggleState (cue->audio.envelope.lockToTrim, juce::dontSendNotification);
    pitchToggle.setToggleState (cue->audio.preservePitch, juce::dontSendNotification);
    linearToggle.setEnabled (cue->audio.envelope.enabled);
    lockToggle.setEnabled (cue->audio.envelope.enabled);

    juce::String length;
    length << ko ("구간 ") << formatTimeMs (cue->regionLength());
    const double effective = cue->effectiveLength();

    if (effective < 0.0)
        length << (cue->audio.infiniteLoop ? ko ("  · 무한 반복") : ko ("  · 무한 슬라이스"));
    else if (! juce::approximatelyEqual (effective, cue->regionLength()))
        length << ko ("  · 전체 ") << formatTimeMs (effective);   // slices / play count / rate change the total

    lengthLabel.setText (length, juce::dontSendNotification);
}

void TimeLoopsPanel::setPlayback (const AudioEngine::PlayingCue* playing)
{
    if (playing != nullptr)
        waveform.setPlayhead (playing->filePositionSeconds, ! playing->paused);
    else
        waveform.setPlayhead (-1.0, false);
}

//==============================================================================
void TimeLoopsPanel::updateSelected (const juce::String& name, const std::function<void (Cue&)>& mutator, const juce::String& coalesceKey, LiveApply apply)
{
    if (refreshing || cancellingEdit)
        return;

    const int index = shownId.isNull() ? document.cues.getSelectedIndex() : document.cues.indexOf (shownId);

    if (! document.cues.isValidIndex (index))
        return;

    document.perform (name, [this, index, mutator] { document.cues.update (index, mutator); }, { coalesceKey, false });

    // only what the edit changed reaches a running instance: a trim seeks, a rate change does not, and
    // next-start properties (pitch mode, envelope shape) leave it alone
    if (const auto* cue = selected(); cue != nullptr && apply != LiveApply::none && engine.isPlaying (cue->id))
    {
        if (apply == LiveApply::region || apply == LiveApply::regionAndRate)
            engine.setLiveRegion (cue->id, cue->audio.startSeconds, cue->audio.endSeconds);

        if (apply == LiveApply::rate || apply == LiveApply::regionAndRate)
            engine.setLiveRate (cue->id, cue->audio.rate);

        if (apply == LiveApply::envelope)
            engine.setLiveEnvelope (cue->id, cue->audio.envelope);   // the fade points / toggles are heard at once

        if (apply == LiveApply::loop)
            engine.setLivePlayCount (cue->id, cue->audio.playCount, cue->audio.infiniteLoop);   // the loop follows the toggle at once
    }

    if (const auto* selected = document.cues.getSelected(); selected != nullptr && selected->id != shownId)
        refresh();
}

void TimeLoopsPanel::pushLiveRegion()
{
    if (const auto* cue = selected(); cue != nullptr && engine.isPlaying (cue->id))
    {
        engine.setLiveRegion (cue->id, cue->audio.startSeconds, cue->audio.endSeconds);
        engine.setLiveRate (cue->id, cue->audio.rate);
    }
}

void TimeLoopsPanel::commitTrim (double start, double end, bool finished)
{
    const auto* cue = selected();

    if (cue == nullptr)
        return;

    const auto key = "trim:" + cue->id.toString();
    updateSelected (ko ("트림"), [start, end] (Cue& c) { c.audio.startSeconds = start; c.audio.endSeconds = end; }, key, LiveApply::region);
    juce::ignoreUnused (finished);
}

void TimeLoopsPanel::commitSlices (const std::vector<Slice>& slices, int firstCount, bool finished)
{
    const auto* cue = selected();

    if (cue == nullptr)
        return;

    const auto key = finished ? juce::String() : "slices:" + cue->id.toString();   // a drag is one undo step; add / delete / count are their own
    const auto id = cue->id;
    updateSelected (ko ("슬라이스"), [slices, firstCount] (Cue& c)
    {
        c.audio.slices = slices;
        c.audio.firstSliceCount = firstCount;
        c.audio.sanitiseSlices (c.durationSeconds);
    }, key, LiveApply::none);   // the slice layout goes live below, without a region seek first

    if (const auto* updated = selected(); updated != nullptr && updated->id == id && engine.isPlaying (id))
        engine.setLiveSlices (id, updated->audio.slices, updated->audio.firstSliceCount);
}

void TimeLoopsPanel::importSliceMarkers()
{
    const auto* cue = selected();

    if (cue == nullptr || ! cue->file.existsAsFile())
        return;

    std::unique_ptr<juce::AudioFormatReader> reader (engine.getFormatManager().createReaderFor (cue->file));

    if (reader == nullptr || reader->sampleRate <= 0.0)
        return;

    // WAV / AIFF cue points ("NumCuePoints", "CueNOffset") become slice markers
    std::vector<Slice> slices;
    const int numCues = reader->metadataValues.getValue ("NumCuePoints", "0").getIntValue();

    for (int i = 0; i < numCues; ++i)
    {
        const auto offset = reader->metadataValues.getValue ("Cue" + juce::String (i) + "Offset", "");

        if (offset.isEmpty())
            continue;

        slices.push_back ({ offset.getDoubleValue() / reader->sampleRate, 1 });
    }

    if (slices.empty())
    {
        if (onStatus)
            onStatus (ko ("파일에 큐 마커가 없습니다"), true);

        return;
    }

    commitSlices (slices, 1, true);
    refresh();
}

void TimeLoopsPanel::commitEnvelope (const Envelope& envelope, bool finished)
{
    const auto* cue = selected();

    if (cue == nullptr)
        return;

    // every callback of one drag (including the final one) shares the key, so a drag is one undo step.
    // The live push (which drops the read-ahead cache) is throttled during the drag; the final one always goes.
    const auto now = juce::Time::getMillisecondCounter();
    const bool pushLive = finished || now - lastLiveEnvelopePush >= 40;

    if (pushLive)
        lastLiveEnvelopePush = now;

    const auto key = "envelope:" + cue->id.toString();
    updateSelected (ko ("엔벨로프 편집"), [envelope] (Cue& c) { c.audio.envelope = envelope; c.audio.envelope.sanitise(); }, key,
                    pushLive ? LiveApply::envelope : LiveApply::none);
}

void TimeLoopsPanel::commitStart()
{
    const auto* cue = selected();

    if (refreshing || cancellingEdit || cue == nullptr)
        return;

    const double value = parseTimeText (startEditor.getText());

    if (value < 0.0 || juce::approximatelyEqual (value, cue->regionStart()))
    {
        startEditor.setText (formatTimeMs (cue->regionStart()), false);
        return;
    }

    const double end = cue->regionEnd();
    const double start = juce::jlimit (0.0, juce::jmax (0.0, end - WaveformView::minRegionSeconds), value);
    updateSelected (ko ("시작 시간"), [start] (Cue& c) { c.audio.startSeconds = start; }, {}, LiveApply::region);
}

void TimeLoopsPanel::commitEnd()
{
    const auto* cue = selected();

    if (refreshing || cancellingEdit || cue == nullptr)
        return;

    const double value = parseTimeText (endEditor.getText());

    if (value < 0.0 || juce::approximatelyEqual (value, cue->regionEnd()))
    {
        endEditor.setText (formatTimeMs (cue->regionEnd()), false);
        return;
    }

    const double length = cue->durationSeconds;
    double end = juce::jmax (cue->regionStart() + WaveformView::minRegionSeconds, value);

    if (length > 0.0 && end >= length - 1e-6)
        end = -1.0;

    updateSelected (ko ("끝 시간"), [end] (Cue& c) { c.audio.endSeconds = end; }, {}, LiveApply::region);
}

void TimeLoopsPanel::commitPlayCount()
{
    const auto* cue = selected();

    if (refreshing || cancellingEdit || cue == nullptr)
        return;

    const int value = juce::jlimit (1, AudioCueData::maxPlayCount, countEditor.getText().getIntValue());

    if (value == cue->audio.playCount || countEditor.getText().trim().isEmpty())
    {
        countEditor.setText (juce::String (cue->audio.playCount), false);
        return;
    }

    updateSelected (ko ("재생 횟수"), [value] (Cue& c) { c.audio.playCount = value; }, {}, LiveApply::loop);
}

void TimeLoopsPanel::commitRate()
{
    const auto* cue = selected();

    if (refreshing || cancellingEdit || cue == nullptr)
        return;

    const auto text = rateEditor.getText().trim();
    const double value = juce::jlimit (AudioCueData::minRate, AudioCueData::maxRate, text.getDoubleValue());

    if (text.isEmpty() || juce::approximatelyEqual (value, cue->audio.rate))
    {
        rateEditor.setText (juce::String (cue->audio.rate, 2), false);
        return;
    }

    updateSelected (ko ("속도"), [value] (Cue& c) { c.audio.rate = value; }, {}, LiveApply::rate);
}

//==============================================================================
void TimeLoopsPanel::showContextMenu (juce::Point<int> screenPosition)
{
    const auto* cue = selected();

    if (cue == nullptr)
        return;

    const juce::File file = cue->file;
    const bool hasFile = file.existsAsFile();
    juce::PopupMenu menu;
    menu.addItem (1, ko ("외부 편집기로 열기"), hasFile);
    menu.addItem (2, ko ("탐색기에서 보기"), hasFile);
    menu.addSeparator();

    juce::PopupMenu channels;
    const int numChannels = waveform.getNumFileChannels();
    channels.addItem (100, ko ("전체 채널"), true, waveform.getViewChannel() < 0);

    for (int ch = 0; ch < numChannels; ++ch)
        channels.addItem (101 + ch, ko ("채널 ") + juce::String (ch + 1), true, waveform.getViewChannel() == ch);

    menu.addSubMenu (ko ("표시 채널"), channels, numChannels > 0);
    menu.addSeparator();
    menu.addItem (3, ko ("구간에 맞춰 줌"));
    menu.addItem (4, ko ("전체 보기"));
    menu.addSeparator();
    menu.addItem (5, ko ("커서에 슬라이스 마커 추가 (M)"));
    menu.addItem (6, ko ("슬라이스 마커 전부 삭제"), ! cue->audio.slices.empty());
    menu.addItem (7, ko ("파일의 큐 마커를 슬라이스로 가져오기"), hasFile);

    juce::Component::SafePointer<TimeLoopsPanel> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ screenPosition.x, screenPosition.y, 1, 1 }),
                        [safeThis, file] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;

        if (result == 1)
            file.startAsProcess();
        else if (result == 2)
            file.revealToUser();
        else if (result == 3)
            safeThis->waveform.zoomToRegion();
        else if (result == 4)
            safeThis->waveform.zoomToFit();
        else if (result == 5)
            safeThis->waveform.addSliceAtCursor();
        else if (result == 6)
            safeThis->waveform.clearSlices();
        else if (result == 7)
            safeThis->importSliceMarkers();
        else if (result == 100)
            safeThis->waveform.setViewChannel (-1);
        else if (result > 100)
            safeThis->waveform.setViewChannel (result - 101);
    });
}

//==============================================================================
void TimeLoopsPanel::resized()
{
    auto area = getLocalBounds().reduced (10, 6);
    const int rowHeight = 24;

    auto row = area.removeFromTop (rowHeight);
    // 834 px in total: fits the 840 px inside the minimum window width
    startLabel.setBounds (row.removeFromLeft (34));
    startEditor.setBounds (row.removeFromLeft (84));
    row.removeFromLeft (8);
    endLabel.setBounds (row.removeFromLeft (24));
    endEditor.setBounds (row.removeFromLeft (84));
    row.removeFromLeft (8);
    lengthLabel.setBounds (row.removeFromLeft (190));
    row.removeFromLeft (8);
    countLabel.setBounds (row.removeFromLeft (64));
    countEditor.setBounds (row.removeFromLeft (52));
    row.removeFromLeft (6);
    infiniteToggle.setBounds (row.removeFromLeft (84));
    row.removeFromLeft (8);
    rateLabel.setBounds (row.removeFromLeft (34));
    rateEditor.setBounds (row.removeFromLeft (56));
    row.removeFromLeft (6);
    pitchToggle.setBounds (row.removeFromLeft (84));

    area.removeFromTop (4);
    row = area.removeFromTop (rowHeight);
    envelopeLabel.setBounds (row.removeFromLeft (104));
    envelopeToggle.setBounds (row.removeFromLeft (60));
    linearToggle.setBounds (row.removeFromLeft (140));
    lockToggle.setBounds (row.removeFromLeft (130));
    row.removeFromLeft (12);
    resetButton.setBounds (row.removeFromLeft (60));

    zoomOutButton.setBounds (row.removeFromRight (28));
    row.removeFromRight (4);
    zoomInButton.setBounds (row.removeFromRight (28));
    row.removeFromRight (4);
    zoomLabel.setBounds (row.removeFromRight (34));
    row.removeFromRight (12);
    sizeDownButton.setBounds (row.removeFromRight (28));
    row.removeFromRight (4);
    sizeUpButton.setBounds (row.removeFromRight (28));
    row.removeFromRight (4);
    sizeLabel.setBounds (row.removeFromRight (34));

    area.removeFromTop (6);
    waveform.setBounds (area);
}

void TimeLoopsPanel::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);
}

} // namespace gocue
