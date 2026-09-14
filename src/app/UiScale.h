#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <utility>
#include <vector>

namespace gocue::UiScale
{

/** 설정 > 글씨·화면 크기: the whole UI (text, buttons, rows, spacing) is drawn this much larger, the
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

/** The percent last asked for through apply() with recordRequest (the saved setting at launch, a dialog choice):
    the settings dialog compares it with what is in force to explain a difference (lowered for the display, or
    safe mode never applied it). */
inline int lastRequestedPercent = defaultPercent;

/** After a scale change (or on a smaller monitor than last time): the window keeps its minimum size and is shrunk
    to its display's work area when it is bigger than that (frame included); with 'moveIntoView' it is also moved
    so all of it is on screen. A maximised or minimised window is left alone; a window that cannot be resized by the
    user (a fixed-size dialog) is never shrunk, only moved. */
inline void fitWindowIntoDisplay (juce::ResizableWindow& window, bool moveIntoView = true)
{
    if (window.isFullScreen() || window.isMinimised())
        return;

    window.setBoundsConstrained (window.getBounds());   // the minimum size first

    auto* peer = window.getPeer();
    const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (window.getScreenBounds());

    if (peer == nullptr || display == nullptr)
        return;

    const auto frame = peer->getFrameSize();
    const auto area = display->userBounds.toNearestInt();
    const auto outer = frame.addedTo (window.getBounds());
    auto fitted = outer;

    if (window.isResizable())   // shrink in place: the position is only touched below, when asked
        fitted.setSize (juce::jmin (outer.getWidth(), area.getWidth()), juce::jmin (outer.getHeight(), area.getHeight()));

    if (moveIntoView)
        fitted.setPosition (juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight() - fitted.getWidth()), fitted.getX()),
                            juce::jlimit (area.getY(), juce::jmax (area.getY(), area.getBottom() - fitted.getHeight()), fitted.getY()));

    if (fitted != outer)
        window.setBoundsConstrained (frame.subtractedFrom (fitted));
}

/** For a window's resized(): coming back from the maximised state after a scale change, JUCE restores the old
    logical size at the new scale, which may be bigger than the screen — shrink it back in (size only, the window
    may sit partly off-screen on purpose). 'guard' is the window's own re-entrancy flag. */
inline void fitOnResized (juce::ResizableWindow& window, bool& guard)
{
    if (guard || ! window.isOnDesktop() || window.isFullScreen() || window.isMinimised())
        return;

    const juce::ScopedValueSetter<bool> setter (guard, true);
    fitWindowIntoDisplay (window, false);
}

/** Applies 'percent', lowered to what the display can fit, and returns what was applied. Every open window is
    re-laid out by JUCE at once (setGlobalScaleFactor refreshes the displays and tells every peer). JUCE keeps a
    window's size on screen, so its logical size would shrink and the layout with it: each window is given its old
    logical size back instead (it grows on screen) at the position JUCE recomputed for the new scale (an old
    position would land on another monitor), then pulled back into its display. A maximised window simply re-lays
    out into the same screen area. */
inline int apply (int percent, const juce::Component* window = nullptr, bool recordRequest = true)
{
    const int wanted = normalise (percent);

    if (recordRequest)
        lastRequestedPercent = wanted;

    const int fitted = fitPercent (wanted, workAreaAt100 (window));

    std::vector<std::pair<juce::Component::SafePointer<juce::ResizableWindow>, juce::Rectangle<int>>> windows;

    for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
        if (auto* w = dynamic_cast<juce::ResizableWindow*> (juce::TopLevelWindow::getTopLevelWindow (i)))
            windows.emplace_back (w, w->getBounds());

    juce::Desktop::getInstance().setGlobalScaleFactor ((float) fitted / 100.0f);

    for (auto& [w, bounds] : windows)
    {
        if (w == nullptr || w->isFullScreen() || w->isMinimised())
            continue;

        w->setBoundsConstrained (w->getBounds().withSize (bounds.getWidth(), bounds.getHeight()));
        fitWindowIntoDisplay (*w);
    }

    return fitted;
}

} // namespace gocue::UiScale
