#include "ui/CueInspector.h"

#include "audio/CueFileInfo.h"
#include "ui/CueTable.h"
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
    /** Returns a reason to refuse the key, or an empty string. */
    std::function<juce::String (const juce::KeyPress&)> validate;

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

        if (validate)
        {
            const auto reason = validate (key);

            if (reason.isNotEmpty())
            {
                setButtonText (reason);
                juce::Component::SafePointer<HotkeyButton> safeThis (this);
                juce::Timer::callAfterDelay (1500, [safeThis] { if (safeThis != nullptr) safeThis->setHotkey (safeThis->hotkey); });
                return true;
            }
        }

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
        hotkeyButton.validate = [this] (const juce::KeyPress& key) -> juce::String
        {
            // keys the app itself uses (GO / pause / fade / panic / preview / load / quick edit / list navigation)
            static const int reserved[] = { juce::KeyPress::spaceKey, juce::KeyPress::escapeKey, juce::KeyPress::returnKey, juce::KeyPress::tabKey,
                                            juce::KeyPress::deleteKey, juce::KeyPress::backspaceKey, juce::KeyPress::insertKey,
                                            juce::KeyPress::upKey, juce::KeyPress::downKey, juce::KeyPress::leftKey, juce::KeyPress::rightKey,
                                            juce::KeyPress::pageUpKey, juce::KeyPress::pageDownKey, juce::KeyPress::homeKey, juce::KeyPress::endKey,
                                            'P', 'F', 'V', 'L', 'N', 'Q', 'E', 'W', 'C', 'O', 'D', juce::KeyPress::F3Key };

            if (key.getModifiers().isCommandDown() || key.getModifiers().isAltDown())
                return ko ("Ctrl / Alt 조합은 메뉴 단축키용입니다");

            for (int code : reserved)
                if (key.getKeyCode() == code)
                    return ko ("앱이 쓰는 키입니다: ") + key.getTextDescription();

            const auto description = key.getTextDescription();
            const auto* selected = cues.getSelected();

            if (document.isHotkeyTaken (description, selected != nullptr ? selected->id : juce::Uuid::null()))   // every list / cart
            {
                juce::String owner;
                document.forEachList ([&] (CueList& list)
                {
                    for (const auto& other : list.getAll())
                        if (other.hotkey == description && (selected == nullptr || other.id != selected->id))
                            owner = other.name;
                });
                return ko ("이미 쓰는 핫키: ") + owner;
            }

            return {};
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

        // while a field is being edited the panel keeps showing (and later commits to) the cue it started on
        bool editing = false;

        for (auto* e : { &numberEditor, &nameEditor, &preEditor, &postEditor, &fadeOutEditor, &notesEditor })
            editing = editing || e->hasKeyboardFocus (true);

        if (editing && ! shownId.isNull() && shownId != cue->id && cues.indexOf (shownId) >= 0)
            return;

        shownId = cue->id;

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

        const bool audio = cue->isAudio();
        const bool sound = cue->makesSound();   // a mic cue has a gain and a stop fade, but no file

        for (auto* c : std::initializer_list<juce::Component*> { &fileLabel, &filePathLabel, &browseButton, &autoLoadToggle })
            c->setVisible (audio);

        for (auto* c : std::initializer_list<juce::Component*> { &fadeOutLabel, &fadeOutEditor, &gainLabel, &gainSlider })
            c->setVisible (sound);

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

        // the cue whose values the fields show (a focus-lost commit may arrive after the selection moved on)
        const int index = shownId.isNull() ? cues.getSelectedIndex() : cues.indexOf (shownId);

        if (! cues.isValidIndex (index))
            return;

        document.perform (name, [this, index, mutator] { cues.update (index, mutator); }, { coalesceKey, false });

        if (const auto* selected = cues.getSelected(); selected != nullptr && selected->id != shownId)
            refresh();   // the edit went to the previous cue: show the selected one now
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
        const int index = shownId.isNull() ? cues.getSelectedIndex() : cues.indexOf (shownId);
        const auto* cue = cues.isValidIndex (index) ? &cues.get (index) : nullptr;

        if (refreshing || cancellingEdit || cue == nullptr)
            return;

        const auto number = numberEditor.getText().trim();

        if (number == cue->number)
            return;

        if (document.isNumberTaken (number, cue->id))
        {
            juce::LookAndFeel::getDefaultLookAndFeel().playAlertSound();   // numbers are unique in the project (every list / cart)
            numberEditor.setText (cue->number, false);
            return;
        }

        edit (ko ("번호"), [number] (Cue& c) { c.number = number; });
    }

    void commitName()
    {
        const auto* cue = shownCue();   // the cue this edit started on, even if the selection moved meanwhile

        if (refreshing || cancellingEdit || cue == nullptr)
            return;

        const auto name = nameEditor.getText().trim();

        if (name == cue->name)
            return;

        edit (ko ("이름 변경"), [name] (Cue& c) { c.name = name; });
    }

    void commitNotes()
    {
        const auto* cue = shownCue();   // the cue this edit started on, even if the selection moved meanwhile

        if (refreshing || cancellingEdit || cue == nullptr)
            return;

        const auto notes = notesEditor.getText();

        if (notes == cue->notes)
            return;

        edit (ko ("메모"), [notes] (Cue& c) { c.notes = notes; });
    }

    void commitWait (bool pre)
    {
        const auto* cue = shownCue();   // the cue this edit started on, even if the selection moved meanwhile

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
        const auto* cue = shownCue();   // the cue this edit started on, even if the selection moved meanwhile

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
        const auto* cue = shownCue();

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
    juce::Uuid shownId = juce::Uuid::null();   // the cue the fields show

    const Cue* shownCue() const { return shownId.isNull() ? cues.getSelected() : cues.findById (shownId); }
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

        {
            bool editing = false;

            for (auto* e : { &hourEditor, &minuteEditor, &secondEditor, &fadeStopSecondsEditor, &duckLevelEditor, &duckSecondsEditor })
                editing = editing || e->hasKeyboardFocus (true);

            if (cue != nullptr && editing && ! shownId.isNull() && shownId != cue->id && cues.indexOf (shownId) >= 0)
                return;

            shownId = cue != nullptr ? cue->id : juce::Uuid::null();
        }

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

        const int index = shownId.isNull() ? cues.getSelectedIndex() : cues.indexOf (shownId);

        if (! cues.isValidIndex (index))
            return;

        document.perform (name, [this, index, mutator] { cues.update (index, mutator); });

        if (const auto* selected = cues.getSelected(); selected != nullptr && selected->id != shownId)
            refresh();
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
    juce::Uuid shownId = juce::Uuid::null();
    bool refreshing = false;
    bool editable = true;
};

//==============================================================================
/** Tab "이펙트": the cue's VST3 insert chain. */
//==============================================================================
/** 레벨 tab: output patch + the level matrix (main / inputs / outputs / crosspoints). */
class CueInspector::LevelsPanel : public juce::Component
{
public:
    LevelsPanel (ProjectDocument& doc, AudioEngine& e) : document (doc), cues (doc.cues), engine (e)
    {
        styleLabel (patchLabel, ko ("패치"));
        addAndMakeVisible (patchLabel);
        patchCombo.setWantsKeyboardFocus (false);
        patchCombo.setTooltip (ko ("이 큐의 출력이 지나가는 오디오 패치 (오디오 > 오디오 패치...)"));
        patchCombo.onChange = [this] { commitPatch(); };
        addAndMakeVisible (patchCombo);

        defaultsButton.setButtonText (ko ("기본 레벨로"));
        defaultsButton.setWantsKeyboardFocus (false);
        defaultsButton.onClick = [this] { applyToLevels (ko ("기본 레벨로"), [] (Cue& c) { c.gainDb = 0.0; c.levels.setDefaults(); }); };
        addAndMakeVisible (defaultsButton);

        silenceButton.setButtonText (ko ("전부 무음"));
        silenceButton.setWantsKeyboardFocus (false);
        silenceButton.onClick = [this] { applyToLevels (ko ("전부 무음"), [] (Cue& c) { c.levels.silenceCrosspoints(); }); };
        addAndMakeVisible (silenceButton);

        styleLabel (hint, ko ("드래그 = 레벨 (Shift = 0.1 dB) · 더블클릭 = 기본값 · 숫자 입력 (부호 없으면 음수, 빈칸 = 무음) · 우클릭 = 겡 · 재생 중에도 즉시 반영. 행 = 파일 채널, 열 = 패치의 큐 출력 (귀퉁이 표시 = 장치에 연결 안 됨)"), 11.0f);
        addAndMakeVisible (hint);

        viewport.setViewedComponent (&grid, false);
        viewport.setScrollBarsShown (true, true);
        addAndMakeVisible (viewport);

        grid.onChange = [this] (double mainDb, const LevelMatrix& m, bool finished) { commitLevels (mainDb, m, finished); };
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

        patchCombo.clear (juce::dontSendNotification);
        int selectedPatch = 0;

        for (int i = 0; i < (int) document.patches.size(); ++i)
        {
            const auto& p = document.patches[(size_t) i];
            patchCombo.addItem (p.name + " (" + juce::String (p.numCueOutputs) + ")", i + 1);

            if (cue != nullptr && &document.patchForCue (*cue) == &p)
                selectedPatch = i + 1;
        }

        patchCombo.setSelectedId (selectedPatch, juce::dontSendNotification);

        for (auto* c : std::initializer_list<juce::Component*> { &patchCombo, &defaultsButton, &silenceButton })
            c->setEnabled (enabled);

        grid.setEditable (enabled);
        grid.setLimits (document.settings.minLevelDb, document.settings.maxLevelDb);

        if (cue == nullptr)
        {
            shownId = juce::Uuid::null();
            grid.setLevels (0.0, LevelMatrix());
            return;
        }

        if (editing && ! shownId.isNull() && shownId != cue->id && cues.indexOf (shownId) >= 0)
            return;   // a drag / typing session is still on the previous cue

        shownId = cue->id;
        const auto& patch = document.patchForCue (*cue);
        LevelMatrix m = cue->levels;
        const int inputs = cue->numChannels > 0 ? cue->numChannels : (m.numInputs() > 0 ? m.numInputs() : 2);
        m.resize (inputs, patch.numCueOutputs);

        juce::StringArray inputNames, outputNames;

        for (int i = 0; i < inputs; ++i)
            inputNames.add (inputs == 1 ? ko ("모노") : ko ("채널 ") + juce::String (i + 1));

        std::vector<bool> connected;
        const int deviceOutputs = engine.getNumDeviceOutputs();

        for (int k = 0; k < patch.numCueOutputs; ++k)
        {
            outputNames.add (patch.cueOutputName (k));
            bool reaches = false;

            for (int d = 0; d < deviceOutputs && ! reaches; ++d)
                reaches = patch.routingGain (k, d) > 0.0f;

            connected.push_back (reaches);
        }

        grid.setLabels (inputNames, outputNames);
        grid.setLevels (cue->gainDb, m);
        grid.setOutputConnected (connected);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 6);
        auto row = area.removeFromTop (24);
        patchLabel.setBounds (row.removeFromLeft (36));
        patchCombo.setBounds (row.removeFromLeft (220));
        row.removeFromLeft (12);
        defaultsButton.setBounds (row.removeFromLeft (100));
        row.removeFromLeft (6);
        silenceButton.setBounds (row.removeFromLeft (90));
        area.removeFromTop (4);
        hint.setBounds (area.removeFromTop (16));
        area.removeFromTop (4);
        viewport.setBounds (area);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

private:
    void edit (const juce::String& name, const std::function<void (Cue&)>& mutator, const juce::String& coalesceKey = {})
    {
        if (refreshing || ! editable)
            return;

        const int index = cues.getSelectedIndex();

        if (! cues.isValidIndex (index))
            return;

        document.perform (name, [this, index, mutator] { cues.update (index, mutator); }, { coalesceKey, false });
    }

