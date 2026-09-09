#include "ChainDrawer.h"

#include "PluginSearch.h"

namespace gocue::livemix
{

namespace
{
    /** The search box of 플러그인 검색: the arrow keys move the list under it instead of the caret in the text. */
    class SearchBox : public juce::TextEditor
    {
    public:
        std::function<bool (const juce::KeyPress&)> onKey;

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (onKey && onKey (key))
                return true;

            return juce::TextEditor::keyPressed (key);
        }
    };

    /** 플러그인 검색: the plugins the query matches by name, maker or format - PluginSearch's rule, the one the plugin
        manager searches by. Enter or a double-click puts the picked one at the end of the chain. A window of its own,
        not modal: the mixer keeps running and the drawer stays visible behind it. */
    class PluginPicker : public juce::Component,
                         private juce::ListBoxModel
    {
    public:
        /** onPick / onCancel are set once the window is up: both have to name the window they belong to, and neither
            may be called twice - what they do deletes this component. */
        std::function<void (const juce::PluginDescription&)> onPick;
        std::function<void()> onCancel;

        PluginPicker (juce::Array<juce::PluginDescription> types, juce::String chainName)
            : all (std::move (types)), shown (all)
        {
            target.setFont (bodyFont (13.0f));
            target.setColour (juce::Label::textColourId, Palette::dimText);
            target.setText (chainName, juce::dontSendNotification);
            addAndMakeVisible (target);

            search.setFont (bodyFont (14.5f));
            search.setTextToShowWhenEmpty (ko ("검색: 이름, 제조사, 형식"), Palette::dimText);
            search.onTextChange = [this] { applyFilter(); };
            search.onReturnKey = [this] { addSelected(); };
            search.onEscapeKey = [this] { if (search.getText().isNotEmpty()) search.setText ({}, true); else if (onCancel) onCancel(); };
            search.onKey = [this] (const juce::KeyPress& key) { return moveSelection (key); };
            addAndMakeVisible (search);

            styleCaption (caption, ko ("이름 · 제조사 · 형식(VST2, VST3)으로 찾습니다. 여러 단어를 띄어 써도 되고, ↑↓ 로 고른 뒤 Enter."));
            addAndMakeVisible (caption);

            list.setModel (this);
            list.setRowHeight (28);
            list.setColour (juce::ListBox::backgroundColourId, Palette::card);
            list.setColour (juce::ListBox::outlineColourId, Palette::line);
            list.setOutlineThickness (1);
            addAndMakeVisible (list);

            countLabel.setFont (bodyFont (13.0f));
            countLabel.setColour (juce::Label::textColourId, Palette::dimText);
            addAndMakeVisible (countLabel);

            addButton.setButtonText (ko ("체인에 추가"));
            addButton.setWantsKeyboardFocus (false);
            addButton.setColour (juce::TextButton::buttonColourId, Palette::accent);
            addButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            addButton.onClick = [this] { addSelected(); };
            addAndMakeVisible (addButton);

            cancelButton.setButtonText (ko ("닫기"));
            cancelButton.setWantsKeyboardFocus (false);
            cancelButton.onClick = [this] { if (onCancel) onCancel(); };
            addAndMakeVisible (cancelButton);

            applyFilter();
            setSize (560, 460);
        }

        /** The box takes the keys as soon as the window is up: typing starts the search, no click needed. */
        void grabSearchFocus()
        {
            juce::Component::SafePointer<juce::TextEditor> box (&search);
            juce::MessageManager::callAsync ([box] { if (box != nullptr && box->isShowing()) box->grabKeyboardFocus(); });
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (18, 14);
            target.setBounds (area.removeFromTop (20));
            area.removeFromTop (6);
            search.setBounds (area.removeFromTop (32));
            area.removeFromTop (8);
            caption.setBounds (area.removeFromTop (18));
            area.removeFromTop (8);
            auto buttons = area.removeFromBottom (34);
            cancelButton.setBounds (buttons.removeFromRight (90));
            buttons.removeFromRight (8);
            addButton.setBounds (buttons.removeFromRight (120));
            countLabel.setBounds (buttons);
            area.removeFromBottom (10);
            list.setBounds (area);
        }

        void paint (juce::Graphics& g) override { g.fillAll (Palette::card); }

    private:
        int getNumRows() override { return shown.size(); }

        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
        {
            if (! juce::isPositiveAndBelow (row, shown.size()))
                return;

            g.fillAll (selected ? Palette::accent.withAlpha (0.35f) : (row % 2 == 0 ? Palette::card : Palette::card2.withAlpha (0.5f)));
            g.setColour (Palette::text);
            g.setFont (bodyFont (13.5f));
            const auto& d = shown.getReference (row);
            g.drawText (d.name + "   " + d.manufacturerName + "  (" + pluginFormatLabel (d.pluginFormatName) + ")",
                        8, 0, width - 16, height, juce::Justification::centredLeft, true);
        }

        void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
        {
            list.selectRow (row);
            addSelected();
        }

        void returnKeyPressed (int) override { addSelected(); }

        /** The top hit is selected after every query, so Enter always adds something without touching the mouse. */
        void applyFilter()
        {
            shown = filterPlugins (all, search.getText());
            list.updateContent();
            list.deselectAllRows();

            if (! shown.isEmpty())
                list.selectRow (0);

            countLabel.setText (shown.isEmpty() ? ko ("찾은 플러그인이 없습니다")
                                                : juce::String (shown.size()) + ko ("개")
                                                      + (shown.size() == all.size() ? juce::String()
                                                                                    : " / " + juce::String (all.size()) + ko ("개")),
                                juce::dontSendNotification);
            addButton.setEnabled (! shown.isEmpty());
            list.repaint();
        }

        bool moveSelection (const juce::KeyPress& key)
        {
            const bool down = key == juce::KeyPress (juce::KeyPress::downKey);
            const bool up = key == juce::KeyPress (juce::KeyPress::upKey);

            if ((! down && ! up) || shown.isEmpty())
                return false;

            list.selectRow (juce::jlimit (0, shown.size() - 1, list.getSelectedRow() + (down ? 1 : -1)));
            return true;
        }

        void addSelected()
        {
            const int row = list.getSelectedRow();

            // one pick per window: Enter twice while the plugin loads must not send a second one
            if (submitted || ! juce::isPositiveAndBelow (row, shown.size()) || ! onPick)
                return;

            submitted = true;
            onPick (shown.getReference (row));   // the drawer copies it, closes this window, and only then loads it
        }

        juce::Array<juce::PluginDescription> all, shown;
        bool submitted = false;
        juce::Label target, caption, countLabel;
        SearchBox search;
        juce::ListBox list;
        juce::TextButton addButton, cancelButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginPicker)
    };

    /** The picker's window. Closing it - the title bar, Esc - tells the drawer, which deletes it. */
    class PickerWindow : public juce::DialogWindow
    {
    public:
        std::function<void()> onCloseRequested;   // set once the window exists (it closes itself by name)

        explicit PickerWindow (const juce::String& title)
            : DialogWindow (title, Palette::card, true, true) {}

        void closeButtonPressed() override
        {
            if (onCloseRequested)
                onCloseRequested();
        }

        /** Esc closes it for good (DialogWindow's own Esc would only hide it, leaving it to reopen invisible). */
        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress (juce::KeyPress::escapeKey))
            {
                closeButtonPressed();
                return true;
            }

            return DialogWindow::keyPressed (key);
        }
    };
}

