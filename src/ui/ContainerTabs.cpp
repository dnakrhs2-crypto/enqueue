#include "ui/ContainerTabs.h"
#include "ui/FooterBar.h"

#include "ui/CueMenuIcons.h"
#include "ui/UiUtils.h"

namespace gocue
{

ContainerTabs::ContainerTabs (ProjectDocument& d) : document (d)
{
    setWantsKeyboardFocus (false);
    refresh();
}

void ContainerTabs::setEditable (bool shouldBeEditable)
{
    editable = shouldBeEditable;

    if (! editable)
        endDrag();   // show mode: tabs do not move

    repaint();
}

void ContainerTabs::setInfoText (juce::String text)
{
    if (infoText == text)
        return;
    infoText = std::move (text);
    resized();
    repaint();
}

void ContainerTabs::setStatusBar (FooterBar& status)
{
    statusBar = &status;
    addAndMakeVisible (status);
    resized();
}

void ContainerTabs::refresh()
{
    tabs.clear();

    for (int i = 0; i < document.getNumContainers(); ++i)
    {
        const auto info = document.getContainerInfo (i);
        Tab t;
        t.id = info.id;
        t.name = info.name;
        t.isCart = info.isCart;
        t.active = i == document.getActiveContainer();
        tabs.push_back (std::move (t));
    }

    resized();
    repaint();
}

void ContainerTabs::resized()
{
    const auto font = Palette::font (Palette::tabSize, true);
    const int edge = juce::jmax (0, getWidth() - 14);
    // the cue count and the broken-cue warnings button always keep their room at the right
    const int essential = statusBar != nullptr ? statusBar->getEssentialWidth() : 0;
    const int stripRight = juce::jmax (8, edge - (essential > 0 ? essential + 16 : 0));
    const int room = juce::jmax (0, stripRight - 8 - 26 - 8);

    // Reserve the add button first. The active name keeps its measured width while every other tab can still have
    // 60; the others share what is left above 60 in proportion to what their names ask for, so together they never
    // run past the room. No list leaves the strip until even 60 each cannot fit (then it scrolls).
    std::vector<int> widths;
    int total = 0, activeTab = -1;
    for (size_t i = 0; i < tabs.size(); ++i)
    {
        widths.push_back (juce::jlimit (60, 220, juce::GlyphArrangement::getStringWidthInt (font, tabs[i].name) + 44));
        total += widths.back() + 2;
        if (tabs[i].active) activeTab = (int) i;
    }
    if (total > room)
    {
        const int count = (int) tabs.size();
        if (activeTab >= 0)
            widths[(size_t) activeTab] = juce::jlimit (60, widths[(size_t) activeTab], room - (count - 1) * (60 + 2) - 2);
        const int spare = juce::jmax (0, room - count * (60 + 2) - (activeTab >= 0 ? widths[(size_t) activeTab] - 60 : 0));
        int asked = 0;
        for (size_t i = 0; i < tabs.size(); ++i)
            if ((int) i != activeTab)
                asked += widths[i] - 60;
        if (asked > spare)
            for (size_t i = 0; i < tabs.size(); ++i)
                if ((int) i != activeTab)
                    widths[i] = 60 + (int) ((juce::int64) (widths[i] - 60) * spare / asked);
    }
    int x = 8;
    for (size_t i = 0; i < tabs.size(); ++i)
    {
        tabs[i].bounds = { x, 6, widths[i], getHeight() - 6 };
        x += widths[i] + 2;
    }

    tabsRight = 8 + juce::jmin (room, x - 8);
    addButton = { tabsRight + 8, 6, 26, getHeight() - 6 };

    // when the tabs do not fit, the strip scrolls (wheel); a newly active list is brought into view, and an
    // active list that was in view stays in view when the strip narrows (unless the wheel had moved away from it)
    maxTabsScroll = juce::jmax (0, x - 2 - tabsRight);
    const auto revealingScroll = [&] (int scroll)
    {
        const auto& active = tabs[(size_t) activeTab].bounds;
        if (active.getRight() - scroll > tabsRight) scroll = active.getRight() - tabsRight;
        if (active.getX() - scroll < 8) scroll = active.getX() - 8;
        return juce::jlimit (0, maxTabsScroll, scroll);
    };
    if (activeTab >= 0 && (tabs[(size_t) activeTab].id != revealedId || followActive))
    {
        tabsScroll = revealingScroll (tabsScroll);
        revealedId = tabs[(size_t) activeTab].id;
    }
    tabsScroll = juce::jlimit (0, maxTabsScroll, tabsScroll);
    followActive = activeTab < 0 || revealingScroll (tabsScroll) == tabsScroll;
    for (auto& t : tabs)
        t.bounds.translate (-tabsScroll, 0);

    // the status (its essential part whole) and the selection info at the right end share what the tabs leave;
    // the selection info and then the status's mode hint give way first
    auto right = juce::Rectangle<int> (0, 6, edge, juce::jmax (0, getHeight() - 6));
    right.setLeft (juce::jlimit (0, edge, juce::jmin (addButton.getRight() + 16, edge - essential)));
    const int infoWidth = juce::GlyphArrangement::getStringWidthInt (Palette::font (Palette::headerSize), infoText);
    int statusWidth = statusBar == nullptr ? 0
        : juce::jlimit (juce::jmin (essential, right.getWidth()), juce::jmax (juce::jmin (essential, right.getWidth()), statusBar->getPreferredWidth()),
                        right.getWidth() - 16 - infoWidth);
    const int shownInfoWidth = juce::jlimit (0, infoWidth, right.getWidth() - statusWidth - (statusWidth > 0 ? 16 : 0));
    const auto firstInfo = infoText.upToFirstOccurrenceOf (ko (" · "), false, false);
    const int firstInfoWidth = juce::GlyphArrangement::getStringWidthInt (Palette::font (Palette::headerSize), firstInfo);
    infoBounds = {};
    infoShown = {};
    if (shownInfoWidth >= infoWidth && infoWidth > 0)
    {
        infoBounds = right.removeFromRight (infoWidth);
        infoShown = infoText;
    }
    else if (shownInfoWidth >= firstInfoWidth && firstInfoWidth > 0)
    {
        // the first item whole ('선택 2') rather than a line the ellipsis cuts down to '선택 …'
        infoBounds = right.removeFromRight (shownInfoWidth);
        infoShown = firstInfo;
    }
    else if (statusBar != nullptr)
        statusWidth = juce::jmin (right.getWidth(), statusBar->getPreferredWidth());
    if (statusBar != nullptr)
    {
        right.removeFromRight (juce::jmin (statusWidth > 0 && infoBounds.getWidth() > 0 ? 16 : 0, right.getWidth()));
        statusBar->setBounds (right.removeFromRight (juce::jmin (statusWidth, right.getWidth())));
    }
    repaint();   // the tabs are painted from these bounds: any relayout (a status change too) must redraw them
}

void ContainerTabs::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel2);
    g.setColour (Palette::outline);
    g.drawLine (0.0f, (float) getHeight() - 0.5f, (float) getWidth(), (float) getHeight() - 0.5f);

    for (const auto& t : tabs)
    {
        juce::Graphics::ScopedSaveState clip (g);
        // the tab strip shows x = 8 .. tabsRight: 8 is where a revealed tab lands, so what is drawn is what is hit
        g.reduceClipRegion (t.bounds.getIntersection ({ 8, 0, juce::jmax (0, tabsRight - 8), getHeight() }));
        auto r = t.bounds;
        Palette::drawTab (g, r, t.active);
        // the tab being dragged stays where it is, dimmed; the marker shows where it will go
        const float alpha = dragging && t.id == draggedId ? Palette::disabledAlpha : 1.0f;
        int textX = r.getX() + 12;
        g.setColour ((t.active ? Palette::text : Palette::muted).withMultipliedAlpha (alpha));
        const float gx = (float) textX, gy = (float) r.getCentreY() - 5.0f;
        juce::Path icon;

        if (t.isCart)
        {
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j)
                    icon.addRoundedRectangle (gx + (float) i * 6.0f, gy + (float) j * 6.0f, 4.0f, 4.0f, 0.5f);
        }
        else
            for (int i = 0; i < 3; ++i)
                icon.addRectangle (gx, gy + 1.0f + (float) i * 3.0f, 9.0f, 1.3f);
        g.fillPath (icon);
        textX += 16;

        g.setColour ((t.active ? Palette::text : Palette::dimText).withMultipliedAlpha (alpha));
        g.setFont (Palette::font (Palette::tabSize, t.active));
        g.drawText (t.name, textX, r.getY(), r.getRight() - textX - 6, r.getHeight(), juce::Justification::centredLeft, true);
    }

    if (const auto marker = getDropMarkerBounds(); ! marker.isEmpty())
    {
        // the cue table's row-drag bar, standing up in the gap between two tabs (5 = the gap before the first tab)
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion ({ 5, 0, juce::jmax (0, tabsRight + 3 - 5), getHeight() });
        g.setColour (Palette::standby);
        g.fillRect (marker);
    }

    g.setColour (Palette::accent.withMultipliedAlpha (editable ? 1.0f : Palette::disabledAlpha));
    g.setFont (Palette::font (Palette::bodySize, true));
    g.drawText ("+", addButton, juce::Justification::centred, false);
    g.setColour (Palette::muted);
    g.setFont (Palette::font (Palette::headerSize));
    g.drawText (infoShown, infoBounds, juce::Justification::centredRight, true);
}