    void pushLive (const Cue& cue)
    {
        engine.setLiveGainDb (cue.id, cue.gainDb);
        engine.setLiveLevels (cue.id, cue.levels, cue.trim);
    }

    void applyToLevels (const juce::String& name, const std::function<void (Cue&)>& mutator)
    {
        const auto* cue = cues.getSelected();

        if (cue == nullptr || ! editable)
            return;

        const int outputs = document.cueOutputsFor (*cue);
        edit (name, [mutator, outputs] (Cue& c)
        {
            if (c.levels.numInputs() > 0)
                c.levels.resize (c.levels.numInputs(), outputs);

            mutator (c);
        });

        if (const auto* updated = cues.getSelected())
            pushLive (*updated);
    }

    void commitLevels (double mainDb, const LevelMatrix& m, bool finished)
    {
        // the grid keeps showing (and committing to) the cue the edit started on, even if the selection moved
        // meanwhile (auto-follow, playhead lock); a finished edit releases it
        const int index = shownId.isNull() ? cues.getSelectedIndex() : cues.indexOf (shownId);

        if (! cues.isValidIndex (index) || refreshing)
            return;

        const Cue& cue = cues.get (index);
        const auto id = cue.id;
        const auto trimCopy = cue.trim;
        editing = ! finished;
        document.perform (ko ("레벨 변경"), [this, index, mainDb, m] { cues.update (index, [mainDb, m] (Cue& c) { c.gainDb = mainDb; c.levels = m; }); },
                          { "levels:" + id.toString(), false });
        engine.setLiveGainDb (id, mainDb);
        engine.setLiveLevels (id, m, trimCopy);
    }

    void commitPatch()
    {
        const auto* cue = cues.getSelected();

        if (cue == nullptr || refreshing || patchCombo.getSelectedId() == 0)
            return;

        const int index = patchCombo.getSelectedId() - 1;

        if (index < 0 || index >= (int) document.patches.size())
            return;

        const auto id = index == 0 ? juce::Uuid::null() : document.patches[(size_t) index].id;
        const int outputs = document.patches[(size_t) index].numCueOutputs;
        edit (ko ("패치 변경"), [id, outputs] (Cue& c)
        {
            c.patchId = id;

            if (c.levels.numInputs() > 0)
                c.levels.resize (c.levels.numInputs(), outputs);
        });
    }

    ProjectDocument& document;
    CueList& cues;
    AudioEngine& engine;
    juce::Uuid shownId = juce::Uuid::null();   // the cue the grid shows / commits to
    bool editing = false;                       // a drag or typing session is in flight
    juce::Label patchLabel, hint;
    juce::ComboBox patchCombo;
    juce::TextButton defaultsButton, silenceButton;
    juce::Viewport viewport;
    LevelMatrixComponent grid;
    bool refreshing = false;
    bool editable = true;
};

//==============================================================================
/** 트림 tab: fixed offsets (main + per cue output) added after the matrix. */
class CueInspector::TrimPanel : public juce::Component
{
public:
    TrimPanel (ProjectDocument& doc, AudioEngine& e) : document (doc), cues (doc.cues), engine (e)
    {
        styleLabel (hint, ko ("트림은 레벨 매트릭스 뒤에 더해지는 고정 오프셋입니다 (페이드 큐의 영향을 받지 않음). 더블클릭 = 0 dB"), 11.0f);
        addAndMakeVisible (hint);

        styleLabel (mainLabel, ko ("메인 트림 (dB)"));
        addAndMakeVisible (mainLabel);
        styleSlider (mainSlider);
        mainSlider.onValueChange = [this] { commit (-1, mainSlider.getValue()); };
        addAndMakeVisible (mainSlider);

        viewport.setViewedComponent (&strip, false);
        viewport.setScrollBarsShown (false, true);
        addAndMakeVisible (viewport);
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
        mainSlider.setEnabled (enabled);

        const int outputs = cue != nullptr ? document.cueOutputsFor (*cue) : 0;

        if (outputs != sliders.size())
        {
            sliders.clear();
            labels.clear();

            for (int k = 0; k < outputs; ++k)
            {
                auto* s = sliders.add (new juce::Slider());
                s->setSliderStyle (juce::Slider::LinearVertical);
                s->setRange (-60.0, LevelMatrix::maxDb, 0.1);
                s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 48, 18);
                s->setDoubleClickReturnValue (true, 0.0);
                s->setWantsKeyboardFocus (false);
                s->onValueChange = [this, k, s] { commit (k, s->getValue()); };
                strip.addAndMakeVisible (s);

                auto* l = labels.add (new juce::Label());
                l->setJustificationType (juce::Justification::centred);
                l->setColour (juce::Label::textColourId, Palette::dimText);
                l->setFont (juce::Font (juce::FontOptions (11.0f)));
                strip.addAndMakeVisible (l);
            }

            layoutStrip();
        }

        if (cue == nullptr)
        {
            mainSlider.setValue (0.0, juce::dontSendNotification);
            return;
        }

        const auto& patch = document.patchForCue (*cue);
        TrimLevels t = cue->trim;
        t.resize (outputs);
        mainSlider.setValue (t.mainDb, juce::dontSendNotification);

        for (int k = 0; k < outputs; ++k)
        {
            sliders[k]->setValue (t.outputDb[(size_t) k], juce::dontSendNotification);
            sliders[k]->setEnabled (enabled);
            labels[k]->setText (patch.cueOutputName (k), juce::dontSendNotification);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 6);
        hint.setBounds (area.removeFromTop (16));
        area.removeFromTop (6);
        auto row = area.removeFromTop (24);
        mainLabel.setBounds (row.removeFromLeft (110));
        mainSlider.setBounds (row.removeFromLeft (juce::jmin (400, row.getWidth())));
        area.removeFromTop (6);
        viewport.setBounds (area);
        layoutStrip();
    }

    void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

private:
    static void styleSlider (juce::Slider& s)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setRange (-60.0, LevelMatrix::maxDb, 0.1);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
        s.setTextValueSuffix (" dB");
        s.setDoubleClickReturnValue (true, 0.0);
        s.setWantsKeyboardFocus (false);
    }

    void layoutStrip()
    {
        const int w = 52;
        const int h = juce::jmax (80, viewport.getHeight() - 4);
        strip.setSize (juce::jmax (viewport.getWidth(), sliders.size() * w), h);

        for (int k = 0; k < sliders.size(); ++k)
        {
            juce::Rectangle<int> col (k * w, 0, w, h);
            labels[k]->setBounds (col.removeFromTop (16));
            sliders[k]->setBounds (col.reduced (2, 0));
        }
    }

    void commit (int output, double value)
    {
        const auto* cue = cues.getSelected();

        if (cue == nullptr || refreshing || ! editable)
            return;

        const int index = cues.getSelectedIndex();
        const int outputs = document.cueOutputsFor (*cue);
        const auto id = cue->id;
        TrimLevels t = cue->trim;
        t.resize (outputs);

        if (output < 0)
            t.mainDb = value;
        else if (output < (int) t.outputDb.size())
            t.outputDb[(size_t) output] = value;

        document.perform (ko ("트림 변경"), [this, index, t] { cues.update (index, [t] (Cue& c) { c.trim = t; }); },
                          { "trim:" + id.toString(), false });
        engine.setLiveLevels (id, cue->levels, t);
    }

    ProjectDocument& document;
    CueList& cues;
    AudioEngine& engine;
    juce::Label hint, mainLabel;
    juce::Slider mainSlider;
    juce::Viewport viewport;
    juce::Component strip;
    juce::OwnedArray<juce::Slider> sliders;
    juce::OwnedArray<juce::Label> labels;
    bool refreshing = false;
    bool editable = true;
};

//==============================================================================
/** 페이드 tab: target, duration, mode, level goals with active cells, rate goal, live preview. */
class CueInspector::FadePanel : public juce::Component
{
public:
    FadePanel (ProjectDocument& doc, AudioEngine& e) : document (doc), cues (doc.cues), engine (e)
    {
        styleLabel (targetLabel, ko ("대상 큐"));
        addAndMakeVisible (targetLabel);
        targetCombo.setWantsKeyboardFocus (false);
        targetCombo.setTooltip (ko ("이 페이드가 움직이는 오디오 큐 (재생 중인 인스턴스의 레벨을 바꿉니다)"));
        targetCombo.onChange = [this] { commitTarget(); };
        addAndMakeVisible (targetCombo);

        styleLabel (durationLabel, ko ("시간"));
        addAndMakeVisible (durationLabel);
        styleNumberEditor (durationEditor, "0123456789:.", 12);
        durationEditor.onReturnKey = [this] { commitDuration(); durationEditor.giveAwayKeyboardFocus(); };
        durationEditor.onFocusLost = [this] { commitDuration(); };
        addAndMakeVisible (durationEditor);

        auto toggle = [this] (juce::ToggleButton& t, const char* text, const char* editName, std::function<void (Cue&, bool)> apply)
        {
            styleToggle (t, ko (text));
            t.onClick = [this, &t, editName, apply]
            {
                const bool on = t.getToggleState();
                edit (ko (editName), [apply, on] (Cue& c) { apply (c, on); });
                refresh();
                applyPreview();
            };
            addAndMakeVisible (t);
        };

        toggle (relativeToggle, "상대 (현재값에 ±)", "상대 페이드", [] (Cue& c, bool v) { c.fade.relative = v; });
        toggle (stopToggle, "완료 시 대상 정지", "완료 시 정지", [] (Cue& c, bool v) { c.fade.stopTargetWhenDone = v; });
        toggle (levelsToggle, "레벨 페이드", "레벨 페이드", [] (Cue& c, bool v) { c.fade.fadeLevels = v; });
        toggle (rateToggle, "속도 페이드", "속도 페이드", [] (Cue& c, bool v) { c.fade.fadeRate = v; });
        relativeToggle.setTooltip (ko ("켜면 값이 목표가 아니라 현재값에 더하는 양(dB)이 됩니다"));

        styleLabel (rateLabel, ko ("목표 속도"));
        addAndMakeVisible (rateLabel);
        styleNumberEditor (rateEditor, "0123456789.", 6);
        rateEditor.onReturnKey = [this] { commitRate(); rateEditor.giveAwayKeyboardFocus(); };
        rateEditor.onFocusLost = [this] { commitRate(); };
        addAndMakeVisible (rateEditor);

        auto button = [this] (juce::TextButton& b, const char* text, std::function<void()> fn)
        {
            b.setButtonText (ko (text));
            b.setWantsKeyboardFocus (false);
            b.onClick = std::move (fn);
            addAndMakeVisible (b);
        };

        button (fetchButton, "대상에서 레벨 가져오기", [this] { fetchFromTarget(); });
        button (allOnButton, "전부 활성", [this] { setAllActive (true); });
        button (allOffButton, "전부 비활성", [this] { setAllActive (false); });
        fetchButton.setTooltip (ko ("대상 큐의 현재 레벨(재생 중이면 실시간 값)을 목표로 복사 (Ctrl+Shift+T)"));

        styleToggle (previewToggle, ko ("라이브 미리보기"));
        previewToggle.setTooltip (ko ("켜면 편집하는 목표값이 재생 중인 대상에 바로 반영되고, 끄면 원래대로 돌아갑니다"));
        previewToggle.onClick = [this] { togglePreview (previewToggle.getToggleState()); };
        addAndMakeVisible (previewToggle);

        styleLabel (hint, ko ("노란 점 = 활성 칸(페이드가 바꾸는 값), 빗금 = 제외. Alt+클릭 또는 우클릭으로 활성/비활성. 값 편집은 레벨 탭과 같음"), 11.0f);
        addAndMakeVisible (hint);

        viewport.setViewedComponent (&grid, false);
        viewport.setScrollBarsShown (true, true);
        addAndMakeVisible (viewport);
        grid.onChange = [this] (double mainDb, const LevelMatrix& m, bool) { commitLevels (mainDb, m); };
        grid.onActiveToggled = [this] (int kind, int in, int out, bool on) { commitActive (kind, in, out, on); };
    }

