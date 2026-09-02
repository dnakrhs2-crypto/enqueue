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

        if (! a.slices.empty())
        {
            juce::Array<juce::var> slices;

            for (const auto& s : a.slices)
            {
                juce::Array<juce::var> pair;
                pair.add (s.seconds);
                pair.add (s.playCount);
                slices.add (juce::var (pair));
            }

            obj->setProperty ("slices", juce::var (slices));
            obj->setProperty ("firstSliceCount", a.firstSliceCount);
        }
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

        if (const auto* slices = v.getProperty ("slices", juce::var()).getArray())
            for (const auto& item : *slices)
                if (const auto* pair = item.getArray(); pair != nullptr && pair->size() >= 2)
                    a.slices.push_back ({ (double) (*pair)[0], (int) (*pair)[1] });

        a.firstSliceCount = intProperty (v, "firstSliceCount", 1);
        a.rate          = (double) v.getProperty ("rate", 1.0);
        a.preservePitch = (bool) v.getProperty ("preservePitch", false);
        a.envelope      = envelopeFromVar (v.getProperty ("envelope", juce::var()));
        return a;
    }

    juce::var flagsToVar (const std::vector<char>& flags)
    {
        juce::Array<juce::var> arr;

        for (char f : flags)
            arr.add (f != 0);

        return juce::var (arr);
    }

    std::vector<char> flagsFromVar (const juce::var& v)
    {
        std::vector<char> result;

        if (const auto* arr = v.getArray())
            for (const auto& item : *arr)
                result.push_back ((bool) item ? 1 : 0);

        return result;
    }

    juce::var fadeToVar (const FadeCueData& f)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("target", f.targetId.isNull() ? juce::String() : f.targetId.toString());
        obj->setProperty ("duration", f.durationSeconds);
        obj->setProperty ("relative", f.relative);
        obj->setProperty ("stopTargetWhenDone", f.stopTargetWhenDone);
        obj->setProperty ("fadeLevels", f.fadeLevels);
        obj->setProperty ("mainDb", dbToVar (f.mainDb));
        obj->setProperty ("levels", f.levels.toVar());
        obj->setProperty ("mainActive", f.mainActive);
        obj->setProperty ("inputActive", flagsToVar (f.inputActive));
        obj->setProperty ("outputActive", flagsToVar (f.outputActive));

        juce::Array<juce::var> rows;

        for (const auto& row : f.crosspointActive)
            rows.add (flagsToVar (row));

        obj->setProperty ("crosspointActive", juce::var (rows));
        obj->setProperty ("fadeRate", f.fadeRate);
        obj->setProperty ("rate", f.rate);

        juce::Array<juce::var> params;

        for (const auto& p : f.params)
        {
            auto* po = new juce::DynamicObject();
            po->setProperty ("slot", p.slot);
            po->setProperty ("parameter", p.parameter);
            po->setProperty ("value", p.value);
            po->setProperty ("active", p.active);
            params.add (juce::var (po));
        }

        obj->setProperty ("params", juce::var (params));
        obj->setProperty ("curve", f.curve.toVar());
        return juce::var (obj);
    }

    FadeCueData fadeFromVar (const juce::var& v)
    {
        FadeCueData f;

        if (v.getDynamicObject() == nullptr)
            return f;

        f.targetId = juce::Uuid (v.getProperty ("target", "").toString());
        f.durationSeconds = (double) v.getProperty ("duration", 5.0);
        f.relative = (bool) v.getProperty ("relative", false);
        f.stopTargetWhenDone = (bool) v.getProperty ("stopTargetWhenDone", false);
        f.fadeLevels = (bool) v.getProperty ("fadeLevels", true);
        f.mainDb = dbFromVar (v.getProperty ("mainDb", 0.0), 0.0);
        f.levels = LevelMatrix::fromVar (v.getProperty ("levels", juce::var()));
        f.mainActive = (bool) v.getProperty ("mainActive", true);
        f.inputActive = flagsFromVar (v.getProperty ("inputActive", juce::var()));
        f.outputActive = flagsFromVar (v.getProperty ("outputActive", juce::var()));

        if (const auto* rows = v.getProperty ("crosspointActive", juce::var()).getArray())
            for (const auto& row : *rows)
                f.crosspointActive.push_back (flagsFromVar (row));

        f.fadeRate = (bool) v.getProperty ("fadeRate", false);
        f.rate = (double) v.getProperty ("rate", 1.0);

        if (const auto* params = v.getProperty ("params", juce::var()).getArray())
        {
            for (const auto& pv : *params)
            {
                if (pv.getDynamicObject() == nullptr)
                    continue;

                ParamFade p;
                p.slot = intProperty (pv, "slot", 0);
                p.parameter = intProperty (pv, "parameter", 0);
                p.value = (float) (double) pv.getProperty ("value", 0.0);
                p.active = (bool) pv.getProperty ("active", true);
                f.params.push_back (p);
            }
        }

        f.curve = FadeCurve::fromVar (v.getProperty ("curve", juce::var()));
        f.sanitise();
        return f;
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
        obj->setProperty ("channels", c.numChannels);

        if (c.levels.numInputs() > 0 || c.levels.numOutputs() > 0)
            obj->setProperty ("levels", c.levels.toVar());

        if (c.trim.mainDb != 0.0 || ! c.trim.outputDb.empty())
            obj->setProperty ("trim", c.trim.toVar());

        if (! c.patchId.isNull())
            obj->setProperty ("patch", c.patchId.toString());
        obj->setProperty ("type", c.type == CueType::fade ? "fade" : c.type == CueType::devamp ? "devamp" : c.type == CueType::group ? "group"
                                : c.type == CueType::control ? "control" : c.type == CueType::mic ? "mic" : "audio");

        if (c.type == CueType::mic)
        {
            auto* mobj = new juce::DynamicObject();
            mobj->setProperty ("firstInput", c.mic.firstInput);
            mobj->setProperty ("numInputs", c.mic.numInputs);
            obj->setProperty ("mic", juce::var (mobj));
        }

        if (c.type == CueType::control)
        {
            static const char* const kinds[] = { "start", "stop", "pause", "load", "reset", "goto", "wait", "memo", "arm", "disarm", "target" };
            auto* cobj = new juce::DynamicObject();
            cobj->setProperty ("kind", kinds[juce::jlimit (0, 10, (int) c.control.kind)]);
            cobj->setProperty ("target", c.control.targetId.isNull() ? juce::String() : c.control.targetId.toString());
            cobj->setProperty ("secondTarget", c.control.secondTargetId.isNull() ? juce::String() : c.control.secondTargetId.toString());
            cobj->setProperty ("seconds", c.control.seconds);
            obj->setProperty ("control", juce::var (cobj));
        }

        if (! c.parentId.isNull())
            obj->setProperty ("parent", c.parentId.toString());

        if (c.type == CueType::group)
        {
            auto* gobj = new juce::DynamicObject();
            gobj->setProperty ("mode", c.group.mode == GroupMode::timeline ? "timeline"
                                     : c.group.mode == GroupMode::playlist ? "playlist"
                                     : c.group.mode == GroupMode::startFirstEnter ? "startFirstEnter"
                                     : c.group.mode == GroupMode::startFirst ? "startFirst" : "random");
            gobj->setProperty ("collapsed", c.group.collapsed);
            gobj->setProperty ("shuffle", c.group.shuffle);
            gobj->setProperty ("loop", c.group.loop);
            gobj->setProperty ("crossfade", c.group.crossfade);
            gobj->setProperty ("crossfadeSeconds", c.group.crossfadeSeconds);
            obj->setProperty ("group", juce::var (gobj));
        }

        if (c.type == CueType::fade)
        {
            obj->setProperty ("fade", fadeToVar (c.fade));
        }
        else if (c.type == CueType::devamp)
        {
            auto* d = new juce::DynamicObject();
            d->setProperty ("target", c.devamp.targetId.isNull() ? juce::String() : c.devamp.targetId.toString());
            d->setProperty ("startNextCue", c.devamp.startNextCue);
            d->setProperty ("stopTarget", c.devamp.stopTarget);
            obj->setProperty ("devamp", juce::var (d));
        }
        else
        {
            obj->setProperty ("audio", audioToVar (c.audio));
        }

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
        c.numChannels     = intProperty (v, "channels", 0);
        c.levels          = LevelMatrix::fromVar (v.getProperty ("levels", juce::var()));
        c.trim            = TrimLevels::fromVar (v.getProperty ("trim", juce::var()));
        c.patchId         = juce::Uuid (v.getProperty ("patch", "").toString());
        c.plugins         = pluginsFromVar (v.getProperty ("plugins", juce::var()));
        c.secondTrigger   = secondTriggerFromText (v.getProperty ("secondTrigger", "hardStopRestart").toString());
        {
            const auto typeText = v.getProperty ("type", "audio").toString();
            c.type = typeText == "fade" ? CueType::fade : typeText == "devamp" ? CueType::devamp : typeText == "group" ? CueType::group
                   : typeText == "control" ? CueType::control : typeText == "mic" ? CueType::mic : CueType::audio;
        }

        if (const auto m = v.getProperty ("mic", juce::var()); m.getDynamicObject() != nullptr)
        {
            c.mic.firstInput = intProperty (m, "firstInput", 0);
            c.mic.numInputs = intProperty (m, "numInputs", 2);
        }

        if (const auto ctl = v.getProperty ("control", juce::var()); ctl.getDynamicObject() != nullptr)
        {
            static const char* const kinds[] = { "start", "stop", "pause", "load", "reset", "goto", "wait", "memo", "arm", "disarm", "target" };
            const auto kindText = ctl.getProperty ("kind", "start").toString();
            c.control.kind = ControlKind::start;

            for (int k = 0; k < 11; ++k)
                if (kindText == kinds[k])
                    c.control.kind = (ControlKind) k;

            const auto targetText = ctl.getProperty ("target", "").toString();
            const auto secondText = ctl.getProperty ("secondTarget", "").toString();
            c.control.targetId = targetText.isNotEmpty() ? juce::Uuid (targetText) : juce::Uuid::null();
            c.control.secondTargetId = secondText.isNotEmpty() ? juce::Uuid (secondText) : juce::Uuid::null();
            c.control.seconds = (double) ctl.getProperty ("seconds", 0.0);
        }

        if (const auto parentText = v.getProperty ("parent", "").toString(); parentText.isNotEmpty())
            c.parentId = juce::Uuid (parentText);

        if (const auto g = v.getProperty ("group", juce::var()); g.getDynamicObject() != nullptr)
        {
            const auto mode = g.getProperty ("mode", "timeline").toString();
            c.group.mode = mode == "playlist" ? GroupMode::playlist
                         : mode == "startFirstEnter" ? GroupMode::startFirstEnter
                         : mode == "startFirst" ? GroupMode::startFirst
                         : mode == "random" ? GroupMode::random : GroupMode::timeline;
            c.group.collapsed = (bool) g.getProperty ("collapsed", false);
            c.group.shuffle = (bool) g.getProperty ("shuffle", false);
            c.group.loop = (bool) g.getProperty ("loop", false);
            c.group.crossfade = (bool) g.getProperty ("crossfade", false);
            c.group.crossfadeSeconds = (double) g.getProperty ("crossfadeSeconds", 2.0);
        }
        c.fade            = fadeFromVar (v.getProperty ("fade", juce::var()));

        if (const auto d = v.getProperty ("devamp", juce::var()); d.getDynamicObject() != nullptr)
        {
            c.devamp.targetId = juce::Uuid (d.getProperty ("target", "").toString());
            c.devamp.startNextCue = (bool) d.getProperty ("startNextCue", true);
            c.devamp.stopTarget = (bool) d.getProperty ("stopTarget", false);
        }

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
        obj->setProperty ("audition", s.audition == WorkspaceSettings::Audition::none ? "none"
                                     : s.audition == WorkspaceSettings::Audition::alternatePatch ? "alternatePatch" : "unchanged");
        obj->setProperty ("auditionPatch", s.auditionPatchId.isNull() ? juce::String() : s.auditionPatchId.toString());
        obj->setProperty ("alwaysAudition", s.alwaysAudition);
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
        {
            const auto mode = v.getProperty ("audition", "unchanged").toString();
            s.audition = mode == "none" ? WorkspaceSettings::Audition::none
                       : mode == "alternatePatch" ? WorkspaceSettings::Audition::alternatePatch : WorkspaceSettings::Audition::unchanged;
        }
        s.auditionPatchId = juce::Uuid (v.getProperty ("auditionPatch", "").toString());
        s.alwaysAudition = (bool) v.getProperty ("alwaysAudition", false);
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

void CueContainer::sanitise()
{
    cartRows = juce::jlimit (1, maxGrid, cartRows);
    cartCols = juce::jlimit (1, maxGrid, cartCols);

    if (id.isNull())
        id = juce::Uuid();

    if (name.isEmpty())
        name = juce::String::fromUTF8 (isCart ? "\xEC\xB9\xB4\xED\x8A\xB8" : "\xED\x81\x90 \xEB\xA6\xAC\xEC\x8A\xA4\xED\x8A\xB8");   // 카트 / 큐 리스트
}

CueContainer& Project::ensureMainList()
{
    if (lists.empty())
    {
        CueContainer main;
        main.name = juce::String::fromUTF8 ("\xEB\xA9\x94\xEC\x9D\xB8 \xED\x81\x90 \xEB\xA6\xAC\xEC\x8A\xA4\xED\x8A\xB8");   // 메인 큐 리스트
        lists.push_back (std::move (main));
    }

    activeList = juce::jlimit (0, (int) lists.size() - 1, activeList);
    return lists.front();
}

std::vector<Cue>& Project::cues()
{
    return ensureMainList().cues;
}

const std::vector<Cue>& Project::cues() const
{
    static const std::vector<Cue> none;
    return lists.empty() ? none : lists.front().cues;
}

const Cue* Project::findCue (const juce::Uuid& id) const noexcept
{
    for (const auto& list : lists)
        for (const auto& c : list.cues)
            if (c.id == id)
                return &c;

    return nullptr;
}

AudioPatch& Project::ensureDefaultPatch()
{
    if (patches.empty())
        patches.push_back (AudioPatch::makeDefault());

    return patches.front();
}

const AudioPatch* Project::findPatch (const juce::Uuid& id) const noexcept
{
    for (const auto& p : patches)
        if (p.id == id)
            return &p;

    return nullptr;
}

const AudioPatch* Project::patchForCue (const Cue& cue) const noexcept
{
    if (const auto* p = findPatch (cue.patchId))
        return p;

    return patches.empty() ? nullptr : &patches.front();
}

namespace ProjectSerializer
{

juce::var toVar (const Project& project, const juce::File& projectDir)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("app", "GoCue");
    root->setProperty ("version", currentVersion);
    root->setProperty ("name", project.name);

    // "cues" stays the main list (older builds read that); "lists" carries every list / cart
    juce::Array<juce::var> cues;

    for (const auto& c : project.cues())
        cues.add (cueToVar (c, projectDir));

    root->setProperty ("cues", juce::var (cues));

    juce::Array<juce::var> lists;

    for (const auto& list : project.lists)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id", list.id.toString());
        obj->setProperty ("name", list.name);
        obj->setProperty ("cart", list.isCart);
        obj->setProperty ("rows", list.cartRows);
        obj->setProperty ("cols", list.cartCols);
        juce::Array<juce::var> listCues;

        for (const auto& c : list.cues)
            listCues.add (cueToVar (c, projectDir));

        obj->setProperty ("cues", juce::var (listCues));
        lists.add (juce::var (obj));
    }

    root->setProperty ("lists", juce::var (lists));
    root->setProperty ("activeList", project.activeList);

    juce::Array<juce::var> patches;

    for (const auto& patch : project.patches)
        patches.add (patch.toVar());

    root->setProperty ("patches", juce::var (patches));

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

    juce::StringArray seenIds;   // a cue id is unique across every list: they would share one player and one plugin chain
    auto readCues = [&] (const juce::var& array, std::vector<Cue>& into)
    {
        const auto* cues = array.getArray();

        if (cues == nullptr)
            return;

        for (const auto& item : *cues)
        {
            if (item.getDynamicObject() == nullptr)
            {
                if (warnings != nullptr)
                    warnings->add ("Skipped a malformed cue entry");

                continue;
            }

            auto cue = cueFromVar (item, projectDir, warnings);

            if (seenIds.contains (cue.id.toString()))
            {
                cue.id = juce::Uuid();

                if (warnings != nullptr)
                    warnings->add ("Duplicate cue id for \"" + cue.name + "\" - assigned a new one");
            }

            seenIds.add (cue.id.toString());
            into.push_back (std::move (cue));
        }
    };

    if (const auto* lists = root.getProperty ("lists", juce::var()).getArray())
    {
        for (const auto& item : *lists)
        {
            if (item.getDynamicObject() == nullptr)
                continue;

            CueContainer list;
            const auto idText = item.getProperty ("id", "").toString();
            list.id = idText.isNotEmpty() ? juce::Uuid (idText) : juce::Uuid();
            list.name = item.getProperty ("name", "").toString();
            list.isCart = (bool) item.getProperty ("cart", false);
            list.cartRows = intProperty (item, "rows", 4);
            list.cartCols = intProperty (item, "cols", 4);
            readCues (item.getProperty ("cues", juce::var()), list.cues);
            list.sanitise();
            project.lists.push_back (std::move (list));
        }
    }
    else
    {
        // version <= 4: one flat cue list
        readCues (root.getProperty ("cues", juce::var()), project.ensureMainList().cues);
    }

    project.ensureMainList();
    project.activeList = juce::jlimit (0, (int) project.lists.size() - 1, intProperty (root, "activeList", 0));

    {
        std::set<juce::String> listIds;

        for (auto& list : project.lists)
        {
            if (list.id.isNull() || listIds.count (list.id.toString()) != 0)
                list.id = juce::Uuid();

            listIds.insert (list.id.toString());
        }
    }

    if (const auto* patches = root.getProperty ("patches", juce::var()).getArray())
        for (const auto& p : *patches)
            if (p.getDynamicObject() != nullptr)
                project.patches.push_back (AudioPatch::fromVar (p));

    {
        // patch ids must be unique (cues and the audition setting refer to them): the first keeps a duplicate id
        std::set<juce::String> seen;

        for (auto& p : project.patches)
        {
            if (p.id.isNull() || seen.count (p.id.toString()) != 0)
            {
                p.id = juce::Uuid();

                if (warnings != nullptr)
                    warnings->add ("Audio patch '" + p.name + "' had a duplicate or empty id and was given a new one.");
            }

            seen.insert (p.id.toString());
        }
    }

    project.ensureDefaultPatch();

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
