#include "model/ProjectSerializer.h"

#include <cmath>
#include <limits>

namespace gocue
{

namespace
{
    /** Reads a JSON number as an int without the undefined float->int overflow of a plain cast. */
    int intProperty (const juce::var& v, const char* name, int defaultValue)
    {
        const auto value = v.getProperty (name, juce::var());

        if (value.isVoid())
            return defaultValue;

        const double d = (double) value;

        if (! std::isfinite (d))
            return defaultValue;

        return (int) juce::jlimit ((double) std::numeric_limits<int>::min(), (double) std::numeric_limits<int>::max(), d);
    }

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
        p.uniqueId         = intProperty (v, "uniqueId", 0);
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

    juce::var envelopeToVar (const Envelope& e)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("enabled", e.enabled);
        obj->setProperty ("linear", e.linear);
        obj->setProperty ("lockToTrim", e.lockToTrim);

        juce::Array<juce::var> points;

        for (const auto& p : e.points)
        {
            juce::Array<juce::var> pair;
            pair.add (p.x);
            pair.add (p.level);
            points.add (juce::var (pair));
        }

        obj->setProperty ("points", juce::var (points));
        return juce::var (obj);
    }

    Envelope envelopeFromVar (const juce::var& v)
    {
        Envelope e;

        if (v.getDynamicObject() == nullptr)
            return e;

        e.enabled    = (bool) v.getProperty ("enabled", false);
        e.linear     = (bool) v.getProperty ("linear", false);
        e.lockToTrim = (bool) v.getProperty ("lockToTrim", true);

        if (const auto* arr = v.getProperty ("points", juce::var()).getArray())
            for (const auto& item : *arr)
                if (const auto* pair = item.getArray(); pair != nullptr && pair->size() >= 2)
                    e.points.push_back ({ (double) (*pair)[0], (double) (*pair)[1] });

        e.sanitise();
        return e;
    }

