#include "ui/ShortcutRouter.h"
#include "ui/PluginManager.h"

#include "app/UiScale.h"
#include "ui/PluginScanListComponent.h"
#include "ui/UiUtils.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace gocue
{

namespace
{
    /** Every word of the query (split at spaces) has to appear in the plugin's name or its maker, case-insensitively;
        an empty query matches every plugin. "waves comp" finds "Waves C6 Compressor". */
    bool pluginMatchesSearch (const juce::PluginDescription& d, const juce::String& query)
    {
        juce::StringArray words;
        words.addTokens (query, " \t", "");
        words.removeEmptyStrings();

        if (words.isEmpty())
            return true;

        const auto haystack = d.name + " " + d.manufacturerName + " " + d.pluginFormatName;

        for (const auto& word : words)
            if (! haystack.containsIgnoreCase (word))
                return false;

        return true;
    }

    juce::Array<juce::PluginDescription> filterPlugins (const juce::Array<juce::PluginDescription>& all, const juce::String& query)
    {
        juce::Array<juce::PluginDescription> shown;

        for (const auto& d : all)
            if (pluginMatchesSearch (d, query))
                shown.add (d);

        return shown;
    }

    juce::Font bodyFont (float size) { return Palette::font (size); }

    void styleCaption (juce::Label& label, const juce::String& text, float size, bool bold = false)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (Palette::font (size, bold));
        label.setColour (juce::Label::textColourId, bold ? Palette::text : Palette::dimText);
        label.setJustificationType (juce::Justification::centredLeft);
        label.setMinimumHorizontalScale (1.0f);
    }

    constexpr int columnFlags = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable;
}