int ContainerTabs::tabAt (juce::Point<int> p) const
{
    if (p.x < 8 || p.x >= tabsRight)
        return -1;   // a scrolled-out part of a tab, or a clipped tab under the status strip
    for (int i = 0; i < (int) tabs.size(); ++i)
        if (tabs[(size_t) i].bounds.contains (p))
            return i;

    return -1;
}

juce::String ContainerTabs::getTooltip()
{
    const int tab = tabAt (getMouseXYRelative());
    return tab >= 0 ? tabs[(size_t) tab].name : juce::String();
}

void ContainerTabs::mouseDown (const juce::MouseEvent& e)
{
    endDrag();   // a drag whose button-up never arrived does not carry over into this gesture
    int tab = tabAt (e.getPosition());
    // the second click of a double-click means the first click's list, found again by its id: the bar may have
    // relaid out in between (the newly selected list's cue count is measured into it) or lists come and gone;
    // if that list is gone the gesture ends
    if (e.getNumberOfClicks() > 1 && ! pressedId.isNull())
    {
        tab = indexOf (pressedId);
        if (tab < 0)
            return;
    }
    else
        pressedId = juce::isPositiveAndBelow (tab, (int) tabs.size()) ? tabs[(size_t) tab].id : juce::Uuid::null();

    if (e.mods.isPopupMenu())
    {
        if (tab >= 0 && editable)
            showTabMenu (tab, e.getScreenPosition());

        return;
    }

    if (tab >= 0)
    {
        if (onSelect && editable)   // show mode: the GO target list does not change by a click
            onSelect (tab);

        return;
    }

    if (addButton.contains (e.getPosition()) && editable)
        showAddMenu();
}