    ~FadePanel() override
    {
        if (previewing)
            togglePreview (false);
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
        const bool enabled = cue != nullptr && cue->isFade() && editable;

        if (cue != nullptr && cue->isFade())
        {
            bool editing = durationEditor.hasKeyboardFocus (true) || rateEditor.hasKeyboardFocus (true);

            if (! (editing && ! shownId.isNull() && shownId != cue->id && cues.indexOf (shownId) >= 0))
                shownId = cue->id;
        }

        if (previewing && (cue == nullptr || cue->id != previewCueId))
            togglePreview (false);

        targetCombo.clear (juce::dontSendNotification);
        targetCombo.addItem (ko ("(없음)"), 1);
        targetIds.clear();
        targetIds.push_back (juce::Uuid::null());
        int selectedTarget = 1;

        document.forEachList ([&] (CueList& list)   // a target may live in another list / cart
        {
            const auto prefix = &list == &cues ? juce::String() : document.getContainerInfo (document.containerOf (list.isEmpty() ? juce::Uuid::null() : list.get (0).id)).name + " / ";

            for (int i = 0; i < list.size(); ++i)
            {
                const auto& c = list.get (i);

                if (! c.makesSound())
                    continue;

                targetIds.push_back (c.id);
                targetCombo.addItem (prefix + (c.number.isNotEmpty() ? c.number + " " : "#" + juce::String (i + 1) + " ") + c.name, (int) targetIds.size());

                if (cue != nullptr && cue->fade.targetId == c.id)
                    selectedTarget = (int) targetIds.size();
            }
        });

        targetCombo.setSelectedId (selectedTarget, juce::dontSendNotification);

        for (auto* c : std::initializer_list<juce::Component*> { &targetCombo, &durationEditor, &relativeToggle, &stopToggle, &levelsToggle,
                                                                 &rateToggle, &rateEditor, &fetchButton, &allOnButton, &allOffButton, &previewToggle })
            c->setEnabled (enabled);

        grid.setEditable (enabled);
        grid.setLimits (document.settings.minLevelDb, document.settings.maxLevelDb);

        if (cue == nullptr || ! cue->isFade())
        {
            grid.setActiveFlags (nullptr, nullptr, nullptr, nullptr);
            grid.setLevels (0.0, LevelMatrix());
            return;
        }

        const auto& f = cue->fade;

        if (! durationEditor.hasKeyboardFocus (true))
            durationEditor.setText (formatTimeMs (f.durationSeconds), false);

        if (! rateEditor.hasKeyboardFocus (true))
            rateEditor.setText (juce::String (f.rate, 2), false);

        relativeToggle.setToggleState (f.relative, juce::dontSendNotification);
        stopToggle.setToggleState (f.stopTargetWhenDone, juce::dontSendNotification);
        levelsToggle.setToggleState (f.fadeLevels, juce::dontSendNotification);
        rateToggle.setToggleState (f.fadeRate, juce::dontSendNotification);
        rateEditor.setEnabled (enabled && f.fadeRate);
        previewToggle.setToggleState (previewing, juce::dontSendNotification);

        // the grid is sized like the target (channels x its patch's outputs)
        const auto* target = cues.findById (f.targetId);
        int inputs = f.levels.numInputs() > 0 ? f.levels.numInputs() : 2;
        int outputs = f.levels.numOutputs() > 0 ? f.levels.numOutputs() : 2;
        juce::StringArray inputNames, outputNames;

        if (target != nullptr)
        {
            inputs = target->numChannels > 0 ? target->numChannels : inputs;
            outputs = document.cueOutputsFor (*target);
            const auto& patch = document.patchForCue (*target);

            for (int k = 0; k < outputs; ++k)
                outputNames.add (patch.cueOutputName (k));
        }

        for (int i = 0; i < inputs; ++i)
            inputNames.add (inputs == 1 ? ko ("모노") : ko ("채널 ") + juce::String (i + 1));

        LevelMatrix goals = f.levels;
        goals.resize (inputs, outputs);
        FadeCueData sized = f;
        sized.resizeActive (inputs, outputs);
        grid.setLabels (inputNames, outputNames);
        grid.setLevels (f.mainDb, goals);
        grid.setActiveFlags (&sized.mainActive, &sized.inputActive, &sized.outputActive, &sized.crosspointActive);
        grid.setEditable (enabled && f.fadeLevels);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 6);
        auto row = area.removeFromTop (24);
        targetLabel.setBounds (row.removeFromLeft (48));
        targetCombo.setBounds (row.removeFromLeft (240));
        row.removeFromLeft (12);
        durationLabel.setBounds (row.removeFromLeft (32));
        durationEditor.setBounds (row.removeFromLeft (84));
        row.removeFromLeft (12);
        relativeToggle.setBounds (row.removeFromLeft (150));
        stopToggle.setBounds (row.removeFromLeft (140));
        area.removeFromTop (4);

        row = area.removeFromTop (24);
        levelsToggle.setBounds (row.removeFromLeft (100));
        fetchButton.setBounds (row.removeFromLeft (160));
        row.removeFromLeft (6);
        allOnButton.setBounds (row.removeFromLeft (80));
        row.removeFromLeft (4);
        allOffButton.setBounds (row.removeFromLeft (90));
        row.removeFromLeft (12);
        rateToggle.setBounds (row.removeFromLeft (100));
        rateLabel.setBounds (row.removeFromLeft (64));
        rateEditor.setBounds (row.removeFromLeft (60));
        row.removeFromLeft (12);
        previewToggle.setBounds (row.removeFromLeft (130));
        area.removeFromTop (4);
        hint.setBounds (area.removeFromTop (16));
        area.removeFromTop (4);
        viewport.setBounds (area);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

    /** Ctrl+Shift+T from the app. */
    void fetchFromTarget()
    {
        const auto* cue = cues.getSelected();

        if (cue == nullptr || ! cue->isFade() || ! editable)
            return;

        const auto* target = cues.findById (cue->fade.targetId);

        if (target == nullptr)
            return;

        double mainDb = target->gainDb;
        LevelMatrix fetched = target->levels;
        AudioEngine::LiveState live;

        if (engine.getLiveState (target->id, live))   // the running instance wins over the stored cue
        {
            mainDb = live.gainDb;
            fetched = live.levels;
        }

        const int inputs = target->numChannels > 0 ? target->numChannels : juce::jmax (1, fetched.numInputs());
        const int outputs = document.cueOutputsFor (*target);
        fetched.resize (inputs, outputs);
        edit (ko ("대상에서 레벨 가져오기"), [mainDb, fetched] (Cue& c)
        {
            c.fade.mainDb = mainDb;
            c.fade.levels = fetched;
            c.fade.resizeActive (fetched.numInputs(), fetched.numOutputs());
        });
        refresh();
        applyPreview();
    }

private:
    void edit (const juce::String& name, const std::function<void (Cue&)>& mutator, const juce::String& coalesceKey = {})
    {
        if (refreshing || ! editable)
            return;

        const int index = shownId.isNull() ? cues.getSelectedIndex() : cues.indexOf (shownId);

        if (! cues.isValidIndex (index) || ! cues.get (index).isFade())
            return;

        document.perform (name, [this, index, mutator] { cues.update (index, mutator); }, { coalesceKey, false });

        if (const auto* selected = cues.getSelected(); selected != nullptr && selected->id != shownId)
            refresh();
    }

    void commitTarget()
    {
        if (refreshing || targetCombo.getSelectedId() <= 0)
            return;

        const int index = targetCombo.getSelectedId() - 1;

        if (index < 0 || index >= (int) targetIds.size())
            return;

        const auto id = targetIds[(size_t) index];

        if (previewing)
            togglePreview (false);   // the preview belongs to the old target: put it back before switching

        const auto* target = cues.findById (id);
        const int inputs = target != nullptr && target->numChannels > 0 ? target->numChannels : 2;
        const int outputs = target != nullptr ? document.cueOutputsFor (*target) : 2;
        edit (ko ("페이드 대상"), [id, inputs, outputs] (Cue& c)
        {
            c.fade.targetId = id;
            c.fade.levels.resize (inputs, outputs);
            c.fade.resizeActive (inputs, outputs);
        });
        refresh();
    }

    void commitDuration()
    {
        if (refreshing)
            return;

        const double seconds = parseTimeText (durationEditor.getText());

        if (seconds < 0.0)
        {
            refresh();
            return;
        }

        edit (ko ("페이드 시간"), [seconds] (Cue& c) { c.fade.durationSeconds = seconds; });
    }

    void commitRate()
    {
        if (refreshing)
            return;

        const double rate = juce::jlimit (AudioCueData::minRate, AudioCueData::maxRate, rateEditor.getText().getDoubleValue());
        edit (ko ("페이드 목표 속도"), [rate] (Cue& c) { c.fade.rate = rate; });
        refresh();
    }

