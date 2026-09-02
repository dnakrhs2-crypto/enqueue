#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue
{

/** The bar between two panes: drag it to resize, the chevron (or a double-click) folds the secondary pane away. */
class SplitDivider : public juce::Component
{
public:
    /** horizontal = spans the width, drags up / down (the pane is below); vertical = spans the height, the pane is to the right. */
    enum class Orientation { horizontal, vertical };

    static constexpr int thickness = 10;

    explicit SplitDivider (Orientation orientation);

    void setCollapsed (bool shouldBeCollapsed);
    bool isCollapsed() const noexcept { return collapsed; }

    std::function<void()> onDragStart;
    /** Pixels from where the drag started: down / right positive. */
    std::function<void (int delta)> onDrag;
    std::function<void()> onDragEnd;
    std::function<void()> onToggle;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;

private:
    void updateShape();

    const Orientation orientation;
    bool collapsed = false, dragging = false;
    int dragStart = 0;
    juce::ShapeButton toggle;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SplitDivider)
};

} // namespace gocue
