#pragma once

#include "ChainDrawer.h"
#include "ChannelCard.h"
#include "FxDrawer.h"
#include "LiveMixSettings.h"
#include "MasterCard.h"
#include "MixDocument.h"
#include "TopBar.h"
#include "WebDavBackup.h"
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

    /** Session files. new / open first secure what is open (see withSessionSecured). */
    void newSession();
    void openSession (const juce::File& file);
    void openSessionDialog();
    bool saveSession();          // to the current file, or "save as" without one
    /** The save dialog; 'then (saved)' runs when it is over (false: cancelled or failed). */
    void saveSessionAs (std::function<void (bool saved)> then = nullptr);
    /** Opens a session file named on the command line. True when one was named (opened, or refused with a notice). */
    bool openFromCommandLine (const juce::String& commandLine);
    /** Saves a dirty session that has a file (autosave / quit). True when nothing is lost: nothing to save, or saved. */
    bool autosaveNow();
    /** Runs 'action' once the open session is safe to leave: a dirty session with a file is saved first (a failed save
        asks whether to go on without it), a dirty session without a file asks to save it, drop it, or stay. */
    void withSessionSecured (std::function<void()> action);
    /** A backup upload is running (a quit would cut it). */
    bool isUploadingBackup() const noexcept { return backup.isBusy(); }
    /** Safe mode (Shift / --safe-mode): a session's device is not opened and plugins are not loaded. */
    void setSafeMode (bool on) noexcept { safeMode = on; }

    /** The ASIO device list changed (settings / hot-plug): refresh names and pickers. */
    void deviceChanged();
    /** A message shown in a bar under the top bar, with a close button - never a modal dialog: a modal alert freezes
        the whole window (no resizing, no mic buttons) until it is dismissed, which is wrong for a live tool. */
    void showNotice (const juce::String& text, bool error);
    void hideNotice();
    /** Adds a line to the notice that is showing (or shows a new one): a startup note must not replace a session warning. */
    void addNotice (const juce::String& text, bool error);
    void refreshAll();

    std::function<void()> onQuitRequested;

    void resized() override;
    void paint (juce::Graphics& g) override;
    void parentHierarchyChanged() override;

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
    void deviceChosen();   // the operator picked a device / buffer: the session remembers it
    void loadSession (const juce::File& file);
    juce::String titleForChainOwner() const;
    void updateDeviceNames();
    juce::File defaultSessionFolder() const;
    void showStatus (const juce::String& text, bool error = false);
    void hideNoticeIf (const juce::String& prefix);   // closes the notice when it is the one that starts so
    CardLayout layoutForWidth (int width) const;

    MixDocument& document;
    LiveMixSettings& settings;
    MixEngine& engine;
    PluginWindowManager windows;
    WebDavBackup backup;
    juce::Component::SafePointer<juce::DialogWindow> pluginManagerDialog;
    bool safeMode = false;

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
    juce::TextEditor noticeText;   // read-only: wraps by word, breaks a long token, scrolls when long
    std::unique_ptr<juce::ResizableCornerComponent> cornerGrip;   // a visible handle: the frame's own edge is thin (and hard to hit over remote desktop)
    juce::TextButton noticeClose { juce::String::fromUTF8 ("\xE2\x9C\x95") };
    bool noticeVisible = false, noticeIsError = false;
    juce::StringArray inputNames, outputNames;
    juce::String statusText;
    double statusUntilMs = 0.0;
    double lastChangeMs = -1.0;
    bool autosavePending = false;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace gocue::livemix