    void commitLevels (double mainDb, const LevelMatrix& m)
    {
        const auto* cue = cues.getSelected();

        if (cue == nullptr || refreshing)
            return;

        edit (ko ("페이드 레벨"), [mainDb, m] (Cue& c)
        {
            c.fade.mainDb = mainDb;
            c.fade.levels = m;
            c.fade.resizeActive (m.numInputs(), m.numOutputs());
        }, "fadelevels:" + cue->id.toString());
        applyPreview();
    }

    void commitActive (int kind, int in, int out, bool on)
    {
        if (refreshing)
            return;

        edit (ko ("페이드 활성 칸"), [kind, in, out, on] (Cue& c)
        {
            c.fade.resizeActive (juce::jmax (c.fade.levels.numInputs(), in + 1), juce::jmax (c.fade.levels.numOutputs(), out + 1));

            switch (kind)
            {
                case 0: c.fade.mainActive = on; break;
                case 1: c.fade.setInputActive (in, on); break;
                case 2: c.fade.setOutputActive (out, on); break;
                default: c.fade.setCrosspointActive (in, out, on); break;
            }
        });
        applyPreview();
    }

    void setAllActive (bool on)
    {
        edit (on ? ko ("전부 활성") : ko ("전부 비활성"), [on] (Cue& c)
        {
            c.fade.resizeActive (c.fade.levels.numInputs(), c.fade.levels.numOutputs());
            c.fade.setAllActive (on);
        });
        refresh();
        applyPreview();
    }

    //==========================================================================
    void togglePreview (bool on)
    {
        const auto* cue = cues.getSelected();

        if (on)
        {
            if (cue == nullptr || ! cue->isFade() || ! engine.getLiveState (cue->fade.targetId, previewBackup))
            {
                previewToggle.setToggleState (false, juce::dontSendNotification);
                previewing = false;
                return;
            }

            previewing = true;
            previewCueId = cue->id;
            previewTargetId = cue->fade.targetId;
            applyPreview();
            return;
        }

        if (previewing)
        {
            previewing = false;

            if (engine.isPlaying (previewTargetId))
            {
                engine.setLiveGainDb (previewTargetId, previewBackup.gainDb);
                engine.setLiveLevels (previewTargetId, previewBackup.levels, previewBackup.trim);
            }
        }

        previewToggle.setToggleState (false, juce::dontSendNotification);
    }

    /** With the preview on: the goals of the active cells are pushed onto the running target at once. */
    void applyPreview()
    {
        if (! previewing)
            return;

        const auto* cue = cues.findById (previewCueId);

        if (cue == nullptr || ! cue->isFade() || ! cue->fade.fadeLevels)
            return;

        AudioEngine::LiveState live;

        if (! engine.getLiveState (previewTargetId, live))
            return;

        const auto& f = cue->fade;
        const double minDb = document.settings.minLevelDb, maxDb = document.settings.maxLevelDb;
        auto goal = [&] (double from, double value)
        {
            if (f.relative)
                return LevelMatrix::isSilent (from) ? from : juce::jlimit (minDb, maxDb, from + value);

            return LevelMatrix::isSilent (value) || value < minDb ? LevelMatrix::silentDb : juce::jmin (maxDb, value);
        };

        LevelMatrix goals = f.levels;
        goals.resize (live.levels.numInputs(), live.levels.numOutputs());

        if (f.mainActive)
            live.gainDb = f.relative ? juce::jlimit (Cue::minGainDb, Cue::maxGainDb, previewBackup.gainDb + f.mainDb) : f.mainDb;

        for (int i = 0; i < live.levels.numInputs(); ++i)
        {
            if (f.isInputActive (i))
                live.levels.inputDb[(size_t) i] = goal (previewBackup.levels.numInputs() > i ? previewBackup.levels.inputDb[(size_t) i] : 0.0, goals.inputDb[(size_t) i]);

            for (int o = 0; o < live.levels.numOutputs(); ++o)
                if (f.isCrosspointActive (i, o))
                {
                    const double from = previewBackup.levels.numInputs() > i && previewBackup.levels.numOutputs() > o ? previewBackup.levels.crosspointDb[(size_t) i][(size_t) o] : 0.0;
                    live.levels.crosspointDb[(size_t) i][(size_t) o] = goal (from, goals.crosspointDb[(size_t) i][(size_t) o]);
                }
        }

        for (int o = 0; o < live.levels.numOutputs(); ++o)
            if (f.isOutputActive (o))
                live.levels.outputDb[(size_t) o] = goal (previewBackup.levels.numOutputs() > o ? previewBackup.levels.outputDb[(size_t) o] : 0.0, goals.outputDb[(size_t) o]);

        engine.setLiveGainDb (previewTargetId, live.gainDb);
        engine.setLiveLevels (previewTargetId, live.levels, live.trim);
    }

    ProjectDocument& document;
    CueList& cues;
    AudioEngine& engine;
    juce::Label targetLabel, durationLabel, rateLabel, hint;
    juce::ComboBox targetCombo;
    std::vector<juce::Uuid> targetIds;
    juce::TextEditor durationEditor, rateEditor;
    juce::ToggleButton relativeToggle, stopToggle, levelsToggle, rateToggle, previewToggle;
    juce::TextButton fetchButton, allOnButton, allOffButton;
    juce::Viewport viewport;
    LevelMatrixComponent grid;
    juce::Uuid shownId = juce::Uuid::null();
    bool previewing = false;
    juce::Uuid previewCueId = juce::Uuid::null(), previewTargetId = juce::Uuid::null();
    AudioEngine::LiveState previewBackup;
    bool refreshing = false;
    bool editable = true;
};

//==============================================================================
/** 커브 tab: the fade's curve shape / domain. */
class CueInspector::CurvePanel : public juce::Component
{
public:
    explicit CurvePanel (ProjectDocument& doc) : document (doc), cues (doc.cues)
    {
        editor.onChange = [this] (const FadeCurve& curve, bool finished) { commit (curve, finished); };
        addAndMakeVisible (editor);
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
        const bool enabled = cue != nullptr && cue->isFade() && editable;
        editor.setEditable (enabled);

        if (cue != nullptr && cue->isFade())
        {
            shownId = cue->id;
            editor.setCurve (cue->fade.curve);
        }
    }

    void resized() override { editor.setBounds (getLocalBounds()); }
    void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

private:
    void commit (const FadeCurve& curve, bool finished)
    {
        if (refreshing || ! editable)
            return;

        const int index = shownId.isNull() ? cues.getSelectedIndex() : cues.indexOf (shownId);

        if (! cues.isValidIndex (index) || ! cues.get (index).isFade())
            return;

        document.perform (ko ("페이드 커브"), [this, index, curve] { cues.update (index, [curve] (Cue& c) { c.fade.curve = curve; }); },
                          { finished ? juce::String() : "curve:" + shownId.toString(), false });
    }

    ProjectDocument& document;
    CueList& cues;
    CurveEditor editor;
    juce::Uuid shownId = juce::Uuid::null();
    bool refreshing = false;
    bool editable = true;
};

//==============================================================================
/** 파라미터 tab of a fade cue: the target's VST3 parameters this fade drives. */
class CueInspector::FadeParamsPanel : public juce::Component
{
public:
    FadeParamsPanel (ProjectDocument& doc, AudioEngine& e) : document (doc), cues (doc.cues), engine (e)
    {
        styleLabel (hint, ko ("대상 큐의 VST3 인서트 파라미터. 체크한 파라미터가 페이드 시간 동안 현재값에서 목표값으로 움직입니다 (큐 출력·장치 출력 인서트는 불가)"), 11.0f);
        addAndMakeVisible (hint);
        viewport.setViewedComponent (&strip, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
    }

    void setEditable (bool shouldBeEditable)
    {
        editable = shouldBeEditable;
        refresh();
    }

    void refresh()
    {
        if (selfEditing)
        {
            // a row's own slider / toggle is calling back: rebuilding would delete the control mid-callback
            if (! refreshQueued)
            {
                refreshQueued = true;
                juce::Component::SafePointer<FadeParamsPanel> safeThis (this);
                juce::MessageManager::callAsync ([safeThis]
                {
                    if (safeThis != nullptr)
                    {
                        safeThis->refreshQueued = false;
                        safeThis->refresh();
                    }
                });
            }

            return;
        }

        const juce::ScopedValueSetter<bool> guard (refreshing, true);
        const auto* cue = cues.getSelected();
        rows.clear();

        if (cue == nullptr || ! cue->isFade())
        {
            hint.setText (ko ("페이드 큐를 선택하세요"), juce::dontSendNotification);
            layoutRows();
            return;
        }

        shownId = cue->id;
        auto* chain = engine.findCueChain (cue->fade.targetId);

        if (chain == nullptr || chain->getNumSlots() == 0)
        {
            hint.setText (ko ("대상 큐에 VST3 인서트가 없습니다 (대상의 이펙트 탭에서 추가)"), juce::dontSendNotification);
            layoutRows();
            return;
        }

        hint.setText (ko ("대상 큐의 VST3 인서트 파라미터. 체크한 파라미터가 페이드 시간 동안 현재값에서 목표값으로 움직입니다"), juce::dontSendNotification);

        for (int slot = 0; slot < chain->getNumSlots(); ++slot)
        {
            auto* plugin = chain->getSlot (slot).plugin.get();

            if (plugin == nullptr)
                continue;

            const auto& parameters = plugin->getParameters();
            const int shown = juce::jmin (parameters.size(), maxParamsPerPlugin);

            for (int p = 0; p < shown; ++p)
            {
                auto* param = parameters[p];

                if (param == nullptr)
                    continue;

                auto* r = rows.add (new Row());
                r->slot = slot;
                r->parameter = p;
                const auto* entry = findEntry (cue->fade, slot, p);

                r->active.setButtonText (juce::String (slot + 1) + ". " + plugin->getName() + " · " + param->getName (40));
                r->active.setColour (juce::ToggleButton::textColourId, Palette::text);
                r->active.setColour (juce::ToggleButton::tickColourId, Palette::standby);
                r->active.setWantsKeyboardFocus (false);
                r->active.setToggleState (entry != nullptr && entry->active, juce::dontSendNotification);
                r->active.setEnabled (editable);
                r->active.onClick = [this, slot, p, r] { setActive (slot, p, r->active.getToggleState()); };
                strip.addAndMakeVisible (r->active);

                r->value.setSliderStyle (juce::Slider::LinearHorizontal);
                r->value.setRange (0.0, 1.0, 0.001);
                r->value.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                r->value.setWantsKeyboardFocus (false);
                r->value.setValue (entry != nullptr ? entry->value : param->getValue(), juce::dontSendNotification);
                r->value.setEnabled (editable && entry != nullptr && entry->active);
                r->value.onValueChange = [this, slot, p, r] { setValue (slot, p, (float) r->value.getValue()); };
                strip.addAndMakeVisible (r->value);

                r->text.setColour (juce::Label::textColourId, Palette::dimText);
                r->text.setFont (juce::Font (juce::FontOptions (12.0f)));
                r->text.setJustificationType (juce::Justification::centredRight);
                r->text.setText (param->getText ((float) r->value.getValue(), 24), juce::dontSendNotification);
                strip.addAndMakeVisible (r->text);
            }
        }

        layoutRows();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 6);
        hint.setBounds (area.removeFromTop (16));
        area.removeFromTop (4);
        viewport.setBounds (area);
        layoutRows();
    }