/** One plugin in the list. Dragging its number reorders the chain (the row follows the mouse). */
struct ChainDrawer::Row : public juce::Component
{
    Row (ChainDrawer& o, int idx) : owner (o), index (idx)
    {
        // the number is the handle now: it says where the plugin sits in the chain and it is what gets dragged
        number.setFont (juce::Font (juce::FontOptions (pt (12.0f), juce::Font::bold)));
        number.setJustificationType (juce::Justification::centred);
        number.setColour (juce::Label::backgroundColourId, Palette::accent);
        number.setColour (juce::Label::textColourId, juce::Colours::white);
        number.setInterceptsMouseClicks (false, false);   // the row takes the mouse (getMouseCursor below answers for it)
        addAndMakeVisible (number);

        name.setFont (juce::Font (juce::FontOptions (pt (14.5f), juce::Font::bold)));
        name.setMinimumHorizontalScale (1.0f);
        name.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (name);

        power.setClickingTogglesState (true);
        power.setWantsKeyboardFocus (false);
        power.setTooltip (ko ("켜짐 / 바이패스 (바이패스면 소리가 그대로 통과)"));
        power.onClick = [this] { owner.toggleBypass (index); };
        addAndMakeVisible (power);

        open.setButtonText (ko ("열기"));
        open.setWantsKeyboardFocus (false);
        open.onClick = [this] { owner.openEditor (index); };
        addAndMakeVisible (open);

        remove.setButtonText (juce::String::fromUTF8 ("\xE2\x9C\x95"));
        remove.setWantsKeyboardFocus (false);
        remove.setTooltip (ko ("체인에서 빼기"));
        remove.onClick = [this] { owner.removeSlot (index); };
        addAndMakeVisible (remove);
    }

