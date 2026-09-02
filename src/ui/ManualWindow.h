#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace gocue
{

/** 도움말 > 사용 설명서: the feature manual, one tab per topic, in a window of its own that hides on close. */
class ManualWindow : public juce::DocumentWindow
{
public:
    ManualWindow();
    ~ManualWindow() override;

    /** Brings the window up (restores it if it was closed) and shows the tab at 'sectionIndex' (-1 keeps the current tab). */
    void open (int sectionIndex = -1);

    void closeButtonPressed() override;

    struct Section
    {
        const char* title;   // UTF-8
        const char* text;    // UTF-8
    };

    /** The manual, in order. */
    static std::vector<Section> sections();

private:
    class Content;
    Content* content = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ManualWindow)
};

} // namespace gocue