    void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

private:
    static constexpr int maxParamsPerPlugin = 96;

    struct Row
    {
        int slot = 0, parameter = 0;
        juce::ToggleButton active;
        juce::Slider value;
        juce::Label text;
    };

    /** The parameter behind a row, looked up afresh (the chain may have been rebuilt by an undo meanwhile). */
    juce::AudioProcessorParameter* resolveParameter (int slot, int parameter) const
    {
        const auto* cue = shownId.isNull() ? cues.getSelected() : cues.findById (shownId);

        if (cue == nullptr || ! cue->isFade())
            return nullptr;

        auto* chain = engine.findCueChain (cue->fade.targetId);

        if (chain == nullptr || slot < 0 || slot >= chain->getNumSlots())
            return nullptr;

        auto* plugin = chain->getSlot (slot).plugin.get();

        if (plugin == nullptr)
            return nullptr;

        const auto& parameters = plugin->getParameters();
        return parameter >= 0 && parameter < parameters.size() ? parameters[parameter] : nullptr;
    }

    static const ParamFade* findEntry (const FadeCueData& f, int slot, int parameter)
    {
        for (const auto& p : f.params)
            if (p.slot == slot && p.parameter == parameter)
                return &p;

        return nullptr;
    }

    void layoutRows()
    {
        const int rowH = 26;
        strip.setSize (juce::jmax (100, viewport.getWidth() - 14), juce::jmax (viewport.getHeight(), rows.size() * rowH));

        for (int i = 0; i < rows.size(); ++i)
        {
            auto* r = rows[i];
            auto row = juce::Rectangle<int> (0, i * rowH, strip.getWidth(), rowH).reduced (0, 2);
            r->active.setBounds (row.removeFromLeft (juce::jmax (200, row.getWidth() / 2)));
            r->text.setBounds (row.removeFromRight (110));
            r->value.setBounds (row.reduced (6, 0));
        }
    }

    void edit (const juce::String& name, const std::function<void (Cue&)>& mutator, const juce::String& coalesceKey = {})
    {
        if (refreshing || ! editable)
            return;

        const int index = shownId.isNull() ? cues.getSelectedIndex() : cues.indexOf (shownId);

        if (! cues.isValidIndex (index) || ! cues.get (index).isFade())
            return;

        document.perform (name, [this, index, mutator] { cues.update (index, mutator); }, { coalesceKey, false });
    }

    void setActive (int slot, int parameter, bool on)
    {
        const juce::ScopedValueSetter<bool> guard (selfEditing, true);
        float current = 0.0f;

        for (auto* r : rows)
            if (r->slot == slot && r->parameter == parameter)
            {
                current = (float) r->value.getValue();
                r->value.setEnabled (editable && on);
            }

        edit (ko ("파라미터 페이드"), [slot, parameter, on, current] (Cue& c)
        {
            for (auto& p : c.fade.params)
                if (p.slot == slot && p.parameter == parameter)
                {
                    p.active = on;
                    return;
                }

            ParamFade p;
            p.slot = slot;
            p.parameter = parameter;
            p.value = current;
            p.active = on;
            c.fade.params.push_back (p);
        });
    }

    void setValue (int slot, int parameter, float value)
    {
        const juce::ScopedValueSetter<bool> guard (selfEditing, true);

        if (auto* param = resolveParameter (slot, parameter))
            for (auto* r : rows)
                if (r->slot == slot && r->parameter == parameter)
                    r->text.setText (param->getText (value, 24), juce::dontSendNotification);

        edit (ko ("파라미터 목표"), [slot, parameter, value] (Cue& c)
        {
            for (auto& p : c.fade.params)
                if (p.slot == slot && p.parameter == parameter)
                {
                    p.value = value;
                    return;
                }

            ParamFade p;
            p.slot = slot;
            p.parameter = parameter;
            p.value = value;
            c.fade.params.push_back (p);
        }, "fadeparam:" + shownId.toString() + ":" + juce::String (slot) + ":" + juce::String (parameter));
    }

    ProjectDocument& document;
    CueList& cues;
    AudioEngine& engine;
    juce::Label hint;
    juce::Viewport viewport;
    juce::Component strip;
    juce::OwnedArray<Row> rows;
    juce::Uuid shownId = juce::Uuid::null();
    bool refreshing = false;
    bool selfEditing = false;    // a row's control is in its callback
    bool refreshQueued = false;
    bool editable = true;
};

//==============================================================================
/** 디밴프 tab: target and what happens at the loop point. */
class CueInspector::DevampPanel : public juce::Component
{
public:
    explicit DevampPanel (ProjectDocument& doc) : document (doc), cues (doc.cues)
    {
        styleLabel (targetLabel, ko ("대상 큐"));
        addAndMakeVisible (targetLabel);
        targetCombo.setWantsKeyboardFocus (false);
        targetCombo.onChange = [this] { commitTarget(); };
        addAndMakeVisible (targetCombo);

        styleToggle (startNextToggle, ko ("반복 끝에서 다음 큐 시작"));
        startNextToggle.setTooltip (ko ("대상이 지금 도는 반복(또는 무한 슬라이스)을 마치는 순간, 이 큐 다음 큐를 시작합니다"));
        startNextToggle.onClick = [this] { const bool on = startNextToggle.getToggleState(); edit (ko ("디밴프: 다음 큐"), [on] (Cue& c) { c.devamp.startNextCue = on; }); };
        addAndMakeVisible (startNextToggle);

        styleToggle (stopToggle, ko ("반복 끝에서 대상 정지"));
        stopToggle.setTooltip (ko ("켜면 반복 끝에서 대상을 멈춥니다. 끄면 반복을 빠져나와 다음 슬라이스 / 나머지 구간을 이어서 재생합니다"));
        stopToggle.onClick = [this] { const bool on = stopToggle.getToggleState(); edit (ko ("디밴프: 대상 정지"), [on] (Cue& c) { c.devamp.stopTarget = on; }); };
        addAndMakeVisible (stopToggle);

        styleLabel (hint, ko ("디밴프 = 루프 중인 큐를 음악적으로 끝내기. 실행 시점에 대상이 재생 중이어야 합니다 (아니면 실패 상태 메시지)."), 11.0f);
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
        const bool enabled = cue != nullptr && cue->isDevamp() && editable;

        targetCombo.clear (juce::dontSendNotification);
        targetCombo.addItem (ko ("(없음)"), 1);
        targetIds.clear();
        targetIds.push_back (juce::Uuid::null());
        int selectedTarget = 1;

        document.forEachList ([&] (CueList& list)
        {
            const auto prefix = &list == &cues ? juce::String() : document.getContainerInfo (document.containerOf (list.isEmpty() ? juce::Uuid::null() : list.get (0).id)).name + " / ";

            for (int i = 0; i < list.size(); ++i)
            {
                const auto& c = list.get (i);

                if (! c.makesSound())
                    continue;

                targetIds.push_back (c.id);
                targetCombo.addItem (prefix + (c.number.isNotEmpty() ? c.number + " " : "#" + juce::String (i + 1) + " ") + c.name, (int) targetIds.size());

                if (cue != nullptr && cue->devamp.targetId == c.id)
                    selectedTarget = (int) targetIds.size();
            }
        });

        targetCombo.setSelectedId (selectedTarget, juce::dontSendNotification);

        for (auto* c : std::initializer_list<juce::Component*> { &targetCombo, &startNextToggle, &stopToggle })
            c->setEnabled (enabled);

        if (cue != nullptr && cue->isDevamp())
        {
            shownId = cue->id;
            startNextToggle.setToggleState (cue->devamp.startNextCue, juce::dontSendNotification);
            stopToggle.setToggleState (cue->devamp.stopTarget, juce::dontSendNotification);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 6);
        auto row = area.removeFromTop (24);
        targetLabel.setBounds (row.removeFromLeft (48));
        targetCombo.setBounds (row.removeFromLeft (260));
        area.removeFromTop (6);
        row = area.removeFromTop (24);
        startNextToggle.setBounds (row.removeFromLeft (200));
        stopToggle.setBounds (row.removeFromLeft (200));
        area.removeFromTop (6);
        hint.setBounds (area.removeFromTop (16));
    }

    void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

private:
    void edit (const juce::String& name, const std::function<void (Cue&)>& mutator)
    {
        if (refreshing || ! editable)
            return;

        const int index = shownId.isNull() ? cues.getSelectedIndex() : cues.indexOf (shownId);

        if (! cues.isValidIndex (index) || ! cues.get (index).isDevamp())
            return;

        document.perform (name, [this, index, mutator] { cues.update (index, mutator); });
    }

    void commitTarget()
    {
        if (refreshing || targetCombo.getSelectedId() <= 0)
            return;

        const int index = targetCombo.getSelectedId() - 1;

        if (index < 0 || index >= (int) targetIds.size())
            return;

        const auto id = targetIds[(size_t) index];
        edit (ko ("디밴프 대상"), [id] (Cue& c) { c.devamp.targetId = id; });
    }

    ProjectDocument& document;
    CueList& cues;
    juce::Label targetLabel, hint;
    juce::ComboBox targetCombo;
    std::vector<juce::Uuid> targetIds;
    juce::ToggleButton startNextToggle, stopToggle;
    juce::Uuid shownId = juce::Uuid::null();
    bool refreshing = false;
    bool editable = true;
};