    void set (const PluginChain::Slot& slot)
    {
        number.setText (juce::String (index + 1), juce::dontSendNotification);
        const bool missing = slot.plugin == nullptr;
        name.setText (missing ? slot.state.name + ko (" (없음)") : slot.plugin->getName(), juce::dontSendNotification);
        name.setTooltip (name.getText());   // a narrow drawer cuts the name short
        bypassed = slot.bypassed.load() || missing;
        name.setColour (juce::Label::textColourId, bypassed ? Palette::dimText : Palette::text);
        power.setToggleState (! bypassed, juce::dontSendNotification);
        power.setEnabled (! missing);
        open.setEnabled (! missing);
        number.setColour (juce::Label::backgroundColourId, bypassed ? Palette::lampOff : Palette::accent);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (dragging ? Palette::card2.brighter (0.1f) : Palette::card2);
        g.fillRoundedRectangle (bounds, 12.0f);
        g.setColour (dragging ? Palette::accent : Palette::line);
        g.drawRoundedRectangle (bounds, 12.0f, dragging ? 2.0f : 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8);
        number.setBounds (r.removeFromLeft (26).reduced (0, 4));
        r.removeFromLeft (10);
        remove.setBounds (r.removeFromRight (30).reduced (0, 3));
        r.removeFromRight (6);
        open.setBounds (r.removeFromRight (56).reduced (0, 3));
        r.removeFromRight (8);
        power.setBounds (r.removeFromRight (44).reduced (0, 4));
        r.removeFromRight (8);
        name.setBounds (r);
    }

    /** The row takes every mouse event, so it is the row that says the handle can be dragged. */
    juce::MouseCursor getMouseCursor() override
    {
        return getMouseXYRelative().x < handleWidth ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.x < handleWidth)   // the number and the gap after it: the handle
        {
            dragging = true;
            owner.dragFrom = index;
            owner.dragTarget = index;
            toFront (false);
            repaint();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging)
            return;

        // the row rides with the mouse; the target index comes from the centre's position among the others
        const auto inHolder = e.getEventRelativeTo (getParentComponent());
        setTopLeftPosition (getX(), juce::jlimit (0, juce::jmax (0, getParentHeight() - getHeight()), inHolder.y - getHeight() / 2));
        const int centre = getY() + getHeight() / 2;
        int target = 0;

        for (auto& row : owner.rows)
            if (row.get() != this && row->getY() + row->getHeight() / 2 < centre)
                ++target;

        owner.dragTarget = target;
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (! dragging)
            return;

        dragging = false;
        const int from = owner.dragFrom, to = owner.dragTarget;
        owner.dragFrom = owner.dragTarget = -1;

        if (from >= 0 && to >= 0 && from != to)
            owner.moveSlot (from, to);
        else
            owner.layoutRows();

        repaint();
    }

    static constexpr int handleWidth = 46;   // the number (10..36) and the gap after it

    ChainDrawer& owner;
    int index;
    bool bypassed = false, dragging = false;
    juce::Label number, name;
    juce::ToggleButton power;
    juce::TextButton open, remove;
};

//==============================================================================
ChainDrawer::~ChainDrawer()
{
    closePluginSearch();   // the search window does not outlive the drawer that feeds it
}

