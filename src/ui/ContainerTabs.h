#pragma once

#include "app/ProjectDocument.h"
#include "ui/UiUtils.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue
{
class FooterBar;

/** The strip above the cue list: one tab per cue list / cart of the project, the active one highlighted,
    and a "+" that adds a list or a cart. Right-click (or double-click) a tab for rename / cart / grid / move / delete;
    in edit mode a tab dragged sideways moves to where it is dropped. */
class ContainerTabs : public juce::Component, public juce::TooltipClient
{
public:
    explicit ContainerTabs (ProjectDocument& document);

    /** Rebuilds the tabs from the document (call on containersChanged). */
    void refresh();
    void setEditable (bool shouldBeEditable);
    void setInfoText (juce::String text);
    const juce::String& getInfoText() const noexcept { return infoText; }
    void setStatusBar (FooterBar& status);

    std::function<void (int index)> onSelect;
    std::function<void()> onAddList;
    std::function<void()> onAddCart;
    std::function<void (int index)> onRename;
    std::function<void (int index)> onRemove;
    std::function<void (int index)> onToggleCart;
    std::function<void (int index)> onGridSize;
    /** A tab moved by a drag or by the tab menu: it should end up at index 'to'. */
    std::function<void (int from, int to)> onMoveTab;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    /** Tabs past the reserved status strip scroll sideways with the wheel. */
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    /** A tab's area in this component (tests: where a click lands). */
    juce::Rectangle<int> getTabBounds (int index) const;
    juce::String getTooltip() override;

    /** The tab menu's items (ids of createTabMenu / performTabMenuItem). */
    enum TabMenuItem { renameItem = 1, cartItem, gridItem, removeItem, moveLeftItem, moveRightItem };
    /** The right-click menu of tab 'index', and what choosing one of its items does (tests: the menu without showing it). */
    juce::PopupMenu createTabMenu (int index) const;
    void performTabMenuItem (int index, int itemId);
    /** Where the drop marker of a tab drag is drawn; empty when no drag would move a tab (tests). */
    juce::Rectangle<int> getDropMarkerBounds() const;
    bool isDraggingTab() const noexcept { return dragging; }

    static constexpr int height = Palette::tabBarHeight;

private:
    struct Tab
    {
        juce::Uuid id = juce::Uuid::null();
        juce::Rectangle<int> bounds;
        juce::String name;
        bool isCart = false;
        bool active = false;
    };

    int tabAt (juce::Point<int> p) const;
    void showTabMenu (int index, juce::Point<int> screenPosition);
    void showAddMenu();
    /** The gap a drop at x lands in: 0 = before the first tab .. tabs.size() = after the last (x kept on the visible strip). */
    int gapAt (int x) const;
    void updateDropGap (int x);
    void scrollWhileDragging (int x);
    void endDrag();

    ProjectDocument& document;
    std::vector<Tab> tabs;
    juce::Rectangle<int> addButton;
    juce::Rectangle<int> infoBounds;
    int tabsRight = 0;   // tabs draw and take clicks only left of this; the + button has its own reserved area
    int tabsScroll = 0, maxTabsScroll = 0;   // px the tab strip is scrolled left (only when it does not fit)
    juce::Uuid revealedId = juce::Uuid::null();   // the active list last scrolled into view (by id: indices shift)
    bool followActive = true;                // the active tab was in view after the last layout: keep it there
    juce::Uuid pressedId = juce::Uuid::null();   // the list the first click of the current gesture landed on
    bool dragging = false;                        // the pressed tab is being dragged sideways
    juce::Uuid draggedId = juce::Uuid::null();   // by id: the strip may relay out (and refresh) during the drag
    int dropGap = -1;                             // gapAt() of the drag, -1 while a drop there would change nothing
    static constexpr int dragThreshold = 5, autoScrollEdge = 24, autoScrollStep = 14, autoScrollRepeatMs = 40;
    int indexOf (const juce::Uuid& id) const;
    juce::String infoText;
    juce::String infoShown;   // what fits of infoText: all of it, its first item, or nothing
    bool editable = true;
    FooterBar* statusBar = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContainerTabs)
};

} // namespace gocue
