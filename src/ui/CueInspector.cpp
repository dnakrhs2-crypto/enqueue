#include "ui/CueInspector.h"

#include "audio/CueFileInfo.h"
#include "ui/PluginChainComponent.h"
#include "ui/UiUtils.h"

namespace gocue
{

//==============================================================================
/** Tab "기본": name, file, stop fade, gain. */
class CueInspector::BasicsPanel : public juce::Component,
                                  private juce::FileDragAndDropTarget
{
public:
    BasicsPanel (ProjectDocument& doc, AudioEngine& e, AppSettings& s)
        : document (doc), cues (doc.cues), engine (e), settings (s)
    {
        auto setupLabel = [this] (juce::Label& label, const char* text)
        {
            label.setText (ko (text), juce::dontSendNotification);
            label.setColour (juce::Label::textColourId, Palette::dimText);
            label.setFont (juce::Font (juce::FontOptions (13.0f)));
            addAndMakeVisible (label);
        };

        setupLabel (nameLabel, "이름");
        setupLabel (fileLabel, "파일");
        setupLabel (fadeOutLabel, "정지 페이드 (ms)");
        setupLabel (gainLabel, "게인 (dB)");
        setupLabel (dropHint, "파일을 여기에 끌어다 놓으면 교체됩니다");
        dropHint.setFont (juce::Font (juce::FontOptions (11.0f)));

        nameEditor.setSelectAllWhenFocused (true);
        nameEditor.onReturnKey = [this] { commitName(); nameEditor.giveAwayKeyboardFocus(); };
        nameEditor.onFocusLost = [this] { commitName(); };
        nameEditor.onEscapeKey = [this] { cancelEditAndPanic(); };
        addAndMakeVisible (nameEditor);

        filePathLabel.setColour (juce::Label::textColourId, Palette::text);
        filePathLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
        filePathLabel.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (filePathLabel);

        browseButton.setButtonText (ko ("찾아보기..."));
        browseButton.setWantsKeyboardFocus (false);
        browseButton.onClick = [this] { chooseFile(); };
        addAndMakeVisible (browseButton);

        fadeOutEditor.setInputRestrictions (7, "0123456789");
        fadeOutEditor.setJustification (juce::Justification::centredRight);
        fadeOutEditor.setSelectAllWhenFocused (true);
        fadeOutEditor.setTooltip (ko ("F(페이드아웃 정지)에 걸리는 시간. 0이면 5 ms 디클릭만"));
        fadeOutEditor.onEscapeKey = [this] { cancelEditAndPanic(); };
        fadeOutEditor.onReturnKey = [this] { commitStopFade(); fadeOutEditor.giveAwayKeyboardFocus(); };
        fadeOutEditor.onFocusLost = [this] { commitStopFade(); };
        addAndMakeVisible (fadeOutEditor);

        gainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        gainSlider.setRange (Cue::minGainDb, Cue::maxGainDb, 0.1);
        gainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
        gainSlider.setTextValueSuffix (" dB");
        gainSlider.setDoubleClickReturnValue (true, 0.0);
        gainSlider.setWantsKeyboardFocus (false);
        gainSlider.onValueChange = [this] { commitGain(); };
        addAndMakeVisible (gainSlider);
    }

    std::function<void()> onPanic;

    void refresh()
    {
        const juce::ScopedValueSetter<bool> guard (refreshing, true);
        const auto* cue = cues.getSelected();
        const bool enabled = cue != nullptr;

        for (auto* c : { static_cast<juce::Component*> (&nameEditor), static_cast<juce::Component*> (&fadeOutEditor),
                         static_cast<juce::Component*> (&gainSlider), static_cast<juce::Component*> (&browseButton) })
            c->setEnabled (enabled);

        if (cue == nullptr)
        {
            nameEditor.setText ("", false);
            filePathLabel.setText ("", juce::dontSendNotification);
            fadeOutEditor.setText ("", false);
            gainSlider.setValue (0.0, juce::dontSendNotification);
            return;
        }

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

        if (! fadeOutEditor.hasKeyboardFocus (true))
            fadeOutEditor.setText (juce::String (cue->fadeOutMs), false);

        gainSlider.setValue (cue->gainDb, juce::dontSendNotification);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 8);
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
        dropArea = filePathLabel.getBounds().expanded (2, 4);
        area.removeFromTop (2);
        dropHint.setBounds (area.removeFromTop (16).withTrimmedLeft (labelWidth));
        area.removeFromTop (4);

        row = area.removeFromTop (rowHeight);
        fadeOutLabel.setBounds (row.removeFromLeft (labelWidth));
        fadeOutEditor.setBounds (row.removeFromLeft (90));
        row.removeFromLeft (24);
        gainLabel.setBounds (row.removeFromLeft (80));
        gainSlider.setBounds (row.removeFromLeft (juce::jmin (360, row.getWidth())));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Palette::panel);

