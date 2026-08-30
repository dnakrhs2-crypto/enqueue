#include "ui/CueInspector.h"

#include "audio/CueFileInfo.h"
#include "ui/UiUtils.h"

namespace gocue
{

CueInspector::CueInspector (CueList& c, juce::AudioFormatManager& f, AppSettings& s)
    : cues (c), formats (f), settings (s)
{
    title.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    title.setColour (juce::Label::textColourId, Palette::dimText);
    addAndMakeVisible (title);

    auto setupLabel = [this] (juce::Label& label, const char* text)
    {
        label.setText (ko (text), juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, Palette::dimText);
        label.setFont (juce::Font (juce::FontOptions (13.0f)));
        addAndMakeVisible (label);
    };

    setupLabel (nameLabel, "이름");
    setupLabel (fileLabel, "파일");
    setupLabel (fadeInLabel, "페이드인 (ms)");
    setupLabel (fadeOutLabel, "페이드아웃 (ms)");
    setupLabel (gainLabel, "게인 (dB)");
    setupLabel (pluginsLabel, "VST3 인서트");

    nameEditor.setSelectAllWhenFocused (true);
    nameEditor.onReturnKey = [this] { commitName(); nameEditor.giveAwayKeyboardFocus(); };
    nameEditor.onFocusLost = [this] { commitName(); };
    addAndMakeVisible (nameEditor);

    filePathLabel.setColour (juce::Label::textColourId, Palette::text);
    filePathLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    filePathLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (filePathLabel);

    browseButton.setButtonText (ko ("찾아보기..."));
    browseButton.setWantsKeyboardFocus (false);
    browseButton.onClick = [this] { chooseFile(); };
    addAndMakeVisible (browseButton);

    setupNumberEditor (fadeInEditor);
    fadeInEditor.onReturnKey = [this] { commitFade (fadeInEditor, true); fadeInEditor.giveAwayKeyboardFocus(); };
    fadeInEditor.onFocusLost = [this] { commitFade (fadeInEditor, true); };

    setupNumberEditor (fadeOutEditor);
    fadeOutEditor.onReturnKey = [this] { commitFade (fadeOutEditor, false); fadeOutEditor.giveAwayKeyboardFocus(); };
    fadeOutEditor.onFocusLost = [this] { commitFade (fadeOutEditor, false); };

    gainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    gainSlider.setRange (Cue::minGainDb, Cue::maxGainDb, 0.1);
    gainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
    gainSlider.setTextValueSuffix (" dB");
    gainSlider.setDoubleClickReturnValue (true, 0.0);
    gainSlider.setWantsKeyboardFocus (false);
    gainSlider.onValueChange = [this] { commitGain(); };
    addAndMakeVisible (gainSlider);

    pluginsPlaceholder.setText (ko ("플러그인 슬롯은 다음 단계에서 추가됩니다."), juce::dontSendNotification);
    pluginsPlaceholder.setColour (juce::Label::textColourId, Palette::dimText);
    pluginsPlaceholder.setFont (juce::Font (juce::FontOptions (13.0f)));
    addAndMakeVisible (pluginsPlaceholder);

    cues.addListener (this);
    refresh();
}

CueInspector::~CueInspector()
{
    cues.removeListener (this);
}

void CueInspector::setupNumberEditor (juce::TextEditor& editor)
{
    editor.setInputRestrictions (7, "0123456789");
    editor.setJustification (juce::Justification::centredRight);
    editor.setSelectAllWhenFocused (true);
    addAndMakeVisible (editor);
}

void CueInspector::cueChanged (int index)
{
    if (index == cues.getSelectedIndex())
        refresh();
}

void CueInspector::refresh()
{
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    const auto* cue = cues.getSelected();
    const bool enabled = cue != nullptr;

    for (auto* c : { static_cast<juce::Component*> (&nameEditor), static_cast<juce::Component*> (&fadeInEditor),
                     static_cast<juce::Component*> (&fadeOutEditor), static_cast<juce::Component*> (&gainSlider),
                     static_cast<juce::Component*> (&browseButton) })
        c->setEnabled (enabled);

    if (cue == nullptr)
    {
        title.setText (ko ("큐 인스펙터 - 선택된 큐 없음"), juce::dontSendNotification);
        nameEditor.setText ("", false);
        filePathLabel.setText ("", juce::dontSendNotification);
        fadeInEditor.setText ("", false);
        fadeOutEditor.setText ("", false);
        gainSlider.setValue (0.0, juce::dontSendNotification);
        return;
    }

    title.setText (ko ("큐 인스펙터 - #") + juce::String (cues.getSelectedIndex() + 1), juce::dontSendNotification);

    if (! nameEditor.hasKeyboardFocus (true))
        nameEditor.setText (cue->name, false);

    if (cue->file == juce::File())
    {
        filePathLabel.setText (ko ("파일 없음"), juce::dontSendNotification);
        filePathLabel.setColour (juce::Label::textColourId, Palette::missing);
    }
    else
    {
        filePathLabel.setText ((cue->fileMissing ? ko ("[없음] ") : juce::String()) + cue->file.getFullPathName(),
                               juce::dontSendNotification);
        filePathLabel.setColour (juce::Label::textColourId, cue->fileMissing ? Palette::missing : Palette::text);
    }

    if (! fadeInEditor.hasKeyboardFocus (true))
        fadeInEditor.setText (juce::String (cue->fadeInMs), false);

    if (! fadeOutEditor.hasKeyboardFocus (true))
        fadeOutEditor.setText (juce::String (cue->fadeOutMs), false);

    gainSlider.setValue (cue->gainDb, juce::dontSendNotification);
}

void CueInspector::commitName()
{
    if (refreshing || cues.getSelected() == nullptr)
        return;

    const auto newName = nameEditor.getText().trim();
    const int index = cues.getSelectedIndex();

    if (cues.get (index).name == newName)
        return;

    cues.update (index, [newName] (Cue& c) { c.name = newName; });
}

void CueInspector::commitFade (juce::TextEditor& editor, bool isFadeIn)
{
    if (refreshing || cues.getSelected() == nullptr)
        return;

    const int index = cues.getSelectedIndex();
    const auto& cue = cues.get (index);
    const int current = isFadeIn ? cue.fadeInMs : cue.fadeOutMs;
    const auto text = editor.getText().trim();

    if (text.isEmpty())
    {
        editor.setText (juce::String (current), false);
        return;
    }

    const int value = juce::jlimit (0, Cue::maxFadeMs, text.getIntValue());

    if (value == current)
    {
        editor.setText (juce::String (current), false);
        return;
    }

    cues.update (index, [value, isFadeIn] (Cue& c)
    {
        if (isFadeIn)
            c.fadeInMs = value;
        else
            c.fadeOutMs = value;
    });
}

void CueInspector::commitGain()
{
    if (refreshing || cues.getSelected() == nullptr)
        return;

    const int index = cues.getSelectedIndex();
    const double value = gainSlider.getValue();

    if (juce::approximatelyEqual (cues.get (index).gainDb, value))
        return;

    cues.update (index, [value] (Cue& c) { c.gainDb = value; });
}

void CueInspector::chooseFile()
{
    if (cues.getSelected() == nullptr || chooser != nullptr)
        return;

    auto startDir = settings.getLastAudioDirectory();

    if (const auto* cue = cues.getSelected(); cue->file != juce::File())
        startDir = cue->file.getParentDirectory();

    chooser = std::make_unique<juce::FileChooser> (ko ("오디오 파일 선택"), startDir, formats.getWildcardForAllFormats());

    const int browseFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    chooser->launchAsync (browseFlags, [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        chooser.reset();

        if (file == juce::File() || cues.getSelected() == nullptr)
            return;

        settings.setLastAudioDirectory (file.getParentDirectory());

        cues.update (cues.getSelectedIndex(), [this, file] (Cue& c)
        {
            c.file = file;

            if (c.name.isEmpty())
                c.name = file.getFileNameWithoutExtension();

            refreshCueFileInfo (formats, c);
        });
    });
}

void CueInspector::resized()
{
    auto area = getLocalBounds().reduced (12, 8);
    title.setBounds (area.removeFromTop (20));
    area.removeFromTop (6);

    const int labelWidth = 110;
    const int rowHeight = 26;

    auto row = area.removeFromTop (rowHeight);
    nameLabel.setBounds (row.removeFromLeft (labelWidth));
    nameEditor.setBounds (row.removeFromLeft (juce::jmin (420, row.getWidth())));
    area.removeFromTop (6);

    row = area.removeFromTop (rowHeight);
    fileLabel.setBounds (row.removeFromLeft (labelWidth));
    browseButton.setBounds (row.removeFromRight (110));
    row.removeFromRight (8);
    filePathLabel.setBounds (row);
    area.removeFromTop (6);

    row = area.removeFromTop (rowHeight);
    fadeInLabel.setBounds (row.removeFromLeft (labelWidth));
    fadeInEditor.setBounds (row.removeFromLeft (90));
    row.removeFromLeft (24);
    fadeOutLabel.setBounds (row.removeFromLeft (labelWidth));
    fadeOutEditor.setBounds (row.removeFromLeft (90));
    row.removeFromLeft (24);
    gainLabel.setBounds (row.removeFromLeft (80));
    gainSlider.setBounds (row.removeFromLeft (juce::jmin (360, row.getWidth())));
    area.removeFromTop (10);

    row = area.removeFromTop (rowHeight);
    pluginsLabel.setBounds (row.removeFromLeft (labelWidth));
    pluginsPlaceholder.setBounds (row);
}

void CueInspector::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);
    g.setColour (Palette::outline);
    g.drawLine (0.0f, 0.5f, (float) getWidth(), 0.5f);
}

} // namespace gocue