ChainDrawer::ChainDrawer (MixDocument& doc, PluginWindowManager& w) : document (doc), windows (w)
{
    title.setFont (titleFont (17.0f));
    title.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (title);
    closeButton.setWantsKeyboardFocus (false);
    closeButton.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeButton);
    styleCaption (note, ko ("번호를 잡고 위아래로 끌면 순서가 바뀝니다. 소리는 1번부터 차례로 통과합니다."));
    note.setFont (bodyFont (12.5f));
    addAndMakeVisible (note);

    viewport.setViewedComponent (&rowsHolder, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    addButton.setButtonText (ko ("+ 플러그인 추가"));
    addButton.setWantsKeyboardFocus (false);
    addButton.onClick = [this] { showAddMenu (&addButton); };
    addAndMakeVisible (addButton);

    styleCaption (legend, ko ("스위치 켜짐 = 동작 · 꺼짐 = 바이패스 (소리는 그대로 통과)"));
    legend.setFont (bodyFont (12.0f));
    addAndMakeVisible (legend);
}

void ChainDrawer::setChain (PluginChain* newChain, const juce::String& newTitle)
{
    ++revision;   // every rebind: deferred work posted for the previous binding is dropped, whatever the pointer
    closePluginSearch();   // the search names the chain it adds to: another chain (or none) means another search
    chain = newChain;
    ownerTitle = newTitle;
    title.setText (newTitle + " · " + ko ("VST3 체인"), juce::dontSendNotification);
    refresh();
}

void ChainDrawer::refresh()
{
    rows.clear();

    if (chain != nullptr)
    {
        for (int i = 0; i < chain->getNumSlots(); ++i)
        {
            auto row = std::make_unique<Row> (*this, i);
            row->set (chain->getSlot (i));
            rowsHolder.addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }
    }

    layoutRows();
}

void ChainDrawer::layoutRows()
{
    const int rowH = 52, gap = 8;
    const int width = juce::jmax (100, viewport.getMaximumVisibleWidth());
    rowsHolder.setSize (width, juce::jmax (1, (int) rows.size() * (rowH + gap)));

    for (size_t i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (0, (int) i * (rowH + gap), width, rowH);
}

void ChainDrawer::showAddMenu (juce::Component* anchor)
{
    showAddMenu (anchor != nullptr ? anchor->getScreenBounds() : getScreenBounds());
}

void ChainDrawer::showAddMenu (juce::Rectangle<int> screenArea)
{
    if (chain == nullptr)
        return;

    const auto types = document.getEngine().getPluginHost().getEffectTypes();
    juce::PopupMenu menu;

    if (types.isEmpty())
        menu.addItem (1, ko ("쓸 수 있는 플러그인이 없습니다 - 플러그인 관리에서 스캔하거나 '사용'을 켜세요..."));
    else
    {
        menu.addItem (3, ko ("검색해서 추가..."));   // many makers, many plugins: typing beats walking the submenus
        menu.addSeparator();
        juce::KnownPluginList::addToMenu (menu, types, juce::KnownPluginList::sortByManufacturer);   // one submenu per maker
        menu.addSeparator();
        menu.addItem (1, ko ("플러그인 관리 (스캔, 사용 여부, 프리셋)..."));
    }

    // the presets of this PC: one goes into the chain whole; the chain as it is can become one
    const auto presets = PluginPreset::listFolder (PluginPreset::defaultFolder());
    juce::PopupMenu presetMenu;

    for (size_t i = 0; i < presets.size(); ++i)
        presetMenu.addItem (1000 + (int) i, presets[i].name + "   (" + presets[i].summary() + ")");

    menu.addSeparator();
    menu.addSubMenu (ko ("프리셋 불러오기"), presetMenu, ! presets.empty());
    menu.addItem (2, ko ("이 체인을 프리셋으로 저장..."), chain->getNumSlots() > 0);

    juce::Component::SafePointer<ChainDrawer> safeThis (this);
    const int forRevision = revision;
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (screenArea), [safeThis, types, presets, forRevision] (int result)
    {
        if (safeThis == nullptr || result == 0 || safeThis->revision != forRevision)
            return;

        if (result == 1)
        {
            if (safeThis->onOpenPluginManager)
                safeThis->onOpenPluginManager();

            return;
        }

        if (result == 2)
        {
            safeThis->saveChainAsPreset();
            return;
        }

        if (result == 3)
        {
            safeThis->showPluginSearch();
            return;
        }

        if (result >= 1000 && result - 1000 < (int) presets.size())
        {
            safeThis->loadPreset (presets[(size_t) (result - 1000)]);
            return;
        }

        const int index = juce::KnownPluginList::getIndexChosenByMenu (types, result);

        if (index >= 0)
            safeThis->addPlugin (types[index]);
    });
}

