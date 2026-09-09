#include "BackupDialog.h"

#include "BackupServer.h"
#include "PluginPreset.h"
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

    /** 계정 만들기: a small window of its own. The id and the password (twice) go in, the account is made on the
        server, "등록완료!" shows, and 확인 closes the window - the backup window then signs in with the new account. */
    class RegisterContent : public juce::Component
    {
    public:
        RegisterContent (WebDavBackup& b, const juce::String& initialId, std::function<void (const juce::String& id, const juce::String& password)> registered)
            : backup (b), onRegistered (std::move (registered))
        {
            styleCaption (idCaption, ko ("아이디"));
            addAndMakeVisible (idCaption);
            idEditor.setFont (bodyFont());
            idEditor.setText (initialId, false);
            idEditor.onReturnKey = [this] { submit(); };
            addAndMakeVisible (idEditor);

            styleCaption (passwordCaption, ko ("비밀번호 (4자 이상)"));
            addAndMakeVisible (passwordCaption);
            passwordEditor.setFont (bodyFont());
            passwordEditor.setPasswordCharacter (0x2022);
            passwordEditor.onReturnKey = [this] { submit(); };
            addAndMakeVisible (passwordEditor);

            styleCaption (confirmCaption, ko ("비밀번호 확인"));
            addAndMakeVisible (confirmCaption);
            confirmEditor.setFont (bodyFont());
            confirmEditor.setPasswordCharacter (0x2022);
            confirmEditor.onReturnKey = [this] { submit(); };
            addAndMakeVisible (confirmEditor);

            styleCaption (hint, ko ("아이디는 2~20자(영문·숫자·한글·_·-). 백업은 이 아이디와 비밀번호로만 올리고 내려받습니다. 비밀번호를 잊으면 되찾을 수 없습니다."));
            hint.setFont (bodyFont (12.5f));
            hint.setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (hint);

            registerButton.setButtonText (ko ("등록"));
            registerButton.setWantsKeyboardFocus (false);
            registerButton.setColour (juce::TextButton::buttonColourId, Palette::accent);
            registerButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            registerButton.onClick = [this] { submit(); };
            addAndMakeVisible (registerButton);

            cancelButton.setButtonText (ko ("취소"));
            cancelButton.setWantsKeyboardFocus (false);
            cancelButton.onClick = [this] { closeWindow(); };
            addAndMakeVisible (cancelButton);

            doneLabel.setText (ko ("등록완료!"), juce::dontSendNotification);
            doneLabel.setFont (juce::Font (juce::FontOptions (pt (26.0f), juce::Font::bold)));
            doneLabel.setJustificationType (juce::Justification::centred);
            doneLabel.setColour (juce::Label::textColourId, Palette::accent);
            addChildComponent (doneLabel);

            doneNote.setJustificationType (juce::Justification::centred);
            doneNote.setFont (bodyFont (13.5f));
            doneNote.setMinimumHorizontalScale (1.0f);
            addChildComponent (doneNote);

            okButton.setButtonText (ko ("확인"));
            okButton.setWantsKeyboardFocus (false);
            okButton.setColour (juce::TextButton::buttonColourId, Palette::accent);
            okButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            okButton.onClick = [this]
            {
                if (onRegistered)
                    onRegistered (registeredId, registeredPassword);

                closeWindow();
            };
            addChildComponent (okButton);

            statusLabel.setFont (bodyFont (13.0f));
            statusLabel.setColour (juce::Label::textColourId, Palette::dimText);
            statusLabel.setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (statusLabel);

            setSize (440, 340);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (20, 16);

            if (done)
            {
                auto centre = area.withSizeKeepingCentre (area.getWidth(), 150);
                doneLabel.setBounds (centre.removeFromTop (60));
                centre.removeFromTop (8);
                doneNote.setBounds (centre.removeFromTop (40));
                centre.removeFromTop (12);
                okButton.setBounds (centre.removeFromTop (34).withSizeKeepingCentre (120, 34));
                return;
            }

            auto rowOf = [&area] (juce::Label& caption, juce::TextEditor& editor)
            {
                auto row = area.removeFromTop (30);
                caption.setBounds (row.removeFromLeft (150));
                editor.setBounds (row);
                area.removeFromTop (8);
            };

            rowOf (idCaption, idEditor);
            rowOf (passwordCaption, passwordEditor);
            rowOf (confirmCaption, confirmEditor);
            hint.setBounds (area.removeFromTop (50));
            area.removeFromTop (8);
            auto buttons = area.removeFromTop (32);
            registerButton.setBounds (buttons.removeFromRight (100));
            buttons.removeFromRight (8);
            cancelButton.setBounds (buttons.removeFromRight (80));
            area.removeFromTop (10);
            statusLabel.setBounds (area.removeFromTop (40));
        }

        void paint (juce::Graphics& g) override { g.fillAll (Palette::card); }

        void grabFirstField()
        {
            (idEditor.getText().trim().isEmpty() ? idEditor : passwordEditor).grabKeyboardFocus();
        }

    private:
        void setStatus (const juce::String& text, bool error)
        {
            statusLabel.setColour (juce::Label::textColourId, error ? Palette::danger : Palette::dimText);
            statusLabel.setText (text, juce::dontSendNotification);
        }

        void setBusy (bool busy)
        {
            for (auto* c : std::initializer_list<juce::Component*> { &idEditor, &passwordEditor, &confirmEditor, &registerButton })
                c->setEnabled (! busy);
        }

        void submit()
        {
            if (! BackupServer::isConfigured())
            {
                setStatus (ko ("이 프로그램에는 백업 서버가 설정되어 있지 않습니다"), true);
                return;
            }

            const auto id = idEditor.getText().trim();
            const auto password = passwordEditor.getText();

            if (const auto bad = WebDavBackup::validateAccountId (id); bad.isNotEmpty())
            {
                setStatus (bad, true);
                idEditor.grabKeyboardFocus();
                return;
            }

            if (password.length() < 4)
            {
                setStatus (ko ("비밀번호는 4자 이상으로 정하세요"), true);
                passwordEditor.grabKeyboardFocus();
                return;
            }

            if (confirmEditor.getText() != password)
            {
                setStatus (ko ("비밀번호 확인이 다릅니다"), true);
                confirmEditor.grabKeyboardFocus();
                return;
            }

            if (backup.isBusy())
            {
                setStatus (ko ("앞의 작업이 끝날 때까지 기다리세요"), true);
                return;
            }

            const auto target = BackupServer::target (id, password);
            juce::Component::SafePointer<RegisterContent> safe (this);
            const auto started = backup.createAccount (target, [safe, id, password] (bool ok, const juce::String& message)
            {
                if (safe == nullptr)
                    return;

                if (ok)
                {
                    safe->showDone (id, password);
                    return;
                }

                safe->setBusy (false);
                safe->setStatus (message, true);
            });

            if (started.failed())
            {
                setStatus (started.getErrorMessage(), true);
                return;
            }

            setBusy (true);
            setStatus (ko ("등록하는 중..."), false);
        }

        void showDone (const juce::String& id, const juce::String& password)
        {
            registeredId = id;
            registeredPassword = password;
            done = true;

            for (auto* c : std::initializer_list<juce::Component*> { &idCaption, &idEditor, &passwordCaption, &passwordEditor, &confirmCaption, &confirmEditor,
                                                                    &hint, &registerButton, &cancelButton, &statusLabel })
                c->setVisible (false);

            doneNote.setText (ko ("아이디 '") + id + ko ("'로 등록했습니다. 확인을 누르면 이 계정으로 로그인합니다."), juce::dontSendNotification);
            doneLabel.setVisible (true);
            doneNote.setVisible (true);
            okButton.setVisible (true);
            resized();
            okButton.grabKeyboardFocus();
        }

        void closeWindow()
        {
            juce::Component::SafePointer<juce::Component> window (getTopLevelComponent());
            juce::MessageManager::callAsync ([window]
            {
                if (window != nullptr)
                    delete window.getComponent();
            });
        }

        WebDavBackup& backup;
        std::function<void (const juce::String&, const juce::String&)> onRegistered;
        juce::String registeredId, registeredPassword;
        bool done = false;

        juce::Label idCaption, passwordCaption, confirmCaption, hint, statusLabel, doneLabel, doneNote;
        juce::TextEditor idEditor, passwordEditor, confirmEditor;
        juce::TextButton registerButton, cancelButton, okButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RegisterContent)
    };

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
            createButton.onClick = [this] { openRegisterWindow(); };
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
            header.addColumn (ko ("종류"), columnKind, 70, 50, 90, columnFlags);
            header.addColumn (ko ("이름 · PC"), columnPc, 150, 60, 400, columnFlags);   // the backup name typed at upload, then the PC
            header.addColumn (ko ("파일"), columnName, 280, 120, 800, columnFlags);
            header.addColumn (ko ("날짜"), columnDate, 140, 100, 200, columnFlags);
            header.addColumn (ko ("크기"), columnSize, 80, 50, 120, columnFlags);
            header.setStretchToFitActive (true);
            header.setColumnVisible (columnOwner, false);
            addAndMakeVisible (table);

            styleCaption (nameCaption, ko ("백업 이름"));
            // the operator's own word for whose session this is: it goes in front of the backup's file name, so the
            // list says "리붕후_PC이름_날짜" instead of a PC and a date the others cannot tell apart
            nameEditor.setFont (bodyFont (14.0f));
            nameEditor.setTooltip (ko ("백업 파일 이름 앞에 붙습니다. 목록에서 누구 세션인지 알아보려고 쓰는 이름입니다."));
            nameEditor.setTextToShowWhenEmpty (ko ("누구 세션인지"), Palette::dimText);
            {
                const auto remembered = settings.getBackupCreator();
                nameEditor.setText (remembered.isNotEmpty() ? remembered : document.getDisplayName(), juce::dontSendNotification);
            }
            addAndMakeVisible (nameCaption);
            addAndMakeVisible (nameEditor);

            uploadButton.setButtonText (ko ("지금 세션 백업"));
            uploadButton.setWantsKeyboardFocus (false);
            uploadButton.onClick = [this] { upload(); };
            addAndMakeVisible (uploadButton);

            uploadPresetsButton.setButtonText (ko ("플러그인 프리셋 백업"));
            uploadPresetsButton.setTooltip (ko ("이 PC의 플러그인 프리셋을 전부 이 계정에 올립니다 (같은 이름은 덮어씁니다)"));
            uploadPresetsButton.setWantsKeyboardFocus (false);
            uploadPresetsButton.onClick = [this] { uploadPresets(); };
            addAndMakeVisible (uploadPresetsButton);

            restoreButton.setButtonText (ko ("선택한 백업 불러오기"));
            restoreButton.setWantsKeyboardFocus (false);
            restoreButton.setColour (juce::TextButton::buttonColourId, Palette::accent);
            restoreButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            restoreButton.setEnabled (false);
            restoreButton.onClick = [this] { restoreSelected(); };
            addAndMakeVisible (restoreButton);

            // what can be done to the backup picked in the list
            styleCaption (selectedCaption, ko ("고른 백업:"));
            addAndMakeVisible (selectedCaption);

            overwriteButton.setButtonText (ko ("지금 세션으로 덮어쓰기"));
            overwriteButton.setTooltip (ko ("고른 백업 파일을 지금 세션으로 바꿔 씁니다 (파일 이름은 그대로)"));
            overwriteButton.setWantsKeyboardFocus (false);
            overwriteButton.setEnabled (false);
            overwriteButton.onClick = [this] { overwriteSelected(); };
            addAndMakeVisible (overwriteButton);

            renameButton.setButtonText (ko ("이름 바꾸기"));
            renameButton.setWantsKeyboardFocus (false);
            renameButton.setEnabled (false);
            renameButton.onClick = [this] { renameSelected(); };
            addAndMakeVisible (renameButton);

            deleteButton.setButtonText (ko ("삭제"));
            deleteButton.setWantsKeyboardFocus (false);
            deleteButton.setColour (juce::TextButton::buttonColourId, Palette::danger);
            deleteButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            deleteButton.setEnabled (false);
            deleteButton.onClick = [this] { deleteSelected(); };
            addAndMakeVisible (deleteButton);

            styleCaption (hint, ko ("백업은 로그인한 계정의 것만 보이고, 올리기와 불러오기도 그 계정의 아이디·비밀번호로만 됩니다. 처음이면 '계정 만들기'로 아이디와 비밀번호를 등록하세요. 세션과 플러그인 프리셋이 함께 목록에 보입니다. '백업 이름'에 적은 이름이 백업 파일 앞에 붙습니다."));
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
            const bool twoRows = area.getWidth() < 760;   // a narrow window (a portrait monitor): the buttons on a row of their own
            auto row = area.removeFromTop (30);
            idCaption.setBounds (row.removeFromLeft (52));
            idEditor.setBounds (row.removeFromLeft (150));
            row.removeFromLeft (12);
            passwordCaption.setBounds (row.removeFromLeft (64));
            passwordEditor.setBounds (row.removeFromLeft (150));
            row.removeFromLeft (12);

            if (twoRows)
            {
                remember.setBounds (row.removeFromLeft (juce::jmin (130, juce::jmax (0, row.getWidth()))));
                area.removeFromTop (6);
                row = area.removeFromTop (30);
            }

            createButton.setBounds (row.removeFromRight (100));
            row.removeFromRight (8);
            signInButton.setBounds (row.removeFromRight (90));
            row.removeFromRight (8);

            if (! twoRows)
                remember.setBounds (row.removeFromLeft (juce::jmin (130, juce::jmax (0, row.getWidth()))));

            area.removeFromTop (6);
            hint.setBounds (area.removeFromTop (34));
            area.removeFromTop (8);

            auto bottom = area.removeFromBottom (30);
            statusLabel.setBounds (bottom);
            area.removeFromBottom (8);
            auto picked = area.removeFromBottom (30);
            deleteButton.setBounds (picked.removeFromRight (80));
            picked.removeFromRight (8);
            renameButton.setBounds (picked.removeFromRight (110));
            picked.removeFromRight (8);
            overwriteButton.setBounds (picked.removeFromRight (170));
            picked.removeFromRight (14);
            selectedCaption.setBounds (picked);
            area.removeFromBottom (8);

            row = area.removeFromBottom (32);
            restoreButton.setBounds (row.removeFromRight (190));
            row.removeFromRight (14);
            uploadButton.setBounds (row.removeFromRight (150));
            row.removeFromRight (8);
            uploadPresetsButton.setBounds (row.removeFromRight (170));
            row.removeFromRight (14);
            nameCaption.setBounds (row.removeFromLeft (juce::jmin (74, juce::jmax (0, row.getWidth()))));
            nameEditor.setBounds (row.removeFromLeft (juce::jmin (230, juce::jmax (0, row.getWidth()))));
            area.removeFromBottom (12);

            table.setBounds (area);
        }

        void paint (juce::Graphics& g) override { g.fillAll (Palette::card); }

        ~BackupContent() override
        {
            if (registerWindow != nullptr)
                delete registerWindow.getComponent();   // the registration window goes with this one
        }

    private:
        enum { columnOwner = 1, columnPc, columnName, columnDate, columnSize, columnKind };

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
            uploadPresetsButton.setEnabled (! busy);
            idEditor.setEnabled (! busy);
            passwordEditor.setEnabled (! busy);
            updateSelectionButtons (busy);
        }

        /** The four buttons that need a backup picked in the list (a preset cannot be overwritten by a session). */
        void updateSelectionButtons (bool busy)
        {
            const int row = table.getSelectedRow();
            const bool picked = ! busy && juce::isPositiveAndBelow (row, (int) entries.size());
            restoreButton.setEnabled (picked);
            renameButton.setEnabled (picked);
            deleteButton.setEnabled (picked);
            overwriteButton.setEnabled (picked && ! entries[(size_t) row].isPreset);
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
            updateSelectionButtons (backup.isBusy());
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

        void openRegisterWindow()
        {
            if (registerWindow != nullptr)
            {
                registerWindow->toFront (true);
                return;
            }

            if (! BackupServer::isConfigured())
            {
                setStatus (ko ("이 프로그램에는 백업 서버가 설정되어 있지 않습니다"), true);
                return;
            }

            juce::Component::SafePointer<BackupContent> safe (this);
            auto* content = new RegisterContent (backup, idEditor.getText().trim(), [safe] (const juce::String& id, const juce::String& password)
            {
                if (safe == nullptr)
                    return;

                safe->idEditor.setText (id, false);
                safe->passwordEditor.setText (password, false);
                safe->whenIdle ([safe] { if (safe != nullptr) safe->signIn(); });   // straight in with the new account
            });

            juce::DialogWindow::LaunchOptions options;
            options.content.setOwned (content);
            options.dialogTitle = ko ("계정 만들기");
            options.dialogBackgroundColour = Palette::card;
            options.escapeKeyTriggersCloseButton = true;
            options.useNativeTitleBar = true;
            options.resizable = false;
            options.componentToCentreAround = this;
            registerWindow = options.launchAsync();
            content->grabFirstField();
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
            const auto label = nameEditor.getText().trim().substring (0, WebDavBackup::maxBackupLabel);
            settings.setBackupCreator (label);   // the same name is offered next time
            const auto remotePath = WebDavBackup::backupPathFor (target.share, target.accountId, label,
                                                                 juce::SystemStats::getComputerName(), juce::Time::getCurrentTime());
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

        void uploadPresets()
        {
            if (! checkReady())
                return;

            const auto folder = PluginPreset::defaultFolder();
            juce::StringArray problems;
            const auto presets = PluginPreset::listFolder (folder, &problems);

            if (! problems.isEmpty())
            {
                // "backup complete" must mean every preset: a file that cannot be read stops it, by name
                setStatus (ko ("백업하지 않았습니다 - 읽을 수 없는 프리셋 파일이 있습니다 (고치거나 지우세요): ") + problems.joinIntoString (" / "), true);
                return;
            }

            if (presets.empty())
            {
                setStatus (ko ("이 PC에 플러그인 프리셋이 없습니다 (플러그인 관리에서 만듭니다)"), true);
                return;
            }

            const auto target = currentTarget();
            std::vector<std::pair<juce::File, juce::String>> files;
            juce::StringArray remotePaths, clashes;

            for (const auto& p : presets)
            {
                // the remote name follows the file name (unique in the folder), not the name inside the file
                const auto remote = WebDavBackup::presetPathFor (target.share, target.accountId, p.file.getFileNameWithoutExtension());

                if (remotePaths.contains (remote))
                {
                    clashes.add (p.file.getFileName());
                    continue;
                }

                remotePaths.add (remote);
                files.emplace_back (p.file, remote);
            }

            if (! clashes.isEmpty())
            {
                setStatus (ko ("백업하지 않았습니다 - 서버에서 같은 이름이 되는 프리셋 파일이 있습니다 (이름을 바꾸세요): ") + clashes.joinIntoString (", "), true);
                return;
            }

            juce::Component::SafePointer<BackupContent> safe (this);
            auto status = callbacks.status;
            auto* prefs = &settings;
            const bool keep = remember.getToggleState();
            const auto started = backup.startUploads (target, std::move (files), [safe, target, status, prefs, keep] (bool ok, const juce::String& message)
            {
                if (ok)
                    persistAccount (*prefs, target, keep);

                if (status)
                    status (message, ! ok);

                if (safe == nullptr)
                    return;

                safe->setStatus (message, ! ok);

                if (ok)
                    safe->whenIdle ([safe] { if (safe != nullptr) safe->signIn(); });   // the presets in the list
                else
                    safe->setBusy (false);
            });

            if (started.failed())
            {
                setStatus (started.getErrorMessage(), true);
                return;
            }

            setBusy (true);
            setStatus (ko ("플러그인 프리셋 ") + juce::String ((int) presets.size()) + ko ("개 올리는 중..."), false);
        }

        /** The backup picked in the list, or nothing (with a word in the status line). */
        const WebDavBackup::Entry* selectedEntry()
        {
            const int row = table.getSelectedRow();

            if (! juce::isPositiveAndBelow (row, (int) entries.size()))
            {
                setStatus (ko ("목록에서 백업을 고르세요"), true);
                return nullptr;
            }

            return &entries[(size_t) row];
        }

        void deleteSelected()
        {
            const auto* entry = selectedEntry();

            if (entry == nullptr || ! checkReady())
                return;

            const auto name = entry->name;
            const auto path = entry->path;
            juce::Component::SafePointer<BackupContent> safe (this);
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                              .withIconType (juce::MessageBoxIconType::WarningIcon)
                                              .withTitle (ko ("백업 삭제"))
                                              .withMessage (ko ("'") + name + ko ("' 백업을 서버에서 지울까요?") + juce::newLine + juce::newLine
                                                            + ko ("되돌릴 수 없습니다."))
                                              .withButton (ko ("삭제"))
                                              .withButton (ko ("취소")),
                                          [safe, path] (int result)
            {
                if (safe == nullptr || result != 1 || ! safe->checkReady())
                    return;

                const auto target = safe->currentTarget();
                auto status = safe->callbacks.status;
                auto* prefs = &safe->settings;
                const bool keep = safe->remember.getToggleState();
                const auto started = safe->backup.startDelete (target, path, [safe, target, status, prefs, keep] (bool ok, const juce::String& message)
                {
                    if (ok)
                        persistAccount (*prefs, target, keep);

                    if (status)
                        status (message, ! ok);

                    if (safe == nullptr)
                        return;

                    safe->setStatus (message, ! ok);

                    if (ok)
                        safe->whenIdle ([safe] { if (safe != nullptr) safe->signIn(); });   // the list without it
                    else
                        safe->setBusy (false);
                });

                if (started.failed())
                {
                    safe->setStatus (started.getErrorMessage(), true);
                    return;
                }

                safe->setBusy (true);
                safe->setStatus (ko ("삭제 중... ") + path.fromLastOccurrenceOf ("/", false, false), false);
            });
        }

        void renameSelected()
        {
            const auto* entry = selectedEntry();

            if (entry == nullptr || ! checkReady())
                return;

            const auto name = entry->name;
            const auto path = entry->path;
            const bool preset = entry->isPreset;
            // what is shown to edit: no extension, and no "프리셋_" in front of a preset
            const auto shown = preset ? WebDavBackup::presetNameFromFileName (name)
                                      : name.upToLastOccurrenceOf (".", false, false);

            auto* alert = new juce::AlertWindow (ko ("백업 이름 바꾸기"),
                                                 ko ("새 이름 (확장자는 그대로 붙습니다)"), juce::MessageBoxIconType::NoIcon);
            alert->addTextEditor ("name", shown, ko ("이름"));
            alert->addButton (ko ("확인"), 1, juce::KeyPress (juce::KeyPress::returnKey));
            alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
            juce::Component::SafePointer<BackupContent> safe (this);
            alert->enterModalState (true, juce::ModalCallbackFunction::create ([safe, alert, name, path, shown] (int result)
            {
                if (safe == nullptr || result != 1)
                    return;

                const auto typed = alert->getTextEditorContents ("name").trim();

                if (typed == shown.trim())
                    return;   // nothing changed

                const auto newName = WebDavBackup::renamedFileName (name, typed);

                if (newName.isEmpty())
                {
                    safe->setStatus (ko ("쓸 수 있는 이름이 아닙니다"), true);
                    return;
                }

                if (! safe->checkReady())
                    return;

                const auto target = safe->currentTarget();
                auto status = safe->callbacks.status;
                auto* prefs = &safe->settings;
                const bool keep = safe->remember.getToggleState();
                const auto started = safe->backup.startRename (target, path, newName, [safe, target, status, prefs, keep] (bool ok, const juce::String& message)
                {
                    if (ok)
                        persistAccount (*prefs, target, keep);

                    if (status)
                        status (message, ! ok);

                    if (safe == nullptr)
                        return;

                    safe->setStatus (message, ! ok);

                    if (ok)
                        safe->whenIdle ([safe] { if (safe != nullptr) safe->signIn(); });
                    else
                        safe->setBusy (false);
                });

                if (started.failed())
                {
                    safe->setStatus (started.getErrorMessage(), true);
                    return;
                }

                safe->setBusy (true);
                safe->setStatus (ko ("이름 바꾸는 중... ") + newName, false);
            }), true);
            focusAlertTextEditor (*alert, "name");
        }

        void overwriteSelected()
        {
            const auto* entry = selectedEntry();

            if (entry == nullptr || ! checkReady())
                return;

            if (entry->isPreset)
            {
                setStatus (ko ("플러그인 프리셋은 세션으로 덮어쓸 수 없습니다"), true);
                return;
            }

            const auto name = entry->name;
            const auto path = entry->path;
            juce::Component::SafePointer<BackupContent> safe (this);
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                              .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                              .withTitle (ko ("백업 덮어쓰기"))
                                              .withMessage (ko ("'") + name + ko ("' 백업을 지금 세션으로 덮어쓸까요?") + juce::newLine + juce::newLine
                                                            + ko ("그 백업에 들어 있던 내용은 사라집니다. 파일 이름은 그대로입니다."))
                                              .withButton (ko ("덮어쓰기"))
                                              .withButton (ko ("취소")),
                                          [safe, path] (int result)
            {
                if (safe == nullptr || result != 1 || ! safe->checkReady())
                    return;

                if (! safe->callbacks.saveBeforeUpload || ! safe->callbacks.saveBeforeUpload())
                {
                    safe->setStatus (ko ("먼저 세션을 저장하세요 (세션 > 저장)"), true);
                    return;
                }

                const auto target = safe->currentTarget();
                auto status = safe->callbacks.status;
                auto* prefs = &safe->settings;
                const bool keep = safe->remember.getToggleState();
                const auto started = safe->backup.start (target, safe->document.getFile(), path, [safe, target, status, prefs, keep] (bool ok, const juce::String& message)
                {
                    if (ok)
                        persistAccount (*prefs, target, keep);

                    if (status)
                        status (message, ! ok);

                    if (safe == nullptr)
                        return;

                    safe->setStatus (message, ! ok);

                    if (ok)
                        safe->whenIdle ([safe] { if (safe != nullptr) safe->signIn(); });   // its new size and date
                    else
                        safe->setBusy (false);
                });

                if (started.failed())
                {
                    safe->setStatus (started.getErrorMessage(), true);
                    return;
                }

                safe->setBusy (true);
                safe->setStatus (ko ("덮어쓰는 중... ") + path.fromLastOccurrenceOf ("/", false, false), false);
            });
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
            const bool preset = entry.isPreset;
            auto folder = preset ? PluginPreset::defaultFolder()
                                 : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("LiveMix");
            folder.createDirectory();
            const auto ownerPrefix = everyoneMode && entry.owner != idEditor.getText().trim() ? entry.owner + "_" : juce::String();
            const auto localName = preset ? PluginPreset::fileNameFor (ownerPrefix + WebDavBackup::presetNameFromFileName (entry.name))
                                          : ownerPrefix + entry.name;
            const auto wanted = folder.getChildFile (WebDavBackup::sanitiseName (localName));   // the name it gets - chosen again once it has arrived

            // the download lands in a staging file outside the folder: nothing half-written or unread sits among the
            // presets / sessions, and a preset made under that name meanwhile keeps it
            auto staging = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LiveMix");
            staging.createDirectory();

            for (const auto& leftover : staging.findChildFiles (juce::File::findFiles, false, "restore*"))
                leftover.deleteFile();   // a restore cancelled, or the app quit, before its callback could tidy up (one restore runs at a time)

            const auto file = staging.getNonexistentChildFile ("restore", wanted.getFileExtension());

            const auto target = currentTarget();
            juce::Component::SafePointer<BackupContent> safe (this);
            juce::Component::SafePointer<juce::Component> mine (getTopLevelComponent());   // this window, not a later one
            auto status = callbacks.status;
            auto restore = callbacks.restore;
            auto presetRestored = callbacks.presetRestored;
            auto* prefs = &settings;
            const bool keep = remember.getToggleState();
            const auto started = backup.startDownload (target, entry.path, file, [safe, mine, target, file, wanted, status, restore, presetRestored, preset, prefs, keep] (bool ok, const juce::String& message)
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
                    file.deleteFile();
                    report (message, true);
                    return;
                }

                persistAccount (*prefs, target, keep);

                // its name in the folder, now that it is here: a file made under that name meanwhile keeps it
                auto destination = wanted;

                if (destination.existsAsFile())
                    destination = destination.getNonexistentSibling();

                if (preset)
                {
                    // a plugin preset: read in the staging file, then written whole into the presets folder; the window stays for more
                    PluginPreset probe;

                    if (const auto check = PluginPreset::load (file, probe); check.failed())
                    {
                        file.deleteFile();
                        report (juce::String::fromUTF8 ("내려받은 파일을 프리셋으로 읽을 수 없습니다: ") + check.getErrorMessage()
                                    + juce::String::fromUTF8 (" (프리셋 폴더에는 넣지 않았습니다)"), true);
                        return;
                    }

                    probe.name = destination.getFileNameWithoutExtension();   // the preset goes by its file name here (an owner prefix, a "(2)"), as an import does
                    const auto saved = probe.save (destination);
                    file.deleteFile();

                    if (saved.failed())
                    {
                        report (juce::String::fromUTF8 ("내려받은 프리셋을 프리셋 폴더에 쓰지 못했습니다: ") + saved.getErrorMessage(), true);
                        return;
                    }

                    report (juce::String::fromUTF8 ("프리셋 불러옴: ") + probe.name + juce::String::fromUTF8 (" (플러그인 관리 창과 '+ 추가 > 프리셋 불러오기'에 보입니다)"), false);

                    if (presetRestored)
                        presetRestored();

                    return;
                }

                // whatever came down must be a session before it is opened as one
                MixSession probe;
                juce::StringArray warnings;

                if (const auto check = MixSession::load (file, probe, &warnings); check.failed())
                {
                    // kept where it landed: the copy on the server is all there is of it, and a newer LiveMix may open it
                    report (juce::String::fromUTF8 ("내려받은 파일을 세션으로 열 수 없습니다: ") + check.getErrorMessage()
                                + juce::String::fromUTF8 (" (파일은 남겨 두었습니다: ") + file.getFullPathName() + ")", true);
                    return;
                }

                if (! file.moveFileTo (destination))
                {
                    report (juce::String::fromUTF8 ("내려받은 세션을 세션 폴더로 옮기지 못했습니다: ") + destination.getFullPathName()
                                + juce::String::fromUTF8 (" (내려받은 파일: ") + file.getFullPathName() + ")", true);
                    return;
                }

                juce::ignoreUnused (message);
                report (juce::String::fromUTF8 ("불러옴: ") + destination.getFileName(), false);

                if (restore)
                    restore (destination);   // the session opens (after the usual unsaved-changes question)

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
                case columnKind:  text = entry.isPreset ? ko ("프리셋") : ko ("세션"); break;
                default: break;
            }

            g.setColour (Palette::text);
            g.setFont (bodyFont (13.5f));
            g.drawText (text, 8, 0, width - 16, height, juce::Justification::centredLeft, true);
        }

        void selectedRowsChanged (int) override
        {
            updateSelectionButtons (backup.isBusy());
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

        juce::Label idCaption, passwordCaption, hint, statusLabel, nameCaption, selectedCaption;
        juce::TextEditor idEditor, passwordEditor, nameEditor;
        juce::ToggleButton remember;
        juce::TextButton signInButton, createButton, uploadButton, uploadPresetsButton, restoreButton;
        juce::TextButton overwriteButton, renameButton, deleteButton;
        juce::TableListBox table;
        juce::Component::SafePointer<juce::DialogWindow> registerWindow;

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