        if (dragOver)
        {
            g.setColour (Palette::standby.withAlpha (0.25f));
            g.fillRoundedRectangle (dropArea.toFloat(), 4.0f);
            g.setColour (Palette::standby);
            g.drawRoundedRectangle (dropArea.toFloat(), 4.0f, 1.5f);
        }
    }

private:
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        if (cues.getSelected() == nullptr)
            return false;

        for (const auto& path : files)
            if (isSupportedAudioFile (engine.getFormatManager(), juce::File (path)))
                return true;

        return false;
    }

    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        dragOver = false;
        repaint();

        for (const auto& path : files)
        {
            const juce::File file (path);

            if (isSupportedAudioFile (engine.getFormatManager(), file))
            {
                replaceFile (file);
                return;
            }
        }
    }

    void cancelEditAndPanic()
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

    void commitName()
    {
        if (refreshing || cancellingEdit || cues.getSelected() == nullptr)
            return;

        const auto newName = nameEditor.getText().trim();
        const int index = cues.getSelectedIndex();

        if (cues.get (index).name == newName)
            return;

        document.perform (ko ("이름 변경"), [this, index, newName]
        {
            cues.update (index, [newName] (Cue& c) { c.name = newName; });
        });
    }

    void commitStopFade()
    {
        if (refreshing || cancellingEdit || cues.getSelected() == nullptr)
            return;

        const int index = cues.getSelectedIndex();
        const int current = cues.get (index).fadeOutMs;
        const auto text = fadeOutEditor.getText().trim();

        if (text.isEmpty())
        {
            fadeOutEditor.setText (juce::String (current), false);
            return;
        }

        const int value = juce::jlimit (0, Cue::maxFadeMs, text.getIntValue());

        if (value == current)
        {
            fadeOutEditor.setText (juce::String (current), false);
            return;
        }

        document.perform (ko ("정지 페이드 변경"), [this, index, value]
        {
            cues.update (index, [value] (Cue& c) { c.fadeOutMs = value; });
        });
    }

    void commitGain()
    {
        if (refreshing || cues.getSelected() == nullptr)
            return;

        const int index = cues.getSelectedIndex();
        const double value = gainSlider.getValue();

        if (juce::approximatelyEqual (cues.get (index).gainDb, value))
            return;

        const auto id = cues.get (index).id;
        const auto key = "gain:" + id.toString();

        document.perform (ko ("게인 변경"), [this, index, value]
        {
            cues.update (index, [value] (Cue& c) { c.gainDb = value; });
        }, { key, false });

        engine.setLiveGainDb (id, value);   // a running instance follows at once
    }

    void replaceFile (const juce::File& file)
    {
        if (cues.getSelected() == nullptr)
            return;

        settings.setLastAudioDirectory (file.getParentDirectory());
        auto& formats = engine.getFormatManager();
        const int index = cues.getSelectedIndex();

        document.perform (ko ("파일 교체"), [this, index, file, &formats]
        {
            cues.update (index, [&formats, file] (Cue& c)
            {
                c.file = file;

                if (c.name.isEmpty())
                    c.name = file.getFileNameWithoutExtension();

                refreshCueFileInfo (formats, c);
            });
        });
    }

    void chooseFile()
    {
        if (cues.getSelected() == nullptr || chooser != nullptr)
            return;

        auto startDir = settings.getLastAudioDirectory();

        if (const auto* cue = cues.getSelected(); cue->file != juce::File())
            startDir = cue->file.getParentDirectory();

        chooser = std::make_unique<juce::FileChooser> (ko ("오디오 파일 선택"), startDir,
                                                       engine.getFormatManager().getWildcardForAllFormats());

        const int browseFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

        chooser->launchAsync (browseFlags, [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            chooser.reset();

            if (file != juce::File())
                replaceFile (file);
        });
    }

    ProjectDocument& document;
    CueList& cues;
    AudioEngine& engine;
    AppSettings& settings;

    juce::Label nameLabel, fileLabel, fadeOutLabel, gainLabel, dropHint, filePathLabel;
    juce::TextEditor nameEditor, fadeOutEditor;
    juce::TextButton browseButton;
    juce::Slider gainSlider;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> dropArea;
    bool refreshing = false;
    bool cancellingEdit = false;
    bool dragOver = false;
};