    juce::var audioToVar (const AudioCueData& a)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("start", a.startSeconds);
        obj->setProperty ("end", a.endSeconds);
        obj->setProperty ("playCount", a.playCount);
        obj->setProperty ("infiniteLoop", a.infiniteLoop);
        obj->setProperty ("rate", a.rate);
        obj->setProperty ("preservePitch", a.preservePitch);
        obj->setProperty ("envelope", envelopeToVar (a.envelope));
        return juce::var (obj);
    }

    AudioCueData audioFromVar (const juce::var& v)
    {
        AudioCueData a;
        a.startSeconds  = (double) v.getProperty ("start", 0.0);
        a.endSeconds    = (double) v.getProperty ("end", -1.0);
        a.playCount     = intProperty (v, "playCount", 1);
        a.infiniteLoop  = (bool) v.getProperty ("infiniteLoop", false);
        a.rate          = (double) v.getProperty ("rate", 1.0);
        a.preservePitch = (bool) v.getProperty ("preservePitch", false);
        a.envelope      = envelopeFromVar (v.getProperty ("envelope", juce::var()));
        return a;
    }

    const char* secondTriggerToText (SecondTriggerAction a)
    {
        switch (a)
        {
            case SecondTriggerAction::nothing:         return "nothing";
            case SecondTriggerAction::panic:           return "panic";
            case SecondTriggerAction::stop:            return "stop";
            case SecondTriggerAction::hardStop:        return "hardStop";
            case SecondTriggerAction::hardStopRestart: return "hardStopRestart";
            case SecondTriggerAction::devamp:          return "devamp";
        }

        return "hardStopRestart";
    }

    SecondTriggerAction secondTriggerFromText (const juce::String& text)
    {
        if (text == "nothing")  return SecondTriggerAction::nothing;
        if (text == "panic")    return SecondTriggerAction::panic;
        if (text == "stop")     return SecondTriggerAction::stop;
        if (text == "hardStop") return SecondTriggerAction::hardStop;
        if (text == "devamp")   return SecondTriggerAction::devamp;
        return SecondTriggerAction::hardStopRestart;
    }

    const char* continueModeToText (ContinueMode m)
    {
        switch (m)
        {
            case ContinueMode::none:         return "none";
            case ContinueMode::autoContinue: return "autoContinue";
            case ContinueMode::autoFollow:   return "autoFollow";
        }

        return "none";
    }

    ContinueMode continueModeFromText (const juce::String& text)
    {
        if (text == "autoContinue") return ContinueMode::autoContinue;
        if (text == "autoFollow")   return ContinueMode::autoFollow;
        return ContinueMode::none;
    }

    const char* scopeToText (FadeStopScope s)
    {
        switch (s)
        {
            case FadeStopScope::peers: return "peers";
            case FadeStopScope::list:  return "list";
            case FadeStopScope::all:   return "all";
        }

        return "list";
    }

    FadeStopScope scopeFromText (const juce::String& text)
    {
        if (text == "peers") return FadeStopScope::peers;
        if (text == "all")   return FadeStopScope::all;
        return FadeStopScope::list;
    }

    juce::var cueToVar (const Cue& c, const juce::File& projectDir)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id", c.id.toString());
        obj->setProperty ("number", c.number);
        obj->setProperty ("name", c.name);
        obj->setProperty ("notes", c.notes);
        obj->setProperty ("color", c.color);
        obj->setProperty ("secondColor", c.secondColor);
        obj->setProperty ("useSecondColor", c.useSecondColor);
        obj->setProperty ("flagged", c.flagged);
        obj->setProperty ("armed", c.armed);
        obj->setProperty ("skipIfDisarmed", c.skipIfDisarmed);
        obj->setProperty ("autoLoad", c.autoLoad);
        obj->setProperty ("preWait", c.preWaitSeconds);
        obj->setProperty ("postWait", c.postWaitSeconds);
        obj->setProperty ("continueMode", continueModeToText (c.continueMode));
        obj->setProperty ("hotkey", c.hotkey);

        {
            auto* wc = new juce::DynamicObject();
            wc->setProperty ("enabled", c.wallClock.enabled);
            wc->setProperty ("hour", c.wallClock.hour);
            wc->setProperty ("minute", c.wallClock.minute);
            wc->setProperty ("second", c.wallClock.second);
            wc->setProperty ("days", c.wallClock.daysMask);
            obj->setProperty ("wallClock", juce::var (wc));

            auto* fs = new juce::DynamicObject();
            fs->setProperty ("enabled", c.fadeStopOthers.enabled);
            fs->setProperty ("seconds", c.fadeStopOthers.seconds);
            fs->setProperty ("scope", scopeToText (c.fadeStopOthers.scope));
            obj->setProperty ("fadeStopOthers", juce::var (fs));

            auto* dk = new juce::DynamicObject();
            dk->setProperty ("enabled", c.duck.enabled);
            dk->setProperty ("levelDb", c.duck.levelDb);
            dk->setProperty ("seconds", c.duck.seconds);
            obj->setProperty ("duck", juce::var (dk));
        }

        obj->setProperty ("file", c.file.getFullPathName());

        if (projectDir.isDirectory() && c.file != juce::File())
            obj->setProperty ("fileRelative", c.file.getRelativePathFrom (projectDir));

        obj->setProperty ("fadeOutMs", c.fadeOutMs);
        obj->setProperty ("gainDb", c.gainDb);
        obj->setProperty ("durationSeconds", c.durationSeconds);
        obj->setProperty ("audio", audioToVar (c.audio));
        obj->setProperty ("secondTrigger", secondTriggerToText (c.secondTrigger));
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

        c.number         = v.getProperty ("number", "").toString();
        c.name           = v.getProperty ("name", "").toString();
        c.notes          = v.getProperty ("notes", "").toString();
        c.color          = (int) v.getProperty ("color", 0);
        c.secondColor    = (int) v.getProperty ("secondColor", 0);
        c.useSecondColor = (bool) v.getProperty ("useSecondColor", false);
        c.flagged        = (bool) v.getProperty ("flagged", false);
        c.armed          = (bool) v.getProperty ("armed", true);
        c.skipIfDisarmed = (bool) v.getProperty ("skipIfDisarmed", false);
        c.autoLoad       = (bool) v.getProperty ("autoLoad", false);
        c.preWaitSeconds = (double) v.getProperty ("preWait", 0.0);
        c.postWaitSeconds = (double) v.getProperty ("postWait", 0.0);
        c.continueMode   = continueModeFromText (v.getProperty ("continueMode", "none").toString());
        c.hotkey         = v.getProperty ("hotkey", "").toString();

        if (const auto wc = v.getProperty ("wallClock", juce::var()); wc.getDynamicObject() != nullptr)
        {
            c.wallClock.enabled  = (bool) wc.getProperty ("enabled", false);
            c.wallClock.hour     = (int) wc.getProperty ("hour", 0);
            c.wallClock.minute   = (int) wc.getProperty ("minute", 0);
            c.wallClock.second   = (int) wc.getProperty ("second", 0);
            c.wallClock.daysMask = (int) wc.getProperty ("days", 0x7f);
        }

        if (const auto fs = v.getProperty ("fadeStopOthers", juce::var()); fs.getDynamicObject() != nullptr)
        {
            c.fadeStopOthers.enabled = (bool) fs.getProperty ("enabled", false);
            c.fadeStopOthers.seconds = (double) fs.getProperty ("seconds", 2.0);
            c.fadeStopOthers.scope   = scopeFromText (fs.getProperty ("scope", "list").toString());
        }

        if (const auto dk = v.getProperty ("duck", juce::var()); dk.getDynamicObject() != nullptr)
        {
            c.duck.enabled = (bool) dk.getProperty ("enabled", false);
            c.duck.levelDb = (double) dk.getProperty ("levelDb", -12.0);
            c.duck.seconds = (double) dk.getProperty ("seconds", 1.0);
        }

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

        c.fadeOutMs       = intProperty (v, "fadeOutMs", 0);
        c.gainDb          = (double) v.getProperty ("gainDb", 0.0);
        c.durationSeconds = (double) v.getProperty ("durationSeconds", 0.0);
        c.plugins         = pluginsFromVar (v.getProperty ("plugins", juce::var()));
        c.secondTrigger   = secondTriggerFromText (v.getProperty ("secondTrigger", "hardStopRestart").toString());

        const auto audio = v.getProperty ("audio", juce::var());

        if (audio.getDynamicObject() != nullptr)
        {
            c.audio = audioFromVar (audio);
        }
        else
        {
            // Version 1: a plain fade-in time becomes the integrated fade envelope.
            const int fadeInMs = juce::jlimit (0, Cue::maxFadeMs, intProperty (v, "fadeInMs", 0));
            c.audio.envelope = Envelope::fromFadeIn (fadeInMs / 1000.0);
        }

        c.sanitise();
        return c;
    }
    juce::var settingsToVar (const WorkspaceSettings& s)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("doubleGoSeconds", s.doubleGoSeconds);
        obj->setProperty ("requireKeyUp", s.requireKeyUp);
        obj->setProperty ("panicSeconds", s.panicSeconds);
        obj->setProperty ("autoNumber", s.autoNumber);
        obj->setProperty ("numberIncrement", s.numberIncrement);
        obj->setProperty ("autoLoadNewCues", s.autoLoadNewCues);
        obj->setProperty ("lockPlayheadToSelection", s.lockPlayheadToSelection);
        obj->setProperty ("startOnOpen", s.startOnOpen);
        obj->setProperty ("startOnOpenCue", s.startOnOpenCue);
        obj->setProperty ("startOnClose", s.startOnClose);
        obj->setProperty ("startOnCloseCue", s.startOnCloseCue);
        obj->setProperty ("maxLevelDb", s.maxLevelDb);
        obj->setProperty ("minLevelDb", s.minLevelDb);
        obj->setProperty ("copyFilesIntoProject", s.copyFilesIntoProject);
        obj->setProperty ("autoBackup", s.autoBackup);
        obj->setProperty ("backupIntervalSeconds", s.backupIntervalSeconds);
        obj->setProperty ("backupBeforeSave", s.backupBeforeSave);
        obj->setProperty ("rotateBackups", s.rotateBackups);
        obj->setProperty ("rowSize", s.rowSize);
        obj->setProperty ("hasCueTemplate", s.hasCueTemplate);

        if (s.hasCueTemplate)
            obj->setProperty ("cueTemplate", cueToVar (s.cueTemplate, juce::File()));

        return juce::var (obj);
    }

    WorkspaceSettings settingsFromVar (const juce::var& v)
    {
        WorkspaceSettings s;

        if (v.getDynamicObject() == nullptr)
            return s;

        s.rowSize = intProperty (v, "rowSize", s.rowSize);
        s.hasCueTemplate = (bool) v.getProperty ("hasCueTemplate", false);

        if (const auto t = v.getProperty ("cueTemplate", juce::var()); s.hasCueTemplate && t.getDynamicObject() != nullptr)
            s.cueTemplate = cueFromVar (t, juce::File(), nullptr);
        else
            s.hasCueTemplate = false;

        s.doubleGoSeconds         = (double) v.getProperty ("doubleGoSeconds", s.doubleGoSeconds);
        s.requireKeyUp            = (bool) v.getProperty ("requireKeyUp", s.requireKeyUp);
        s.panicSeconds            = (double) v.getProperty ("panicSeconds", s.panicSeconds);
        s.autoNumber              = (bool) v.getProperty ("autoNumber", s.autoNumber);
        s.numberIncrement         = (double) v.getProperty ("numberIncrement", s.numberIncrement);
        s.autoLoadNewCues         = (bool) v.getProperty ("autoLoadNewCues", s.autoLoadNewCues);
        s.lockPlayheadToSelection = (bool) v.getProperty ("lockPlayheadToSelection", s.lockPlayheadToSelection);
        s.startOnOpen             = (bool) v.getProperty ("startOnOpen", s.startOnOpen);
        s.startOnOpenCue          = v.getProperty ("startOnOpenCue", s.startOnOpenCue).toString();
        s.startOnClose            = (bool) v.getProperty ("startOnClose", s.startOnClose);
        s.startOnCloseCue         = v.getProperty ("startOnCloseCue", s.startOnCloseCue).toString();
        s.maxLevelDb              = (double) v.getProperty ("maxLevelDb", s.maxLevelDb);
        s.minLevelDb              = (double) v.getProperty ("minLevelDb", s.minLevelDb);
        s.copyFilesIntoProject    = (bool) v.getProperty ("copyFilesIntoProject", s.copyFilesIntoProject);
        s.autoBackup              = (bool) v.getProperty ("autoBackup", s.autoBackup);
        s.backupIntervalSeconds   = intProperty (v, "backupIntervalSeconds", s.backupIntervalSeconds);
        s.backupBeforeSave        = (bool) v.getProperty ("backupBeforeSave", s.backupBeforeSave);
        s.rotateBackups           = (bool) v.getProperty ("rotateBackups", s.rotateBackups);
        s.sanitise();
        return s;
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
    root->setProperty ("settings", settingsToVar (project.settings));

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

    const int version = intProperty (root, "version", 1);

    if (version > currentVersion && warnings != nullptr)
        warnings->add ("This project was saved by a newer GoCue (file version " + juce::String (version)
                       + ", this build reads version " + juce::String (currentVersion)
                       + "). Unknown settings were ignored.");

    Project project;
    project.name = root.getProperty ("name", "").toString();

    if (const auto* cues = root.getProperty ("cues", juce::var()).getArray())
    {
        juce::StringArray seenIds;

        for (const auto& item : *cues)
        {
            if (item.getDynamicObject() == nullptr)
            {
                if (warnings != nullptr)
                    warnings->add ("Skipped a malformed cue entry");

                continue;
            }

            auto cue = cueFromVar (item, projectDir, warnings);

            // Two cues must never share an id: they would share one player and one plugin chain.
            if (seenIds.contains (cue.id.toString()))
            {
                cue.id = juce::Uuid();

                if (warnings != nullptr)
                    warnings->add ("Duplicate cue id for \"" + cue.name + "\" - assigned a new one");
            }

            seenIds.add (cue.id.toString());
            project.cues.push_back (std::move (cue));
        }
    }

    const auto master = root.getProperty ("master", juce::var());

    if (master.getDynamicObject() != nullptr)
        project.masterPlugins = pluginsFromVar (master.getProperty ("plugins", juce::var()));

    project.settings = settingsFromVar (root.getProperty ("settings", juce::var()));

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