void ChainDrawer::showPluginSearch()
{
    if (pickerWindow != nullptr)   // already open: to the front rather than a second one
    {
        pickerWindow->setVisible (true);
        pickerWindow->toFront (true);
        return;
    }

    if (chain == nullptr)
        return;

    const auto types = document.getEngine().getPluginHost().getEffectTypes();

    if (types.isEmpty())
    {
        if (onStatus)
            onStatus (ko ("쓸 수 있는 플러그인이 없습니다 - 플러그인 관리에서 스캔하거나 '사용'을 켜세요."), true);

        return;
    }

    auto* picker = new PluginPicker (types, ownerTitle + " · " + ko ("VST3 체인"));

    // a window of its own, not a modal dialog: the mixer (and every plugin editor) stays usable meanwhile
    auto* window = new PickerWindow (ko ("플러그인 검색"));
    window->setUsingNativeTitleBar (true);
    window->setContentOwned (picker, true);
    window->setResizable (true, false);
    window->centreAroundComponent (this, window->getWidth(), window->getHeight());

    juce::Component::SafePointer<ChainDrawer> safeThis (this);
    juce::Component::SafePointer<juce::Component> safeWindow (window);
    const int forRevision = revision;

    // never from inside the window's own callback, and never a window other than this one (a deferred close must not
    // take away a picker opened since)
    auto closeThisWindow = [safeThis, safeWindow]
    {
        juce::MessageManager::callAsync ([safeThis, safeWindow]
        {
            // the window may be gone already (another chain came in): a null one must not stand for "whichever is open"
            if (safeThis != nullptr && safeWindow != nullptr)
                safeThis->closePluginSearch (safeWindow.getComponent());
        });
    };

    picker->onCancel = closeThisWindow;
    window->onCloseRequested = closeThisWindow;
    picker->onPick = [safeThis, safeWindow, forRevision] (const juce::PluginDescription& description)
    {
        // by value: loading a plugin (and opening its editor) can pump native messages, so nothing of the picker's
        // may still be in use by then. The window goes first, the plugin comes after.
        const auto picked = description;
        juce::MessageManager::callAsync ([safeThis, safeWindow, forRevision, picked]
        {
            if (safeThis == nullptr)
                return;

            if (safeWindow != nullptr)
                safeThis->closePluginSearch (safeWindow.getComponent());

            if (safeThis->revision == forRevision)   // the drawer took another chain while the window was open
                safeThis->addPickedPlugin (picked);
        });
    };

    window->setVisible (true);
    window->toFront (true);
    picker->grabSearchFocus();
    pickerWindow = window;
}

void ChainDrawer::addPickedPlugin (const juce::PluginDescription& description)
{
    // the plugin manager is not modal: '사용' may have been switched off (or a scan may have replaced the entry)
    // while the search window stood open, so what goes in is the list's plugin as it is now, not the copy
    const auto key = PluginHost::keyFor (description);

    for (const auto& d : document.getEngine().getPluginHost().getEffectTypes())
        if (PluginHost::keyFor (d) == key)
        {
            addPlugin (d);
            return;
        }

    if (onStatus)
        onStatus (description.name + ko ("은(는) 지금 쓸 수 없습니다 (플러그인 관리에서 '사용'이 꺼졌거나 목록이 바뀌었습니다)"), true);
}

void ChainDrawer::closePluginSearch (juce::Component* only)
{
    auto* window = pickerWindow.getComponent();

    if (window != nullptr && (only == nullptr || only == window))
        delete window;
}

void ChainDrawer::loadPreset (const PluginPreset& preset)
{
    if (chain == nullptr)
        return;

    if (chain->getNumSlots() == 0)
    {
        applyPreset (preset, true);
        return;
    }

    juce::Component::SafePointer<ChainDrawer> safeThis (this);
    const int forRevision = revision;
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle (ko ("프리셋 불러오기"))
                                      .withMessage (ko ("'") + preset.name + ko ("' 프리셋을 어떻게 넣을까요?\n\n바꾸기: 지금 체인을 지우고 프리셋으로\n뒤에 추가: 지금 체인 뒤에 이어서"))
                                      .withButton (ko ("바꾸기"))
                                      .withButton (ko ("뒤에 추가"))
                                      .withButton (ko ("취소")),
                                  [safeThis, preset, forRevision] (int result)
    {
        if (safeThis == nullptr || safeThis->revision != forRevision)
            return;

        if (result == 1)
            safeThis->applyPreset (preset, true);
        else if (result == 2)
            safeThis->applyPreset (preset, false);
    });
}

