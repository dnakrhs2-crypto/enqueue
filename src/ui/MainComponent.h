#pragma once

#include "app/AppSettings.h"
#include "app/CueController.h"
#include "app/ProjectDocument.h"
#include "audio/AudioEngine.h"
#include "ui/CueInspector.h"
#include "ui/CueTable.h"
#include "ui/PluginWindows.h"
#include "ui/TransportBar.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>

namespace gocue
{

/** The single main window content: menu bar / transport / cue table / inspector.
    Owns the project document, the cue controller, the plugin editor windows and dispatches every command. */
class MainComponent : public juce::Component,
                      public juce::ApplicationCommandTarget,
                      public juce::MenuBarModel,
                      private juce::FileDragAndDropTarget,
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

    /** Opens a .gocue file passed on the command line (file association / drag onto the exe). */
    void openProjectFromCommandLine (const juce::String& commandLine);
    void openProjectFile (const juce::File& file);

    /** Runs 'action' immediately, or once the user has saved / discarded unsaved changes. */
    void confirmDiscardChangesThen (std::function<void()> action);

    /** Thread-safe snapshot of the dirty flag (the updater asks from a background thread). */
    bool hasUnsavedChanges() const noexcept { return unsavedChanges.load (std::memory_order_relaxed); }

    std::function<void (const juce::String& title)> onWindowTitleChanged;

private:
    // FileDragAndDropTarget: audio files / folders dropped anywhere else in the window are appended,
    // a .gocue file is opened.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    void addCuesFromFiles (const juce::StringArray& files, int insertAt);
    void addCueViaDialog();
    void removeSelectedCue();
    void duplicateSelectedCue();
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
    void documentStateChanged() override;

    AudioEngine& engine;
    AppSettings& settings;
    juce::ApplicationCommandManager& commands;
    ProjectDocument document;
    PluginWindowManager pluginWindows;
    CueController controller;
    std::atomic<bool> unsavedChanges { false };
    double ignorePluginChangesUntilMs = 0.0;
    std::map<juce::String, double> lastSaveBackupByPath;
    double nextAutoBackupMs = 0.0;

    juce::MenuBarComponent menuBar;
    TransportBar transport;
    CueTable table;
    CueInspector inspector;
    std::unique_ptr<juce::FileChooser> chooser;
    bool dragOverWindow = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace gocue
