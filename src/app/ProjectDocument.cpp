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

juce::Result ProjectDocument::load (const juce::File& projectFile, juce::StringArray* warnings)
{
    Project project;
    const auto result = ProjectSerializer::load (projectFile, project, warnings);

    if (result.failed())
        return result;

    cues.replaceAll (std::move (project.cues));
    masterPlugins = std::move (project.masterPlugins);
    file = projectFile;
    markClean();
    return juce::Result::ok();
}

juce::Result ProjectDocument::save (const juce::File& projectFile)
{
    const auto result = ProjectSerializer::save (toProject(), projectFile);

    if (result.failed())
        return result;

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
