#include "model/ProjectSerializer.h"

namespace gocue
{

namespace
{
    juce::var pluginToVar (const PluginSlotState& p)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("format", p.format);
        obj->setProperty ("name", p.name);
        obj->setProperty ("fileOrIdentifier", p.fileOrIdentifier);
        obj->setProperty ("uniqueId", p.uniqueId);
        obj->setProperty ("state", p.stateBase64);
        obj->setProperty ("description", p.descriptionXml);
        obj->setProperty ("bypassed", p.bypassed);
        return juce::var (obj);
    }

    PluginSlotState pluginFromVar (const juce::var& v)
    {
        PluginSlotState p;
        p.format           = v.getProperty ("format", "VST3").toString();
        p.name             = v.getProperty ("name", "").toString();
        p.fileOrIdentifier = v.getProperty ("fileOrIdentifier", "").toString();
        p.uniqueId         = (int) v.getProperty ("uniqueId", 0);
        p.stateBase64      = v.getProperty ("state", "").toString();
        p.descriptionXml   = v.getProperty ("description", "").toString();
        p.bypassed         = (bool) v.getProperty ("bypassed", false);
        return p;
    }

    juce::var pluginsToVar (const std::vector<PluginSlotState>& plugins)
    {
        juce::Array<juce::var> arr;

        for (const auto& p : plugins)
            arr.add (pluginToVar (p));

        return juce::var (arr);
    }

    std::vector<PluginSlotState> pluginsFromVar (const juce::var& v)
    {
        std::vector<PluginSlotState> result;

        if (const auto* arr = v.getArray())
            for (const auto& item : *arr)
                if (item.getDynamicObject() != nullptr)
                    result.push_back (pluginFromVar (item));

        return result;
    }

    juce::var cueToVar (const Cue& c, const juce::File& projectDir)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id", c.id.toString());
        obj->setProperty ("name", c.name);
        obj->setProperty ("file", c.file.getFullPathName());

        if (projectDir.isDirectory() && c.file != juce::File())
            obj->setProperty ("fileRelative", c.file.getRelativePathFrom (projectDir));

        obj->setProperty ("fadeInMs", c.fadeInMs);
        obj->setProperty ("fadeOutMs", c.fadeOutMs);
        obj->setProperty ("gainDb", c.gainDb);
        obj->setProperty ("durationSeconds", c.durationSeconds);
        obj->setProperty ("plugins", pluginsToVar (c.plugins));
        return juce::var (obj);
    }

    Cue cueFromVar (const juce::var& v, const juce::File& projectDir, juce::StringArray* warnings)
    {
        Cue c;

        const auto idText = v.getProperty ("id", "").toString();

        if (idText.isNotEmpty())
        {
            const juce::Uuid parsed (idText);

            if (! parsed.isNull())
                c.id = parsed;
        }

        c.name = v.getProperty ("name", "").toString();

        const auto path = v.getProperty ("file", "").toString();

        if (path.isNotEmpty() && juce::File::isAbsolutePath (path))
            c.file = juce::File (path);

        if (! c.file.existsAsFile())
        {
            const auto relative = v.getProperty ("fileRelative", "").toString();

            if (relative.isNotEmpty() && projectDir.isDirectory())
            {
                const auto candidate = projectDir.getChildFile (relative);

                if (candidate.existsAsFile())
                    c.file = candidate;
            }
        }

        if (c.file != juce::File() && ! c.file.existsAsFile())
        {
            c.fileMissing = true;

            if (warnings != nullptr)
                warnings->add ("File not found: " + c.file.getFullPathName());
        }

        c.fadeInMs        = (int) v.getProperty ("fadeInMs", 0);
        c.fadeOutMs       = (int) v.getProperty ("fadeOutMs", 0);
        c.gainDb          = (double) v.getProperty ("gainDb", 0.0);
        c.durationSeconds = (double) v.getProperty ("durationSeconds", 0.0);
        c.plugins         = pluginsFromVar (v.getProperty ("plugins", juce::var()));
        c.sanitise();
        return c;
    }
} // namespace

namespace ProjectSerializer
{

juce::var toVar (const Project& project, const juce::File& projectDir)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("app", "GoCue");
    root->setProperty ("version", currentVersion);
    root->setProperty ("name", project.name);

    juce::Array<juce::var> cues;

    for (const auto& c : project.cues)
        cues.add (cueToVar (c, projectDir));

    root->setProperty ("cues", juce::var (cues));

    auto* master = new juce::DynamicObject();
    master->setProperty ("plugins", pluginsToVar (project.masterPlugins));
    root->setProperty ("master", juce::var (master));

    return juce::var (root);
}

juce::String toJson (const Project& project, const juce::File& projectDir)
{
    return juce::JSON::toString (toVar (project, projectDir), false);
}

juce::Result fromJson (const juce::String& json, Project& out, juce::StringArray* warnings, const juce::File& projectDir)
{
    juce::var root;
    const auto parsed = juce::JSON::parse (json, root);

    if (parsed.failed())
        return juce::Result::fail ("Invalid project file (JSON): " + parsed.getErrorMessage());

    // Note: var::isObject() is also true for arrays, so check for a real JSON object.
    if (root.getDynamicObject() == nullptr)
        return juce::Result::fail ("Invalid project file: top level is not an object");

    const int version = (int) root.getProperty ("version", 1);

    if (version > currentVersion && warnings != nullptr)
        warnings->add ("This project was saved by a newer GoCue (file version " + juce::String (version)
                       + ", this build reads version " + juce::String (currentVersion)
                       + "). Unknown settings were ignored.");

    Project project;
    project.name = root.getProperty ("name", "").toString();

    if (const auto* cues = root.getProperty ("cues", juce::var()).getArray())
    {
        for (const auto& item : *cues)
        {
            if (item.getDynamicObject() != nullptr)
                project.cues.push_back (cueFromVar (item, projectDir, warnings));
            else if (warnings != nullptr)
                warnings->add ("Skipped a malformed cue entry");
        }
    }

    const auto master = root.getProperty ("master", juce::var());

    if (master.getDynamicObject() != nullptr)
        project.masterPlugins = pluginsFromVar (master.getProperty ("plugins", juce::var()));

    out = std::move (project);
    return juce::Result::ok();
}

juce::Result save (const Project& project, const juce::File& file)
{
    const auto dir = file.getParentDirectory();

    if (! dir.exists())
    {
        const auto created = dir.createDirectory();

        if (created.failed())
            return created;
    }

    const auto json = toJson (project, dir);

    if (! file.replaceWithText (json))
        return juce::Result::fail ("Could not write " + file.getFullPathName());

    return juce::Result::ok();
}

juce::Result load (const juce::File& file, Project& out, juce::StringArray* warnings)
{
    if (! file.existsAsFile())
        return juce::Result::fail ("File not found: " + file.getFullPathName());

    return fromJson (file.loadFileAsString(), out, warnings, file.getParentDirectory());
}

} // namespace ProjectSerializer
} // namespace gocue
