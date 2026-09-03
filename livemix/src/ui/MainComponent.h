#pragma once

#include "ChainDrawer.h"
#include "ChannelCard.h"
#include "FxDrawer.h"
#include "LiveMixSettings.h"
#include "MasterCard.h"
#include "MixDocument.h"
#include "TopBar.h"
#include "ui/PluginWindows.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace gocue::livemix
{

/** The window's content: top bar, the scrolling channel cards, the docked master, the drawers, the status bar.
    Everything the operator does goes through the document; the timer feeds meters and autosave. */
class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    MainComponent (MixDocument& document, LiveMixSettings& settings);
    ~MainComponent() override;

    /** Session files. */
    void newSession();
    void openSession (const juce::File& file);
    void openSessionDialog();
    bool saveSession();          // to the current file, or "save as" without one
    void saveSessionAs();
    void openFromCommandLine (const juce::String& commandLine);
    /** Saves a dirty session with a file (autosave / quit). */
    void autosaveNow();

    /** The ASIO device list changed (settings / hot-plug): refresh names and pickers. */
    void deviceChanged();
    void refreshAll();

    std::function<void()> onQuitRequested;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    enum class Drawer { none, chain, fx };

    void timerCallback() override;
    void rebuildCards();
    void refreshValues();
    void layoutCards();
    void showDrawer (Drawer which);
    void openChainFor (PluginChain* chain, const juce::String& title);
    void addPluginTo (PluginChain* chain, const juce::String& title, juce::Component* anchor);
    void showSessionMenu (juce::Component* anchor);
    void showHelpMenu (juce::Component* anchor);
    void showBackupDialog();
    void showSettingsDialog();
    void showPluginManager();
    void chooseDevice (const juce::String& name);
    void updateDeviceNames();
    juce::File defaultSessionFolder() const;
    void showStatus (const juce::String& text, bool error = false);
    CardLayout layoutForWidth (int width) const;

    MixDocument& document;
    LiveMixSettings& settings;
    MixEngine& engine;
    PluginWindowManager windows;

    TopBar topBar;
    juce::Viewport viewport;
    juce::Component cardsHolder;
    std::vector<std::unique_ptr<ChannelCard>> cards;
    juce::TextButton addChannelButton;
    MasterCard masterCard;
    ChainDrawer chainDrawer;
    FxDrawer fxDrawer;
    Drawer drawer = Drawer::none;
    juce::Uuid chainOwnerId = juce::Uuid::null();   // the channel / FX whose chain the drawer shows (null = master)
    bool chainIsFx = false;
    juce::Label statusLeft, statusRight;
    juce::StringArray inputNames, outputNames;
    juce::String statusText;
    double statusUntilMs = 0.0;
    double lastChangeMs = -1.0;
    bool autosavePending = false;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace gocue::livemix
