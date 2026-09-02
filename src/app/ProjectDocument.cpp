#include "app/ProjectDocument.h"

namespace gocue
{

ProjectDocument::ProjectDocument()
{
    clock = [] { return juce::Time::getMillisecondCounterHiRes(); };
    cues.addListener (this);
}

ProjectDocument::~ProjectDocument()
{
    cues.removeListener (this);
}

juce::String ProjectDocument::getDisplayName() const
{
    if (hasFile())
        return file.getFileNameWithoutExtension();

    return juce::String::fromUTF8 ("제목 없음");
}

juce::String ProjectDocument::getWindowTitle() const
{
    juce::String title ("GoCue - ");
    title << getDisplayName();

    if (dirty)
        title << "*";

    return title;
}

void ProjectDocument::newProject()
{
    history.clear();
    cues.clear();
    masterPlugins.clear();
    file = juce::File();
    markClean();
}

juce::Result ProjectDocument::parse (const juce::File& projectFile, Project& out, juce::StringArray* warnings)
{
    return ProjectSerializer::load (projectFile, out, warnings);
}

void ProjectDocument::adopt (Project project, const juce::File& projectFile)
{
    history.clear();
    cues.replaceAll (std::move (project.cues));
    masterPlugins = std::move (project.masterPlugins);
    file = projectFile;
    markClean();
}

juce::Result ProjectDocument::load (const juce::File& projectFile, juce::StringArray* warnings)
{
    Project project;
    const auto result = parse (projectFile, project, warnings);

    if (result.failed())
        return result;

    adopt (std::move (project), projectFile);
    return juce::Result::ok();
}

juce::Result ProjectDocument::save (const juce::File& projectFile, const std::function<void (Project&)>& decorate)
{
    auto project = toProject();

    if (decorate)
        decorate (project);

    const auto result = ProjectSerializer::save (project, projectFile);

    if (result.failed())
        return result;

    // Keep the in-memory snapshot in sync with what was written (plugin states).
    masterPlugins = project.masterPlugins;

    for (size_t i = 0; i < project.cues.size() && (int) i < cues.size(); ++i)
        if (project.cues[i].id == cues.get ((int) i).id)
            cues.setPluginStatesQuietly ((int) i, project.cues[i].plugins);

    file = projectFile;
    markClean();
    return juce::Result::ok();
}

void ProjectDocument::markDirty()
{
    if (dirty)
        return;

    dirty = true;
    notify();
}

void ProjectDocument::markClean()
{
    dirty = false;
    notify();
}

Project ProjectDocument::toProject() const
{
    Project project;
    project.name = getDisplayName();
    project.cues = cues.getAll();
    project.masterPlugins = masterPlugins;
    return project;
}

//==============================================================================
ProjectSnapshot ProjectDocument::makeSnapshot (bool capturePluginStates) const
{
    ProjectSnapshot snapshot;
    snapshot.project = toProject();

    if (const auto* selected = cues.getSelected())
        snapshot.selectedId = selected->id;

    if (capturePluginStates && snapshotDecorator)
    {
        snapshotDecorator (snapshot.project);
        snapshot.pluginStatesCaptured = true;
    }

    return snapshot;
}

void ProjectDocument::restoreSnapshot (const ProjectSnapshot& snapshot)
{
    cues.replaceAll (snapshot.project.cues);
    masterPlugins = snapshot.project.masterPlugins;

    if (! snapshot.selectedId.isNull())
    {
        const int index = cues.indexOf (snapshot.selectedId);

        if (index >= 0)
            cues.setSelectedIndex (index);
    }

    dirty = true;
    notify();

    if (onSnapshotRestored)
        onSnapshotRestored (snapshot);
}

void ProjectDocument::perform (const juce::String& name, const std::function<void()>& edit, const EditOptions& options)
{
    history.push (makeSnapshot (options.capturePluginStates), name, options.coalesceKey, clock ? clock() : 0.0);

    if (edit)
        edit();

    notify();
}

bool ProjectDocument::undo()
{
    auto restored = history.undo ([this] (bool capture) { return makeSnapshot (capture); });

    if (! restored.has_value())
        return false;

    restoreSnapshot (*restored);
    return true;
}

bool ProjectDocument::redo()
{
    auto restored = history.redo ([this] (bool capture) { return makeSnapshot (capture); });

    if (! restored.has_value())
        return false;

    restoreSnapshot (*restored);
    return true;
}

void ProjectDocument::notify()
{
    listeners.call ([] (Listener& l) { l.documentStateChanged(); });
}

} // namespace gocue