void ContainerTabs::mouseDrag (const juce::MouseEvent& e)
{
    if (! editable || e.mods.isPopupMenu() || pressedId.isNull())
        return;

    if (! dragging)
    {
        // a press that wanders a few px is still a click; past that the pressed tab follows the mouse
        if (std::abs (e.getDistanceFromDragStartX()) < dragThreshold || indexOf (pressedId) < 0)
            return;

        dragging = true;
        draggedId = pressedId;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        beginDragAutoRepeat (autoScrollRepeatMs);   // drag events keep coming while the mouse rests at an edge
    }

    scrollWhileDragging (e.x);
    updateDropGap (e.x);
}

void ContainerTabs::mouseUp (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    updateDropGap (e.x);   // the drop position itself, not the last drag event
    const int from = indexOf (draggedId);
    const int gap = dropGap;
    endDrag();

    if (from >= 0 && gap >= 0 && onMoveTab)
        onMoveTab (from, gap > from ? gap - 1 : gap);
}

int ContainerTabs::gapAt (int x) const
{
    // only what is visible counts: past the strip's ends the drop goes to the first / last visible gap
    x = juce::jlimit (8, juce::jmax (8, tabsRight - 1), x);

    for (int i = 0; i < (int) tabs.size(); ++i)
        if (x < tabs[(size_t) i].bounds.getCentreX())
            return i;

    return (int) tabs.size();
}

void ContainerTabs::updateDropGap (int x)
{
    const int from = indexOf (draggedId);
    const int gap = gapAt (x);
    const int shown = from < 0 || gap == from || gap == from + 1 ? -1 : gap;   // either side of itself: no move

    if (shown != dropGap)
    {
        dropGap = shown;
        repaint();
    }
}

void ContainerTabs::scrollWhileDragging (int x)
{
    if (maxTabsScroll <= 0)
        return;

    const int step = x < 8 + autoScrollEdge ? -autoScrollStep : x > tabsRight - autoScrollEdge ? autoScrollStep : 0;
    const int scroll = juce::jlimit (0, maxTabsScroll, tabsScroll + step);

    if (scroll == tabsScroll)
        return;

    tabsScroll = scroll;
    followActive = false;   // like the wheel: this relayout does not pull the strip back to the active tab
    resized();
}

void ContainerTabs::endDrag()
{
    if (! dragging)
        return;

    dragging = false;
    draggedId = juce::Uuid::null();
    dropGap = -1;
    beginDragAutoRepeat (0);
    setMouseCursor (juce::MouseCursor::NormalCursor);
    repaint();
}