//==============================================================================
/** 입력 tab: which device inputs a mic cue takes. */
class CueInspector::MicPanel : public juce::Component
{
public:
    MicPanel (ProjectDocument& doc, AudioEngine& e) : document (doc), cues (doc.cues), engine (e)
    {
        styleLabel (firstLabel, ko ("첫 입력 채널"));
        addAndMakeVisible (firstLabel);
        firstEditor.setJustification (juce::Justification::centredRight);
        firstEditor.setSelectAllWhenFocused (true);
        firstEditor.setInputRestrictions (3, "0123456789");
        firstEditor.onReturnKey = [this] { commit(); };
        firstEditor.onFocusLost = [this] { commit(); };
        addAndMakeVisible (firstEditor);

        styleLabel (countLabel, ko ("채널 수"));
        addAndMakeVisible (countLabel);
        countEditor.setJustification (juce::Justification::centredRight);
        countEditor.setSelectAllWhenFocused (true);
        countEditor.setInputRestrictions (2, "0123456789");
        countEditor.onReturnKey = [this] { commit(); };
        countEditor.onFocusLost = [this] { commit(); };
        addAndMakeVisible (countEditor);

        styleLabel (deviceLabel, "", 12.0f);
        addAndMakeVisible (deviceLabel);
        styleLabel (hint, ko ("장치 입력이 레벨 탭의 행이 됩니다 (마이크 큐는 정지할 때까지 재생). 입력 채널은 오디오 > 오디오 설정에서 켭니다 — 프로젝트에 마이크 큐가 있으면 필요한 만큼 자동으로 켭니다."), 11.0f);
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
        const bool enabled = cue != nullptr && cue->isMic() && editable;
        firstEditor.setEnabled (enabled);
        countEditor.setEnabled (enabled);

        if (cue == nullptr || ! cue->isMic())
            return;

        shownId = cue->id;

        if (! firstEditor.hasKeyboardFocus (true))
            firstEditor.setText (juce::String (cue->mic.firstInput + 1), false);

        if (! countEditor.hasKeyboardFocus (true))
            countEditor.setText (juce::String (cue->mic.numInputs), false);

        const int available = engine.getNumDeviceInputs();
        const int needed = cue->mic.firstInput + cue->mic.numInputs;
        deviceLabel.setText (available <= 0 ? ko ("장치 입력이 열려 있지 않습니다")
                             : needed > available ? ko ("장치 입력 ") + juce::String (available) + ko ("개 열림 — 이 큐는 ") + juce::String (needed) + ko ("개까지 필요 (없는 채널은 무음)")
                             : ko ("장치 입력 ") + juce::String (available) + ko ("개 열림"), juce::dontSendNotification);
        deviceLabel.setColour (juce::Label::textColourId, available <= 0 || needed > available ? Palette::missing : Palette::dimText);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 6);
        auto row = area.removeFromTop (24);
        firstLabel.setBounds (row.removeFromLeft (80));
        firstEditor.setBounds (row.removeFromLeft (60));
        row.removeFromLeft (16);
        countLabel.setBounds (row.removeFromLeft (56));
        countEditor.setBounds (row.removeFromLeft (50));
        row.removeFromLeft (16);
        deviceLabel.setBounds (row);
        area.removeFromTop (6);
        hint.setBounds (area.removeFromTop (16));
    }

    void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

private:
    void commit()
    {
        if (refreshing || ! editable)
            return;

        const int index = shownId.isNull() ? cues.getSelectedIndex() : cues.indexOf (shownId);

        if (! cues.isValidIndex (index) || ! cues.get (index).isMic())
            return;

        const auto& cue = cues.get (index);
        const int first = juce::jlimit (1, 64, firstEditor.getText().getIntValue()) - 1;
        const int count = juce::jlimit (1, LevelMatrix::maxInputs, countEditor.getText().getIntValue());

        if (first == cue.mic.firstInput && count == cue.mic.numInputs)
            return;

        document.perform (ko ("마이크 입력"), [this, index, first, count]
        {
            cues.update (index, [first, count] (Cue& c)
            {
                c.mic.firstInput = first;
                c.mic.numInputs = count;
                c.levels.resize (count, c.levels.numOutputs() > 0 ? c.levels.numOutputs() : 2);
            });
        });
    }

    ProjectDocument& document;
    CueList& cues;
    AudioEngine& engine;
    juce::Label firstLabel, countLabel, deviceLabel, hint;
    juce::TextEditor firstEditor, countEditor;
    juce::Uuid shownId = juce::Uuid::null();
    bool refreshing = false;
    bool editable = true;
};

//==============================================================================
/** 동작 tab: what a control cue does, to which cue, with which time / new target. */
class CueInspector::ControlPanel : public juce::Component
{
public:
    explicit ControlPanel (ProjectDocument& doc) : document (doc), cues (doc.cues)
    {
        styleLabel (kindLabel, ko ("동작"));
        addAndMakeVisible (kindLabel);
        kindCombo.addItem (ko ("시작 — 대상을 GO처럼 시작 (플레이헤드는 그대로), 일시정지 중이면 재개"), 1);
        kindCombo.addItem (ko ("정지 — 대상을 바로 정지"), 2);
        kindCombo.addItem (ko ("일시정지 — 대상을 멈춤 (시작 큐로 재개)"), 3);
        kindCombo.addItem (ko ("로드 — 대상을 지정 시각에 미리 로드"), 4);
        kindCombo.addItem (ko ("리셋 — 대상 정지 + 예약 취소 + 재생 기록 지움"), 5);
        kindCombo.addItem (ko ("이동 — 플레이헤드를 대상으로"), 6);
        kindCombo.addItem (ko ("대기 — 정해진 시간 동안 아무것도 안 함"), 7);
        kindCombo.addItem (ko ("메모 — 아무것도 안 함"), 8);
        kindCombo.addItem (ko ("활성화 — 대상을 아밍"), 9);
        kindCombo.addItem (ko ("비활성화 — 대상의 아밍 해제"), 10);
        kindCombo.addItem (ko ("대상 변경 — 대상(페이드/디밴프/제어 큐)의 대상을 바꿈"), 11);
        kindCombo.setWantsKeyboardFocus (false);
        kindCombo.onChange = [this]
        {
            if (refreshing || kindCombo.getSelectedId() <= 0)
                return;

            const auto kind = (ControlKind) (kindCombo.getSelectedId() - 1);
            edit (ko ("제어 큐 동작"), [kind] (Cue& c) { c.control.kind = kind; });
        };
        addAndMakeVisible (kindCombo);

        styleLabel (targetLabel, ko ("대상 큐"));
        addAndMakeVisible (targetLabel);
        targetCombo.setWantsKeyboardFocus (false);
        targetCombo.onChange = [this] { commitTarget (targetCombo, targetIds, false); };
        addAndMakeVisible (targetCombo);

        styleLabel (secondLabel, ko ("새 대상"));
        addAndMakeVisible (secondLabel);
        secondCombo.setWantsKeyboardFocus (false);
        secondCombo.onChange = [this] { commitTarget (secondCombo, secondIds, true); };
        addAndMakeVisible (secondCombo);

        styleLabel (secondsLabel, ko ("시간"));
        addAndMakeVisible (secondsLabel);
        secondsEditor.setJustification (juce::Justification::centredRight);
        secondsEditor.setSelectAllWhenFocused (true);
        secondsEditor.onReturnKey = [this] { commitSeconds(); };
        secondsEditor.onFocusLost = [this] { commitSeconds(); };
        addAndMakeVisible (secondsEditor);
        styleLabel (secondsUnit, ko ("초"));
        addAndMakeVisible (secondsUnit);

        styleLabel (hint, ko ("제어 큐는 실행되는 순간 한 번 동작합니다. 대기 큐 뒤에 자동 팔로우를 걸면 그 시간 뒤에 다음 큐가 시작됩니다."), 11.0f);
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
        const bool enabled = cue != nullptr && cue->isControl() && editable;

        for (auto* c : std::initializer_list<juce::Component*> { &kindCombo, &targetCombo, &secondCombo, &secondsEditor })
            c->setEnabled (enabled);

        if (cue == nullptr || ! cue->isControl())
            return;

        shownId = cue->id;
        kindCombo.setSelectedId ((int) cue->control.kind + 1, juce::dontSendNotification);
        fillTargets (targetCombo, targetIds, cue->control.targetId, cue->id, false);
        fillTargets (secondCombo, secondIds, cue->control.secondTargetId, cue->id, true);

        const bool needsTarget = cue->control.needsTarget();
        const bool isTarget = cue->control.kind == ControlKind::target;
        const bool hasSeconds = cue->control.kind == ControlKind::wait || cue->control.kind == ControlKind::load;

        for (auto* c : std::initializer_list<juce::Component*> { &targetLabel, &targetCombo })
            c->setVisible (needsTarget);

        for (auto* c : std::initializer_list<juce::Component*> { &secondLabel, &secondCombo })
            c->setVisible (isTarget);

        for (auto* c : std::initializer_list<juce::Component*> { &secondsLabel, &secondsEditor, &secondsUnit })
            c->setVisible (hasSeconds);

        secondsLabel.setText (cue->control.kind == ControlKind::load ? ko ("로드 시각") : ko ("대기 시간"), juce::dontSendNotification);

        if (! secondsEditor.hasKeyboardFocus (true))
            secondsEditor.setText (juce::String (cue->control.seconds, 2), false);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 6);
        auto row = area.removeFromTop (24);
        kindLabel.setBounds (row.removeFromLeft (48));
        kindCombo.setBounds (row.removeFromLeft (420));
        area.removeFromTop (6);
        row = area.removeFromTop (24);
        targetLabel.setBounds (row.removeFromLeft (48));
        targetCombo.setBounds (row.removeFromLeft (260));
        row.removeFromLeft (12);
        secondLabel.setBounds (row.removeFromLeft (48));
        secondCombo.setBounds (row.removeFromLeft (260));
        row.removeFromLeft (12);
        secondsLabel.setBounds (row.removeFromLeft (60));
        secondsEditor.setBounds (row.removeFromLeft (70));
        secondsUnit.setBounds (row.removeFromLeft (24));
        area.removeFromTop (6);
        hint.setBounds (area.removeFromTop (16));
    }

    void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

private:
    void fillTargets (juce::ComboBox& combo, std::vector<juce::Uuid>& ids, const juce::Uuid& current, const juce::Uuid& selfId, bool onlyTargeting)
    {
        combo.clear (juce::dontSendNotification);
        combo.addItem (ko ("(없음)"), 1);
        ids.clear();
        ids.push_back (juce::Uuid::null());
        int selectedId = 1;

        document.forEachList ([&] (CueList& list)
        {
            const auto prefix = &list == &cues ? juce::String() : document.getContainerInfo (document.containerOf (list.isEmpty() ? juce::Uuid::null() : list.get (0).id)).name + " / ";

            for (int i = 0; i < list.size(); ++i)
            {
                const auto& c = list.get (i);

                if (c.id == selfId || (onlyTargeting && ! c.hasTarget()))
                    continue;

                ids.push_back (c.id);
                combo.addItem (prefix + (c.number.isNotEmpty() ? c.number + " " : "#" + juce::String (i + 1) + " ") + c.name, (int) ids.size());

                if (current == c.id)
                    selectedId = (int) ids.size();
            }
        });

        combo.setSelectedId (selectedId, juce::dontSendNotification);
    }

    void edit (const juce::String& name, const std::function<void (Cue&)>& mutator)
    {
        if (refreshing || ! editable)
            return;

        const int index = shownId.isNull() ? cues.getSelectedIndex() : cues.indexOf (shownId);

        if (! cues.isValidIndex (index) || ! cues.get (index).isControl())
            return;

        document.perform (name, [this, index, mutator] { cues.update (index, mutator); });
    }

    void commitTarget (juce::ComboBox& combo, const std::vector<juce::Uuid>& ids, bool second)
    {
        if (refreshing || combo.getSelectedId() <= 0)
            return;

        const int index = combo.getSelectedId() - 1;

        if (index < 0 || index >= (int) ids.size())
            return;

        const auto id = ids[(size_t) index];
        edit (second ? ko ("제어 큐 새 대상") : ko ("제어 큐 대상"), [id, second] (Cue& c) { (second ? c.control.secondTargetId : c.control.targetId) = id; });
    }

    void commitSeconds()
    {
        const double seconds = secondsEditor.getText().trim().getDoubleValue();
        const auto* cue = cues.getSelected();

        if (cue == nullptr || ! cue->isControl() || juce::approximatelyEqual (seconds, cue->control.seconds))
            return;

        edit (ko ("제어 큐 시간"), [seconds] (Cue& c) { c.control.seconds = juce::jlimit (0.0, Cue::maxWaitSeconds, seconds); });
    }

    ProjectDocument& document;
    CueList& cues;
    juce::Label kindLabel, targetLabel, secondLabel, secondsLabel, secondsUnit, hint;
    juce::ComboBox kindCombo, targetCombo, secondCombo;
    juce::TextEditor secondsEditor;
    std::vector<juce::Uuid> targetIds, secondIds;
    juce::Uuid shownId = juce::Uuid::null();
    bool refreshing = false;
    bool editable = true;
};

