#include "ui/TransportBar.h"

#include "app/Commands.h"
#include "ui/GroupModeLabels.h"
#include "ui/UiUtils.h"

namespace gocue
{

namespace
{
    void drawSurface (juce::Graphics& g, juce::Rectangle<int> bounds, juce::Colour fill, juce::Colour edge,
                      bool over, bool down)
    {
        const auto r = bounds.toFloat().reduced (0.5f);
        juce::Path shape;
        shape.addRoundedRectangle (r, Palette::cornerRadius);
        g.setColour (down ? fill.darker (Palette::pressedDarken) : over ? fill.brighter (Palette::hoverBrighten) : fill);
        g.fillPath (shape);
        g.setColour (edge);
        g.strokePath (shape, juce::PathStrokeType (Palette::borderWidth));
        Palette::drawTopHighlight (g, shape, r);
    }

    int keyWidth (const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidthInt (Palette::monoFont (Palette::keySize), text) + 10;
    }

    void drawKey (juce::Graphics& g, juce::Rectangle<int> bounds, juce::Colour colour, const juce::String& text)
    {
        g.setColour (colour.withAlpha (Palette::keyAlpha));
        g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), Palette::keyRadius, Palette::borderWidth);
        g.setFont (Palette::monoFont (Palette::keySize));
        g.drawText (text, bounds, juce::Justification::centred, false);
    }
}

void TransportBar::GoButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    const auto colour = findColour (juce::TextButton::buttonColourId);
    drawSurface (g, getLocalBounds(), colour, locked ? Palette::stopButton : colour, over, down);
    if (locked)
    {
        g.setColour (Palette::stopButton);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), Palette::cornerRadius, Palette::selectionWidth);
    }
    g.setColour (findColour (juce::TextButton::textColourOffId));
    Palette::drawHeavyText (g, "GO", getLocalBounds(), Palette::goFont(), juce::Justification::centred, Palette::goTextStroke);
}

void TransportBar::TransportButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    const auto colour = findColour (juce::TextButton::textColourOffId);
    drawSurface (g, getLocalBounds(), findColour (juce::TextButton::buttonColourId), colour, over, down);
    const auto font = Palette::font (Palette::bodySize, true);
    const int captionWidth = juce::GlyphArrangement::getStringWidthInt (font, getButtonText());
    const bool stop = icon == Icon::stop;
    const int totalWidth = Palette::statusIconSize + 8 + captionWidth + (stop ? 0 : 8 + keyWidth (key));
    auto line = getLocalBounds().withSizeKeepingCentre (juce::jmin (getWidth() - 16, totalWidth), 22);
    if (stop)
        line.translate (0, -8);
    const auto iconArea = line.removeFromLeft (Palette::statusIconSize).toFloat();
    const auto c = iconArea.getCentre();
    juce::Path path;
    g.setColour (colour);
    if (icon == Icon::pause)
    {
        path.addRectangle (c.x - 4.0f, c.y - 4.0f, 2.5f, 8.0f);
        path.addRectangle (c.x + 1.5f, c.y - 4.0f, 2.5f, 8.0f);
    }
    else if (icon == Icon::fade)
    {
        path.startNewSubPath (c.x - 5.0f, c.y - 5.0f);
        path.lineTo (c.x - 1.0f, c.y + 1.0f);
        path.lineTo (c.x + 5.0f, c.y + 4.0f);
        path.lineTo (c.x - 5.0f, c.y + 4.0f);
        path.closeSubPath();
        g.strokePath (path, juce::PathStrokeType (1.5f));
        path.clear();
    }
    else
        path.addRectangle (c.x - 3.0f, c.y - 3.0f, 6.0f, 6.0f);
    g.fillPath (path);
    line.removeFromLeft (8);
    if (! stop)
    {
        drawKey (g, line.removeFromRight (keyWidth (key)).withSizeKeepingCentre (keyWidth (key), 15), colour, key);
        line.removeFromRight (8);
    }
    g.setColour (colour);
    g.setFont (font);
    g.drawText (getButtonText(), line, juce::Justification::centred, true);
    if (stop)
    {
        const int detailWidth = juce::GlyphArrangement::getStringWidthInt (Palette::font (Palette::fileSize, true), detail);
        auto bottom = getLocalBounds().withSizeKeepingCentre (keyWidth (key) + 4 + detailWidth, 16).translated (0, 11);
        drawKey (g, bottom.removeFromLeft (keyWidth (key)), colour, key);
        bottom.removeFromLeft (4);
        g.setColour (colour);
        g.setFont (Palette::font (Palette::fileSize, true));
        g.drawText (detail, bottom, juce::Justification::centredLeft, true);
    }
}