void ChainDrawer::applyPreset (const PluginPreset& preset, bool replace)
{
    if (chain == nullptr)
        return;

    auto& engine = document.getEngine();
    auto& host = engine.getPluginHost();
    juce::StringArray errors;

    if (replace)
    {
        errors = chain->restore (preset.plugins, host.makeFactory (engine.getSampleRate(), engine.getBlockSize()));
    }
    else
    {
        for (const auto& state : preset.plugins)
        {
            if (chain->getNumSlots() >= MixSession::maxChainSlots)
            {
                errors.add (state.name + ": " + ko ("체인이 가득 찼습니다 (") + juce::String (MixSession::maxChainSlots) + ko ("개)"));
                continue;
            }

            juce::String error;
            auto instance = host.createInstance (state, engine.getSampleRate(), engine.getBlockSize(), error);

            if (instance == nullptr)
            {
                chain->addMissingSlot (state);   // the slot stays, empty, like a missing plugin in a session
                errors.add (state.name + ": " + (error.isNotEmpty() ? error : ko ("이 PC에 없는 플러그인입니다 (자리는 비워 둡니다)")));
                continue;
            }

            chain->addPlugin (std::move (instance), state);
        }
    }

    refresh();
    document.markDirty();

    if (onChainEdited)
        onChainEdited();

    if (onStatus)
        onStatus (ko ("프리셋 넣음: ") + preset.name + " (" + preset.summary() + ")", false);

    if (! errors.isEmpty())
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle (ko ("프리셋의 일부를 넣지 못했습니다"))
                                          .withMessage (errors.joinIntoString ("\n"))
                                          .withButton (ko ("확인")),
                                      [] (int) {});
}

void ChainDrawer::saveChainAsPreset()
{
    if (chain == nullptr || chain->getNumSlots() == 0)
        return;

    auto* alert = new juce::AlertWindow (ko ("체인을 프리셋으로 저장"), ko ("이 체인의 플러그인과 지금 설정이 프리셋이 됩니다. 이름:"), juce::MessageBoxIconType::NoIcon);
    alert->addTextEditor ("name", ownerTitle, ko ("이름"));
    alert->addButton (ko ("저장"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<ChainDrawer> safeThis (this);
    const int forRevision = revision;
    alert->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, alert, forRevision] (int r)
    {
        if (safeThis == nullptr || r != 1 || safeThis->revision != forRevision || safeThis->chain == nullptr)
            return;

        const auto name = alert->getTextEditorContents ("name").trim();

        if (name.isEmpty())
            return;

        PluginPreset preset;
        preset.name = name;
        bool complete = true;
        preset.plugins = safeThis->chain->getStates (&complete);

        if (! complete)
        {
            // a preset with a plugin's stale or empty settings would load later as if they were meant: not saved
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("프리셋을 저장하지 않았습니다"),
                                                    ko ("일부 플러그인의 설정을 읽지 못했습니다. 그 플러그인의 창을 닫거나 세션을 다시 연 뒤 다시 저장하세요."), ko ("확인"));
            return;
        }

        for (auto& p : preset.plugins)
            p.bypassed = false;   // a preset carries the plugins and their settings, not a momentary bypass

        const auto folder = PluginPreset::defaultFolder();
        auto file = PluginPreset::fileFor (name, folder);

        if (file.existsAsFile())
        {
            file = file.getNonexistentSibling (true);   // "이름(2)": the preset already there is not touched
            preset.name = file.getFileNameWithoutExtension();
        }

        if (const auto result = preset.save (file); result.failed())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("프리셋을 저장하지 못했습니다"), result.getErrorMessage(), ko ("확인"));
            return;
        }

        if (safeThis->onStatus)
            safeThis->onStatus (ko ("프리셋 저장: ") + preset.name + (complete ? juce::String() : ko (" (일부 플러그인의 설정은 읽지 못했습니다)")), ! complete);

        if (safeThis->onPresetSaved)
            safeThis->onPresetSaved();
    }), true);
    focusAlertTextEditor (*alert, "name");
}

