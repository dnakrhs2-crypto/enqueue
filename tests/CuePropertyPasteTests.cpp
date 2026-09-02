#include "model/CuePropertyPaste.h"
#include "model/WorkspaceSettings.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

class CuePropertyPasteTests : public juce::UnitTest
{
public:
    CuePropertyPasteTests() : juce::UnitTest ("CuePropertyPaste", "GoCue") {}

    static Cue makeSource()
    {
        Cue s;
        s.number = "7";
        s.name = "Source";
        s.notes = "notes";
        s.file = juce::File ("C:/audio/source.wav");
        s.durationSeconds = 30.0;
        s.color = 4;
        s.secondColor = 2;
        s.useSecondColor = true;
        s.flagged = true;
        s.armed = false;
        s.skipIfDisarmed = true;
        s.autoLoad = true;
        s.preWaitSeconds = 2.0;
        s.postWaitSeconds = 1.0;
        s.continueMode = ContinueMode::autoFollow;
        s.secondTrigger = SecondTriggerAction::panic;
        s.hotkey = "F9";
        s.wallClock.enabled = true;
        s.wallClock.hour = 20;
        s.fadeStopOthers.enabled = true;
        s.fadeStopOthers.seconds = 4.0;
        s.duck.enabled = true;
        s.duck.levelDb = -12.0;
        s.audio.startSeconds = 5.0;
        s.audio.endSeconds = 25.0;
        s.audio.playCount = 3;
        s.audio.rate = 1.25;
        s.audio.envelope.enabled = true;
        s.gainDb = -6.0;
        s.fadeOutMs = 1200;

        PluginSlotState plugin;
        plugin.name = "EQ";
        s.plugins.push_back (plugin);
        return s;
    }

    static Cue makeTarget()
    {
        Cue t;
        t.number = "1";
        t.name = "Target";
        t.file = juce::File ("C:/audio/target.wav");
        t.durationSeconds = 10.0;
        t.hotkey = "F1";
        return t;
    }

    void runTest() override
    {
        beginTest ("every group pasted: identity, number, name, file, duration and hotkey stay");
        {
            const auto source = makeSource();
            auto target = makeTarget();
            const auto id = target.id;

            CuePropertyPaste::Selection all;
            all.effects = true;
            CuePropertyPaste::apply (source, target, all);

            expect (target.id == id);
            expectEquals (target.number, juce::String ("1"));
            expectEquals (target.name, juce::String ("Target"));
            expect (target.file == juce::File ("C:/audio/target.wav"));
            expectWithinAbsoluteError (target.durationSeconds, 10.0, 1e-12);
            expectEquals (target.hotkey, juce::String ("F1"));

            expectEquals (target.color, 4);
            expect (target.useSecondColor && target.flagged && ! target.armed && target.skipIfDisarmed && target.autoLoad);
            expectEquals (target.notes, juce::String ("notes"));
            expectWithinAbsoluteError (target.preWaitSeconds, 2.0, 1e-12);
            expect (target.continueMode == ContinueMode::autoFollow);
            expect (target.secondTrigger == SecondTriggerAction::panic);
            expect (target.wallClock.enabled && target.wallClock.hour == 20);
            expect (target.fadeStopOthers.enabled);
            expect (target.duck.enabled);
            expectEquals (target.audio.playCount, 3);
            expectWithinAbsoluteError (target.audio.rate, 1.25, 1e-12);
            expect (target.audio.envelope.enabled);
            expectWithinAbsoluteError (target.gainDb, -6.0, 1e-12);
            expectEquals (target.fadeOutMs, 1200);
            expectEquals ((int) target.plugins.size(), 1);
        }

        beginTest ("a trim from a longer file is clamped to the target file");
        {
            const auto source = makeSource();   // trim 5..25 s of a 30 s file
            auto target = makeTarget();         // 10 s file

            CuePropertyPaste::Selection onlyTime;
            onlyTime.basics = onlyTime.timing = onlyTime.triggers = onlyTime.levels = false;
            CuePropertyPaste::apply (source, target, onlyTime);

            expectWithinAbsoluteError (target.audio.startSeconds, 5.0, 1e-12);
            expectWithinAbsoluteError (target.audio.endSeconds, -1.0, 1e-12);   // 25 s is beyond the 10 s file -> to the end
            expectEquals (target.audio.playCount, 3);
            expectWithinAbsoluteError (target.gainDb, 0.0, 1e-12);            // levels were not selected
            expectEquals (target.color, 0);
        }

        beginTest ("unselected groups are untouched");
        {
            const auto source = makeSource();
            auto target = makeTarget();
            target.gainDb = -1.0;
            target.color = 9;

            CuePropertyPaste::Selection onlyLevels;
            onlyLevels.basics = onlyLevels.timing = onlyLevels.triggers = onlyLevels.timeLoops = false;
            CuePropertyPaste::apply (source, target, onlyLevels);

            expectWithinAbsoluteError (target.gainDb, -6.0, 1e-12);
            expectEquals (target.fadeOutMs, 1200);
            expectEquals (target.color, 9);
            expect (! target.duck.enabled);
            expectWithinAbsoluteError (target.preWaitSeconds, 0.0, 1e-12);
            expectEquals ((int) target.plugins.size(), 0);
        }

        beginTest ("the new-cue template copies settings but not identity, name, number, file, hotkey or wall clock");
        {
            WorkspaceSettings settings;
            settings.hasCueTemplate = true;
            settings.cueTemplate = makeSource();

            Cue fresh;
            fresh.name = "fresh";
            fresh.number = "3";
            fresh.file = juce::File ("C:/audio/fresh.wav");
            fresh.durationSeconds = 8.0;
            const auto id = fresh.id;

            settings.applyTemplate (fresh);

            expect (fresh.id == id);
            expectEquals (fresh.name, juce::String ("fresh"));
            expectEquals (fresh.number, juce::String ("3"));
            expect (fresh.file == juce::File ("C:/audio/fresh.wav"));
            expectWithinAbsoluteError (fresh.durationSeconds, 8.0, 1e-12);
            expect (fresh.hotkey.isEmpty());
            expect (! fresh.wallClock.enabled);
            expectWithinAbsoluteError (fresh.gainDb, -6.0, 1e-12);
            expectEquals (fresh.fadeOutMs, 1200);
            expectEquals (fresh.color, 4);
            expect (fresh.duck.enabled);
            expectEquals ((int) fresh.plugins.size(), 1);

            settings.hasCueTemplate = false;
            Cue untouched;
            untouched.gainDb = -2.0;
            settings.applyTemplate (untouched);
            expectWithinAbsoluteError (untouched.gainDb, -2.0, 1e-12);
        }
    }
};

static CuePropertyPasteTests cuePropertyPasteTests;

} // namespace gocue::tests