void TransportBar::GearButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    drawSurface (g, getLocalBounds(), Palette::panel2, Palette::outline, over, down);
    const auto c = getLocalBounds().toFloat().getCentre();
    g.setColour (Palette::muted);
    g.drawEllipse (c.x - 3.0f, c.y - 3.0f, 6.0f, 6.0f, 1.5f);
    for (int i = 0; i < 8; ++i)
    {
        const float a = juce::MathConstants<float>::twoPi * (float) i / 8.0f;
        g.drawLine (c.x + std::cos (a) * 5.5f, c.y + std::sin (a) * 5.5f,
                    c.x + std::cos (a) * 8.0f, c.y + std::sin (a) * 8.0f, 1.8f);
    }
}

void TransportBar::MetaLabel::paint (juce::Graphics& g)
{
    auto rest = getText();
    auto area = getLocalBounds();
    const auto labelFont = Palette::font (Palette::fileSize);
    const auto valueFont = Palette::monoFont (Palette::fileSize).boldened();
    // These are prefixes of the existing cueMeta strings, not a second source of cue descriptions.
    const juce::StringArray labels { ko ("길이 "), ko ("페이드인 "), ko ("정지 페이드 "), ko ("게인 "), ko ("패치 "),
                                    ko ("제어 큐: "), ko ("페이드 "), ko ("속도 → "), ko ("그룹 큐"), ko ("마이크 큐"), ko ("디밴프") };
    while (rest.isNotEmpty() && area.getWidth() > 0)
    {
        const int separator = rest.indexOf ("   ");
        const auto part = separator < 0 ? rest : rest.substring (0, separator);
        rest = separator < 0 ? juce::String() : rest.substring (separator + 3).trimStart();
        juce::String label, value = part;
        for (const auto& prefix : labels)
            if (part.startsWith (prefix))
            {
                label = prefix;
                value = part.substring (prefix.length());
                break;
            }
        const int labelWidth = juce::GlyphArrangement::getStringWidthInt (labelFont, label);
        g.setFont (labelFont);
        g.setColour (Palette::muted);
        g.drawText (label, area.removeFromLeft (juce::jmin (area.getWidth(), labelWidth)), juce::Justification::centredLeft, true);
        const int valueWidth = juce::GlyphArrangement::getStringWidthInt (valueFont, value);
        g.setFont (valueFont);
        g.setColour (Palette::text);
        g.drawText (value, area.removeFromLeft (juce::jmin (area.getWidth(), valueWidth)), juce::Justification::centredLeft, true);
        area.removeFromLeft (juce::jmin (16, area.getWidth()));
    }
}

