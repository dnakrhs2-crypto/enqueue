#pragma once

#include "ChainDrawer.h"
#include "ChannelCard.h"
#include "FxDrawer.h"
#include "GlobalHotkeys.h"
#include "LiveMixSettings.h"
#include "MasterCard.h"
#include "MixDocument.h"
#include "MuteGroups.h"
#include "TopBar.h"
#include "WebDavBackup.h"
#include "ui/PluginWindows.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace gocue::livemix
{

/** The window's content: top bar, the scrolling channel cards, the docked master, the drawers, the status bar.
    Everything the operator does goes through the document; the timer feeds the meters. */
class MainComponent : public juce::Component,
                      public juce::KeyListener,   // Ctrl+S / Ctrl+Shift+S / Ctrl+N, registered on the window
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
    /** Saves a dirty session that has a file (quitting, and right before a backup). True when nothing is lost:
        nothing to save, or saved. There is no timed autosave: a session is set up once and saved by hand. */
    bool saveIfDirty();
    /** Runs 'action' once the open session is safe to leave: a dirty session with a file is saved first (a failed save
        asks whether to go on without it), a dirty session without a file asks to save it, drop it, or stay. */
    void withSessionSecured (std::function<void()> action);
    /** A backup job (an upload, a listing, a download) is running: a quit would cut it. */
    bool isUploadingBackup() const noexcept { return backup.isBusy(); }
    /** Safe mode (Shift / --safe-mode): a session's device is not opened and plugins are not loaded. */
    void setSafeMode (bool on) noexcept { safeMode = on; }

    /** The ASIO device list changed (settings / hot-plug): refresh names and pickers. */
    void deviceChanged();
    /** The two mute groups (the tray menu toggles them too). */
    MuteGroups& getMuteGroups() noexcept { return muteGroups; }
    /** The notice bar under the top bar, with a close button - never a modal dialog: a modal alert freezes the whole
        window (no resizing, no mic buttons) until it is dismissed, which is wrong for a live tool. Three lines that
        come and go on their own: the session note (the last open: its failure, or its warnings), the startup note
        (the ASIO error until a device runs, or the safe-mode note), the save-failure note (until a save succeeds).
        The close button clears them all. An empty text clears that line. */
    void setSessionNote (const juce::String& text, bool error);
    void setStartupNote (const juce::String& text, bool error, bool safeModeNote);
    void hideNotice();
    void refreshAll();

    std::function<void()> onQuitRequested;

    void resized() override;
    void paint (juce::Graphics& g) override;
    void parentHierarchyChanged() override;
    using juce::Component::keyPressed;
    bool keyPressed (const juce::KeyPress& key, juce::Component* origin) override;

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
    void registerHotkeys();      // from the settings; a refused key goes to the status line
    void layoutFxDrawer();       // the drawer's size inside its viewport
    void muteGroupsChanged();    // badges, cards, drawer
    void showPluginManager();
    void chooseDevice (const juce::String& name);
    void deviceChosen();   // the operator picked a device / buffer: the session remembers it
    void loadSession (const juce::File& file);
    juce::String titleForChainOwner() const;
    void updateDeviceNames();
    juce::File defaultSessionFolder() const;
    void showStatus (const juce::String& text, bool error = false);
    void setSaveError (const juce::String& message);   // the save-failure line (empty = cleared)
    void refreshNotice();
    CardLayout layoutForWidth (int width) const;

    MixDocument& document;
    LiveMixSettings& settings;
    MixEngine& engine;
    MuteGroups muteGroups { document };
    GlobalHotkeys hotkeys;
    PluginWindowManager windows;
    WebDavBackup backup;
    juce::Component::SafePointer<juce::DialogWindow> pluginManagerDialog;
    bool safeMode = false;
    juce::StringArray faultedPlugins, stalledPlugins;   // told once each, kept in the notice until the session changes

    TopBar topBar;
    juce::Viewport viewport;
    juce::Component cardsHolder;
    std::vector<std::unique_ptr<ChannelCard>> cards;
    juce::TextButton addChannelButton;
    MasterCard masterCard;
    ChainDrawer chainDrawer;
    FxDrawer fxDrawer;
    juce::Viewport fxDrawerViewport;   // the FX drawer scrolls when its content is taller than the window
    Drawer drawer = Drawer::none;
    juce::Uuid chainOwnerId = juce::Uuid::null();   // the channel / FX whose chain the drawer shows (null = master)
    bool chainIsFx = false;
    juce::Label statusLeft, statusRight;
    juce::TextEditor noticeText;   // read-only: wraps by word, breaks a long token, scrolls when long
    std::unique_ptr<juce::ResizableCornerComponent> cornerGrip;   // a visible handle: the frame's own edge is thin (and hard to hit over remote desktop)
    juce::TextButton noticeClose { juce::String::fromUTF8 ("\xE2\x9C\x95") };
    bool noticeVisible = false, noticeIsError = false;
    juce::String sessionNote, startupNote, saveErrorNote;   // the lines of the bar (see setSessionNote)
    juce::String pluginNote;    // plugins that faulted (dry from then on), accumulated until the bar is closed
    juce::String latencyNote;   // a plugin with latency in a mic chain
    bool sessionNoteIsError = false, startupNoteIsError = false, startupNoteIsSafeMode = false;
    int ticksUntilLatencyCheck = 0;
    juce::File pendingCommandLineFile;   // a session named while a question was open: opened once the question is answered
    juce::StringArray inputNames, outputNames;
    juce::String statusText;
    double statusUntilMs = 0.0;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace gocue::livemix
