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
                        * (Palette::activeCardHeight + Palette::cardInset) + 2 * Palette::cardInset;
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
    const auto state = settings.getActiveCuesWindowState();
    if (state.isNotEmpty()) restoreWindowStateFromString (state);
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
    if (! isOnDesktop()) addToDesktop (getDesktopWindowStyleFlags());
    setMinimised (false);
    UiScale::fitWindowIntoDisplay (*this);
    setVisible (true);
    toFront (true);
    focusContent();
}

void ActiveCuesWindow::closeButtonPressed() { saveState(); setVisible (false); }
void ActiveCuesWindow::moved() { saveState(); }
void ActiveCuesWindow::visibilityChanged() { if (! isVisible()) saveState(); }
void ActiveCuesWindow::saveState() { if (ready) settings.setActiveCuesWindowState (getWindowStateAsString()); }

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
