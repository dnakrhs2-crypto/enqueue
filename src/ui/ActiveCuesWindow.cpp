#include "ui/ActiveCuesWindow.h"

#include "app/UiScale.h"
#include "ui/ShortcutRouter.h"
#include "ui/UiUtils.h"

#include <cmath>

namespace gocue
{
class ActiveCuesWindow::Content : public juce::Component
{
public:
    Content (AudioEngine& engine, CueList& cues) : panel (engine, cues)
    {
        setWantsKeyboardFocus (true);
        panel.setViewOnly (true);
        addAndMakeVisible (panel);
    }

    void resized() override { layout(); }
    void layout()
    {
        const auto size = getLocalBounds();
        const int cards = panel.getCardCount();
        if (size == lastSize && cards == lastCards) return;
        lastSize = size;
        lastCards = cards;
        const float scale = ActiveCuesWindow::scaleFor (getWidth(), getHeight(), cards);
        panel.setTransform (juce::AffineTransform::scale (scale));
        panel.setBounds (0, 0, (int) std::floor ((float) getWidth() / scale), (int) std::floor ((float) getHeight() / scale));
    }
    ActiveCuesPanel panel;

private:
    juce::Rectangle<int> lastSize;
    int lastCards = -1;
};

float ActiveCuesWindow::scaleFor (int width, int height, int cards) noexcept
{
    const double needed = Palette::cardHeaderHeight + (double) juce::jmax (1, cards)
                        * (Palette::activeViewCardHeight + Palette::cardInset) + 2 * Palette::cardInset;
    return juce::jlimit (Palette::activeViewMinScale, Palette::activeViewMaxScale,
                        (float) juce::jmin ((double) width / Palette::activeViewWidth, (double) height / needed));
}

ActiveCuesWindow::ActiveCuesWindow (AudioEngine& engine, CueList& cues, AppSettings& appSettings)
    : DocumentWindow (ko ("활성 큐 크게보기 - Enqueue"), Palette::background, DocumentWindow::allButtons, false),
      settings (appSettings)
{
    // Share the existing router and its held-key state. The fixed-key owner below only forwards table keys.
    getProperties().set ("activeCuesBigView", true);
    ShortcutRouter::setWindowScope (*this, ShortcutKeyContext::Window::main);
    ShortcutRouter::setComponentScope (*this, ShortcutScope::cueTable);
    setUsingNativeTitleBar (true);
    content = new Content (engine, cues);
    setContentOwned (content, false);
    setResizable (true, false);
    setResizeLimits (Palette::activeViewMinWidth, Palette::activeViewMinHeight, Palette::windowMaxSize, Palette::windowMaxSize);
    centreWithSize (Palette::activeViewDefaultWidth, Palette::activeViewDefaultHeight);
    // the saved state is restored in open(), once the native window exists: before that JUCE cannot bring back a
    // maximised window (it would come back as a normal window on the main monitor)
    ready = true;
}

ActiveCuesWindow::~ActiveCuesWindow()
{
    saveState();
    ready = false;
    clearContentComponent();
    content = nullptr;
}

void ActiveCuesWindow::open()
{
    bool maximise = false;

    if (! isOnDesktop())
    {
        addToDesktop (getDesktopWindowStyleFlags());

        if (! opened)
        {
            // the normal place first, and maximised only once the window shows: JUCE keeps as the normal place the one
            // it had at the moment of maximising, so maximising the still hidden window would keep the default place
            // (and the next save would put the window back on the main monitor)
            const juce::ScopedValueSetter<bool> restoring (ready, false);   // the steps of the restore are not new states
            auto state = settings.getActiveCuesWindowState().trim();
            maximise = state.startsWithIgnoreCase ("fs");

            if (maximise)
                state = state.substring (2).trim();

            if (state.isNotEmpty())
                restoreWindowStateFromString (state);
        }
    }
    opened = true;   // from now on this window's own place is the one to keep
    setMinimised (false);
    UiScale::fitWindowIntoDisplay (*this);
    setVisible (true);

    if (maximise)
        setFullScreen (true);

    toFront (true);
    focusContent();
}

void ActiveCuesWindow::closeButtonPressed() { saveState(); setVisible (false); }
// the base classes keep the window's normal place up to date (and bring a shown window forward): call them first
void ActiveCuesWindow::moved() { DocumentWindow::moved(); saveState(); }
void ActiveCuesWindow::visibilityChanged() { DocumentWindow::visibilityChanged(); if (! isVisible()) saveState(); }
// not while minimised: Windows reports a minimised window as not maximised, which would forget a maximised big view
void ActiveCuesWindow::saveState() { if (ready && opened && ! isMinimised()) settings.setActiveCuesWindowState (getWindowStateAsString()); }

void ActiveCuesWindow::resized()
{
    DocumentWindow::resized();
    UiScale::fitOnResized (*this, fitting);
    saveState();
}

bool ActiveCuesWindow::keyPressed (const juce::KeyPress& key) { return onTableKey && onTableKey (key); }
void ActiveCuesWindow::focusContent() { if (content != nullptr) content->grabKeyboardFocus(); }
ActiveCuesPanel& ActiveCuesWindow::getPanel() noexcept { return content->panel; }

void ActiveCuesWindow::setPlayingCues (const std::vector<AudioEngine::PlayingCue>& playing, const std::vector<WaitProgress>& waits,
                                       double now, const VolumeCue::Badges& badges)
{
    content->panel.setPlayingCues (playing, waits, now, badges);
    content->layout();
}
}
