#include "BackupDialog.h"

#include "BackupServer.h"
#include "Widgets.h"

namespace gocue::livemix
{

namespace
{
    class BackupWindow;
    juce::Component::SafePointer<BackupWindow> openWindow;
    juce::Component* currentWindow();   // the open window as a Component (BackupWindow is defined below)

    /** The account worked: keep what the operator asked to keep - also when the window is already gone. */
    void persistAccount (LiveMixSettings& settings, const WebDavBackup::Target& target, bool remember)
    {
        settings.setBackupUser (target.accountId);
        settings.setBackupRememberPassword (remember);
        settings.setBackupPassword (remember ? target.accountPassword : juce::String());
    }

    class BackupContent : public juce::Component,
                          private juce::TableListBoxModel
    {
    public:
        BackupContent (MixDocument& d, LiveMixSettings& s, WebDavBackup& b, BackupDialog::Callbacks cb)
            : document (d), settings (s), backup (b), callbacks (std::move (cb))
        {
            styleCaption (idCaption, ko ("아이디"));
            addAndMakeVisible (idCaption);
            idEditor.setFont (bodyFont());
            idEditor.setText (settings.getBackupUser(), false);
            idEditor.onReturnKey = [this] { signIn(); };
            addAndMakeVisible (idEditor);

            styleCaption (passwordCaption, ko ("비밀번호"));
            addAndMakeVisible (passwordCaption);
            passwordEditor.setFont (bodyFont());
            passwordEditor.setPasswordCharacter (0x2022);
            passwordEditor.onReturnKey = [this] { signIn(); };
            addAndMakeVisible (passwordEditor);

            remember.setButtonText (ko ("이 PC에 기억"));
            remember.setToggleState (settings.getBackupRememberPassword(), juce::dontSendNotification);
            remember.setWantsKeyboardFocus (false);
            addAndMakeVisible (remember);

            if (remember.getToggleState())
                passwordEditor.setText (settings.getBackupPassword(), false);

            signInButton.setButtonText (ko ("로그인"));
            signInButton.setWantsKeyboardFocus (false);
            signInButton.setColour (juce::TextButton::buttonColourId, Palette::accent);
            signInButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            signInButton.onClick = [this] { signIn(); };
            addAndMakeVisible (signInButton);

            createButton.setButtonText (ko ("계정 만들기"));
            createButton.setWantsKeyboardFocus (false);
            createButton.onClick = [this] { createAccount(); };
            addAndMakeVisible (createButton);

            table.setModel (this);
            table.setHeaderHeight (30);
            table.setRowHeight (30);
            table.setColour (juce::ListBox::backgroundColourId, Palette::card);
            table.setColour (juce::ListBox::outlineColourId, Palette::line);
            table.setOutlineThickness (1);
            auto& header = table.getHeader();
            header.setColour (juce::TableHeaderComponent::backgroundColourId, Palette::card2);
            header.setColour (juce::TableHeaderComponent::textColourId, Palette::dimText);
            header.setColour (juce::TableHeaderComponent::outlineColourId, Palette::line);
            header.setColour (juce::TableHeaderComponent::highlightColourId, Palette::card2);
            const int columnFlags = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable;
            header.addColumn (ko ("계정"), columnOwner, 120, 60, 300, columnFlags);
            header.addColumn ("PC", columnPc, 150, 60, 400, columnFlags);
            header.addColumn (ko ("파일"), columnName, 280, 120, 800, columnFlags);
            header.addColumn (ko ("날짜"), columnDate, 140, 100, 200, columnFlags);
            header.addColumn (ko ("크기"), columnSize, 80, 50, 120, columnFlags);
            header.setStretchToFitActive (true);
            header.setColumnVisible (columnOwner, false);
            addAndMakeVisible (table);

            uploadButton.setButtonText (ko ("지금 세션 백업"));
            uploadButton.setWantsKeyboardFocus (false);
            uploadButton.onClick = [this] { upload(); };
            addAndMakeVisible (uploadButton);

            restoreButton.setButtonText (ko ("선택한 백업 불러오기"));
            restoreButton.setWantsKeyboardFocus (false);
            restoreButton.setColour (juce::TextButton::buttonColourId, Palette::accent);
            restoreButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            restoreButton.setEnabled (false);
            restoreButton.onClick = [this] { restoreSelected(); };
            addAndMakeVisible (restoreButton);

            styleCaption (hint, ko ("백업은 로그인한 계정의 것만 보이고, 올리기와 불러오기도 그 계정의 아이디·비밀번호로만 됩니다. 처음이면 아이디와 비밀번호를 정해 '계정 만들기'를 누르세요."));
            hint.setFont (bodyFont (12.5f));
            hint.setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (hint);

            statusLabel.setFont (bodyFont (13.0f));
            statusLabel.setColour (juce::Label::textColourId, Palette::dimText);
            statusLabel.setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (statusLabel);

            setSize (820, 580);

            if (! BackupServer::isConfigured())
                setStatus (ko ("이 프로그램에는 백업 서버가 설정되어 있지 않습니다"), true);
            else if (idEditor.getText().trim().isNotEmpty() && passwordEditor.getText().isNotEmpty())
                signIn();   // a remembered account: the list comes up at once
            else
                setStatus (ko ("아이디와 비밀번호를 넣고 로그인을 누르세요"), false);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (20, 16);
            auto row = area.removeFromTop (30);
            idCaption.setBounds (row.removeFromLeft (52));
            idEditor.setBounds (row.removeFromLeft (150));
            row.removeFromLeft (12);
            passwordCaption.setBounds (row.removeFromLeft (64));
            passwordEditor.setBounds (row.removeFromLeft (150));
            row.removeFromLeft (12);
            createButton.setBounds (row.removeFromRight (100));
            row.removeFromRight (8);
            signInButton.setBounds (row.removeFromRight (90));
            row.removeFromRight (8);
            remember.setBounds (row.removeFromLeft (juce::jmin (130, juce::jmax (0, row.getWidth()))));
            area.removeFromTop (6);
            hint.setBounds (area.removeFromTop (34));
            area.removeFromTop (8);

            auto bottom = area.removeFromBottom (30);
            statusLabel.setBounds (bottom);
            area.removeFromBottom (8);
            row = area.removeFromBottom (32);
            restoreButton.setBounds (row.removeFromRight (190));
            row.removeFromRight (14);
            uploadButton.setBounds (row.removeFromRight (150));
            area.removeFromBottom (12);

            table.setBounds (area);
        }

        void paint (juce::Graphics& g) override { g.fillAll (Palette::card); }

    private:
        enum { columnOwner = 1, columnPc, columnName, columnDate, columnSize };

        WebDavBackup::Target currentTarget() const
        {
            return BackupServer::target (idEditor.getText(), passwordEditor.getText());
        }

        bool checkReady (bool creating = false)
        {
            if (! BackupServer::isConfigured())
            {
                setStatus (ko ("이 프로그램에는 백업 서버가 설정되어 있지 않습니다"), true);
                return false;
            }

            if (backup.isBusy())
            {
                setStatus (ko ("앞의 작업이 끝날 때까지 기다리세요"), true);
                return false;
            }

            if (const auto bad = WebDavBackup::validateAccountId (idEditor.getText()); bad.isNotEmpty())
            {
                setStatus (bad, true);
                return false;
            }

            if (passwordEditor.getText().isEmpty() || (creating && passwordEditor.getText().length() < 4))
            {
                setStatus (creating ? ko ("비밀번호는 4자 이상으로 정하세요") : ko ("비밀번호를 넣으세요"), true);
                return false;
            }

            return true;
        }

        void setBusy (bool busy)
        {
            signInButton.setEnabled (! busy);
            createButton.setEnabled (! busy);
            uploadButton.setEnabled (! busy);
            restoreButton.setEnabled (! busy && table.getSelectedRow() >= 0);
            idEditor.setEnabled (! busy);
            passwordEditor.setEnabled (! busy);
        }

        void setStatus (const juce::String& text, bool error)
        {
            statusLabel.setColour (juce::Label::textColourId, error ? Palette::danger : Palette::dimText);
            statusLabel.setText (text, juce::dontSendNotification);
        }

        /** Runs 'next' once the worker thread is really idle. A job's result is posted from inside its run(), so
            the thread can still count as running for a moment; the controls stay busy meanwhile. */
        void whenIdle (std::function<void()> next, int attempt = 0)
        {
            if (! backup.isBusy())
            {
                next();
                return;
            }

            if (attempt > 200)   // ~6 s: something is wrong, give the controls back
            {
                setBusy (false);
                setStatus (ko ("앞의 작업이 끝나지 않았습니다. 잠시 후 다시 시도하세요."), true);
                return;
            }

            juce::Component::SafePointer<BackupContent> safe (this);
            juce::Timer::callAfterDelay (30, [safe, next, attempt]
            {
                if (safe != nullptr)
                    safe->whenIdle (next, attempt + 1);
            });
        }

        void showEntries (std::vector<WebDavBackup::Entry> found, bool everyone)
        {
            entries = std::move (found);
            everyoneMode = everyone;
            table.getHeader().setColumnVisible (columnOwner, everyone);
            table.deselectAllRows();
            table.updateContent();
            table.repaint();
            restoreButton.setEnabled (false);
        }

        void signIn()
        {
            if (! checkReady())
                return;

            const auto target = currentTarget();
            juce::Component::SafePointer<BackupContent> safe (this);
            auto* prefs = &settings;   // outlives every window (the application's)
            const bool keep = remember.getToggleState();
            const auto started = backup.signIn (target, [safe, target, prefs, keep] (bool ok, const juce::String& message, std::vector<WebDavBackup::Entry> found, bool everyone)
            {
                if (ok)
                    persistAccount (*prefs, target, keep);

                if (safe == nullptr)
                    return;

                safe->setBusy (false);

                if (ok)
                    safe->showEntries (std::move (found), everyone);
                else
                    safe->showEntries ({}, false);

                safe->setStatus (message, ! ok);
            });

            if (started.failed())
            {
                setStatus (started.getErrorMessage(), true);
                return;
            }

            setBusy (true);
            setStatus (ko ("로그인 중..."), false);
        }

        void createAccount()
        {
            if (! checkReady (true))
                return;

            const auto target = currentTarget();
            juce::Component::SafePointer<BackupContent> safe (this);
            auto status = callbacks.status;
            auto* prefs = &settings;
            const bool keep = remember.getToggleState();
            const auto started = backup.createAccount (target, [safe, target, status, prefs, keep] (bool ok, const juce::String& message)
            {
                if (ok)
                    persistAccount (*prefs, target, keep);   // the account exists: remembered even when the window is gone

                if (status)
                    status (message, ! ok);

                if (safe == nullptr)
                    return;

                safe->setStatus (message, ! ok);

                if (ok)
                    safe->whenIdle ([safe] { if (safe != nullptr) safe->signIn(); });   // and straight in
                else
                    safe->setBusy (false);
            });

            if (started.failed())
            {
                setStatus (started.getErrorMessage(), true);
                return;
            }

            setBusy (true);
            setStatus (ko ("계정 만드는 중..."), false);
        }

        void upload()
        {
            if (! checkReady())
                return;

            if (! callbacks.saveBeforeUpload || ! callbacks.saveBeforeUpload())
            {
                setStatus (ko ("먼저 세션을 저장하세요 (세션 > 저장)"), true);
                return;
            }

            const auto target = currentTarget();
            const auto remotePath = WebDavBackup::backupPathFor (target.share, target.accountId, juce::SystemStats::getComputerName(), juce::Time::getCurrentTime());
            juce::Component::SafePointer<BackupContent> safe (this);
            auto status = callbacks.status;
            auto* prefs = &settings;
            const bool keep = remember.getToggleState();
            const auto started = backup.start (target, document.getFile(), remotePath, [safe, target, status, prefs, keep] (bool ok, const juce::String& message)
            {
                if (ok)
                    persistAccount (*prefs, target, keep);

                if (status)
                    status (message, ! ok);   // the main window hears the result even when this window is gone

                if (safe == nullptr)
                    return;

                safe->setStatus (message, ! ok);

                if (ok)
                    safe->whenIdle ([safe] { if (safe != nullptr) safe->signIn(); });   // the new backup in the list
                else
                    safe->setBusy (false);
            });

            if (started.failed())
            {
                setStatus (started.getErrorMessage(), true);
                return;
            }

            setBusy (true);
            setStatus (ko ("백업 중... ") + remotePath.fromLastOccurrenceOf ("/", false, false), false);
        }

        void restoreSelected()
        {
            const int row = table.getSelectedRow();

            if (row < 0 || row >= (int) entries.size())
            {
                setStatus (ko ("불러올 백업을 목록에서 고르세요"), true);
                return;
            }

            if (! checkReady())
                return;

            const auto entry = entries[(size_t) row];
            auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("LiveMix");
            folder.createDirectory();
            const auto localName = (everyoneMode && entry.owner != idEditor.getText().trim() ? entry.owner + "_" : juce::String()) + entry.name;
            auto file = folder.getChildFile (WebDavBackup::sanitiseName (localName));

            if (file.existsAsFile())
                file = file.getNonexistentSibling();   // an earlier restore of the same backup keeps its file

            const auto target = currentTarget();
            juce::Component::SafePointer<BackupContent> safe (this);
            juce::Component::SafePointer<juce::Component> mine (getTopLevelComponent());   // this window, not a later one
            auto status = callbacks.status;
            auto restore = callbacks.restore;
            auto* prefs = &settings;
            const bool keep = remember.getToggleState();
            const auto started = backup.startDownload (target, entry.path, file, [safe, mine, target, file, status, restore, prefs, keep] (bool ok, const juce::String& message)
            {
                auto report = [&] (const juce::String& text, bool error)
                {
                    if (status)
                        status (text, error);

                    if (safe != nullptr)
                    {
                        safe->setBusy (false);
                        safe->setStatus (text, error);
                    }
                };

                if (! ok)
                {
                    report (message, true);
                    return;
                }

                persistAccount (*prefs, target, keep);

                // whatever came down must be a session before it is opened as one
                MixSession probe;
                juce::StringArray warnings;

                if (const auto check = MixSession::load (file, probe, &warnings); check.failed())
                {
                    file.deleteFile();
                    report (juce::String::fromUTF8 ("내려받은 파일이 세션 파일이 아닙니다: ") + check.getErrorMessage(), true);
                    return;
                }

                report (message, false);

                if (restore)
                    restore (file);   // the session opens (after the usual unsaved-changes question)

                juce::MessageManager::callAsync ([mine]
                {
                    if (mine != nullptr && mine.getComponent() == currentWindow())
                        delete mine.getComponent();
                });
            });

            if (started.failed())
            {
                setStatus (started.getErrorMessage(), true);
                return;
            }

            setBusy (true);
            setStatus (ko ("불러오는 중... ") + entry.name, false);
        }

        //==============================================================================
        int getNumRows() override { return (int) entries.size(); }

        void paintRowBackground (juce::Graphics& g, int rowNumber, int, int, bool rowIsSelected) override
        {
            g.fillAll (rowIsSelected ? Palette::accent.withAlpha (0.35f) : (rowNumber % 2 == 0 ? Palette::card : Palette::card2.withAlpha (0.5f)));
        }

        void paintCell (juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool) override
        {
            if (rowNumber < 0 || rowNumber >= (int) entries.size())
                return;

            const auto& entry = entries[(size_t) rowNumber];
            juce::String text;

            switch (columnId)
            {
                case columnOwner: text = entry.owner; break;
                case columnPc:    text = entry.pc; break;
                case columnName:  text = entry.name; break;
                case columnDate:  text = entry.modified == juce::Time() ? juce::String() : entry.modified.formatted ("%Y-%m-%d %H:%M"); break;
                case columnSize:  text = juce::File::descriptionOfSizeInBytes (entry.size); break;
                default: break;
            }

            g.setColour (Palette::text);
            g.setFont (bodyFont (13.5f));
            g.drawText (text, 8, 0, width - 16, height, juce::Justification::centredLeft, true);
        }

        void selectedRowsChanged (int lastRowSelected) override
        {
            restoreButton.setEnabled (lastRowSelected >= 0 && ! backup.isBusy());
        }

        void cellDoubleClicked (int rowNumber, int, const juce::MouseEvent&) override
        {
            table.selectRow (rowNumber);
            restoreSelected();
        }

        MixDocument& document;
        LiveMixSettings& settings;
        WebDavBackup& backup;
        BackupDialog::Callbacks callbacks;
        std::vector<WebDavBackup::Entry> entries;
        bool everyoneMode = false;

        juce::Label idCaption, passwordCaption, hint, statusLabel;
        juce::TextEditor idEditor, passwordEditor;
        juce::ToggleButton remember;
        juce::TextButton signInButton, createButton, uploadButton, restoreButton;
        juce::TableListBox table;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BackupContent)
    };