juce::Rectangle<int> ContainerTabs::getDropMarkerBounds() const
{
    if (! dragging || ! juce::isPositiveAndNotGreaterThan (dropGap, (int) tabs.size()) || tabs.empty())
        return {};

    // 3 px over the 2 px gap; above the strip's bottom border
    const auto& first = tabs.front().bounds;
    const int x = dropGap < (int) tabs.size() ? tabs[(size_t) dropGap].bounds.getX() - 2 : tabs.back().bounds.getRight();
    return { x, first.getY(), 3, juce::jmax (0, first.getHeight() - 1) };
}

void ContainerTabs::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (maxTabsScroll <= 0 || e.x >= tabsRight)
    {
        juce::Component::mouseWheelMove (e, wheel);
        return;
    }
    const float delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
    tabsScroll = juce::jlimit (0, maxTabsScroll, tabsScroll - juce::roundToInt (delta * 160.0f));
    followActive = false;   // the wheel moves the strip on purpose: this relayout does not pull it back
    resized();
}

int ContainerTabs::indexOf (const juce::Uuid& id) const
{
    for (int i = 0; i < (int) tabs.size(); ++i)
        if (tabs[(size_t) i].id == id)
            return i;
    return -1;
}

juce::Rectangle<int> ContainerTabs::getTabBounds (int index) const
{
    return juce::isPositiveAndBelow (index, (int) tabs.size()) ? tabs[(size_t) index].bounds : juce::Rectangle<int>();
}

void ContainerTabs::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int tab = ! pressedId.isNull() ? indexOf (pressedId) : tabAt (e.getPosition());

    if (tab >= 0 && editable && onRename)
        onRename (tab);
}

void ContainerTabs::showAddMenu()
{
    juce::PopupMenu menu;
    menu.addItem (juce::PopupMenu::Item (juce::String::fromUTF8 ("새 큐 리스트"))
                      .setID (1).setImage (CueMenuIcons::create (CommandIDs::addCueList)));
    menu.addItem (juce::PopupMenu::Item (juce::String::fromUTF8 ("새 카트 (버튼 격자)"))
                      .setID (2).setImage (CueMenuIcons::create (CommandIDs::addCart)));
    juce::Component::SafePointer<ContainerTabs> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (addButton)), [safeThis] (int result)
    {
        if (safeThis == nullptr)
            return;

        if (result == 1 && safeThis->onAddList)
            safeThis->onAddList();
        else if (result == 2 && safeThis->onAddCart)
            safeThis->onAddCart();
    });
}

juce::PopupMenu ContainerTabs::createTabMenu (int index) const
{
    const auto info = document.getContainerInfo (index);
    const int count = document.getNumContainers();
    juce::PopupMenu menu;
    menu.addItem (renameItem, juce::String::fromUTF8 ("이름 바꾸기..."));
    menu.addItem (cartItem, info.isCart ? juce::String::fromUTF8 ("큐 리스트로 전환") : juce::String::fromUTF8 ("카트로 전환"));
    menu.addItem (gridItem, juce::String::fromUTF8 ("카트 격자 크기...") + " (" + juce::String (info.cartRows) + " x " + juce::String (info.cartCols) + ")", info.isCart);
    menu.addSeparator();
    menu.addItem (moveLeftItem, juce::String::fromUTF8 ("왼쪽으로 이동"), index > 0);
    menu.addItem (moveRightItem, juce::String::fromUTF8 ("오른쪽으로 이동"), index < count - 1);
    menu.addSeparator();
    menu.addItem (removeItem, juce::String::fromUTF8 ("삭제"), count > 1);
    return menu;
}

void ContainerTabs::performTabMenuItem (int index, int itemId)
{
    if (itemId == renameItem && onRename)            onRename (index);
    else if (itemId == cartItem && onToggleCart)     onToggleCart (index);
    else if (itemId == gridItem && onGridSize)       onGridSize (index);
    else if (itemId == removeItem && onRemove)       onRemove (index);
    else if (itemId == moveLeftItem && onMoveTab && index > 0)
        onMoveTab (index, index - 1);
    else if (itemId == moveRightItem && onMoveTab && index + 1 < document.getNumContainers())
        onMoveTab (index, index + 1);
}

void ContainerTabs::showTabMenu (int index, juce::Point<int> screenPosition)
{
    juce::Component::SafePointer<ContainerTabs> safeThis (this);
    createTabMenu (index).showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ screenPosition.x, screenPosition.y, 1, 1 }),
                                         [safeThis, index] (int result)
    {
        if (safeThis != nullptr && result != 0)
            safeThis->performTabMenuItem (index, result);
    });
}

} // namespace gocue