//==============================================================================
class PluginManagerWindow::Content : public juce::Component,
                                     private juce::ChangeListener,
                                     private juce::Timer
{
public:
    Content (PluginHost& h, AppSettings& s) : host (h), settings (s), model (*this)
    {
        styleCaption (caption, ko ("VST3 플러그인"), 16.0f, true);
        addAndMakeVisible (caption);

        styleCaption (note, ko ("\"사용\"을 끈 플러그인은 모든 \"+ 추가\" 메뉴에서 빠집니다 (이미 체인에 든 것은 그대로). 스캔 결과와 \"사용\" 설정은 이 PC의 사용자 설정에 저장됩니다."), 13.5f);
        addAndMakeVisible (note);

        // the search: the table shows the plugins it matches (every word: name, maker); Esc clears it
        search.setFont (bodyFont (15.0f));
        search.setTextToShowWhenEmpty (ko ("검색: 이름, 제조사 (예: waves comp)"), Palette::dimText);
        search.setSelectAllWhenFocused (true);
        search.onTextChange = [this] { applySearch(); };
        search.onEscapeKey = [this] { search.setText ({}, true); };   // with the change message: the table shows everything again
        addAndMakeVisible (search);

        styleCaption (count, "", 13.5f);
        count.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (count);

        table.setModel (&model);
        table.setHeaderHeight (30);
        table.setRowHeight (30);
        table.setColour (juce::ListBox::backgroundColourId, Palette::panel);
        table.setColour (juce::ListBox::outlineColourId, Palette::outline);
        table.setOutlineThickness (1);
        auto& header = table.getHeader();
        header.setColour (juce::TableHeaderComponent::backgroundColourId, Palette::header);
        header.setColour (juce::TableHeaderComponent::textColourId, Palette::dimText);
        header.setColour (juce::TableHeaderComponent::outlineColourId, Palette::outline);
        header.setColour (juce::TableHeaderComponent::highlightColourId, Palette::header);
        header.setStretchToFitActive (true);
        header.addColumn (ko ("사용"), colEnabled, 56, 56, 56, juce::TableHeaderComponent::visible);
        header.addColumn (ko ("이름"), colName, 260, 100, 600, columnFlags);
        header.addColumn (ko ("제조사"), colMaker, 170, 60, 400, columnFlags);
        header.addColumn (ko ("파일"), colFile, 320, 100, 1200, columnFlags);
        addAndMakeVisible (table);

        auto button = [this] (juce::TextButton& b, const juce::String& text, std::function<void()> fn)
        {
            b.setButtonText (text);
            b.setWantsKeyboardFocus (false);
            b.onClick = std::move (fn);
            addAndMakeVisible (b);
        };

        button (scanButton, ko ("VST3 스캔..."), [this] { scan(); });
        button (enableAllButton, ko ("전부 사용"), [this] { setAll (true); });
        button (disableAllButton, ko ("전부 해제"), [this] { setAll (false); });
        button (removeButton, ko ("목록에서 빼기"), [this] { removeSelected(); });
        button (resetListButton, ko ("목록 초기화"), [this] { resetList(); });
        removeButton.setEnabled (false);

        PluginScanCoordinator::Options scanOptions;
        scanOptions.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        scanOptions.logFile = settings.getPluginScanLogFile();
        auto isolatedScanner = std::make_unique<PluginScanCoordinator> (scanOptions);
        scanCoordinator = isolatedScanner.get();
        host.getKnownPlugins().setCustomScanner (std::move (isolatedScanner));

        // JUCE drives progress/cancellation; plugin code runs on the worker's message thread.
        scanner = std::make_unique<PluginScanListComponent> (host.getFormatManager(), host.getKnownPlugins(),
                                                            settings.getDeadMansPedalFile(), *scanCoordinator,
                                                            ko ("Enqueue 플러그인 스캔"), settings.getPropertiesFile());
        addChildComponent (*scanner);

        styleCaption (status, "", 13.5f);
        addAndMakeVisible (status);

        host.getKnownPlugins().addChangeListener (this);
        refreshPlugins();
        setSize (900, 600);
    }

    ~Content() override
    {
        stopTimer();
        host.getKnownPlugins().removeChangeListener (this);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (18, 14);
        caption.setBounds (area.removeFromTop (24));
        area.removeFromTop (2);
        note.setBounds (area.removeFromTop (36));
        area.removeFromTop (6);

        auto searchRow = area.removeFromTop (30);
        count.setBounds (searchRow.removeFromRight (juce::jmin (260, searchRow.getWidth() / 3)));
        searchRow.removeFromRight (8);
        search.setBounds (searchRow.removeFromLeft (juce::jmin (440, searchRow.getWidth())));
        area.removeFromTop (8);

        status.setBounds (area.removeFromBottom (24));
        area.removeFromBottom (6);

        auto buttons = area.removeFromBottom (32);
        scanButton.setBounds (buttons.removeFromLeft (120));
        buttons.removeFromLeft (8);
        enableAllButton.setBounds (buttons.removeFromLeft (96));
        buttons.removeFromLeft (8);
        disableAllButton.setBounds (buttons.removeFromLeft (96));
        resetListButton.setBounds (buttons.removeFromRight (100));
        buttons.removeFromRight (8);
        removeButton.setBounds (buttons.removeFromRight (130));
        area.removeFromBottom (8);
        table.setBounds (area);
    }

    void paint (juce::Graphics& g) override { Palette::drawDialog (g, getLocalBounds()); }

    void focusSearch()
    {
        juce::Component::SafePointer<juce::TextEditor> editor (&search);
        juce::MessageManager::callAsync ([editor] { if (editor != nullptr && editor->isShowing()) editor->grabKeyboardFocus(); });
    }

    /** Show mode: looking and searching only - the switches, the scan and the removal are off. */
    void setLocked (bool shouldLock)
    {
        locked = shouldLock;

        for (auto* b : { &scanButton, &enableAllButton, &disableAllButton, &resetListButton })
            b->setEnabled (! locked);

        removeButton.setEnabled (! locked && table.getSelectedRow() >= 0);
        table.repaint();
        setStatus (locked ? ko ("쇼 모드: 편집 잠김 (보기와 검색만 됩니다)") : juce::String(), false);
    }

private:
    enum { colEnabled = 1, colName, colMaker, colFile };

    struct Model : public juce::TableListBoxModel
    {
        explicit Model (Content& o) : owner (o) {}
        int getNumRows() override { return owner.shown.size(); }

        void paintRowBackground (juce::Graphics& g, int row, int, int, bool selected) override
        {
            g.fillAll (selected ? Palette::selected : (row % 2 == 0 ? Palette::rowEven : Palette::rowOdd));
        }

        void paintCell (juce::Graphics& g, int row, int columnId, int width, int height, bool) override
        {
            if (row < 0 || row >= owner.shown.size())
                return;

            const auto& d = owner.shown.getReference (row);
            const bool enabled = ! owner.host.isPluginSwitchedOff (d);

            if (columnId == colEnabled)
            {
                const auto box = juce::Rectangle<float> (Palette::tickSize, Palette::tickSize).withCentre ({ (float) width * 0.5f, (float) height * 0.5f });
                g.setColour (enabled ? Palette::accent : Palette::field);
                g.fillRoundedRectangle (box, Palette::tickRadius);
                g.setColour (enabled ? Palette::accent : Palette::muted);
                g.drawRoundedRectangle (box, Palette::tickRadius, Palette::borderWidth);

                if (enabled)
                {
                    g.setColour (Palette::accentInk);
                    juce::Path tick;
                    tick.startNewSubPath (box.getX() + 4.0f, box.getCentreY());
                    tick.lineTo (box.getCentreX() - 1.0f, box.getBottom() - 5.0f);
                    tick.lineTo (box.getRight() - 4.0f, box.getY() + 5.0f);
                    g.strokePath (tick, juce::PathStrokeType (2.2f));
                }

                return;
            }

            juce::String text;

            switch (columnId)
            {
                case colName:  text = d.name; break;
                case colMaker: text = d.manufacturerName; break;
                case colFile:  text = d.fileOrIdentifier; break;
                default: break;
            }

            g.setColour (enabled ? Palette::text : Palette::dimText);
            g.setFont (bodyFont (15.0f));
            g.drawText (text, 8, 0, width - 16, height, juce::Justification::centredLeft, true);
        }

        void cellClicked (int row, int columnId, const juce::MouseEvent&) override
        {
            if (columnId == colEnabled && row >= 0 && row < owner.shown.size())
                owner.toggle (owner.shown.getReference (row));
        }

        void cellDoubleClicked (int row, int columnId, const juce::MouseEvent&) override
        {
            if (columnId != colEnabled && row >= 0 && row < owner.shown.size())
                owner.toggle (owner.shown.getReference (row));   // a double-click anywhere on the row flips its switch too
        }

        void selectedRowsChanged (int) override { owner.removeButton.setEnabled (! owner.locked && owner.table.getSelectedRow() >= 0); }

        Content& owner;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refreshPlugins(); }

    /** Runs while JUCE's scanner is up: when it is gone (done or cancelled), the status line says what is known now. */
    void timerCallback() override
    {
        if (scanner->isScanning())
            return;

        stopTimer();
        refreshPlugins();
        const auto skipped = scanCoordinator->getSkippedMessage();
        setStatus (skipped.isNotEmpty() ? skipped : ko ("스캔 끝: 플러그인 ") + juce::String (all.size()) + ko ("개"),
                   skipped.isNotEmpty());
        status.setTooltip (skipped);
    }

    void refreshPlugins()
    {
        all = host.getAllEffectTypes();
        applySearch();
    }

    bool isSearching() const { return search.getText().trim().isNotEmpty(); }

    /** The table shows the plugins the search box matches. The selection stays on the same plugin when it is still
        shown (a row number would point at another plugin once rows are hidden), otherwise nothing is selected. */
    void applySearch()
    {
        const int selectedRow = table.getSelectedRow();
        const auto selectedKey = selectedRow >= 0 && selectedRow < shown.size() ? PluginHost::keyFor (shown.getReference (selectedRow)) : juce::String();
        shown = filterPlugins (all, search.getText());
        table.updateContent();
        table.deselectAllRows();

        if (selectedKey.isNotEmpty())
            for (int i = 0; i < shown.size(); ++i)
                if (PluginHost::keyFor (shown.getReference (i)) == selectedKey)
                {
                    table.selectRow (i);
                    break;
                }

        table.repaint();
        removeButton.setEnabled (! locked && table.getSelectedRow() >= 0);
        count.setText (! isSearching() ? ko ("플러그인 ") + juce::String (all.size()) + ko ("개")
                                       : ko ("검색 결과 ") + juce::String (shown.size()) + ko ("개 / 전체 ") + juce::String (all.size()) + ko ("개"),
                       juce::dontSendNotification);
    }

    void toggle (const juce::PluginDescription& d)
    {
        if (locked)
            return;

        host.setPluginEnabled (d, host.isPluginSwitchedOff (d));
        settings.setDisabledPlugins (host.getDisabledPlugins());
        table.repaint();
        setStatus ((host.isPluginSwitchedOff (d) ? ko ("사용 안 함: ") : ko ("사용: ")) + d.name, false);
    }

    /** '전부 사용' / '전부 해제': the plugins the table shows - all of them, or the ones a search narrowed it to. */
    void setAll (bool enabled)
    {
        if (locked)
            return;

        for (const auto& d : shown)
            host.setPluginEnabled (d, enabled);

        settings.setDisabledPlugins (host.getDisabledPlugins());
        table.repaint();
        setStatus ((isSearching() ? ko ("검색 결과 ") + juce::String (shown.size()) + ko ("개를") : ko ("플러그인 ") + juce::String (shown.size()) + ko ("개를"))
                       + (enabled ? ko (" \"사용\"으로 켰습니다") : ko (" \"사용 안 함\"으로 껐습니다")), false);
    }

    void setStatus (const juce::String& text, bool error)
    {
        status.setColour (juce::Label::textColourId, error ? Palette::missing : Palette::dimText);
        status.setText (text, juce::dontSendNotification);
    }

    void scan()
    {
        if (locked)
            return;

        auto* format = host.getVST3Format();

        if (format == nullptr)
        {
            setStatus (ko ("이 빌드에는 VST3 지원이 없습니다."), true);
            return;
        }

        if (scanner->isScanning())
        {
            setStatus (ko ("이미 찾는 중입니다 - 스캔 창을 먼저 끝내세요."), true);
            return;
        }

        scanCoordinator->prepareForScan (host.getKnownPlugins(), settings.getDeadMansPedalFile());
        status.setTooltip ({});
        setStatus (ko ("VST3 플러그인을 찾는 중... (폴더를 고르는 창이 뜹니다)"), false);
        scanner->scanFor (*format);
        startTimer (250);
    }

    /** Reset the catalogue and crash records; '사용' switches and cue chains keep their own state. */
    void resetList()
    {
        if (locked)
            return;

        if (scanner->isScanning())
        {
            setStatus (ko ("스캔 중에는 초기화할 수 없습니다 - 스캔 창을 먼저 끝내세요."), true);
            return;
        }

        juce::Component::SafePointer<Content> safe (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle (ko ("플러그인 목록 초기화"))
                                          .withMessage (ko ("찾아 둔 플러그인 목록과 '문제 파일' 기록을 모두 지웁니다. 초기화한 뒤 [VST3 스캔]으로 다시 찾으세요.\n"
                                                            "체인에 넣어 둔 플러그인과 '사용' 체크는 그대로입니다."))
                                          .withButton (ko ("초기화"))
                                          .withButton (ko ("취소")),
                                      [safe] (int result)
        {
            if (safe == nullptr || result != 1 || safe->locked || safe->scanner->isScanning())
                return;

            auto& list = safe->host.getKnownPlugins();
            list.clear();
            list.clearBlacklistedFiles();
            safe->settings.getDeadMansPedalFile().deleteFile();
            safe->status.setTooltip ({});
            safe->setStatus (ko ("플러그인 목록을 비웠습니다 - [VST3 스캔]으로 다시 찾으세요."), false);
        });
    }

    void removeSelected()
    {
        const int row = table.getSelectedRow();

        if (locked || row < 0 || row >= shown.size())
            return;

        const auto name = shown.getReference (row).name;
        host.getKnownPlugins().removeType (shown.getReference (row));   // the list announces the change: the table refreshes
        setStatus (ko ("목록에서 뺐습니다 (다시 스캔하면 돌아옵니다): ") + name, false);
    }

    PluginHost& host;
    AppSettings& settings;
    PluginScanCoordinator* scanCoordinator = nullptr; // owned by host's KnownPluginList
    juce::Array<juce::PluginDescription> all, shown;   // every known effect plugin; the ones the search shows (the table's rows)
    Model model;
    std::unique_ptr<PluginScanListComponent> scanner;

    juce::Label caption, note, count, status;
    juce::TextEditor search;
    juce::TableListBox table;
    juce::TextButton scanButton, enableAllButton, disableAllButton, removeButton, resetListButton;
    bool locked = false;   // show mode
};

