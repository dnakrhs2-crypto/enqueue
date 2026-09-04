#include "ChainDrawer.h"

namespace gocue::livemix
{

/** One plugin in the list. The drag handle reorders (mouse drag on the ≡, the row follows the mouse). */
struct ChainDrawer::Row : public juce::Component
{
    Row (ChainDrawer& o, int idx) : owner (o), index (idx)
    {
        grip.setText (juce::String::fromUTF8 ("\xE2\x89\xA1"), juce::dontSendNotification);   // ≡
        grip.setFont (juce::Font (juce::FontOptions (pt (18.0f))));
        grip.setColour (juce::Label::textColourId, Palette::dimText);
        grip.setJustificationType (juce::Justification::centred);
        grip.setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        grip.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (grip);

        number.setFont (juce::Font (juce::FontOptions (pt (12.0f), juce::Font::bold)));
        number.setJustificationType (juce::Justification::centred);
        number.setColour (juce::Label::backgroundColourId, Palette::accent);
        number.setColour (juce::Label::textColourId, juce::Colours::white);
        number.setInterceptsMouseClicks (false, false);
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
        grip.setBounds (r.removeFromLeft (22));
        r.removeFromLeft (6);
        number.setBounds (r.removeFromLeft (24).reduced (0, 4));
        r.removeFromLeft (10);
        remove.setBounds (r.removeFromRight (30).reduced (0, 3));
        r.removeFromRight (6);
        open.setBounds (r.removeFromRight (56).reduced (0, 3));
        r.removeFromRight (8);
        power.setBounds (r.removeFromRight (44).reduced (0, 4));
        r.removeFromRight (8);
        name.setBounds (r);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.x < 40)   // the handle
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

    ChainDrawer& owner;
    int index;
    bool bypassed = false, dragging = false;
    juce::Label grip, number, name;
    juce::ToggleButton power;
    juce::TextButton open, remove;
};

//==============================================================================
ChainDrawer::~ChainDrawer() = default;

ChainDrawer::ChainDrawer (MixDocument& doc, PluginWindowManager& w) : document (doc), windows (w)
{
    title.setFont (titleFont (17.0f));
    title.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (title);
    closeButton.setWantsKeyboardFocus (false);
    closeButton.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeButton);
    styleCaption (note, ko ("≡ 를 잡고 위아래로 끌면 순서가 바뀝니다. 소리는 1번부터 차례로 통과합니다."));
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
    if (chain == nullptr)
        return;

    const auto types = document.getEngine().getPluginHost().getEffectTypes();
    juce::PopupMenu menu;

    if (types.isEmpty())
        menu.addItem (1, ko ("스캔된 VST3 플러그인이 없습니다 - 플러그인 관리에서 스캔..."));
    else
    {
        juce::KnownPluginList::addToMenu (menu, types, juce::KnownPluginList::sortByManufacturer);   // one submenu per maker
        menu.addSeparator();
        menu.addItem (1, ko ("플러그인 관리 (스캔)..."));
    }

    juce::Component::SafePointer<ChainDrawer> safeThis (this);
    const int forRevision = revision;
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (anchor), [safeThis, types, forRevision] (int result)
    {
        if (safeThis == nullptr || result == 0 || safeThis->revision != forRevision)
            return;

        if (result == 1)
        {
            if (safeThis->onOpenPluginManager)
                safeThis->onOpenPluginManager();

            return;
        }

        const int index = juce::KnownPluginList::getIndexChosenByMenu (types, result);

        if (index >= 0)
            safeThis->addPlugin (types[index]);
    });
}

void ChainDrawer::addPlugin (const juce::PluginDescription& description)
{
    if (chain == nullptr)
        return;

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
    closeButton.setBounds (head.removeFromRight (34).reduced (0, 3));
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