TransportBar::TransportBar (juce::ApplicationCommandManager& cm)
    : commands (cm)
{
    goButton.setColour (juce::TextButton::buttonColourId, Palette::goButton);
    goButton.setColour (juce::TextButton::textColourOffId, Palette::onBright);
    goButton.setWantsKeyboardFocus (false);
    goButton.onClick = [this] { commands.invokeDirectly (CommandIDs::go, true); };
    addAndMakeVisible (goButton);

    pauseButton.setButtonText (ko ("일시정지"));
    pauseButton.key = "P";
    styleButton (pauseButton, Palette::paused);
    pauseButton.onClick = [this] { commands.invokeDirectly (CommandIDs::pauseToggle, true); };

    fadeOutButton.setButtonText (ko ("페이드아웃"));
    fadeOutButton.key = "F";
    styleButton (fadeOutButton, Palette::fadingOut);
    fadeOutButton.onClick = [this] { commands.invokeDirectly (CommandIDs::fadeOutSelected, true); };

    panicButton.setButtonText (ko ("전체 페이드 정지"));
    panicButton.key = "Esc";
    styleButton (panicButton, Palette::stopButton);
    panicButton.setColour (juce::TextButton::buttonColourId, Palette::panel2.interpolatedWith (Palette::stopButton, Palette::stopTintAlpha));
    panicButton.onClick = [this] { commands.invokeDirectly (CommandIDs::panicAll, true); };
    setPanicSeconds (panicSeconds);

    panicSettingsButton.setTooltip (ko ("전체 페이드 정지의 페이드아웃 시간 설정"));
    panicSettingsButton.setWantsKeyboardFocus (false);
    panicSettingsButton.onClick = [this]
    {
        if (onPanicSettings)
            onPanicSettings (panicSettingsButton.getScreenBounds().getBottomLeft());
    };
    addAndMakeVisible (panicSettingsButton);

    standbyTitle.setText (ko ("다음 큐"), juce::dontSendNotification);
    standbyTitle.setColour (juce::Label::textColourId, Palette::accent);
    auto kickerFont = Palette::font (Palette::kickerSize, true);
    kickerFont.setExtraKerningFactor (Palette::kickerTracking);
    standbyTitle.setFont (kickerFont);
    addAndMakeVisible (standbyTitle);

    cueNumber.setFont (Palette::monoFont (Palette::nextNameSize).boldened());
    cueNumber.setColour (juce::Label::textColourId, Palette::standby);
    cueNumber.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (cueNumber);

    cueName.setFont (Palette::font (Palette::nextNameSize, true));
    cueName.setColour (juce::Label::textColourId, Palette::text);
    addAndMakeVisible (cueName);

    cueFile.setFont (Palette::monoFont (Palette::fileSize));
    cueFile.setColour (juce::Label::textColourId, Palette::dimText);
    addAndMakeVisible (cueFile);

    cueMeta.setFont (Palette::font (Palette::fileSize));
    cueMeta.setColour (juce::Label::textColourId, Palette::dimText);
    addAndMakeVisible (cueMeta);

    momentaryLabel.setText (ko ("실시간 · LUFS"), juce::dontSendNotification);
    averageLabel.setText (ko ("평균 · LUFS"), juce::dontSendNotification);
    for (auto* label : { &momentaryLabel, &averageLabel, &momentaryValue, &averageValue })
    {
        const bool value = label == &momentaryValue || label == &averageValue;
        label->setFont (value ? Palette::monoFont (Palette::loudnessSize).boldened() : Palette::font (Palette::kickerSize));
        label->setColour (juce::Label::textColourId, value ? Palette::text : Palette::muted);
        label->setBorderSize (juce::BorderSize<int> (0));
        label->setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (label);
    }
    averageWindow.getProperties().set ("slateTextOnly", true);
    averageWindow.setColour (juce::TextButton::textColourOffId, Palette::muted);
    averageWindow.setWantsKeyboardFocus (false);
    averageWindow.onClick = [this] { showLoudnessWindowMenu(); };
    addAndMakeVisible (averageWindow);
    setLoudness (false, 0.0, false, 0.0, averageSeconds);

    playingLabel.setFont (Palette::font (Palette::fileSize));
    playingLabel.setColour (juce::Label::textColourId, Palette::dimText);
    playingLabel.setJustificationType (juce::Justification::centredRight);
    addChildComponent (playingLabel);   // retained for setPlayingCount; the visible count lives in ActiveCuesPanel

    statusLabel.setFont (Palette::font (Palette::fileSize));
    statusLabel.setJustificationType (juce::Justification::centredRight);
    addChildComponent (statusLabel);

    contextLabel.setFont (Palette::font (Palette::fileSize));
    contextLabel.setColour (juce::Label::textColourId, Palette::muted);
    contextLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (contextLabel);
    for (auto* label : { &standbyTitle, &cueNumber, &cueName, &cueFile, &contextLabel, &statusLabel })
    {
        label->setBorderSize (juce::BorderSize<int> (0));
        label->setMinimumHorizontalScale (1.0f);
    }

    setStandbyCue (-1, nullptr);
    setPlayingCount (0, 0);
}

void TransportBar::styleButton (juce::TextButton& button, juce::Colour colour)
{
    button.setColour (juce::TextButton::buttonColourId, Palette::panel2);
    button.setColour (juce::TextButton::textColourOffId, colour);
    button.setWantsKeyboardFocus (false);
    addAndMakeVisible (button);
}

void TransportBar::setStandbyCue (int index, const Cue* cue)
{
    const auto previousNumber = cueNumber.getText();
    updateStandbyCue (index, cue);
    cueNumber.setTooltip (cueNumber.getText());
    cueName.setTooltip (cueName.getText());
    cueFile.setTooltip (cueFile.getText());
    cueMeta.setTooltip (cueMeta.getText());
    if (cueNumber.getText() != previousNumber)
        resized();
}

