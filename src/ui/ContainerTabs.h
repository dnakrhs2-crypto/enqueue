#pragma once

#include "app/ProjectDocument.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue
{

/** The strip above the cue list: one tab per cue list / cart of the project, the active one highlighted,
    and a "+" that adds a list or a cart. Right-click (or double-click) a tab for rename / cart / grid / delete. */
class ContainerTabs : public juce::Component
{
public:
    explicit ContainerTabs (ProjectDocument& document);

    /** Rebuilds the tabs from the document (call on containersChanged). */
    void refresh();
    void setEditable (bool shouldBeEditable);

    std::function<void (int index)> onSelect;
    std::function<void()> onAddList;
    std::function<void()> onAddCart;
    std::function<void (int index)> onRename;
    std::function<void (int index)> onRemove;
    std::function<void (int index)> onToggleCart;
    std::function<void (int index)> onGridSize;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;

    static constexpr int height = 26;

private:
    struct Tab
    {
        juce::Rectangle<int> bounds;
        juce::String name;
        bool isCart = false;
        bool active = false;
    };

    int tabAt (juce::Point<int> p) const;
    void showTabMenu (int index, juce::Point<int> screenPosition);
    void showAddMenu();

    ProjectDocument& document;
    std::vector<Tab> tabs;
    juce::Rectangle<int> addButton;
    bool editable = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContainerTabs)
};

} // namespace gocue