//==============================================================================
/** Tab "트리거": what a second GO does while the cue is running. */
class CueInspector::TriggersPanel : public juce::Component
{
public:
    explicit TriggersPanel (ProjectDocument& doc) : document (doc), cues (doc.cues)
    {
        label.setText (ko ("재생 중에 다시 GO 하면"), juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, Palette::dimText);
        label.setFont (juce::Font (juce::FontOptions (13.0f)));
        addAndMakeVisible (label);

        combo.addItem (ko ("무시 (계속 재생)"), 1);
        combo.addItem (ko ("전체 페이드 정지 시간으로 페이드 정지"), 2);
        combo.addItem (ko ("정지 페이드로 정지"), 3);
        combo.addItem (ko ("즉시 정지"), 4);
        combo.addItem (ko ("즉시 정지 후 처음부터 재시작"), 5);
        combo.addItem (ko ("이번 반복만 마치고 끝 (루프 큐)"), 6);
        combo.setWantsKeyboardFocus (false);
        combo.onChange = [this] { commit(); };
        addAndMakeVisible (combo);

        hint.setText (ko ("GO 사이 최소 시간(더블 GO 방지)은 파일 > 프로젝트 설정에 있습니다"), juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, Palette::dimText);
        hint.setFont (juce::Font (juce::FontOptions (11.0f)));
        addAndMakeVisible (hint);
    }

    void refresh()
    {
        const juce::ScopedValueSetter<bool> guard (refreshing, true);
        const auto* cue = cues.getSelected();
        combo.setEnabled (cue != nullptr);

        if (cue == nullptr)
        {
            combo.setSelectedId (0, juce::dontSendNotification);
            return;
        }

        combo.setSelectedId ((int) cue->secondTrigger + 1, juce::dontSendNotification);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 8);
        auto row = area.removeFromTop (26);
        label.setBounds (row.removeFromLeft (170));
        combo.setBounds (row.removeFromLeft (300));
        area.removeFromTop (6);
        hint.setBounds (area.removeFromTop (18));
    }

    void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

private:
    void commit()
    {
        if (refreshing || cues.getSelected() == nullptr || combo.getSelectedId() == 0)
            return;

        const auto action = (SecondTriggerAction) (combo.getSelectedId() - 1);
        const int index = cues.getSelectedIndex();

        if (cues.get (index).secondTrigger == action)
            return;

        document.perform (ko ("2차 트리거"), [this, index, action]
        {
            cues.update (index, [action] (Cue& c) { c.secondTrigger = action; });
        });
    }

    ProjectDocument& document;
    CueList& cues;
    juce::Label label, hint;
    juce::ComboBox combo;
    bool refreshing = false;
};

//==============================================================================
/** Tab "이펙트": the cue's VST3 insert chain. */
class CueInspector::EffectsPanel : public juce::Component
{
public:
    EffectsPanel (ProjectDocument& doc, AudioEngine& e, PluginWindowManager& windows)
        : document (doc), cues (doc.cues), engine (e), chainStrip (e, windows)
    {
        hint.setText (ko ("이 큐만 통과하는 VST3 인서트 (파일 → 페이드 → 게인 → 인서트 → 믹스)"), juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, Palette::dimText);
        hint.setFont (juce::Font (juce::FontOptions (12.0f)));
        addAndMakeVisible (hint);

        chainStrip.performEdit = [this] (const juce::String& name, const std::function<void()>& edit)
        {
            document.perform (name, edit, { {}, true });
        };
        addAndMakeVisible (chainStrip);
    }