//==============================================================================
PluginManagerWindow::PluginManagerWindow (PluginHost& host, AppSettings& settings)
    : DocumentWindow (ko ("플러그인 관리"), Palette::background, DocumentWindow::allButtons)
{
    ShortcutRouter::setWindowScope (*this, ShortcutKeyContext::Window::auxiliary);
    setUsingNativeTitleBar (true);
    auto* c = new Content (host, settings);
    content = c;
    setContentOwned (c, true);
    setResizable (true, false);
    setResizeLimits (640, 420, 10000, 10000);
    centreWithSize (getWidth(), getHeight());   // the owner then centres it on its own display (centreAroundComponent)
    UiScale::fitWindowIntoDisplay (*this);      // at 150% 글씨·화면 크기 the default size may not fit the display
}

void PluginManagerWindow::resized()
{
    DocumentWindow::resized();
    UiScale::fitOnResized (*this, fitting);   // back from maximised after a 글씨·화면 크기 change: no bigger than the screen
}

PluginManagerWindow::~PluginManagerWindow()
{
    clearContentComponent();
}

void PluginManagerWindow::open()
{
    setVisible (true);
    toFront (true);

    if (content != nullptr)
        content->focusSearch();   // typing starts the search at once
}

void PluginManagerWindow::closeButtonPressed()
{
    setVisible (false);   // kept: reopening is instant, and a scan in progress carries on
}

void PluginManagerWindow::setLocked (bool locked)
{
    if (content != nullptr)
        content->setLocked (locked);
}

} // namespace gocue
