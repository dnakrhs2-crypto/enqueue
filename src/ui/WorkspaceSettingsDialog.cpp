#include "ui/WorkspaceSettingsDialog.h"

#include "ui/UiUtils.h"

namespace gocue::WorkspaceSettingsDialog
{

namespace
{
    juce::Component::SafePointer<juce::DialogWindow> dialog;

    /** A labelled row of controls laid out left to right. */
    struct Row
    {
        juce::Rectangle<int> area;
        int x = 0;

        juce::Rectangle<int> take (int width)
        {
            auto r = area.withX (area.getX() + x).withWidth (width);
            x += width + 8;
            return r;
        }
    };

    class SettingsTab : public juce::Component
    {
    public:
        SettingsTab (ProjectDocument& doc) : document (doc) {}

        juce::Label& addLabel (const juce::String& text)
        {
            auto* label = labels.add (new juce::Label());
            label->setText (text, juce::dontSendNotification);
            label->setColour (juce::Label::textColourId, Palette::dimText);
            label->setFont (juce::Font (juce::FontOptions (13.0f)));
            addAndMakeVisible (label);
            return *label;
        }

        juce::ToggleButton& addToggle (const juce::String& text, bool initial, std::function<void (WorkspaceSettings&, bool)> apply)
        {
            auto* toggle = toggles.add (new juce::ToggleButton (text));
            toggle->setColour (juce::ToggleButton::textColourId, Palette::text);
            toggle->setColour (juce::ToggleButton::tickColourId, Palette::standby);
            toggle->setToggleState (initial, juce::dontSendNotification);
            toggle->onClick = [this, toggle, apply]
            {
                auto s = document.settings;
                apply (s, toggle->getToggleState());
                document.setSettings (s);
            };
            addAndMakeVisible (toggle);
            return *toggle;
        }

        juce::TextEditor& addNumber (const juce::String& initial, const juce::String& allowed,
                                     std::function<void (WorkspaceSettings&, const juce::String&)> apply,
                                     std::function<juce::String (const WorkspaceSettings&)> current)
        {
            auto* editor = editors.add (new juce::TextEditor());
            editor->setInputRestrictions (10, allowed);
            editor->setJustification (juce::Justification::centredRight);
            editor->setSelectAllWhenFocused (true);
            editor->setText (initial, false);

            auto commit = [this, editor, apply, current]
            {
                auto s = document.settings;
                apply (s, editor->getText());
                document.setSettings (s);
                editor->setText (current (document.settings), false);
            };
            editor->onReturnKey = [commit, editor] { commit(); editor->giveAwayKeyboardFocus(); };
            editor->onFocusLost = commit;
            editor->onEscapeKey = [editor, current, this] { editor->setText (current (document.settings), false); editor->giveAwayKeyboardFocus(); };
            addAndMakeVisible (editor);
            return *editor;
        }

        void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

    protected:
        ProjectDocument& document;
        juce::OwnedArray<juce::Label> labels;
        juce::OwnedArray<juce::ToggleButton> toggles;
        juce::OwnedArray<juce::TextEditor> editors;
    };