    class BackupWindow : public juce::DialogWindow
    {
    public:
        BackupWindow() : juce::DialogWindow (ko ("온라인 백업"), Palette::card, true, true)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, false);
            setResizeLimits (700, 440, 3000, 2000);
        }

        void closeButtonPressed() override
        {
            juce::MessageManager::callAsync ([] { BackupDialog::closeIfOpen(); });   // not from inside the window's own callback
        }
    };

    juce::Component* currentWindow()
    {
        return openWindow.getComponent();
    }
}

void BackupDialog::show (MixDocument& document, LiveMixSettings& settings, WebDavBackup& backup, juce::Component* centreAround, Callbacks callbacks)
{
    if (openWindow != nullptr)
    {
        openWindow->toFront (true);
        return;
    }

    auto* window = new BackupWindow();
    window->setContentOwned (new BackupContent (document, settings, backup, std::move (callbacks)), true);

    if (centreAround != nullptr)
        window->centreAroundComponent (centreAround, window->getWidth(), window->getHeight());
    else
        window->centreWithSize (window->getWidth(), window->getHeight());

    window->setVisible (true);
    window->toFront (true);
    openWindow = window;
}

void BackupDialog::closeIfOpen()
{
    if (openWindow != nullptr)
        delete openWindow.getComponent();   // the pointer clears itself
}

} // namespace gocue::livemix
