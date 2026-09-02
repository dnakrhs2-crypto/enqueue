#include "ui/CueInspector.h"

#include "audio/CueFileInfo.h"
#include "model/CueColors.h"
#include "ui/PluginChainComponent.h"
#include "ui/UiUtils.h"

namespace gocue
{

namespace
{
    void styleLabel (juce::Label& label, const juce::String& text, float size = 13.0f)
    {
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, Palette::dimText);
        label.setFont (juce::Font (juce::FontOptions (size)));
    }

    void styleToggle (juce::ToggleButton& toggle, const juce::String& text)
    {
        toggle.setButtonText (text);
        toggle.setColour (juce::ToggleButton::textColourId, Palette::text);
        toggle.setColour (juce::ToggleButton::tickColourId, Palette::standby);
        toggle.setWantsKeyboardFocus (false);
    }

    void styleNumberEditor (juce::TextEditor& editor, const juce::String& allowed, int maxLength)
    {
        editor.setInputRestrictions (maxLength, allowed);
        editor.setJustification (juce::Justification::centredRight);
        editor.setSelectAllWhenFocused (true);
    }

    void fillColourCombo (juce::ComboBox& combo)
    {
        combo.addItem (ko ("없음"), 1);

        for (int i = 1; i <= CueColors::numColors; ++i)
            combo.addItem (juce::String::fromUTF8 (CueColors::name (i)), i + 1);
    }
}

//==============================================================================
/** A button that captures the next key press as the cue's hotkey. */
class HotkeyButton : public juce::TextButton
{
public:
    HotkeyButton() { setWantsKeyboardFocus (false); }

    std::function<void (const juce::String& description)> onHotkeyChanged;

    void setHotkey (const juce::String& description)
    {
        hotkey = description;

        if (! capturing)
            setButtonText (hotkey.isEmpty() ? ko ("핫키: 없음") : ko ("핫키: ") + hotkey);
    }

    void clicked() override
    {
        capturing = true;
        setWantsKeyboardFocus (true);
        grabKeyboardFocus();
        setButtonText (ko ("키를 누르세요... (Esc 취소)"));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (! capturing)
            return false;

        finishCapture();

        if (key.isKeyCode (juce::KeyPress::escapeKey))
            return true;

        if (key.getModifiers().isAnyModifierKeyDown() && key.getKeyCode() == 0)
            return true;   // a lone modifier

        if (onHotkeyChanged)
            onHotkeyChanged (key.getTextDescription());

        return true;
    }

    void focusLost (FocusChangeType) override
    {
        if (capturing)
            finishCapture();
    }

private:
    void finishCapture()
    {
        capturing = false;
        setWantsKeyboardFocus (false);
        setHotkey (hotkey);
    }

    juce::String hotkey;
    bool capturing = false;
};