    class GeneralTab : public SettingsTab
    {
    public:
        GeneralTab (ProjectDocument& doc) : SettingsTab (doc)
        {
            const auto& s = doc.settings;

            goLabel = &addLabel (ko ("GO 사이 최소 시간 (초, 0 = 끔)"));
            goEditor = &addNumber (juce::String (s.doubleGoSeconds, 2), "0123456789.",
                                   [] (WorkspaceSettings& w, const juce::String& t) { w.doubleGoSeconds = t.getDoubleValue(); },
                                   [] (const WorkspaceSettings& w) { return juce::String (w.doubleGoSeconds, 2); });
            goHint = &addLabel (ko ("이 시간 안에 들어온 GO는 무시하고 GO 버튼이 빨갛게 깜빡입니다"));
            goHint->setFont (juce::Font (juce::FontOptions (11.0f)));

            keyUpToggle = &addToggle (ko ("키를 뗀 뒤에만 다시 GO"), s.requireKeyUp,
                                      [] (WorkspaceSettings& w, bool v) { w.requireKeyUp = v; });

            panicLabel = &addLabel (ko ("전체 페이드 정지 시간 (초) - Esc"));
            panicEditor = &addNumber (juce::String (s.panicSeconds, 1), "0123456789.",
                                      [] (WorkspaceSettings& w, const juce::String& t) { w.panicSeconds = t.getDoubleValue(); },
                                      [] (const WorkspaceSettings& w) { return juce::String (w.panicSeconds, 1); });
            panicHint = &addLabel (ko ("Esc 한 번 = 이 시간 동안 전체 페이드아웃 후 정지, 0.5초 안에 두 번 = 즉시 정지"));
            panicHint->setFont (juce::Font (juce::FontOptions (11.0f)));

            autoNumberToggle = &addToggle (ko ("새 큐에 자동 번호"), s.autoNumber,
                                           [] (WorkspaceSettings& w, bool v) { w.autoNumber = v; });
            incrementLabel = &addLabel (ko ("증가값"));
            incrementEditor = &addNumber (juce::String (s.numberIncrement, 2), "0123456789.",
                                          [] (WorkspaceSettings& w, const juce::String& t) { w.numberIncrement = t.getDoubleValue(); },
                                          [] (const WorkspaceSettings& w) { return juce::String (w.numberIncrement, 2); });

            lockToggle = &addToggle (ko ("플레이헤드를 선택에 잠금"), s.lockPlayheadToSelection,
                                     [] (WorkspaceSettings& w, bool v) { w.lockPlayheadToSelection = v; });

            openToggle = &addToggle (ko ("프로젝트를 열 때 큐 시작:"), s.startOnOpen,
                                     [] (WorkspaceSettings& w, bool v) { w.startOnOpen = v; });
            openEditor = &addNumber (s.startOnOpenCue, "", [] (WorkspaceSettings& w, const juce::String& t) { w.startOnOpenCue = t.trim(); },
                                     [] (const WorkspaceSettings& w) { return w.startOnOpenCue; });
            openEditor->setJustification (juce::Justification::centredLeft);

            closeToggle = &addToggle (ko ("프로젝트를 닫을 때 큐 시작:"), s.startOnClose,
                                      [] (WorkspaceSettings& w, bool v) { w.startOnClose = v; });
            closeEditor = &addNumber (s.startOnCloseCue, "", [] (WorkspaceSettings& w, const juce::String& t) { w.startOnCloseCue = t.trim(); },
                                      [] (const WorkspaceSettings& w) { return w.startOnCloseCue; });
            closeEditor->setJustification (juce::Justification::centredLeft);

            rowSizeLabel = &addLabel (ko ("큐 리스트 행 크기"));
            rowSizeBox.addItem (ko ("작게"), 1);
            rowSizeBox.addItem (ko ("보통"), 2);
            rowSizeBox.addItem (ko ("크게"), 3);
            rowSizeBox.setSelectedId (juce::jlimit (0, 2, s.rowSize) + 1, juce::dontSendNotification);
            rowSizeBox.onChange = [this]
            {
                auto w = document.settings;
                w.rowSize = juce::jlimit (0, 2, rowSizeBox.getSelectedId() - 1);
                document.setSettings (w);
            };
            addAndMakeVisible (rowSizeBox);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (16, 12);
            const int rowHeight = 26;
            auto next = [&] { auto r = area.removeFromTop (rowHeight); area.removeFromTop (6); return Row { r, 0 }; };

            auto row = next();
            goLabel->setBounds (row.take (230));
            goEditor->setBounds (row.take (70));
            row = next();
            goHint->setBounds (row.take (area.getWidth()));
            row = next();
            keyUpToggle->setBounds (row.take (300));
            area.removeFromTop (6);

            row = next();
            panicLabel->setBounds (row.take (230));
            panicEditor->setBounds (row.take (70));
            row = next();
            panicHint->setBounds (row.take (area.getWidth()));
            area.removeFromTop (6);

            row = next();
            autoNumberToggle->setBounds (row.take (200));
            incrementLabel->setBounds (row.take (50));
            incrementEditor->setBounds (row.take (70));
            row = next();
            lockToggle->setBounds (row.take (300));
            area.removeFromTop (6);

            row = next();
            openToggle->setBounds (row.take (230));
            openEditor->setBounds (row.take (100));
            row = next();
            closeToggle->setBounds (row.take (230));
            closeEditor->setBounds (row.take (100));
            area.removeFromTop (6);

            row = next();
            rowSizeLabel->setBounds (row.take (230));
            rowSizeBox.setBounds (row.take (100));
        }

    private:
        juce::Label *goLabel, *goHint, *panicLabel, *panicHint, *incrementLabel, *rowSizeLabel;
        juce::ComboBox rowSizeBox;
        juce::TextEditor *goEditor, *panicEditor, *incrementEditor, *openEditor, *closeEditor;
        juce::ToggleButton *keyUpToggle, *autoNumberToggle, *lockToggle, *openToggle, *closeToggle;
    };

