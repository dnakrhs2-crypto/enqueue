#include "BackupDialog.h"

#include "Widgets.h"

namespace gocue::livemix
{

namespace
{
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
            idEditor.onReturnKey = [this] { refreshList(); };
            addAndMakeVisible (idEditor);

            styleCaption (passwordCaption, ko ("비밀번호"));
            addAndMakeVisible (passwordCaption);
            passwordEditor.setFont (bodyFont());
            passwordEditor.setPasswordCharacter (0x2022);
            passwordEditor.onReturnKey = [this] { refreshList(); };
            addAndMakeVisible (passwordEditor);

            remember.setButtonText (ko ("이 PC에 기억"));
            remember.setToggleState (settings.getBackupRememberPassword(), juce::dontSendNotification);
            remember.setWantsKeyboardFocus (false);
            addAndMakeVisible (remember);

            if (remember.getToggleState())
                passwordEditor.setText (settings.getBackupPassword(), false);

            listButton.setButtonText (ko ("목록 보기"));
            listButton.setWantsKeyboardFocus (false);
            listButton.onClick = [this] { refreshList(); };
            addAndMakeVisible (listButton);

            table.setModel (this);
            table.setHeaderHeight (28);
            table.setRowHeight (28);
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
            header.addColumn ("PC", columnPc, 140, 60, 400, columnFlags);
            header.addColumn (ko ("파일"), columnName, 300, 120, 800, columnFlags);
            header.addColumn (ko ("날짜"), columnDate, 140, 100, 200, columnFlags);
            header.addColumn (ko ("크기"), columnSize, 80, 50, 120, columnFlags);
            header.setStretchToFitActive (true);
            header.setColumnVisible (columnOwner, false);
            addAndMakeVisible (table);

            styleCaption (creatorCaption, ko ("크리에이터 이름"));
            addAndMakeVisible (creatorCaption);
            creatorEditor.setFont (bodyFont());
            creatorEditor.setText (settings.getBackupCreator(), false);
            addAndMakeVisible (creatorEditor);

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

            statusLabel.setFont (bodyFont (13.0f));
            statusLabel.setColour (juce::Label::textColourId, Palette::dimText);
            statusLabel.setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (statusLabel);

            setSize (820, 560);

            if (idEditor.getText().trim().isNotEmpty() && passwordEditor.getText().isNotEmpty())
                refreshList();   // a remembered account: the list comes up at once
            else
                setStatus (ko ("아이디와 비밀번호를 넣고 목록 보기를 누르세요"), false);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (20, 16);
            auto row = area.removeFromTop (30);
            idCaption.setBounds (row.removeFromLeft (52));
            idEditor.setBounds (row.removeFromLeft (170));
            row.removeFromLeft (14);
            passwordCaption.setBounds (row.removeFromLeft (64));
            passwordEditor.setBounds (row.removeFromLeft (170));
            row.removeFromLeft (14);
            listButton.setBounds (row.removeFromRight (100));
            row.removeFromRight (8);
            remember.setBounds (row.removeFromLeft (juce::jmin (140, row.getWidth())));
            area.removeFromTop (12);

            auto bottom = area.removeFromBottom (30);
            statusLabel.setBounds (bottom);
            area.removeFromBottom (8);
            row = area.removeFromBottom (32);
            restoreButton.setBounds (row.removeFromRight (190));
            row.removeFromRight (14);
            uploadButton.setBounds (row.removeFromRight (150));
            row.removeFromRight (8);
            creatorCaption.setBounds (row.removeFromLeft (110));
            creatorEditor.setBounds (row.removeFromLeft (juce::jmax (100, juce::jmin (220, row.getWidth()))));
            area.removeFromBottom (12);

            table.setBounds (area);
        }

        void paint (juce::Graphics& g) override { g.fillAll (Palette::card); }

    private:
        enum { columnOwner = 1, columnPc, columnName, columnDate, columnSize };

        WebDavBackup::Target currentTarget() const
        {
            WebDavBackup::Target target;
            target.baseUrl = settings.getBackupUrl();
            target.folder = settings.getBackupFolder();
            target.user = idEditor.getText().trim();
            target.password = passwordEditor.getText();
            return target;
        }

        bool checkReady()
        {
            if (backup.isBusy())
            {
                setStatus (ko ("앞의 작업이 끝날 때까지 기다리세요"), true);
                return false;
            }

            if (const auto bad = WebDavBackup::validateBaseUrl (settings.getBackupUrl()); bad.isNotEmpty())
            {
                setStatus (bad + ko (" - 설정에서 주소를 고치세요"), true);
                return false;
            }

            if (idEditor.getText().trim().isEmpty() || passwordEditor.getText().isEmpty())
            {
                setStatus (ko ("아이디와 비밀번호를 넣으세요"), true);
                return false;
            }

            return true;
        }