void ChainDrawer::addPlugin (const juce::PluginDescription& description)
{
    if (chain == nullptr)
        return;

    if (chain->getNumSlots() >= MixSession::maxChainSlots)
    {
        // the session keeps 16 per chain: a seventeenth would be dropped on save
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::InfoIcon)
                                          .withTitle (ko ("체인이 가득 찼습니다"))
                                          .withMessage (ko ("한 체인에는 플러그인을 ") + juce::String (MixSession::maxChainSlots) + ko ("개까지 넣을 수 있습니다."))
                                          .withButton (ko ("확인")),
                                      [] (int) {});
        return;
    }

    auto& engine = document.getEngine();
    juce::String error;
    auto instance = engine.getPluginHost().createInstance (description, engine.getSampleRate(), engine.getBlockSize(), error);

    if (instance == nullptr)
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle (ko ("플러그인을 불러오지 못했습니다"))
                                          .withMessage (description.name + "\n" + error)
                                          .withButton (ko ("확인")),
                                      [] (int) {});
        return;
    }

    chain->addPlugin (std::move (instance));
    refresh();
    document.markDirty();

    if (onChainEdited)
        onChainEdited();

    openEditor (chain->getNumSlots() - 1);
}

void ChainDrawer::openEditor (int index)
{
    if (chain == nullptr || index < 0 || index >= chain->getNumSlots())
        return;

    auto& slot = chain->getSlot (index);

    if (slot.plugin != nullptr)
        windows.open (*slot.plugin, ownerTitle + " - " + slot.plugin->getName());
}

void ChainDrawer::removeSlot (int index)
{
    // deferred: the click comes from a button inside the row about to go
    juce::Component::SafePointer<ChainDrawer> safeThis (this);
    const int forRevision = revision;
    juce::MessageManager::callAsync ([safeThis, index, forRevision]
    {
        if (safeThis == nullptr || safeThis->chain == nullptr || safeThis->revision != forRevision || index >= safeThis->chain->getNumSlots())
            return;

        safeThis->chain->removePlugin (index);
        safeThis->refresh();
        safeThis->document.markDirty();

        if (safeThis->onChainEdited)
            safeThis->onChainEdited();
    });
}

void ChainDrawer::moveSlot (int from, int to)
{
    // deferred: the drag ends inside a row that the refresh would destroy under it (the chain's listener refreshes the views)
    juce::Component::SafePointer<ChainDrawer> safeThis (this);
    const int forRevision = revision;
    juce::MessageManager::callAsync ([safeThis, from, to, forRevision]
    {
        if (safeThis == nullptr || safeThis->chain == nullptr || safeThis->revision != forRevision)
            return;

        if (safeThis->chain->movePlugin (from, to))
        {
            safeThis->document.markDirty();

            if (safeThis->onChainEdited)
                safeThis->onChainEdited();
        }

        safeThis->refresh();
    });
}

void ChainDrawer::toggleBypass (int index)
{
    // deferred for the same reason: the switch that was clicked sits in a row the refresh replaces
    juce::Component::SafePointer<ChainDrawer> safeThis (this);
    const int forRevision = revision;
    juce::MessageManager::callAsync ([safeThis, index, forRevision]
    {
        if (safeThis == nullptr || safeThis->chain == nullptr || safeThis->revision != forRevision || index < 0 || index >= safeThis->chain->getNumSlots())
            return;

        safeThis->chain->setBypassed (index, ! safeThis->chain->getSlot (index).bypassed.load());
        safeThis->document.markDirty();

        if (safeThis->onChainEdited)
            safeThis->onChainEdited();

        safeThis->refresh();
    });
}

void ChainDrawer::resized()
{
    auto area = getLocalBounds().reduced (18, 16);
    auto head = area.removeFromTop (34);
    closeButton.setBounds (head.removeFromRight (36));   // a 34 px target
    head.removeFromRight (8);
    title.setBounds (head);
    area.removeFromTop (8);
    note.setBounds (area.removeFromTop (36));
    area.removeFromTop (8);
    legend.setBounds (area.removeFromBottom (20));
    area.removeFromBottom (8);
    addButton.setBounds (area.removeFromBottom (40));
    area.removeFromBottom (10);
    viewport.setBounds (area);
    layoutRows();
}

void ChainDrawer::paint (juce::Graphics& g)
{
    g.fillAll (Palette::bar);
    g.setColour (Palette::line);
    g.fillRect (getLocalBounds().removeFromLeft (1));
}

} // namespace gocue::livemix
