#pragma once

#include "model/CueList.h"
#include "model/ProjectSerializer.h"

namespace gocue
{

/** The open project: cue list + master plugin state + file / dirty bookkeeping. */
class ProjectDocument : private CueList::Listener
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        /** File, name or dirty state changed (title bar refresh). */
        virtual void documentStateChanged() = 0;
    };

    ProjectDocument();
    ~ProjectDocument() override;

    CueList cues;
    std::vector<PluginSlotState> masterPlugins;

    const juce::File& getFile() const noexcept { return file; }
    bool hasFile() const noexcept { return file != juce::File(); }
    bool isDirty() const noexcept { return dirty; }

    /** File name without extension, or a placeholder for unsaved projects. */
    juce::String getDisplayName() const;
    /** e.g. "GoCue - show.gocue*" */
    juce::String getWindowTitle() const;

    void newProject();
    juce::Result load (const juce::File& projectFile, juce::StringArray* warnings = nullptr);
    juce::Result save (const juce::File& projectFile);

    void markDirty();
    void markClean();

    Project toProject() const;

    void addListener (Listener* l) { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

private:
    void cueListStructureChanged() override { markDirty(); }
    void cueChanged (int) override { markDirty(); }
    void notify();

    juce::File file;
    bool dirty = false;
    juce::ListenerList<Listener> listeners;
};

} // namespace gocue