        /** The account worked (a listing, an upload or a download went through): keep what the operator asked to keep. */
        void rememberAccount (const WebDavBackup::Target& target)
        {
            settings.setBackupUser (target.user);
            settings.setBackupRememberPassword (remember.getToggleState());
            settings.setBackupPassword (remember.getToggleState() ? target.password : juce::String());
        }

        void setBusy (bool busy)
        {
            listButton.setEnabled (! busy);
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

        void refreshList()
        {
            if (! checkReady())
                return;

            const auto target = currentTarget();
            juce::Component::SafePointer<BackupContent> safe (this);
            const auto started = backup.startList (target, [safe, target] (bool ok, const juce::String& message, std::vector<WebDavBackup::Entry> found, bool everyone)
            {
                if (safe != nullptr)
                    safe->listFinished (ok, message, std::move (found), everyone, target);
            });

            if (started.failed())
            {
                setStatus (started.getErrorMessage(), true);
                return;
            }

            setBusy (true);
            setStatus (ko ("목록 가져오는 중..."), false);
        }

        void listFinished (bool ok, const juce::String& message, std::vector<WebDavBackup::Entry> found, bool everyone, const WebDavBackup::Target& target)
        {
            setBusy (false);

            if (! ok)
            {
                entries.clear();
                table.updateContent();
                table.repaint();
                restoreButton.setEnabled (false);
                setStatus (message, true);
                return;
            }

            entries = std::move (found);
            table.getHeader().setColumnVisible (columnOwner, everyone);
            table.deselectAllRows();
            table.updateContent();
            table.repaint();
            restoreButton.setEnabled (false);
            rememberAccount (target);
            setStatus (message, false);
        }

        void upload()
        {
            if (! checkReady())
                return;

            const auto creator = creatorEditor.getText().trim();

            if (creator.isEmpty())
            {
                setStatus (ko ("크리에이터 이름을 넣으세요"), true);
                return;
            }

            if (! callbacks.saveBeforeUpload || ! callbacks.saveBeforeUpload())
            {
                setStatus (ko ("먼저 세션을 저장하세요 (세션 > 저장)"), true);
                return;
            }

            settings.setBackupCreator (creator);
            const auto target = currentTarget();
            const auto remotePath = WebDavBackup::remotePathFor (WebDavBackup::homeFolder (target.folder), juce::SystemStats::getComputerName(), creator, juce::Time::getCurrentTime());
            juce::Component::SafePointer<BackupContent> safe (this);
            auto status = callbacks.status;
            const auto started = backup.start (target, document.getFile(), remotePath, [safe, target, status] (bool ok, const juce::String& message)
            {
                if (status)
                    status (message, ! ok);   // the main window hears the result even when this window is gone

                if (safe == nullptr)
                    return;

                safe->setBusy (false);
                safe->setStatus (message, ! ok);

                if (ok)
                {
                    safe->rememberAccount (target);
                    safe->refreshList();   // the new backup in the list
                }
            });

            if (started.failed())
            {
                setStatus (started.getErrorMessage(), true);
                return;
            }

            setBusy (true);
            setStatus (ko ("백업 중... ") + remotePath, false);
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
            auto file = folder.getChildFile (WebDavBackup::sanitiseName (entry.name));

            if (file.existsAsFile())
                file = file.getNonexistentSibling();   // an earlier restore of the same backup keeps its file

            const auto target = currentTarget();
            juce::Component::SafePointer<BackupContent> safe (this);
            auto status = callbacks.status;
            auto restore = callbacks.restore;
            const auto started = backup.startDownload (target, entry.path, file, [safe, target, file, status, restore] (bool ok, const juce::String& message)
            {
                if (status)
                    status (message, ! ok);

                if (safe != nullptr)
                {
                    safe->setBusy (false);
                    safe->setStatus (message, ! ok);

                    if (ok)
                        safe->rememberAccount (target);
                }

                if (! ok)
                    return;

                if (restore)
                    restore (file);   // the session opens (after the usual unsaved-changes question)

                juce::MessageManager::callAsync ([] { BackupDialog::closeIfOpen(); });
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

        juce::Label idCaption, passwordCaption, creatorCaption, statusLabel;
        juce::TextEditor idEditor, passwordEditor, creatorEditor;
        juce::ToggleButton remember;
        juce::TextButton listButton, uploadButton, restoreButton;
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
            setResizeLimits (680, 420, 3000, 2000);
        }

        void closeButtonPressed() override
        {
            juce::MessageManager::callAsync ([] { BackupDialog::closeIfOpen(); });   // not from inside the window's own callback
        }
    };

    juce::Component::SafePointer<BackupWindow> openWindow;
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