//==============================================================================
/** Tab "기본". */
class CueInspector::BasicsPanel : public juce::Component,
                                  private juce::FileDragAndDropTarget
{
public:
    BasicsPanel (ProjectDocument& doc, AudioEngine& e, AppSettings& s)
        : document (doc), cues (doc.cues), engine (e), settings (s)
    {
        for (auto* l : { &numberLabel, &nameLabel, &colourLabel, &fileLabel, &preLabel, &postLabel, &continueLabel,
                         &fadeOutLabel, &gainLabel, &notesLabel })
            addAndMakeVisible (l);

        styleLabel (numberLabel, ko ("번호"));
        styleLabel (nameLabel, ko ("이름"));
        styleLabel (colourLabel, ko ("색"));
        styleLabel (fileLabel, ko ("파일"));
        styleLabel (preLabel, ko ("프리웨이트"));
        styleLabel (postLabel, ko ("포스트웨이트"));
        styleLabel (continueLabel, ko ("진행"));
        styleLabel (fadeOutLabel, ko ("정지 페이드 (ms)"));
        styleLabel (gainLabel, ko ("게인 (dB)"));
        styleLabel (notesLabel, ko ("메모"));

        auto textEditor = [this] (juce::TextEditor& editor, std::function<void()> commit)
        {
            editor.setSelectAllWhenFocused (true);
            editor.onReturnKey = [commit, &editor] { commit(); editor.giveAwayKeyboardFocus(); };
            editor.onFocusLost = commit;
            editor.onEscapeKey = [this] { cancelEditAndPanic(); };
            addAndMakeVisible (editor);
        };

        textEditor (numberEditor, [this] { commitNumber(); });
        numberEditor.setJustification (juce::Justification::centredLeft);
        textEditor (nameEditor, [this] { commitName(); });
        textEditor (preEditor, [this] { commitWait (true); });
        styleNumberEditor (preEditor, "0123456789:.", 12);
        textEditor (postEditor, [this] { commitWait (false); });
        styleNumberEditor (postEditor, "0123456789:.", 12);
        textEditor (fadeOutEditor, [this] { commitStopFade(); });
        styleNumberEditor (fadeOutEditor, "0123456789", 7);
        fadeOutEditor.setTooltip (ko ("F(페이드아웃 정지)에 걸리는 시간. 0이면 5 ms 디클릭만"));

        fillColourCombo (colourCombo);
        colourCombo.setWantsKeyboardFocus (false);
        colourCombo.onChange = [this] { commitColour (false); };
        addAndMakeVisible (colourCombo);

        styleToggle (secondColourToggle, ko ("두 번째 색"));
        secondColourToggle.setTooltip (ko ("한 번 재생한 뒤에는 이 색으로 표시"));
        secondColourToggle.onClick = [this]
        {
            const bool on = secondColourToggle.getToggleState();
            edit (ko ("두 번째 색"), [on] (Cue& c) { c.useSecondColor = on; });
        };
        addAndMakeVisible (secondColourToggle);

        fillColourCombo (secondColourCombo);
        secondColourCombo.setWantsKeyboardFocus (false);
        secondColourCombo.onChange = [this] { commitColour (true); };
        addAndMakeVisible (secondColourCombo);

        filePathLabel.setColour (juce::Label::textColourId, Palette::text);
        filePathLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
        filePathLabel.setMinimumHorizontalScale (1.0f);
        filePathLabel.setTooltip (ko ("파일을 여기에 끌어다 놓으면 교체됩니다"));
        addAndMakeVisible (filePathLabel);

        browseButton.setButtonText (ko ("찾아보기..."));
        browseButton.setWantsKeyboardFocus (false);
        browseButton.onClick = [this] { chooseFile(); };
        addAndMakeVisible (browseButton);

        continueCombo.addItem (ko ("계속 안 함"), 1);
        continueCombo.addItem (ko ("자동 계속"), 2);
        continueCombo.addItem (ko ("자동 팔로우"), 3);
        continueCombo.setTooltip (ko ("자동 계속 = 포스트웨이트 뒤 다음 큐 시작 / 자동 팔로우 = 이 큐가 끝나면 다음 큐 시작"));
        continueCombo.setWantsKeyboardFocus (false);
        continueCombo.onChange = [this]
        {
            if (refreshing || continueCombo.getSelectedId() == 0)
                return;

            const auto mode = (ContinueMode) (continueCombo.getSelectedId() - 1);
            edit (ko ("진행 모드"), [mode] (Cue& c) { c.continueMode = mode; });
        };
        addAndMakeVisible (continueCombo);

        hotkeyButton.onHotkeyChanged = [this] (const juce::String& description)
        {
            edit (ko ("핫키"), [description] (Cue& c) { c.hotkey = description; });
        };
        addAndMakeVisible (hotkeyButton);

        clearHotkeyButton.setButtonText ("x");
        clearHotkeyButton.setTooltip (ko ("핫키 지우기"));
        clearHotkeyButton.setWantsKeyboardFocus (false);
        clearHotkeyButton.onClick = [this] { edit (ko ("핫키 지우기"), [] (Cue& c) { c.hotkey.clear(); }); };
        addAndMakeVisible (clearHotkeyButton);

        auto toggle = [this] (juce::ToggleButton& t, const char* text, const char* editName, std::function<void (Cue&, bool)> apply)
        {
            styleToggle (t, ko (text));
            t.onClick = [this, &t, editName, apply]
            {
                const bool on = t.getToggleState();
                edit (ko (editName), [apply, on] (Cue& c) { apply (c, on); });
            };
            addAndMakeVisible (t);
        };

        toggle (flagToggle, "깃발", "깃발", [] (Cue& c, bool v) { c.flagged = v; });
        toggle (armedToggle, "아밍 (활성)", "아밍", [] (Cue& c, bool v) { c.armed = v; });
        toggle (skipToggle, "비활성이면 건너뛰기", "건너뛰기", [] (Cue& c, bool v) { c.skipIfDisarmed = v; });
        toggle (autoLoadToggle, "자동 로드", "자동 로드", [] (Cue& c, bool v) { c.autoLoad = v; });
        skipToggle.setTooltip (ko ("켜면 비활성 큐를 시퀀스에서 아예 건너뜁니다. 끄면 소리만 안 나고 진행 모드는 그대로 적용됩니다"));
        autoLoadToggle.setTooltip (ko ("플레이헤드가 이 큐에 오면 미리 로드해 GO 지연을 없앱니다"));

        gainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        gainSlider.setRange (Cue::minGainDb, Cue::maxGainDb, 0.1);
        gainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
        gainSlider.setTextValueSuffix (" dB");
        gainSlider.setDoubleClickReturnValue (true, 0.0);
        gainSlider.setWantsKeyboardFocus (false);
        gainSlider.onValueChange = [this] { commitGain(); };
        addAndMakeVisible (gainSlider);

        notesEditor.setMultiLine (true, true);
        notesEditor.setReturnKeyStartsNewLine (true);
        notesEditor.setScrollbarsShown (true);
        notesEditor.onFocusLost = [this] { commitNotes(); };
        notesEditor.onEscapeKey = [this] { cancelEditAndPanic(); };
        addAndMakeVisible (notesEditor);
    }

    std::function<void()> onPanic;

    void focusNotes() { notesEditor.grabKeyboardFocus(); }

    void setEditable (bool shouldBeEditable)
    {
        editable = shouldBeEditable;
        refresh();
    }

    void refresh()
    {
        const juce::ScopedValueSetter<bool> guard (refreshing, true);
        const auto* cue = cues.getSelected();
        const bool enabled = cue != nullptr && editable;

        for (auto* c : std::initializer_list<juce::Component*> { &numberEditor, &nameEditor, &colourCombo, &secondColourToggle, &secondColourCombo,
                                                                 &preEditor, &postEditor, &continueCombo, &hotkeyButton, &clearHotkeyButton,
                                                                 &flagToggle, &armedToggle, &skipToggle, &autoLoadToggle,
                                                                 &fadeOutEditor, &gainSlider, &browseButton, &notesEditor })
            c->setEnabled (enabled);

        if (cue == nullptr)
        {
            for (auto* e : { &numberEditor, &nameEditor, &preEditor, &postEditor, &fadeOutEditor, &notesEditor })
                e->setText ("", false);

            filePathLabel.setText ("", juce::dontSendNotification);
            colourCombo.setSelectedId (0, juce::dontSendNotification);
            secondColourCombo.setSelectedId (0, juce::dontSendNotification);
            continueCombo.setSelectedId (0, juce::dontSendNotification);
            hotkeyButton.setHotkey ({});
            gainSlider.setValue (0.0, juce::dontSendNotification);
            return;
        }

        auto setIfIdle = [] (juce::TextEditor& e, const juce::String& text) { if (! e.hasKeyboardFocus (true)) e.setText (text, false); };
        setIfIdle (numberEditor, cue->number);
        setIfIdle (nameEditor, cue->name);
        setIfIdle (preEditor, formatTimeMs (cue->preWaitSeconds));
        setIfIdle (postEditor, formatTimeMs (cue->postWaitSeconds));
        setIfIdle (fadeOutEditor, juce::String (cue->fadeOutMs));
        setIfIdle (notesEditor, cue->notes);

        colourCombo.setSelectedId (cue->color + 1, juce::dontSendNotification);
        secondColourToggle.setToggleState (cue->useSecondColor, juce::dontSendNotification);
        secondColourCombo.setSelectedId (cue->secondColor + 1, juce::dontSendNotification);
        secondColourCombo.setEnabled (enabled && cue->useSecondColor);
        continueCombo.setSelectedId ((int) cue->continueMode + 1, juce::dontSendNotification);
        hotkeyButton.setHotkey (cue->hotkey);
        flagToggle.setToggleState (cue->flagged, juce::dontSendNotification);
        armedToggle.setToggleState (cue->armed, juce::dontSendNotification);
        skipToggle.setToggleState (cue->skipIfDisarmed, juce::dontSendNotification);
        autoLoadToggle.setToggleState (cue->autoLoad, juce::dontSendNotification);
        gainSlider.setValue (cue->gainDb, juce::dontSendNotification);

        if (cue->file == juce::File())
        {
            filePathLabel.setText (ko ("파일 없음"), juce::dontSendNotification);
            filePathLabel.setColour (juce::Label::textColourId, Palette::missing);
        }
        else
        {
            filePathLabel.setText ((cue->fileMissing ? ko ("[없음] ") : juce::String()) + cue->file.getFullPathName(), juce::dontSendNotification);
            filePathLabel.setColour (juce::Label::textColourId, cue->fileMissing ? Palette::missing : Palette::text);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 6);
        const int rowHeight = 24;
        auto nextRow = [&] { auto r = area.removeFromTop (rowHeight); area.removeFromTop (5); return r; };

        auto row = nextRow();
        numberLabel.setBounds (row.removeFromLeft (36));
        numberEditor.setBounds (row.removeFromLeft (70));
        row.removeFromLeft (10);
        nameLabel.setBounds (row.removeFromLeft (36));
        nameEditor.setBounds (row.removeFromLeft (juce::jmax (120, row.getWidth() - 420)));
        row.removeFromLeft (10);
        colourLabel.setBounds (row.removeFromLeft (24));
        colourCombo.setBounds (row.removeFromLeft (110));
        row.removeFromLeft (8);
        secondColourToggle.setBounds (row.removeFromLeft (96));
        secondColourCombo.setBounds (row.removeFromLeft (110));

        row = nextRow();
        fileLabel.setBounds (row.removeFromLeft (36));
        browseButton.setBounds (row.removeFromRight (96));
        row.removeFromRight (8);
        filePathLabel.setBounds (row);
        dropArea = filePathLabel.getBounds().expanded (2, 3);

        row = nextRow();
        preLabel.setBounds (row.removeFromLeft (70));
        preEditor.setBounds (row.removeFromLeft (84));
        row.removeFromLeft (10);
        postLabel.setBounds (row.removeFromLeft (84));
        postEditor.setBounds (row.removeFromLeft (84));
        row.removeFromLeft (10);
        continueLabel.setBounds (row.removeFromLeft (34));
        continueCombo.setBounds (row.removeFromLeft (130));
        row.removeFromLeft (14);
        hotkeyButton.setBounds (row.removeFromLeft (190));
        row.removeFromLeft (4);
        clearHotkeyButton.setBounds (row.removeFromLeft (26));

        row = nextRow();
        flagToggle.setBounds (row.removeFromLeft (64));
        armedToggle.setBounds (row.removeFromLeft (104));
        skipToggle.setBounds (row.removeFromLeft (156));
        autoLoadToggle.setBounds (row.removeFromLeft (92));
        row.removeFromLeft (10);
        fadeOutLabel.setBounds (row.removeFromLeft (104));
        fadeOutEditor.setBounds (row.removeFromLeft (70));
        row.removeFromLeft (10);
        gainLabel.setBounds (row.removeFromLeft (62));
        gainSlider.setBounds (row.removeFromLeft (juce::jmin (300, row.getWidth())));

        row = area.removeFromTop (juce::jmax (40, area.getHeight()));
        notesLabel.setBounds (row.removeFromLeft (36).withHeight (24));
        notesEditor.setBounds (row);
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
    void edit (const juce::String& name, const std::function<void (Cue&)>& mutator, const juce::String& coalesceKey = {})
    {
        if (refreshing || cancellingEdit || ! editable)
            return;

        const int index = cues.getSelectedIndex();

        if (! cues.isValidIndex (index))
            return;

        document.perform (name, [this, index, mutator] { cues.update (index, mutator); }, { coalesceKey, false });
    }

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        if (cues.getSelected() == nullptr || ! editable)
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

    void commitNumber()
    {
        const auto* cue = cues.getSelected();

        if (refreshing || cancellingEdit || cue == nullptr)
            return;

        const auto number = numberEditor.getText().trim();

        if (number == cue->number)
            return;

        edit (ko ("번호"), [number] (Cue& c) { c.number = number; });
    }

    void commitName()
    {
        const auto* cue = cues.getSelected();

        if (refreshing || cancellingEdit || cue == nullptr)
            return;

        const auto name = nameEditor.getText().trim();

        if (name == cue->name)
            return;

        edit (ko ("이름 변경"), [name] (Cue& c) { c.name = name; });
    }

    void commitNotes()
    {
        const auto* cue = cues.getSelected();

        if (refreshing || cancellingEdit || cue == nullptr)
            return;

        const auto notes = notesEditor.getText();

        if (notes == cue->notes)
            return;

        edit (ko ("메모"), [notes] (Cue& c) { c.notes = notes; });
    }

    void commitWait (bool pre)
    {
        const auto* cue = cues.getSelected();

        if (refreshing || cancellingEdit || cue == nullptr)
            return;

        auto& editor = pre ? preEditor : postEditor;
        const double current = pre ? cue->preWaitSeconds : cue->postWaitSeconds;
        const double value = parseTimeText (editor.getText());

        if (value < 0.0 || juce::approximatelyEqual (value, current))
        {
            editor.setText (formatTimeMs (current), false);
            return;
        }

        edit (pre ? ko ("프리웨이트") : ko ("포스트웨이트"), [value, pre] (Cue& c)
        {
            if (pre)
                c.preWaitSeconds = value;
            else
                c.postWaitSeconds = value;
        });
    }

    void commitColour (bool second)
    {
        auto& combo = second ? secondColourCombo : colourCombo;

        if (refreshing || combo.getSelectedId() == 0)
            return;

        const int colour = combo.getSelectedId() - 1;
        edit (second ? ko ("두 번째 색") : ko ("색상"), [colour, second] (Cue& c)
        {
            if (second)
                c.secondColor = colour;
            else
                c.color = colour;
        });
    }

    void commitStopFade()
    {
        const auto* cue = cues.getSelected();

        if (refreshing || cancellingEdit || cue == nullptr)
            return;

        const auto text = fadeOutEditor.getText().trim();

        if (text.isEmpty())
        {
            fadeOutEditor.setText (juce::String (cue->fadeOutMs), false);
            return;
        }

        const int value = juce::jlimit (0, Cue::maxFadeMs, text.getIntValue());

        if (value == cue->fadeOutMs)
        {
            fadeOutEditor.setText (juce::String (value), false);
            return;
        }

        edit (ko ("정지 페이드 변경"), [value] (Cue& c) { c.fadeOutMs = value; });
    }

    void commitGain()
    {
        const auto* cue = cues.getSelected();

        if (refreshing || cue == nullptr)
            return;

        const double value = gainSlider.getValue();

        if (juce::approximatelyEqual (cue->gainDb, value))
            return;

        const auto id = cue->id;
        edit (ko ("게인 변경"), [value] (Cue& c) { c.gainDb = value; }, "gain:" + id.toString());
        engine.setLiveGainDb (id, value);   // a running instance follows at once
    }

    void replaceFile (const juce::File& file)
    {
        if (cues.getSelected() == nullptr)
            return;

        settings.setLastAudioDirectory (file.getParentDirectory());
        auto& formats = engine.getFormatManager();

        edit (ko ("파일 교체"), [&formats, file] (Cue& c)
        {
            c.file = file;

            if (c.name.isEmpty())
                c.name = file.getFileNameWithoutExtension();

            refreshCueFileInfo (formats, c);
        });
    }

    void chooseFile()
    {
        if (cues.getSelected() == nullptr || chooser != nullptr)
            return;

        auto startDir = settings.getLastAudioDirectory();

        if (const auto* cue = cues.getSelected(); cue->file != juce::File())
            startDir = cue->file.getParentDirectory();

        chooser = std::make_unique<juce::FileChooser> (ko ("오디오 파일 선택"), startDir, engine.getFormatManager().getWildcardForAllFormats());
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

    juce::Label numberLabel, nameLabel, colourLabel, fileLabel, preLabel, postLabel, continueLabel, fadeOutLabel, gainLabel, notesLabel, filePathLabel;
    juce::TextEditor numberEditor, nameEditor, preEditor, postEditor, fadeOutEditor, notesEditor;
    juce::ComboBox colourCombo, secondColourCombo, continueCombo;
    juce::ToggleButton secondColourToggle, flagToggle, armedToggle, skipToggle, autoLoadToggle;
    HotkeyButton hotkeyButton;
    juce::TextButton clearHotkeyButton, browseButton;
    juce::Slider gainSlider;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> dropArea;
    bool refreshing = false;
    bool cancellingEdit = false;
    bool dragOver = false;
    bool editable = true;
};

//==============================================================================
/** Tab "트리거". */
class CueInspector::TriggersPanel : public juce::Component
{
public:
    explicit TriggersPanel (ProjectDocument& doc) : document (doc), cues (doc.cues)
    {
        styleLabel (secondLabel, ko ("재생 중에 다시 GO 하면"));
        addAndMakeVisible (secondLabel);

        secondCombo.addItem (ko ("무시 (계속 재생)"), 1);
        secondCombo.addItem (ko ("전체 페이드 정지 시간으로 페이드 정지"), 2);
        secondCombo.addItem (ko ("정지 페이드로 정지"), 3);
        secondCombo.addItem (ko ("즉시 정지"), 4);
        secondCombo.addItem (ko ("즉시 정지 후 처음부터 재시작"), 5);
        secondCombo.addItem (ko ("이번 반복만 마치고 끝 (루프 큐)"), 6);
        secondCombo.setWantsKeyboardFocus (false);
        secondCombo.onChange = [this]
        {
            if (refreshing || secondCombo.getSelectedId() == 0)
                return;

            const auto action = (SecondTriggerAction) (secondCombo.getSelectedId() - 1);
            edit (ko ("2차 트리거"), [action] (Cue& c) { c.secondTrigger = action; });
        };
        addAndMakeVisible (secondCombo);

        // wall clock
        styleToggle (wallToggle, ko ("시간 트리거 (시:분:초)"));
        wallToggle.onClick = [this] { const bool on = wallToggle.getToggleState(); edit (ko ("시간 트리거"), [on] (Cue& c) { c.wallClock.enabled = on; }); };
        addAndMakeVisible (wallToggle);

        for (auto* e : { &hourEditor, &minuteEditor, &secondEditor })
        {
            styleNumberEditor (*e, "0123456789", 2);
            e->setJustification (juce::Justification::centred);
            e->onReturnKey = [this, e] { commitWallClock(); e->giveAwayKeyboardFocus(); };
            e->onFocusLost = [this] { commitWallClock(); };
            addAndMakeVisible (*e);
        }

        static const char* const dayNames[] = { "\xEC\x9D\xBC", "\xEC\x9B\x94", "\xED\x99\x94", "\xEC\x88\x98", "\xEB\xAA\xA9", "\xEA\xB8\x88", "\xED\x86\xA0" };   // 일 월 화 수 목 금 토

        for (int d = 0; d < 7; ++d)
        {
            auto& b = dayButtons[(size_t) d];
            b.setButtonText (juce::String::fromUTF8 (dayNames[d]));
            b.setClickingTogglesState (true);
            b.setWantsKeyboardFocus (false);
            b.setColour (juce::TextButton::buttonOnColourId, Palette::standby);
            b.onClick = [this, d]
            {
                const bool on = dayButtons[(size_t) d].getToggleState();
                edit (ko ("시간 트리거 요일"), [d, on] (Cue& c)
                {
                    if (on)
                        c.wallClock.daysMask |= (1 << d);
                    else
                        c.wallClock.daysMask &= ~(1 << d);
                });
            };
            addAndMakeVisible (b);
        }

        // fade & stop others
        styleToggle (fadeStopToggle, ko ("시작할 때 다른 큐 페이드 정지"));
        fadeStopToggle.onClick = [this] { const bool on = fadeStopToggle.getToggleState(); edit (ko ("다른 큐 페이드 정지"), [on] (Cue& c) { c.fadeStopOthers.enabled = on; }); };
        addAndMakeVisible (fadeStopToggle);
        styleLabel (fadeStopSecondsLabel, ko ("시간 (초)"));
        addAndMakeVisible (fadeStopSecondsLabel);
        styleNumberEditor (fadeStopSecondsEditor, "0123456789.", 7);
        fadeStopSecondsEditor.onReturnKey = [this] { commitFadeStop(); fadeStopSecondsEditor.giveAwayKeyboardFocus(); };
        fadeStopSecondsEditor.onFocusLost = [this] { commitFadeStop(); };
        addAndMakeVisible (fadeStopSecondsEditor);
        styleLabel (fadeStopScopeLabel, ko ("범위"));
        addAndMakeVisible (fadeStopScopeLabel);
        fadeStopScopeCombo.addItem (ko ("같은 계층"), 1);
        fadeStopScopeCombo.addItem (ko ("이 리스트"), 2);
        fadeStopScopeCombo.addItem (ko ("전체"), 3);
        fadeStopScopeCombo.setWantsKeyboardFocus (false);
        fadeStopScopeCombo.onChange = [this]
        {
            if (refreshing || fadeStopScopeCombo.getSelectedId() == 0)
                return;

            const auto scope = (FadeStopScope) (fadeStopScopeCombo.getSelectedId() - 1);
            edit (ko ("페이드 정지 범위"), [scope] (Cue& c) { c.fadeStopOthers.scope = scope; });
        };
        addAndMakeVisible (fadeStopScopeCombo);

        // duck
        styleToggle (duckToggle, ko ("재생 중 다른 큐 덕 / 부스트"));
        duckToggle.onClick = [this] { const bool on = duckToggle.getToggleState(); edit (ko ("덕/부스트"), [on] (Cue& c) { c.duck.enabled = on; }); };
        addAndMakeVisible (duckToggle);
        styleLabel (duckLevelLabel, ko ("레벨 (dB, 음수 = 덕)"));
        addAndMakeVisible (duckLevelLabel);
        styleNumberEditor (duckLevelEditor, "-0123456789.", 7);
        duckLevelEditor.onReturnKey = [this] { commitDuck(); duckLevelEditor.giveAwayKeyboardFocus(); };
        duckLevelEditor.onFocusLost = [this] { commitDuck(); };
        addAndMakeVisible (duckLevelEditor);
        styleLabel (duckSecondsLabel, ko ("시간 (초)"));
        addAndMakeVisible (duckSecondsLabel);
        styleNumberEditor (duckSecondsEditor, "0123456789.", 7);
        duckSecondsEditor.onReturnKey = [this] { commitDuck(); duckSecondsEditor.giveAwayKeyboardFocus(); };
        duckSecondsEditor.onFocusLost = [this] { commitDuck(); };
        addAndMakeVisible (duckSecondsEditor);

        styleLabel (hint, ko ("GO 사이 최소 시간(더블 GO 방지)과 전체 페이드 정지 시간은 파일 > 프로젝트 설정에 있습니다"), 11.0f);
        addAndMakeVisible (hint);
    }

    void setEditable (bool shouldBeEditable)
    {
        editable = shouldBeEditable;
        refresh();
    }

    void refresh()
    {
        const juce::ScopedValueSetter<bool> guard (refreshing, true);
        const auto* cue = cues.getSelected();
        const bool enabled = cue != nullptr && editable;

        for (auto* c : std::initializer_list<juce::Component*> { &secondCombo, &wallToggle, &hourEditor, &minuteEditor, &secondEditor,
                                                                 &fadeStopToggle, &fadeStopSecondsEditor, &fadeStopScopeCombo,
                                                                 &duckToggle, &duckLevelEditor, &duckSecondsEditor })
            c->setEnabled (enabled);

        for (auto& b : dayButtons)
            b.setEnabled (enabled);

        if (cue == nullptr)
        {
            secondCombo.setSelectedId (0, juce::dontSendNotification);
            return;
        }

        secondCombo.setSelectedId ((int) cue->secondTrigger + 1, juce::dontSendNotification);
        wallToggle.setToggleState (cue->wallClock.enabled, juce::dontSendNotification);

        auto setIfIdle = [] (juce::TextEditor& e, const juce::String& text) { if (! e.hasKeyboardFocus (true)) e.setText (text, false); };
        setIfIdle (hourEditor, juce::String (cue->wallClock.hour).paddedLeft ('0', 2));
        setIfIdle (minuteEditor, juce::String (cue->wallClock.minute).paddedLeft ('0', 2));
        setIfIdle (secondEditor, juce::String (cue->wallClock.second).paddedLeft ('0', 2));

        for (int d = 0; d < 7; ++d)
            dayButtons[(size_t) d].setToggleState ((cue->wallClock.daysMask & (1 << d)) != 0, juce::dontSendNotification);

        fadeStopToggle.setToggleState (cue->fadeStopOthers.enabled, juce::dontSendNotification);
        setIfIdle (fadeStopSecondsEditor, juce::String (cue->fadeStopOthers.seconds, 2));
        fadeStopScopeCombo.setSelectedId ((int) cue->fadeStopOthers.scope + 1, juce::dontSendNotification);
        duckToggle.setToggleState (cue->duck.enabled, juce::dontSendNotification);
        setIfIdle (duckLevelEditor, juce::String (cue->duck.levelDb, 1));
        setIfIdle (duckSecondsEditor, juce::String (cue->duck.seconds, 2));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 6);
        const int rowHeight = 24;
        auto nextRow = [&] { auto r = area.removeFromTop (rowHeight); area.removeFromTop (6); return r; };

        auto row = nextRow();
        secondLabel.setBounds (row.removeFromLeft (160));
        secondCombo.setBounds (row.removeFromLeft (300));

        row = nextRow();
        wallToggle.setBounds (row.removeFromLeft (180));
        hourEditor.setBounds (row.removeFromLeft (34));
        row.removeFromLeft (4);
        minuteEditor.setBounds (row.removeFromLeft (34));
        row.removeFromLeft (4);
        secondEditor.setBounds (row.removeFromLeft (34));
        row.removeFromLeft (14);

        for (auto& b : dayButtons)
        {
            b.setBounds (row.removeFromLeft (30));
            row.removeFromLeft (3);
        }

        row = nextRow();
        fadeStopToggle.setBounds (row.removeFromLeft (230));
        fadeStopSecondsLabel.setBounds (row.removeFromLeft (60));
        fadeStopSecondsEditor.setBounds (row.removeFromLeft (60));
        row.removeFromLeft (14);
        fadeStopScopeLabel.setBounds (row.removeFromLeft (36));
        fadeStopScopeCombo.setBounds (row.removeFromLeft (120));

        row = nextRow();
        duckToggle.setBounds (row.removeFromLeft (230));
        duckLevelLabel.setBounds (row.removeFromLeft (130));
        duckLevelEditor.setBounds (row.removeFromLeft (60));
        row.removeFromLeft (14);
        duckSecondsLabel.setBounds (row.removeFromLeft (60));
        duckSecondsEditor.setBounds (row.removeFromLeft (60));

        row = nextRow();
        hint.setBounds (row);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

private:
    void edit (const juce::String& name, const std::function<void (Cue&)>& mutator)
    {
        if (refreshing || ! editable)
            return;

        const int index = cues.getSelectedIndex();

        if (! cues.isValidIndex (index))
            return;

        document.perform (name, [this, index, mutator] { cues.update (index, mutator); });
    }

    void commitWallClock()
    {
        if (refreshing || cues.getSelected() == nullptr)
            return;

        const int h = juce::jlimit (0, 23, hourEditor.getText().getIntValue());
        const int m = juce::jlimit (0, 59, minuteEditor.getText().getIntValue());
        const int s = juce::jlimit (0, 59, secondEditor.getText().getIntValue());
        const auto& wc = cues.getSelected()->wallClock;

        if (h == wc.hour && m == wc.minute && s == wc.second)
        {
            refresh();
            return;
        }

        edit (ko ("시간 트리거 시각"), [h, m, s] (Cue& c) { c.wallClock.hour = h; c.wallClock.minute = m; c.wallClock.second = s; });
    }

    void commitFadeStop()
    {
        if (refreshing || cues.getSelected() == nullptr)
            return;

        const double seconds = juce::jlimit (0.0, 600.0, fadeStopSecondsEditor.getText().getDoubleValue());

        if (juce::approximatelyEqual (seconds, cues.getSelected()->fadeStopOthers.seconds))
        {
            refresh();
            return;
        }

        edit (ko ("페이드 정지 시간"), [seconds] (Cue& c) { c.fadeStopOthers.seconds = seconds; });
    }

    void commitDuck()
    {
        if (refreshing || cues.getSelected() == nullptr)
            return;

        const double level = juce::jlimit (Cue::minGainDb, Cue::maxGainDb, duckLevelEditor.getText().getDoubleValue());
        const double seconds = juce::jlimit (0.0, 600.0, duckSecondsEditor.getText().getDoubleValue());
        const auto& duck = cues.getSelected()->duck;

        if (juce::approximatelyEqual (level, duck.levelDb) && juce::approximatelyEqual (seconds, duck.seconds))
        {
            refresh();
            return;
        }

        edit (ko ("덕/부스트 값"), [level, seconds] (Cue& c) { c.duck.levelDb = level; c.duck.seconds = seconds; });
    }

    ProjectDocument& document;
    CueList& cues;
    juce::Label secondLabel, fadeStopSecondsLabel, fadeStopScopeLabel, duckLevelLabel, duckSecondsLabel, hint;
    juce::ComboBox secondCombo, fadeStopScopeCombo;
    juce::ToggleButton wallToggle, fadeStopToggle, duckToggle;
    juce::TextEditor hourEditor, minuteEditor, secondEditor, fadeStopSecondsEditor, duckLevelEditor, duckSecondsEditor;
    std::array<juce::TextButton, 7> dayButtons;
    bool refreshing = false;
    bool editable = true;
};

//==============================================================================
/** Tab "이펙트": the cue's VST3 insert chain. */
class CueInspector::EffectsPanel : public juce::Component
{
public:
    EffectsPanel (ProjectDocument& doc, AudioEngine& e, PluginWindowManager& windows)
        : document (doc), cues (doc.cues), engine (e), chainStrip (e, windows)
    {
        styleLabel (hint, ko ("이 큐만 통과하는 VST3 인서트 (파일 → 페이드 → 게인 → 인서트 → 믹스)"), 12.0f);
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

    void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

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
    tabs.setCurrentTabIndex (0);
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
            if (p.id == cue->id && ! p.loaded)
                found = &p;

    timeLoops->setPlayback (found);
}

void CueInspector::setEditable (bool shouldBeEditable)
{
    editable = shouldBeEditable;
    basics->setEditable (editable);
    triggers->setEditable (editable);
    timeLoops->setEnabled (editable);
    effects->setEnabled (editable);
}

void CueInspector::showNotes()
{
    tabs.setCurrentTabIndex (0);
    basics->focusNotes();
}

void CueInspector::showTimeTab()
{
    tabs.setCurrentTabIndex (1);
}

void CueInspector::refresh()
{
    const auto* cue = cues.getSelected();

    if (cue == nullptr)
    {
        title.setText (ko ("큐 인스펙터 - 선택된 큐 없음"), juce::dontSendNotification);
    }
    else
    {
        const int count = (int) cues.getSelectedIndices().size();
        juce::String text = ko ("큐 인스펙터 - ") + (cue->number.isNotEmpty() ? cue->number + " " : juce::String()) + cue->name;

        if (count > 1)
            text << ko ("  (") << count << ko ("개 선택, 표에서 한꺼번에 편집)");

        title.setText (text, juce::dontSendNotification);
    }

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