void TransportBar::updateStandbyCue (int index, const Cue* cue)
{
    if (cue == nullptr)
    {
        cueNumber.setText ("--", juce::dontSendNotification);
        cueName.setText (ko ("선택된 큐 없음"), juce::dontSendNotification);
        cueFile.setText ("", juce::dontSendNotification);
        cueMeta.setText ("", juce::dontSendNotification);
        return;
    }

    cueNumber.setText (cue->number.isNotEmpty() ? cue->number : "#" + juce::String (index + 1), juce::dontSendNotification);   // the cue number; the row position when it has none
    cueName.setText (cue->name.isNotEmpty() ? cue->name : ko ("(이름 없음)"), juce::dontSendNotification);

    if (cue->isGroup())
    {
        cueFile.setText (describeGroup ? describeGroup (*cue) : ko ("그룹"), juce::dontSendNotification);
        cueFile.setColour (juce::Label::textColourId, Palette::dimText);
        cueMeta.setText (ko ("그룹 큐   ") + groupPresetName (cue->group) + ko (" — ") + groupPresetDescription (cue->group), juce::dontSendNotification);
        return;
    }

    if (cue->isMic())
    {
        cueFile.setText (ko ("장치 입력 ") + juce::String (cue->mic.firstInput + 1) + (cue->mic.numInputs > 1 ? "-" + juce::String (cue->mic.firstInput + cue->mic.numInputs) : juce::String()), juce::dontSendNotification);
        cueFile.setColour (juce::Label::textColourId, Palette::dimText);
        cueMeta.setText (ko ("마이크 큐   정지할 때까지   정지 페이드 ") + juce::String (cue->fadeOutMs) + ko (" ms   게인 ") + juce::String (cue->gainDb, 1) + " dB", juce::dontSendNotification);
        return;
    }

    if (cue->isControl())
    {
        static const char* const names[] = { "시작", "정지", "일시정지", "로드", "리셋", "이동", "대기", "메모", "활성화", "비활성화", "대상 변경" };
        const auto kindName = ko (names[juce::jlimit (0, 10, (int) cue->control.kind)]);

        if (cue->control.needsTarget())
        {
            const auto target = describeFadeTarget ? describeFadeTarget (*cue) : juce::String();
            cueFile.setText (target.isNotEmpty() ? target : ko ("대상 없음"), juce::dontSendNotification);
            cueFile.setColour (juce::Label::textColourId, target.isNotEmpty() ? Palette::dimText : Palette::missing);
        }
        else
        {
            cueFile.setText (cue->control.kind == ControlKind::wait ? ko ("대기 ") + juce::String (cue->control.seconds, 2) + ko ("초") : ko ("메모"), juce::dontSendNotification);
            cueFile.setColour (juce::Label::textColourId, Palette::dimText);
        }

        cueMeta.setText (ko ("제어 큐: ") + kindName, juce::dontSendNotification);
        return;
    }

    if (cue->isDevamp())
    {
        const auto target = describeFadeTarget ? describeFadeTarget (*cue) : juce::String();
        cueFile.setText (target.isNotEmpty() ? target : ko ("디밴프 대상 없음"), juce::dontSendNotification);
        cueFile.setColour (juce::Label::textColourId, target.isNotEmpty() ? Palette::dimText : Palette::missing);
        cueMeta.setText (ko ("디밴프") + (cue->devamp.stopTarget ? ko ("   반복 끝에서 대상 정지") : ko ("   반복 끝에서 이어감"))
                         + (cue->devamp.startNextCue ? ko ("   그 순간 다음 큐 시작") : juce::String()), juce::dontSendNotification);
        return;
    }

    if (cue->isFade())
    {
        const auto target = describeFadeTarget ? describeFadeTarget (*cue) : juce::String();
        cueFile.setText (target.isNotEmpty() ? target : ko ("페이드 대상 없음"), juce::dontSendNotification);
        cueFile.setColour (juce::Label::textColourId, target.isNotEmpty() ? Palette::dimText : Palette::missing);

        juce::String meta;
        meta << ko ("페이드 ") << formatSeconds (cue->fade.durationSeconds)
             << (cue->fade.relative ? ko ("   상대") : ko ("   절대"))
             << (cue->fade.fadeLevels ? ko ("   레벨") : juce::String())
             << (cue->fade.fadeRate ? ko ("   속도 → ") + juce::String (cue->fade.rate, 2) : juce::String())
             << (cue->fade.stopTargetWhenDone ? ko ("   완료 시 정지") : juce::String());
        cueMeta.setText (meta, juce::dontSendNotification);
        return;
    }

    if (cue->file == juce::File())
        cueFile.setText (ko ("파일 없음"), juce::dontSendNotification);
    else
        cueFile.setText (cue->file.getFileName(), juce::dontSendNotification);

    cueFile.setColour (juce::Label::textColourId, cue->fileMissing ? Palette::missing : Palette::dimText);

    juce::String meta;
    const double effective = cue->effectiveLength();
    meta << ko ("길이 ") << (effective < 0.0 ? juce::String::fromUTF8 ("\xE2\x88\x9E") : formatSeconds (effective > 0.0 ? effective : cue->durationSeconds))
         << "   " << ko ("페이드인 ") << juce::roundToInt (cue->audio.envelope.fadeInSeconds (cue->regionLength()) * 1000.0) << " ms"
         << "   " << ko ("정지 페이드 ") << cue->fadeOutMs << " ms"
         << "   " << ko ("게인 ") << juce::String (cue->gainDb, 1) << " dB";
    if (describePatch)
        meta << ko ("   패치 ") << describePatch (*cue);
    cueMeta.setText (meta, juce::dontSendNotification);
}