//==============================================================================
/** 그룹 tab: mode, playlist options and (for a timeline group) the children's start times as draggable bars. */
class CueInspector::GroupPanel : public juce::Component
{
public:
    explicit GroupPanel (ProjectDocument& doc) : document (doc), cues (doc.cues), timeline (*this)
    {
        styleLabel (modeLabel, ko ("모드"));
        addAndMakeVisible (modeLabel);
        modeCombo.addItem (ko ("타임라인 — 자식 전부 동시에 (각자 프리웨이트)"), 1);
        modeCombo.addItem (ko ("플레이리스트 — 차례로"), 2);
        modeCombo.addItem (ko ("첫 큐 시작 후 그룹 안으로 진입"), 3);
        modeCombo.addItem (ko ("첫 큐 시작 (플레이헤드는 그룹 뒤로)"), 4);
        modeCombo.addItem (ko ("랜덤 — 한 바퀴에 한 번씩"), 5);
        modeCombo.setWantsKeyboardFocus (false);
        modeCombo.onChange = [this]
        {
            if (refreshing || modeCombo.getSelectedId() <= 0)
                return;

            const auto mode = (GroupMode) (modeCombo.getSelectedId() - 1);
            edit (ko ("그룹 모드"), [mode] (Cue& c) { c.group.mode = mode; });
        };
        addAndMakeVisible (modeCombo);

        styleToggle (loopToggle, ko ("반복"));
        loopToggle.setTooltip (ko ("플레이리스트: 마지막 자식 뒤에 처음부터 다시"));
        loopToggle.onClick = [this] { const bool on = loopToggle.getToggleState(); edit (ko ("플레이리스트 반복"), [on] (Cue& c) { c.group.loop = on; }); };
        addAndMakeVisible (loopToggle);

        styleToggle (shuffleToggle, ko ("셔플"));
        shuffleToggle.setTooltip (ko ("플레이리스트: 한 바퀴마다 순서를 섞음"));
        shuffleToggle.onClick = [this] { const bool on = shuffleToggle.getToggleState(); edit (ko ("플레이리스트 셔플"), [on] (Cue& c) { c.group.shuffle = on; }); };
        addAndMakeVisible (shuffleToggle);

        styleToggle (crossfadeToggle, ko ("크로스페이드"));
        crossfadeToggle.setTooltip (ko ("플레이리스트: 다음 자식을 이 시간만큼 먼저 시작하고 현재 자식을 그 시간에 걸쳐 페이드아웃"));
        crossfadeToggle.onClick = [this] { const bool on = crossfadeToggle.getToggleState(); edit (ko ("크로스페이드"), [on] (Cue& c) { c.group.crossfade = on; }); };
        addAndMakeVisible (crossfadeToggle);

        crossfadeEditor.setJustification (juce::Justification::centredRight);
        crossfadeEditor.setSelectAllWhenFocused (true);
        crossfadeEditor.onReturnKey = [this] { commitCrossfade(); };
        crossfadeEditor.onFocusLost = [this] { commitCrossfade(); };
        addAndMakeVisible (crossfadeEditor);
        styleLabel (crossfadeUnit, ko ("초"));
        addAndMakeVisible (crossfadeUnit);

        styleLabel (hint, ko ("타임라인: 아래 막대를 끌어 자식의 시작(프리웨이트)을 바꿉니다. 막대 선택 후 Alt+←/→ = 0.1 s, Shift+Alt = 0.01 s. 자식은 그룹 아래로 끌어다 넣거나 Ctrl+G로 묶습니다."), 11.0f);
        addAndMakeVisible (hint);
        addAndMakeVisible (timeline);
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
        const bool enabled = cue != nullptr && cue->isGroup() && editable;

        for (auto* c : std::initializer_list<juce::Component*> { &modeCombo, &loopToggle, &shuffleToggle, &crossfadeToggle, &crossfadeEditor })
            c->setEnabled (enabled);

        if (cue != nullptr && cue->isGroup())
        {
            shownId = cue->id;
            modeCombo.setSelectedId ((int) cue->group.mode + 1, juce::dontSendNotification);
            loopToggle.setToggleState (cue->group.loop, juce::dontSendNotification);
            shuffleToggle.setToggleState (cue->group.shuffle, juce::dontSendNotification);
            crossfadeToggle.setToggleState (cue->group.crossfade, juce::dontSendNotification);

            if (! crossfadeEditor.hasKeyboardFocus (true))
                crossfadeEditor.setText (juce::String (cue->group.crossfadeSeconds, 2), false);

            const bool playlist = cue->group.mode == GroupMode::playlist;

            for (auto* c : std::initializer_list<juce::Component*> { &loopToggle, &shuffleToggle, &crossfadeToggle, &crossfadeEditor, &crossfadeUnit })
                c->setVisible (playlist);

            timeline.setVisible (cue->group.mode == GroupMode::timeline);
        }

        timeline.refresh();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 6);
        auto row = area.removeFromTop (24);
        modeLabel.setBounds (row.removeFromLeft (40));
        modeCombo.setBounds (row.removeFromLeft (320));
        row.removeFromLeft (16);
        loopToggle.setBounds (row.removeFromLeft (70));
        shuffleToggle.setBounds (row.removeFromLeft (70));
        crossfadeToggle.setBounds (row.removeFromLeft (110));
        crossfadeEditor.setBounds (row.removeFromLeft (60));
        crossfadeUnit.setBounds (row.removeFromLeft (24));
        area.removeFromTop (4);
        hint.setBounds (area.removeFromTop (16));
        area.removeFromTop (4);
        timeline.setBounds (area);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

    /** The selected group's index in the list (-1 when none). */
    int groupIndex() const
    {
        const int index = shownId.isNull() ? cues.getSelectedIndex() : cues.indexOf (shownId);
        return cues.isValidIndex (index) && cues.get (index).isGroup() ? index : -1;
    }

    /** Changes one child's pre-wait: live (no history) while dragging, an undoable step at the end. */
    void setChildPreWait (int childIndex, double seconds, bool undoable, double undoFrom)
    {
        if (! editable || ! cues.isValidIndex (childIndex))
            return;

        seconds = juce::jlimit (0.0, Cue::maxWaitSeconds, seconds);

        if (! undoable)
        {
            cues.update (childIndex, [seconds] (Cue& c) { c.preWaitSeconds = seconds; });
            return;
        }

        cues.update (childIndex, [undoFrom] (Cue& c) { c.preWaitSeconds = undoFrom; });   // the history snapshot starts from the old value
        document.perform (ko ("타임라인: 시작 시각"), [this, childIndex, seconds] { cues.update (childIndex, [seconds] (Cue& c) { c.preWaitSeconds = seconds; }); });
    }

private:
    /** Bars: one per child, from its pre-wait to pre-wait + length, on a shared time axis. */
    class TimelineEditor : public juce::Component
    {
    public:
        explicit TimelineEditor (GroupPanel& o) : owner (o) { setWantsKeyboardFocus (true); }

        void refresh()
        {
            const int index = owner.groupIndex();
            children = index >= 0 ? owner.cues.childrenOf (index) : std::vector<int>();

            if (selectedChild >= 0 && std::find (children.begin(), children.end(), selectedChild) == children.end())
                selectedChild = children.empty() ? -1 : children.front();

            repaint();
        }

        double axisSeconds() const
        {
            double longest = 0.0;

            for (int child : children)
            {
                const double length = owner.cues.effectiveLengthOf (child);
                longest = std::max (longest, owner.cues.get (child).preWaitSeconds + (length > 0.0 ? length : 1.0));
            }

            return std::max (10.0, std::ceil (longest * 1.1));
        }

        juce::Rectangle<int> barArea() const { return getLocalBounds().withTrimmedLeft (labelWidth).withTrimmedTop (axisHeight).reduced (2, 0); }
        float xFor (double seconds) const { const auto a = barArea(); return (float) a.getX() + (float) (seconds / axisSeconds()) * (float) a.getWidth(); }
        double secondsFor (float x) const { const auto a = barArea(); return juce::jmax (0.0, (double) (x - (float) a.getX()) / (double) juce::jmax (1, a.getWidth()) * axisSeconds()); }

        int rowHeight() const { return children.empty() ? 20 : juce::jlimit (16, 26, (getHeight() - axisHeight) / (int) children.size()); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Palette::background);
            const auto area = barArea();
            const double axis = axisSeconds();

            // axis ticks
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            const double step = axis <= 20.0 ? 1.0 : axis <= 60.0 ? 5.0 : axis <= 300.0 ? 30.0 : 60.0;

            for (double t = 0.0; t <= axis + 1e-9; t += step)
            {
                const float x = xFor (t);
                g.setColour (Palette::outline);
                g.drawVerticalLine ((int) x, (float) axisHeight, (float) getHeight());
                g.setColour (Palette::dimText);
                g.drawText (formatSeconds (t), (int) x + 2, 0, 60, axisHeight, juce::Justification::centredLeft);
            }

            const int h = rowHeight();
            int y = axisHeight;

            for (int child : children)
            {
                const auto& c = owner.cues.get (child);
                const double length = owner.cues.effectiveLengthOf (child);
                const bool selected = child == selectedChild;
                g.setColour (selected ? Palette::text : Palette::dimText);
                g.setFont (juce::Font (juce::FontOptions (12.0f, selected ? juce::Font::bold : juce::Font::plain)));
                g.drawText ((c.number.isNotEmpty() ? c.number + " " : juce::String()) + c.name, 4, y, labelWidth - 8, h, juce::Justification::centredLeft, true);

                const float x0 = xFor (c.preWaitSeconds);
                const float x1 = length < 0.0 ? (float) area.getRight() : xFor (c.preWaitSeconds + juce::jmax (length, 0.05));
                juce::Rectangle<float> bar (x0, (float) y + 3.0f, juce::jmax (6.0f, x1 - x0), (float) h - 6.0f);
                auto colour = c.isGroup() ? CueTable::groupModeColour (c.group.mode) : (c.color > 0 ? CueColors::get (c.color) : Palette::standby);

                if (! c.armed)
                    colour = colour.withAlpha (0.4f);

                g.setColour (colour.withAlpha (selected ? 0.95f : 0.7f));
                g.fillRoundedRectangle (bar, 3.0f);

                if (selected)
                {
                    g.setColour (juce::Colours::white);
                    g.drawRoundedRectangle (bar, 3.0f, 1.5f);
                }

                g.setColour (juce::Colours::black.withAlpha (0.8f));
                g.setFont (juce::Font (juce::FontOptions (11.0f)));
                g.drawText (formatTimeMs (c.preWaitSeconds), bar.toNearestInt().withTrimmedLeft (4), juce::Justification::centredLeft, true);
                y += h;
            }

