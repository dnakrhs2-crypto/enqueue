#pragma once

#include "app/AppSettings.h"
#include "app/CueController.h"
#include "app/ProjectDocument.h"
#include "app/Scheduler.h"
#include "audio/AudioEngine.h"
#include "ui/ActiveCuesPanel.h"
#include "ui/ContainerTabs.h"
#include "ui/CueCartView.h"
#include "ui/CueInspector.h"
#include "ui/SplitDivider.h"
#include "ui/ManualWindow.h"
#include "ui/CueTable.h"
#include "ui/FooterBar.h"
#include "ui/PluginWindows.h"
#include "ui/TransportBar.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>

namespace gocue
{

/** The single main window content: menu bar / transport / cue table / inspector / footer.
    Owns the project document, the scheduler, the cue controller, the plugin editor windows and
    dispatches every command. */
class MainComponent : public juce::Component,
                      public juce::ApplicationCommandTarget,
                      public juce::MenuBarModel,
                      public juce::DragAndDropContainer,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer,
                      private CueList::Listener,
                      private ProjectDocument::Listener
{
public:
    MainComponent (AudioEngine& engine, AppSettings& settings, juce::ApplicationCommandManager& commands);
    ~MainComponent() override;

    void resized() override;
    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;

    // ApplicationCommandTarget
    juce::ApplicationCommandTarget* getNextCommandTarget() override;
    void getAllCommands (juce::Array<juce::CommandID>& commands) override;
    void getCommandInfo (juce::CommandID commandID, juce::ApplicationCommandInfo& result) override;
    bool perform (const InvocationInfo& info) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected (int menuItemID, int topLevelMenuIndex) override;

    /** Opens a project file (.enqueue / .gocue) passed on the command line (file association / drag onto the exe). */
    void openProjectFromCommandLine (const juce::String& commandLine);
    void openProjectFile (const juce::File& file, bool allowAutoStart = true);
    juce::File getProjectFile() const noexcept { return document.getFile(); }

    /** Runs 'action' immediately, or once the user has saved / discarded unsaved changes. */
    void confirmDiscardChangesThen (std::function<void()> action);
    /** Replacing the whole project while cues play is asked about first (a double-clicked file must not end a
        running show by accident), then confirmDiscardChangesThen() runs 'action'. */
    void confirmReplaceProjectThen (std::function<void()> action, const juce::String& question = {});
    /** Nothing plays and show mode is off: the moment for anything that may put a window up (update checks). */
    bool isIdleForInterruptions() const noexcept
    {
        return engine.getNumPlaying() == 0 && ! showModeFlag.load (std::memory_order_acquire) && ! controller.hasPendingStarts();
    }

    /** The keyboard hook (Main.cpp) saw an Esc press anywhere in the app, native plugin editors included: a panic when
        something plays, is about to start, or show mode is on. A press the main window handled itself is not repeated. */
    void panicFromAnywhere();
    /** Off for a launch that must not start cues by itself (safe mode, an update restart, a fallback device). */
    void setAutoStartOnOpenAllowed (bool allowed) noexcept { autoStartOnOpenAllowed = allowed; }

    /** Fires the "start on close" cue (if configured) and runs 'then' once it has finished (or after 2 minutes). */
    void fireCloseCueThen (std::function<void()> then);

    /** Thread-safe snapshot of the dirty flag (the updater asks from a background thread). */
    bool hasUnsavedChanges() const noexcept { return unsavedChanges.load (std::memory_order_relaxed); }

    std::function<void (const juce::String& title)> onWindowTitleChanged;

    /** Esc (panic) and Space (GO) reach the show from every window the app owns: plugin editors, the manual, the
        settings dialogs. Attached to each top-level window by the timer; text fields keep Space for typing. */
    class OperationalKeys : public juce::KeyListener
    {
    public:
        explicit OperationalKeys (MainComponent& o) : owner (o) {}
        bool keyPressed (const juce::KeyPress& key, juce::Component* origin) override;
        bool keyStateChanged (bool isKeyDown, juce::Component*) override;
    private:
        MainComponent& owner;
        bool spaceHeld = false;   // auto-repeat of a held Space is one GO, not many
    };

private:
    /** Cue hotkeys are checked before the command shortcuts. */
    struct HotkeyListener : public juce::KeyListener
    {
        explicit HotkeyListener (MainComponent& o) : owner (o) {}
        bool keyPressed (const juce::KeyPress& key, juce::Component*) override;
        bool keyStateChanged (bool isKeyDown, juce::Component*) override;
        MainComponent& owner;
        std::set<int> heldKeys;   // OS key repeat must not re-fire a hotkey while it is held
    };

    // FileDragAndDropTarget: audio files / folders dropped anywhere else in the window are appended,
    // a .gocue file is opened.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    /** Inserts audio cues at 'insertAt', or as the last children of the group at 'intoGroup' when that is a group. */
    void addCuesFromFiles (const juce::StringArray& files, int insertAt, int intoGroup = -1);
    void addCueViaDialog();
    /** Inserts a 페이드 인 / 페이드 아웃 cue after the selection, targeting the selected sound cue. */
    void addFadeCue();
    void addFadeOutCue();
    void addFadeCueOfMode (FadeMode mode);
    /** Inserts a devamp cue after the selection, targeting the selected audio cue. */
    void addDevampCue();
    /** Inserts an empty group after the selection (a sibling of it). */
    void addGroupCue();
    /** Inserts a control cue of 'kind' after the selection (targeting it, when the kind needs a target). */
    void addControlCue (ControlKind kind);
    /** Inserts a mic cue (device inputs 1-2) after the selection. */
    void addMicCue();
    /** Opens as many device inputs as the project's mic cues need. */
    void updateInputsWanted();
    /** Starts recording cue starts; stopping turns them into a timeline group of start cues. */
    void toggleSequenceRecording();
    /** Cue lists / carts (the tabs above the list). */
    void addContainer (bool cart);
    void renameContainer (int index);
    void removeContainer (int index);
    void setContainerGrid (int index);
    void toggleContainerCart (int index);
    /** Shows the table or the cart for the active container and refreshes the tabs. */
    void updateContainerView();
    void containersChanged() override;
    /** Wraps the selected cues (with their subtrees) in a new group. */
    void groupSelectedCues();
    /** Dissolves the selected group: its children move up one level. */
    void ungroupSelected();
    void setAllGroupsCollapsed (bool collapsed);
    void removeSelectedCues();
    void duplicateSelectedCue();
    void moveSelection (int delta);
    void moveRows (const std::vector<int>& rows, int insertIndex);
    void editCues (const std::vector<int>& rows, const juce::String& name, const std::function<void (Cue&)>& mutator);
    void setShowMode (bool shouldBeShowMode);
    void showLoadToTimeDialog();
    void showRenumberDialog();
    void deleteNumbersOfSelection();
    void copySelectedCues();
    void cutSelectedCues();
    void pasteCues();
    void pasteCueProperties();
    void showFindDialog();
    /** Selects the next cue whose number / name / file / notes contain the last search text (wraps around). */
    void findNext (bool includeCurrent);
    void saveCueTemplate();
    void clearCueTemplate();
    int findCueIndexByNumber (const juce::String& number) const;
    /** After the new-cue template and file info: restores a template plugin chain onto freshly added cues. */
    void restoreChainsForCues (const std::vector<juce::Uuid>& ids);
    void findMissingFiles();
    void showWarnings();
    int countBrokenCues() const;
    void newProject();
    void openProjectViaDialog();
    void saveProject (bool saveAs, std::function<void (bool ok)> then = {});
    void restorePluginChainsFromDocument (juce::StringArray& errors);
    void refreshFileInfoForAllCues();
    /** Copies the file that is about to be overwritten into the backup folder (settings permitting, once a minute). */
    void backupBeforeSave (const juce::File& file);
    /** Writes the unsaved state into the backup folder every backupIntervalSeconds while the project is dirty. */
    void autoBackupIfDue();
    /** Copies the live plugin chain states into a project (for saving and for undo snapshots). */
    void captureLivePluginStates (Project& project);
    /** After undo / redo: makes the engine's plugin chains match the restored project. */
    void reconcileChainsAfterRestore (const ProjectSnapshot& snapshot);
    /** Plugins may report state changes right after their state was restored; ignore those for a moment. */
    void ignorePluginChangesBriefly();
    void showPluginManager();
    void showAbout();
    void showAlert (const juce::String& title, const juce::String& message, bool isError);
    void updateTransportStandby();

    void timerCallback() override;
    void cueListStructureChanged() override;
    void cueChanged (int index) override;
    void cueSelectionChanged (int index) override;
    void playheadChanged (int index) override;
    void documentStateChanged() override;

    AudioEngine& engine;
    AppSettings& settings;
    juce::ApplicationCommandManager& commands;
    ProjectDocument document;
    PluginWindowManager pluginWindows;
    Scheduler scheduler;
    CueController controller;
    HotkeyListener hotkeyListener { *this };
    std::atomic<bool> unsavedChanges { false };
    double ignorePluginChangesUntilMs = 0.0;
    std::map<juce::String, double> lastSaveBackupByPath;
    double nextAutoBackupMs = 0.0;
    bool showMode = false;
    std::atomic<bool> showModeFlag { false };   // the same, for readers off the message thread (the updater's callbacks)
    OperationalKeys operationalKeys { *this };
    std::vector<juce::Component::SafePointer<juce::Component>> keyedWindows;   // top-level windows that carry operationalKeys
    int windowScanCountdown = 0;
    bool escHeld = false;                  // the Esc mapping fires on the down edge only (a held key must not hard-cut)
    double lastPanicKeyMs = -1.0e9;        // when the main window last handled an Esc press (the hook skips that press)
    bool autoStartOnOpenAllowed = true;
    void attachOperationalKeysToWindows();
    void installEscapePolicy (juce::Component& root);

    struct CueClipboard
    {
        std::vector<Cue> cues;
        std::vector<std::vector<PluginSlotState>> plugins;   // live chain states, parallel to 'cues'
    };
    CueClipboard clipboard;
    juce::String lastSearch;
    std::function<void()> closeContinuation;
    double closeDeadlineMs = 0.0;
    juce::Uuid closeCueId = juce::Uuid::null();
    juce::Uuid autoLoadedId = juce::Uuid::null();   // the standby cue we loaded automatically (dropped when the playhead moves)
    juce::Component::SafePointer<juce::AlertWindow> discardDialog;

    juce::MenuBarComponent menuBar;
    TransportBar transport;
    ContainerTabs containerTabs;
    CueTable table;
    CueCartView cart;
    CueInspector inspector;
    ActiveCuesPanel activeCues;
    FooterBar footer;
    std::unique_ptr<juce::FileChooser> chooser;
    bool dragOverWindow = false;
    bool activeCuesVisible = true;
    bool inspectorCollapsed = false;
    double inspectorFraction = 0.45, activeCuesFraction = 0.27;   // shares of the split areas: kept across window resizes
    int splitHeight = 0, splitWidth = 0, dragStartSize = 0;
    SplitDivider inspectorDivider { SplitDivider::Orientation::horizontal }, activeCuesDivider { SplitDivider::Orientation::vertical };

    void ensureInspectorShown();
    void showPanicSecondsMenu (juce::Point<int> screenPosition);
    void applyPanicSeconds (double seconds);
    std::unique_ptr<ManualWindow> manualWindow;   // made on first use, hidden on close

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace gocue
