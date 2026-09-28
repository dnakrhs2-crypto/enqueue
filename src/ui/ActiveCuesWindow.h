#pragma once

#include "app/AppSettings.h"
#include "ui/ActiveCuesPanel.h"

namespace gocue
{
/** A second, view-only panel. The main panel stays in its original parent and layout. */
class ActiveCuesWindow : public juce::DocumentWindow
{
public:
    ActiveCuesWindow (AudioEngine&, CueList&, AppSettings&);
    ~ActiveCuesWindow() override;
    void open();
    void closeButtonPressed() override;
    void resized() override;
    void moved() override;
    void visibilityChanged() override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusContent();

    ActiveCuesPanel& getPanel() noexcept;
    void setPlayingCues (const std::vector<AudioEngine::PlayingCue>&, const std::vector<WaitProgress>&,
                         double now, const VolumeCue::Badges&);
    std::function<bool (const juce::KeyPress&)> onTableKey;
    static float scaleFor (int width, int height, int cards) noexcept;

private:
    class Content;
    void saveState();
    AppSettings& settings;
    Content* content = nullptr;
    bool fitting = false, ready = false;
    bool opened = false;   // until the first open() the saved place is not ours to overwrite (restored there)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ActiveCuesWindow)
};
}
