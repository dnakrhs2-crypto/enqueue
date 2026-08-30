#pragma once

#include "app/AppSettings.h"
#include "app/ProjectDocument.h"
#include "audio/AudioEngine.h"
#include "ui/CueInspector.h"
#include "ui/CueTable.h"
#include "ui/PluginWindows.h"
#include "ui/TransportBar.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace gocue
{

/** The single main window content: menu bar / transport / cue table / inspector.
    Owns the project document, the plugin editor windows and dispatches every command. */
class MainComponent : public juce::Component,
                      public juce::ApplicationCommandTarget,
                      public juce::MenuBarModel,
                      private juce::Timer,
                      private CueList::Listener,
                      private ProjectDocument::Listener
{
public:
    MainComponent (AudioEngine& engine, AppSettings& settings, juce::ApplicationCommandManager& commands);
    ~MainComponent() override;

    void resized() override;
    void paint (juce::Graphics& g) override;

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

    std::function<void (const juce::String& title)> onWindowTitleChanged;

private:
    void go();
    juce::Uuid resolveTargetCue (bool ignoreFadingOut) const;
    void addCuesFromFiles (const juce::StringArray& files, int insertAt);
    void addCueViaDialog();
    void removeSelectedCue();
    void duplicateSelectedCue();
    void newProject();
    void openProjectViaDialog();
    void saveProject (bool saveAs, std::function<void (bool ok)> then = {});
    void restorePluginChainsFromDocument (juce::StringArray& errors);
    void refreshFileInfoForAllCues();
    void showPluginManager();
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

    juce::MenuBarComponent menuBar;
    TransportBar transport;
    CueTable table;
    CueInspector inspector;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace gocue