    PluginChainComponent chainStrip;

    void refresh()
    {
        const auto* cue = cues.getSelected();

        if (cue == nullptr)
        {
            chainStrip.setChain (nullptr, {});
            return;
        }

        const auto cueTitle = "#" + juce::String (cues.getSelectedIndex() + 1) + " " + cue->name;
        auto* chain = &engine.getCueChain (cue->id);

        if (chainStrip.getChain() != chain)
            chainStrip.setChain (chain, cueTitle);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 8);
        hint.setBounds (area.removeFromTop (18));
        area.removeFromTop (6);
        chainStrip.setBounds (area.removeFromTop (juce::jmax (54, juce::jmin (70, area.getHeight()))));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Palette::panel);
    }

private:
    ProjectDocument& document;
    CueList& cues;
    AudioEngine& engine;
    juce::Label hint;
};

//==============================================================================
CueInspector::CueInspector (ProjectDocument& doc, AudioEngine& e, AppSettings& s, PluginWindowManager& windows)
    : document (doc), cues (doc.cues), engine (e), settings (s)
{
    title.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    title.setColour (juce::Label::textColourId, Palette::dimText);
    addAndMakeVisible (title);

    basics = new BasicsPanel (document, engine, settings);
    basics->onPanic = [this] { if (onPanic) onPanic(); };

    timeLoops = new TimeLoopsPanel (document, engine, thumbnailCache);
    timeLoops->onPanic = [this] { if (onPanic) onPanic(); };
    timeLoops->onPreview = [this] { if (onPreview) onPreview(); };
    timeLoops->onReset = [this] { if (onResetCue) onResetCue(); };

    triggers = new TriggersPanel (document);

    effects = new EffectsPanel (document, engine, windows);
    effects->chainStrip.onOpenPluginManager = [this] { if (onOpenPluginManager) onOpenPluginManager(); };

    tabs.setTabBarDepth (26);
    tabs.setOutline (0);
    tabs.setColour (juce::TabbedComponent::backgroundColourId, Palette::panel);
    tabs.addTab (ko ("기본"), Palette::panel, basics, true);
    tabs.addTab (ko ("시간·루프"), Palette::panel, timeLoops, true);
    tabs.addTab (ko ("트리거"), Palette::panel, triggers, true);
    tabs.addTab (ko ("이펙트"), Palette::panel, effects, true);
    tabs.setCurrentTabIndex (1);
    addAndMakeVisible (tabs);

    cues.addListener (this);
    refresh();
}

CueInspector::~CueInspector()
{
    cues.removeListener (this);
    tabs.clearTabs();
}

void CueInspector::pluginChainChanged (PluginChain* chain)
{
    effects->chainStrip.chainChanged (chain);
}

void CueInspector::cueChanged (int index)
{
    if (index == cues.getSelectedIndex())
        refresh();
}

void CueInspector::refreshPlugins()
{
    effects->chainStrip.refresh();
}

void CueInspector::setPlayback (const std::vector<AudioEngine::PlayingCue>& playing)
{
    const auto* cue = cues.getSelected();
    const AudioEngine::PlayingCue* found = nullptr;

    if (cue != nullptr)
        for (const auto& p : playing)
            if (p.id == cue->id)
                found = &p;

    timeLoops->setPlayback (found);
}

void CueInspector::refresh()
{
    const auto* cue = cues.getSelected();

    if (cue == nullptr)
        title.setText (ko ("큐 인스펙터 - 선택된 큐 없음"), juce::dontSendNotification);
    else
        title.setText (ko ("큐 인스펙터 - #") + juce::String (cues.getSelectedIndex() + 1) + " " + cue->name, juce::dontSendNotification);

    basics->refresh();
    timeLoops->refresh();
    triggers->refresh();
    effects->refresh();
}

void CueInspector::resized()
{
    auto area = getLocalBounds();
    title.setBounds (area.removeFromTop (24).reduced (12, 2));
    tabs.setBounds (area);
}

void CueInspector::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);
    g.setColour (Palette::outline);
    g.drawLine (0.0f, 0.5f, (float) getWidth(), 0.5f);
}

} // namespace gocue
