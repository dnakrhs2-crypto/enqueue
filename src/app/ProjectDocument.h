#pragma once

#include "app/ProjectHistory.h"
#include "model/CueList.h"
#include "model/ProjectSerializer.h"

#include <functional>

namespace gocue
{

/** The open project: cue list + master plugin state + file / dirty bookkeeping + undo history. */
class ProjectDocument : private CueList::Listener
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        /** File, name, dirty state or undo history changed (title bar / menu refresh). */
        virtual void documentStateChanged() = 0;
    };

    struct EditOptions
    {
        juce::String coalesceKey;          // non-empty: merge with the previous step of the same key (slider drags)
        bool capturePluginStates = false;  // structural edits: snapshot the live plugin chains too
    };

    ProjectDocument();
    ~ProjectDocument() override;

    CueList cues;
    std::vector<PluginSlotState> masterPlugins;
    std::vector<AudioPatch> patches;   // never empty; patches[0] is the default
    WorkspaceSettings settings;

    /** Replaces the settings (not an undo step: like QLab, settings sit outside the undo history). */
    void setSettings (const WorkspaceSettings& newSettings);
    /** Replaces the audio patches (not an undo step either). An empty list gets the default patch. */
    void setPatches (std::vector<AudioPatch> newPatches);
    const AudioPatch* findPatch (const juce::Uuid& id) const noexcept;
    /** The cue's patch, falling back to the default patch. */
    const AudioPatch& patchForCue (const Cue& cue) const noexcept;
    /** Number of cue outputs of the cue's patch (the level matrix columns). */
    int cueOutputsFor (const Cue& cue) const noexcept { return patchForCue (cue).numCueOutputs; }

    const juce::File& getFile() const noexcept { return file; }
    bool hasFile() const noexcept { return file != juce::File(); }
    bool isDirty() const noexcept { return dirty; }

    /** File name without extension, or a placeholder for unsaved projects. */
    juce::String getDisplayName() const;
    /** e.g. "GoCue - show.gocue*" */
    juce::String getWindowTitle() const;

    void newProject();
    /** Parses a project file without touching this document (validate first, then adopt()). */
    static juce::Result parse (const juce::File& projectFile, Project& out, juce::StringArray* warnings = nullptr);
    /** Replaces the document's content with a parsed project (clears the undo history). */
    void adopt (Project project, const juce::File& projectFile);
    /** parse() + adopt() in one step. */
    juce::Result load (const juce::File& projectFile, juce::StringArray* warnings = nullptr);
    /** @param decorate  optional hook that fills in live state (plugin chains) right before writing. */
    juce::Result save (const juce::File& projectFile, const std::function<void (Project&)>& decorate = {});

    void markDirty();
    void markClean();

    Project toProject() const;

    //==========================================================================
    // Undo / redo

    /** Records an undo step named 'name', then runs 'edit', which changes the model through the normal
        CueList / masterPlugins API. Every user edit must go through here. */
    void perform (const juce::String& name, const std::function<void()>& edit, const EditOptions& options = {});

    bool canUndo() const noexcept { return history.canUndo(); }
    bool canRedo() const noexcept { return history.canRedo(); }
    juce::String getUndoName() const { return history.getUndoName(); }
    juce::String getRedoName() const { return history.getRedoName(); }
    bool undo();
    bool redo();
    ProjectHistory& getHistory() noexcept { return history; }

    /** Adds live state (plugin chain states) to a snapshot; only called for capturePluginStates edits. Set by the app. */
    std::function<void (Project&)> snapshotDecorator;
    /** Called after undo / redo replaced the model so the app can reconcile live objects (plugin chains). */
    std::function<void (const ProjectSnapshot&)> onSnapshotRestored;
    /** Millisecond clock used for coalescing; tests inject a fake one. */
    std::function<double()> clock;

    ProjectSnapshot makeSnapshot (bool capturePluginStates) const;
    void restoreSnapshot (const ProjectSnapshot& snapshot);

    void addListener (Listener* l) { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

private:
    void cueListStructureChanged() override { markDirty(); }
    void cueChanged (int) override { markDirty(); }
    void notify();

    juce::File file;
    bool dirty = false;
    ProjectHistory history;
    juce::ListenerList<Listener> listeners;
};

} // namespace gocue
