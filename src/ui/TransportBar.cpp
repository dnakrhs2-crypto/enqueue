#include "ui/TransportBar.h"

#include "app/Commands.h"
#include "ui/UiUtils.h"

namespace gocue
{

TransportBar::TransportBar (juce::ApplicationCommandManager& cm)
    : commands (cm)
{
    goButton.setColour (juce::TextButton::buttonColourId, Palette::goButton);
    goButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    goButton.setWantsKeyboardFocus (false);
    goButton.onClick = [this] { commands.invokeDirectly (CommandIDs::go, true); };
    addAndMakeVisible (goButton);

    pauseButton.setButtonText (ko ("일시정지 (P)"));
    styleButton (pauseButton, Palette::standby.darker (0.4f));
    pauseButton.onClick = [this] { commands.invokeDirectly (CommandIDs::pauseToggle, true); };

    fadeOutButton.setButtonText (ko ("페이드아웃 (F)"));
    styleButton (fadeOutButton, Palette::fadingOut);
    fadeOutButton.onClick = [this] { commands.invokeDirectly (CommandIDs::fadeOutSelected, true); };

    panicButton.setButtonText (ko ("전체 페이드 정지 (Esc)"));
    styleButton (panicButton, Palette::stopButton);
    panicButton.onClick = [this] { commands.invokeDirectly (CommandIDs::panicAll, true); };

    standbyTitle.setText (ko ("다음 큐"), juce::dontSendNotification);
    standbyTitle.setColour (juce::Label::textColourId, Palette::dimText);
    standbyTitle.setFont (juce::Font (juce::FontOptions (13.0f)));
    addAndMakeVisible (standbyTitle);

    cueNumber.setFont (juce::Font (juce::FontOptions (30.0f, juce::Font::bold)));
    cueNumber.setColour (juce::Label::textColourId, Palette::standby);
    cueNumber.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (cueNumber);

    cueName.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
    cueName.setColour (juce::Label::textColourId, Palette::text);
    addAndMakeVisible (cueName);

    cueFile.setFont (juce::Font (juce::FontOptions (13.0f)));
    cueFile.setColour (juce::Label::textColourId, Palette::dimText);
    addAndMakeVisible (cueFile);

    cueMeta.setFont (juce::Font (juce::FontOptions (13.0f)));
    cueMeta.setColour (juce::Label::textColourId, Palette::dimText);
    addAndMakeVisible (cueMeta);

    playingLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    playingLabel.setColour (juce::Label::textColourId, Palette::dimText);
    playingLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (playingLabel);

    statusLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    statusLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (statusLabel);

    setStandbyCue (-1, nullptr);
    setPlayingCount (0, 0);
}

void TransportBar::styleButton (juce::TextButton& button, juce::Colour colour)
{
    button.setColour (juce::TextButton::buttonColourId, colour);
    button.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    button.setWantsKeyboardFocus (false);
    addAndMakeVisible (button);
}

void TransportBar::setStandbyCue (int index, const Cue* cue)
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
        cueMeta.setText (ko ("그룹 큐   ") + (cue->group.mode == GroupMode::timeline ? ko ("자식 전부 동시에 시작 (각자 프리웨이트)")
                                            : cue->group.mode == GroupMode::playlist ? ko ("자식 차례로 재생 (두 번째 GO = 다음 곡)")
                                            : cue->group.mode == GroupMode::startFirstEnter ? ko ("첫 자식 시작, 플레이헤드는 그룹 안으로")
                                            : cue->group.mode == GroupMode::startFirst ? ko ("첫 자식 시작, 플레이헤드는 그룹 뒤로")
                                            : ko ("자식 중 하나를 랜덤으로 (한 바퀴에 한 번씩)")), juce::dontSendNotification);
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
    cueMeta.setText (meta, juce::dontSendNotification);
}

void TransportBar::setPlayingCount (int numPlaying, int numPaused)
{
    juce::String text = ko ("재생 중 ") + juce::String (numPlaying);

    if (numPaused > 0)
        text << ko ("  (일시정지 ") << numPaused << ")";

    playingLabel.setText (text, juce::dontSendNotification);
    playingLabel.setColour (juce::Label::textColourId, numPlaying > 0 ? Palette::playing.brighter (0.6f) : Palette::dimText);
}

void TransportBar::showStatus (const juce::String& message, bool isError)
{
    statusLabel.setText (message, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, isError ? Palette::missing : Palette::dimText);
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
    goButton.setButtonText (auditionMode ? ko ("GO (\xEC\x98\xA4\xEB\x94\x94\xEC\x85\x98)") : juce::String ("GO"));   // GO (오디션)
    updateGoLook();
}

void TransportBar::updateGoLook()
{
    goButton.setColour (juce::TextButton::buttonColourId, goFlashing ? Palette::stopButton : (auditionMode ? Palette::standby : Palette::goButton));
    repaint();
}

void TransportBar::timerCallback()
{
    stopTimer();
    statusLabel.setText ("", juce::dontSendNotification);
}

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced (10, 8);

    goButton.setBounds (area.removeFromLeft (150));
    area.removeFromLeft (14);

    auto right = area.removeFromRight (230);
    auto buttons = right.removeFromBottom (28);
    panicButton.setBounds (buttons);
    right.removeFromBottom (4);
    buttons = right.removeFromBottom (28);
    pauseButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 2));
    buttons.removeFromLeft (4);
    fadeOutButton.setBounds (buttons);
    playingLabel.setBounds (right.removeFromTop (22));
    statusLabel.setBounds (right);

    area.removeFromRight (10);

    standbyTitle.setBounds (area.removeFromTop (16));
    auto main = area.removeFromTop (36);
    cueNumber.setBounds (main.removeFromLeft (70));
    cueName.setBounds (main);
    cueFile.setBounds (area.removeFromTop (18));
    cueMeta.setBounds (area.removeFromTop (18));
}

void TransportBar::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);
    g.setColour (Palette::outline);
    g.drawLine (0.0f, (float) getHeight() - 0.5f, (float) getWidth(), (float) getHeight() - 0.5f);

    if (goLocked || goFlashing)
    {
        g.setColour (goFlashing ? Palette::missing : Palette::stopButton.brighter (0.4f));
        g.drawRect (goButton.getBounds().expanded (3), 3);
    }
}

} // namespace gocue