            if (children.empty())
            {
                g.setColour (Palette::dimText);
                g.setFont (juce::Font (juce::FontOptions (12.0f)));
                g.drawText (ko ("자식 큐가 없습니다 — 큐를 이 그룹 아래로 끌어다 넣거나, 큐를 선택하고 Ctrl+G로 묶으세요"), getLocalBounds(), juce::Justification::centred);
            }
        }

        int childAt (int y) const
        {
            if (y < axisHeight || children.empty())
                return -1;

            const int row = (y - axisHeight) / rowHeight();
            return row >= 0 && row < (int) children.size() ? children[(size_t) row] : -1;
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            grabKeyboardFocus();
            dragging = -1;
            const int child = childAt (e.y);

            if (child < 0)
                return;

            selectedChild = child;
            owner.cues.setSelectedIndex (owner.groupIndex());   // stay on the group in the inspector
            const auto& c = owner.cues.get (child);
            dragStartSeconds = c.preWaitSeconds;
            dragOffset = secondsFor ((float) e.x) - c.preWaitSeconds;
            dragging = e.x >= barArea().getX() ? child : -1;
            repaint();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragging < 0)
                return;

            double seconds = secondsFor ((float) e.x) - dragOffset;

            if (! e.mods.isShiftDown())
                seconds = std::round (seconds * 10.0) / 10.0;   // 0.1 s grid; Shift = free

            owner.setChildPreWait (dragging, seconds, false, dragStartSeconds);
            repaint();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (dragging < 0)
                return;

            const double final = owner.cues.get (dragging).preWaitSeconds;
            const int child = dragging;
            dragging = -1;

            if (! juce::approximatelyEqual (final, dragStartSeconds))
                owner.setChildPreWait (child, final, true, dragStartSeconds);
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (selectedChild < 0 || ! owner.cues.isValidIndex (selectedChild))
                return false;

            const bool left = key.isKeyCode (juce::KeyPress::leftKey), right = key.isKeyCode (juce::KeyPress::rightKey);

            if ((left || right) && key.getModifiers().isAltDown())
            {
                const double step = key.getModifiers().isShiftDown() ? 0.01 : 0.1;
                const double from = owner.cues.get (selectedChild).preWaitSeconds;
                owner.setChildPreWait (selectedChild, from + (right ? step : -step), true, from);
                return true;
            }

            if (key.isKeyCode (juce::KeyPress::upKey) || key.isKeyCode (juce::KeyPress::downKey))
            {
                const auto it = std::find (children.begin(), children.end(), selectedChild);

                if (it != children.end())
                {
                    const int pos = (int) (it - children.begin()) + (key.isKeyCode (juce::KeyPress::downKey) ? 1 : -1);

                    if (pos >= 0 && pos < (int) children.size())
                    {
                        selectedChild = children[(size_t) pos];
                        repaint();
                    }
                }

                return true;
            }

            return false;
        }

    private:
        static constexpr int labelWidth = 150;
        static constexpr int axisHeight = 14;
        GroupPanel& owner;
        std::vector<int> children;
        int selectedChild = -1;
        int dragging = -1;
        double dragStartSeconds = 0.0, dragOffset = 0.0;
    };

    void edit (const juce::String& name, const std::function<void (Cue&)>& mutator)
    {
        if (refreshing || ! editable)
            return;

        const int index = groupIndex();

        if (index < 0)
            return;

        document.perform (name, [this, index, mutator] { cues.update (index, mutator); });
    }

    void commitCrossfade()
    {
        const double seconds = crossfadeEditor.getText().trim().getDoubleValue();
        const auto* cue = cues.getSelected();

        if (cue == nullptr || ! cue->isGroup() || juce::approximatelyEqual (seconds, cue->group.crossfadeSeconds))
            return;

        edit (ko ("크로스페이드 시간"), [seconds] (Cue& c) { c.group.crossfadeSeconds = juce::jlimit (0.0, GroupCueData::maxCrossfadeSeconds, seconds); });
    }

    ProjectDocument& document;
    CueList& cues;
    juce::Label modeLabel, hint, crossfadeUnit;
    juce::ComboBox modeCombo;
    juce::ToggleButton loopToggle, shuffleToggle, crossfadeToggle;
    juce::TextEditor crossfadeEditor;
    TimelineEditor timeline;
    juce::Uuid shownId = juce::Uuid::null();
    bool refreshing = false;
    bool editable = true;
};

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

    basicsPanel = std::make_unique<BasicsPanel> (document, engine, settings);
    basics = basicsPanel.get();
    basics->onPanic = [this] { if (onPanic) onPanic(); };

    timeLoopsPanel = std::make_unique<TimeLoopsPanel> (document, engine, thumbnailCache);
    timeLoops = timeLoopsPanel.get();
    timeLoops->onPanic = [this] { if (onPanic) onPanic(); };
    timeLoops->onPreview = [this] { if (onPreview) onPreview(); };
    timeLoops->onReset = [this] { if (onResetCue) onResetCue(); };

    levelsPanel = std::make_unique<LevelsPanel> (document, engine);
    levels = levelsPanel.get();
    trimPanel = std::make_unique<TrimPanel> (document, engine);
    trim = trimPanel.get();
    triggersPanel = std::make_unique<TriggersPanel> (document);
    triggers = triggersPanel.get();

    effectsPanel = std::make_unique<EffectsPanel> (document, engine, windows);
    effects = effectsPanel.get();
    effects->chainStrip.onOpenPluginManager = [this] { if (onOpenPluginManager) onOpenPluginManager(); };

    fadePanel = std::make_unique<FadePanel> (document, engine);
    curvePanel = std::make_unique<CurvePanel> (document);
    fadeParamsPanel = std::make_unique<FadeParamsPanel> (document, engine);
    devampPanel = std::make_unique<DevampPanel> (document);
    groupPanel = std::make_unique<GroupPanel> (document);
    controlPanel = std::make_unique<ControlPanel> (document);
    micPanel = std::make_unique<MicPanel> (document, engine);

    tabs.setTabBarDepth (26);
    tabs.setOutline (0);
    tabs.setColour (juce::TabbedComponent::backgroundColourId, Palette::panel);
    tabs.onTabShown = [this]
    {
        auto* focused = juce::Component::getCurrentlyFocusedComponent();

        if (focused != nullptr && tabs.isParentOf (focused) && onReturnFocus)
            onReturnFocus();
    };

    rebuildTabs (0);
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
    levels->setEditable (editable);
    trim->setEditable (editable);
    triggers->setEditable (editable);
    timeLoops->setEnabled (editable);
    effects->setEnabled (editable);
    fadePanel->setEditable (editable);
    curvePanel->setEditable (editable);
    fadeParamsPanel->setEditable (editable);
    devampPanel->setEditable (editable);
    groupPanel->setEditable (editable);
    controlPanel->setEditable (editable);
    micPanel->setEditable (editable);
}

void CueInspector::rebuildTabs (int wanted)
{
    if (tabSet == wanted)
        return;

    tabSet = wanted;
    tabs.clearTabs();

    if (wanted == 5)
    {
        tabs.addTab (ko ("기본"), Palette::panel, basics, false);
        tabs.addTab (ko ("입력"), Palette::panel, micPanel.get(), false);
        tabs.addTab (ko ("레벨"), Palette::panel, levels, false);
        tabs.addTab (ko ("트림"), Palette::panel, trim, false);
        tabs.addTab (ko ("트리거"), Palette::panel, triggers, false);
        tabs.addTab (ko ("이펙트"), Palette::panel, effects, false);
        tabs.setCurrentTabIndex (1);
    }
    else if (wanted == 4)
    {
        tabs.addTab (ko ("기본"), Palette::panel, basics, false);
        tabs.addTab (ko ("동작"), Palette::panel, controlPanel.get(), false);
        tabs.addTab (ko ("트리거"), Palette::panel, triggers, false);
        tabs.setCurrentTabIndex (1);
    }
    else if (wanted == 3)
    {
        tabs.addTab (ko ("기본"), Palette::panel, basics, false);
        tabs.addTab (ko ("그룹"), Palette::panel, groupPanel.get(), false);
        tabs.addTab (ko ("트리거"), Palette::panel, triggers, false);
        tabs.setCurrentTabIndex (1);
    }
    else if (wanted == 2)
    {
        tabs.addTab (ko ("기본"), Palette::panel, basics, false);
        tabs.addTab (ko ("디밴프"), Palette::panel, devampPanel.get(), false);
        tabs.addTab (ko ("트리거"), Palette::panel, triggers, false);
        tabs.setCurrentTabIndex (1);
    }
    else if (wanted == 1)
    {
        tabs.addTab (ko ("기본"), Palette::panel, basics, false);
        tabs.addTab (ko ("페이드"), Palette::panel, fadePanel.get(), false);
        tabs.addTab (ko ("커브"), Palette::panel, curvePanel.get(), false);
        tabs.addTab (ko ("트리거"), Palette::panel, triggers, false);
        tabs.addTab (ko ("파라미터"), Palette::panel, fadeParamsPanel.get(), false);
        tabs.setCurrentTabIndex (1);
    }
    else
    {
        tabs.addTab (ko ("기본"), Palette::panel, basics, false);
        tabs.addTab (ko ("시간·루프"), Palette::panel, timeLoops, false);
        tabs.addTab (ko ("레벨"), Palette::panel, levels, false);
        tabs.addTab (ko ("트림"), Palette::panel, trim, false);
        tabs.addTab (ko ("트리거"), Palette::panel, triggers, false);
        tabs.addTab (ko ("이펙트"), Palette::panel, effects, false);
        tabs.setCurrentTabIndex (0);
    }
}

void CueInspector::showNotes()
{
    tabs.setCurrentTabIndex (0);
    basics->focusNotes();
}

void CueInspector::showTimeTab()
{
    tabs.setCurrentTabIndex (1);   // 시간·루프 for audio cues, 페이드 for fade cues
}

void CueInspector::fetchFadeLevelsFromTarget()
{
    fadePanel->fetchFromTarget();
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

    rebuildTabs (cue == nullptr ? 0 : cue->isFade() ? 1 : cue->isDevamp() ? 2 : cue->isGroup() ? 3 : cue->isControl() ? 4 : cue->isMic() ? 5 : 0);
    basics->refresh();
    timeLoops->refresh();
    levels->refresh();
    trim->refresh();
    triggers->refresh();
    effects->refresh();
    fadePanel->refresh();
    curvePanel->refresh();
    fadeParamsPanel->refresh();
    devampPanel->refresh();
    groupPanel->refresh();
    controlPanel->refresh();
    micPanel->refresh();
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