    class FilesTab : public SettingsTab
    {
    public:
        FilesTab (ProjectDocument& doc) : SettingsTab (doc)
        {
            const auto& s = doc.settings;
            copyToggle = &addToggle (ko ("큐를 추가할 때 오디오 파일을 프로젝트 폴더(audio)로 복사"), s.copyFilesIntoProject,
                                     [] (WorkspaceSettings& w, bool v) { w.copyFilesIntoProject = v; });
            backupToggle = &addToggle (ko ("자동 백업"), s.autoBackup,
                                       [] (WorkspaceSettings& w, bool v) { w.autoBackup = v; });
            intervalLabel = &addLabel (ko ("간격 (초, 5~600)"));
            intervalEditor = &addNumber (juce::String (s.backupIntervalSeconds), "0123456789",
                                         [] (WorkspaceSettings& w, const juce::String& t) { w.backupIntervalSeconds = t.getIntValue(); },
                                         [] (const WorkspaceSettings& w) { return juce::String (w.backupIntervalSeconds); });
            beforeSaveToggle = &addToggle (ko ("저장하기 전에 이전 파일 백업"), s.backupBeforeSave,
                                           [] (WorkspaceSettings& w, bool v) { w.backupBeforeSave = v; });
            rotateToggle = &addToggle (ko ("오래된 백업 정리 (최근 1시간 20개, 하루 동안 시간별, 그 뒤 일별)"), s.rotateBackups,
                                       [] (WorkspaceSettings& w, bool v) { w.rotateBackups = v; });
            hint = &addLabel (ko ("백업은 프로젝트 파일 옆 \"<이름>.gocue.backups\" 폴더에 쌓입니다. 저장한 적 없는 프로젝트는 백업하지 않습니다."));
            hint->setFont (juce::Font (juce::FontOptions (11.0f)));
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (16, 12);
            const int rowHeight = 26;
            auto next = [&] { auto r = area.removeFromTop (rowHeight); area.removeFromTop (6); return Row { r, 0 }; };

            auto row = next();
            copyToggle->setBounds (row.take (area.getWidth()));
            area.removeFromTop (8);
            row = next();
            backupToggle->setBounds (row.take (120));
            intervalLabel->setBounds (row.take (130));
            intervalEditor->setBounds (row.take (70));
            row = next();
            beforeSaveToggle->setBounds (row.take (area.getWidth()));
            row = next();
            rotateToggle->setBounds (row.take (area.getWidth()));
            row = next();
            hint->setBounds (row.take (area.getWidth()));
        }

    private:
        juce::ToggleButton *copyToggle, *backupToggle, *beforeSaveToggle, *rotateToggle;
        juce::Label *intervalLabel, *hint;
        juce::TextEditor* intervalEditor;
    };

    class AudioTab : public SettingsTab
    {
    public:
        AudioTab (ProjectDocument& doc) : SettingsTab (doc)
        {
            const auto& s = doc.settings;
            maxLabel = &addLabel (ko ("레벨 상한 (dB, 매트릭스·페이드에서 이 위로 못 올림)"));
            maxEditor = &addNumber (juce::String (s.maxLevelDb, 1), "-0123456789.",
                                    [] (WorkspaceSettings& w, const juce::String& t) { w.maxLevelDb = t.getDoubleValue(); },
                                    [] (const WorkspaceSettings& w) { return juce::String (w.maxLevelDb, 1); });
            minLabel = &addLabel (ko ("레벨 하한 (dB, 이 아래는 무음 -\xE2\x88\x9E)"));
            minEditor = &addNumber (juce::String (s.minLevelDb, 1), "-0123456789.",
                                    [] (WorkspaceSettings& w, const juce::String& t) { w.minLevelDb = t.getDoubleValue(); },
                                    [] (const WorkspaceSettings& w) { return juce::String (w.minLevelDb, 1); });
            hint = &addLabel (ko ("출력 라우팅·출력 이름·출력 인서트는 오디오 > 오디오 패치... 에서, 장치와 채널 수는 오디오 > 오디오 출력 설정에서 바꿉니다."));
            hint->setFont (juce::Font (juce::FontOptions (11.0f)));
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (16, 12);
            const int rowHeight = 26;
            auto next = [&] { auto r = area.removeFromTop (rowHeight); area.removeFromTop (6); return Row { r, 0 }; };

            auto row = next();
            maxLabel->setBounds (row.take (360));
            maxEditor->setBounds (row.take (70));
            row = next();
            minLabel->setBounds (row.take (360));
            minEditor->setBounds (row.take (70));
            area.removeFromTop (8);
            row = next();
            hint->setBounds (row.take (area.getWidth()));
        }

    private:
        juce::Label *maxLabel, *minLabel, *hint;
        juce::TextEditor *maxEditor, *minEditor;
    };

    class Content : public juce::Component
    {
    public:
        explicit Content (ProjectDocument& document)
        {
            tabs.setTabBarDepth (28);
            tabs.setOutline (0);
            tabs.addTab (ko ("일반"), Palette::panel, new GeneralTab (document), true);
            tabs.addTab (ko ("파일"), Palette::panel, new FilesTab (document), true);
            tabs.addTab (ko ("오디오"), Palette::panel, new AudioTab (document), true);
            addAndMakeVisible (tabs);
            setSize (640, 380);
        }

        void resized() override { tabs.setBounds (getLocalBounds()); }
        void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

    private:
        juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    };
}

void show (ProjectDocument& document, juce::Component* centreAround)
{
    if (dialog != nullptr)
    {
        dialog->toFront (true);
        return;
    }

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = ko ("프로젝트 설정");
    options.content.setOwned (new Content (document));
    options.componentToCentreAround = centreAround;
    options.dialogBackgroundColour = Palette::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    dialog = options.launchAsync();
}

void closeIfOpen()
{
    if (dialog != nullptr)
        delete dialog.getComponent();
}

} // namespace gocue::WorkspaceSettingsDialog
