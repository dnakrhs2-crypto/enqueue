#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <utility>
#include <vector>

namespace gocue::UiScale
{

/** 프로젝트 설정 > 일반 "글씨·화면 크기": the whole UI (text, buttons, rows, spacing) is drawn this much larger, the
    way a Windows display scale works, on top of the monitor's own DPI scale. Kept per PC (AppSettings), never in
    the project file: the same show opens on a laptop and on a desk monitor. */
constexpr int allowedPercents[] = { 100, 110, 125, 150 };
constexpr int defaultPercent = 100;

/** The main window's smallest size at 100% (MainWindow::setResizeLimits). A scale at which that no longer fits
    into the display is never applied: the window could not be shrunk back into view. */
constexpr int minWindowWidth = 860, minWindowHeight = 640;

inline bool isAllowed (int percent) noexcept
{
    for (const int p : allowedPercents)
        if (p == percent)
            return true;

    return false;
}

/** A stored value that is not one of the choices (an edited settings file) means 100. */
inline int normalise (int percent) noexcept
{
    return isAllowed (percent) ? percent : defaultPercent;
}

/** The largest choice <= 'wanted' at which the main window's minimum size still fits 'availableAt100' (the display's
    work area in logical pixels as it is at 100%). Never below 100: a display too small even for that keeps 100. */
inline int fitPercent (int wanted, juce::Rectangle<int> availableAt100) noexcept
{
    int best = defaultPercent;

    for (const int p : allowedPercents)
        if (p <= wanted
            && minWindowWidth * p <= availableAt100.getWidth() * 100
            && minWindowHeight * p <= availableAt100.getHeight() * 100)
            best = juce::jmax (best, p);

    return best;
}

inline int currentPercent()
{
    return juce::roundToInt (juce::Desktop::getInstance().getGlobalScaleFactor() * 100.0f);
}

/** The work area of the display 'window' sits on (the primary display without a window) as it is at 100%. JUCE
    reports display areas divided by the scale in force, so that is undone here. */
inline juce::Rectangle<int> workAreaAt100 (const juce::Component* window = nullptr)
{
    auto& desktop = juce::Desktop::getInstance();
    const auto& displays = desktop.getDisplays();
    const auto* display = (window != nullptr && window->isOnDesktop()) ? displays.getDisplayForRect (window->getScreenBounds())
                                                                       : displays.getPrimaryDisplay();

    if (display == nullptr)
        return { 100000, 100000 };   // headless (the tests): nothing to fit into

    return (display->userBounds * desktop.getGlobalScaleFactor()).toNearestInt();
}

/** After a scale change (or on a smaller monitor than last time): the window keeps its minimum size and is then
    shrunk / moved so that all of it, frame included, stays inside its display's work area. A maximised or
    minimised window is left alone. */
inline void fitWindowIntoDisplay (juce::ResizableWindow& window)
{
    if (window.isFullScreen() || window.isMinimised())
        return;

    window.setBoundsConstrained (window.getBounds());   // the minimum size first

    auto* peer = window.getPeer();
    const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (window.getScreenBounds());

    if (peer == nullptr || display == nullptr)
        return;

    const auto frame = peer->getFrameSize();
    const auto outer = frame.addedTo (window.getBounds()).constrainedWithin (display->userBounds.toNearestInt());
    window.setBoundsConstrained (frame.subtractedFrom (outer));
}

/** Applies 'percent', lowered to what the display can fit, and returns what was applied. Every open window is
    re-laid out by JUCE at once (setGlobalScaleFactor refreshes the displays and tells every peer). JUCE keeps a
    window's size on screen, so its logical size would shrink and the layout with it: each window is given its old
    logical bounds back instead (it grows on screen), then pulled back into its display. A maximised window simply
    re-lays out into the same screen area. */
inline int apply (int percent, const juce::Component* window = nullptr)
{
    const int fitted = fitPercent (normalise (percent), workAreaAt100 (window));

    std::vector<std::pair<juce::Component::SafePointer<juce::ResizableWindow>, juce::Rectangle<int>>> windows;

    for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
        if (auto* w = dynamic_cast<juce::ResizableWindow*> (juce::TopLevelWindow::getTopLevelWindow (i)))
            windows.emplace_back (w, w->getBounds());

    juce::Desktop::getInstance().setGlobalScaleFactor ((float) fitted / 100.0f);

    for (auto& [w, bounds] : windows)
    {
        if (w == nullptr || w->isFullScreen() || w->isMinimised())
            continue;

        w->setBoundsConstrained (bounds);
        fitWindowIntoDisplay (*w);
    }

    return fitted;
}

} // namespace gocue::UiScale