void TransportBar::setContextText (const juce::String& text)
{
    contextLabel.setText (text, juce::dontSendNotification);
    contextLabel.setTooltip (text);
}

void TransportBar::setPlayingCount (int numPlaying, int numPaused)
{
    juce::String text = ko ("재생 중 ") + juce::String (numPlaying);

    if (numPaused > 0)
        text << ko ("  (일시정지 ") << numPaused << ")";

    playingLabel.setText (text, juce::dontSendNotification);
    playingLabel.setColour (juce::Label::textColourId, numPlaying > 0 ? Palette::playing : Palette::dimText);
}

void TransportBar::showStatus (const juce::String& message, bool isError)
{
    statusLabel.setText (message, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, isError ? Palette::missing : Palette::dimText);
    statusLabel.setTooltip (message);
    statusLabel.setVisible (true);
    contextLabel.setVisible (false);
    startTimer (isError ? 6000 : 3000);
}

void TransportBar::setGoLocked (bool locked)
{
    if (goLocked == locked)
        return;

    goLocked = locked;
    updateGoLook();
}

void TransportBar::flashGoRejected()
{
    goFlashing = true;
    updateGoLook();

    juce::Component::SafePointer<TransportBar> safeThis (this);
    juce::Timer::callAfterDelay (180, [safeThis]
    {
        if (safeThis != nullptr)
        {
            safeThis->goFlashing = false;
            safeThis->updateGoLook();
        }
    });
}

void TransportBar::setAuditionMode (bool auditioning)
{
    if (auditionMode == auditioning)
        return;

    auditionMode = auditioning;
    goButton.setTooltip (auditionMode ? ko ("GO (오디션)") : juce::String ("GO"));
    updateGoLook();
}

void TransportBar::updateGoLook()
{
    goButton.setColour (juce::TextButton::buttonColourId, goFlashing ? Palette::stopButton : (auditionMode ? Palette::standby : Palette::goButton));
    goButton.setColour (juce::TextButton::textColourOffId, auditionMode ? Palette::accentInk : Palette::onBright);
    goButton.locked = goLocked || goFlashing;
    goButton.repaint();
    repaint();
}

void TransportBar::timerCallback()
{
    stopTimer();
    statusLabel.setText ("", juce::dontSendNotification);
    statusLabel.setVisible (false);
    contextLabel.setVisible (true);
}

void TransportBar::setPanicSeconds (double seconds)
{
    panicSeconds = seconds;
    const bool whole = std::abs (seconds - std::round (seconds)) < 0.001;
    panicButton.detail = juce::String (seconds, whole ? 0 : 1) + ko ("초");
    panicButton.setTooltip (ko ("전체 페이드 정지 (Esc) ") + panicButton.detail);
    panicButton.repaint();
}

