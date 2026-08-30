#include "app/ProjectDocument.h"

namespace gocue
{

ProjectDocument::ProjectDocument()
{
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

void ProjectDocument::notify()
{
    listeners.call ([] (Listener& l) { l.documentStateChanged(); });
}

} // namespace gocue