void TransportBar::setLoudness (bool momentaryValid, double momentaryLufs, bool averageValid, double averageLufs, int windowSeconds)
{
    const auto reading = [] (bool valid, double value)
    {
        return valid && std::isfinite (value) ? juce::String (value, 1) : ko ("—");
    };
    momentaryValue.setText (reading (momentaryValid, momentaryLufs), juce::dontSendNotification);
    averageValue.setText (reading (averageValid, averageLufs), juce::dontSendNotification);
    averageSeconds = windowSeconds;
    const auto caption = juce::String (windowSeconds) + ko ("초 ▾");
    if (averageWindow.getButtonText() != caption)
        averageWindow.setButtonText (caption);
}

void TransportBar::showLoudnessWindowMenu()
{
    juce::PopupMenu menu;
    for (const int seconds : { 5, 10, 20, 30, 60 })
        menu.addItem (seconds, juce::String (seconds) + ko ("초"), true, averageSeconds == seconds);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&averageWindow),
                       [safeThis = juce::Component::SafePointer<TransportBar> (this)] (int seconds)
    {
        if (safeThis != nullptr && seconds > 0 && safeThis->onLufsAverageSecondsChanged)
            safeThis->onLufsAverageSecondsChanged (seconds);
    });
}

void TransportBar::resized()
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;
    auto area = getLocalBounds();
    goButton.setBounds (area.removeFromLeft (juce::jmin (Palette::goWidth, getWidth() / 6)));
    area.removeFromLeft (Palette::gap);
    auto right = area.removeFromRight (juce::jlimit (Palette::minTransportWidth, Palette::transportWidth, getWidth() / 4));
    const int halfWidth = juce::jmax (0, (right.getWidth() - Palette::buttonGap) / 2);
    auto top = right.removeFromTop (juce::jmax (0, (right.getHeight() - Palette::buttonGap) / 2));
    pauseButton.setBounds (top.removeFromLeft (halfWidth));
    top.removeFromLeft (Palette::buttonGap);
    fadeOutButton.setBounds (top);
    right.removeFromTop (Palette::buttonGap);
    panicButton.setBounds (right.removeFromLeft (halfWidth));
    right.removeFromLeft (Palette::buttonGap);
    panicSettingsButton.setBounds (right);
    area.removeFromRight (Palette::gap);
    nextCard = area;
    const juce::Rectangle<int> surfaces[] = { nextCard, goButton.getBounds(), pauseButton.getBounds(), fadeOutButton.getBounds(),
                                             panicButton.getBounds(), panicSettingsButton.getBounds() };
    for (int i = 0; i < 6; ++i)
        shadows[i].resize (surfaces[i]);
    area.reduce (16, 12);
    auto heading = area.removeFromTop (18);
    standbyTitle.setBounds (heading.removeFromLeft (60));
    contextLabel.setBounds (heading);
    statusLabel.setBounds (heading);
    area.removeFromTop (4);
    auto readings = area.removeFromRight (juce::jmin (Palette::loudnessWidth, area.getWidth() / 2));
    auto live = readings.removeFromLeft (readings.getWidth() / 2);
    live.removeFromRight (Palette::buttonGap);
    momentaryLabel.setBounds (live.removeFromTop (18));
    momentaryValue.setBounds (live.removeFromTop (32));
    auto averageHeading = readings.removeFromTop (18);
    averageWindow.setBounds (averageHeading.removeFromLeft (Palette::loudnessWindowWidth));
    averageLabel.setBounds (averageHeading);
    averageValue.setBounds (readings.removeFromTop (32));
    area.removeFromRight (Palette::buttonGap);
    auto main = area.removeFromTop (33);
    const int numberWidth = juce::GlyphArrangement::getStringWidthInt (cueNumber.getFont(), cueNumber.getText());
    cueNumber.setBounds (main.removeFromLeft (juce::jmin (numberWidth, juce::jmax (0, main.getWidth() / 3))));
    main.removeFromLeft (10);
    cueName.setBounds (main);
    area.removeFromTop (4);
    cueFile.setBounds (area.removeFromTop (18));
    area.removeFromTop (4);
    cueMeta.setBounds (area.removeFromTop (18));
}

void TransportBar::paint (juce::Graphics& g)
{
    for (const auto& shadow : shadows)
        shadow.draw (g);
    Palette::drawCard (g, nextCard);
}

} // namespace gocue
