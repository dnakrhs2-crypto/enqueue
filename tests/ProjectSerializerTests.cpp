#include "model/ProjectSerializer.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

class ProjectSerializerTests : public juce::UnitTest
{
public:
    ProjectSerializerTests() : juce::UnitTest ("ProjectSerializer", "Enqueue") {}

    void runTest() override
    {
        const auto tempRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("gocue_serializer_" + juce::Uuid().toString());
        expect (tempRoot.createDirectory().wasOk());

        beginTest ("round trip preserves cues, audio settings, master chain and plugin states");
        {
            Project p;
            p.name = "Show";

            Cue a;
            a.number = "1.5";
            a.name = "Intro";
            a.notes = "house lights out";
            a.color = 3;
            a.secondColor = 8;
            a.useSecondColor = true;
            a.flagged = true;
            a.armed = false;
            a.skipIfDisarmed = true;
            a.autoLoad = true;
            a.preWaitSeconds = 1.25;
            a.postWaitSeconds = 0.5;
            a.continueMode = ContinueMode::autoContinue;
            a.hotkey = "F5";
            a.wallClock.enabled = true;
            a.wallClock.hour = 19;
            a.wallClock.minute = 30;
            a.wallClock.second = 5;
            a.wallClock.daysMask = 0x3e;
            a.fadeStopOthers.enabled = true;
            a.fadeStopOthers.seconds = 3.0;
            a.fadeStopOthers.scope = FadeStopScope::all;
            a.duck.enabled = true;
            a.duck.levelDb = -9.0;
            a.duck.seconds = 0.75;
            a.file = tempRoot.getChildFile ("intro.wav");
            a.fadeOutMs = 1500;
            a.gainDb = -3.5;
            a.durationSeconds = 12.25;
            a.audio.startSeconds = 1.5;
            a.audio.endSeconds = 10.0;
            a.audio.playCount = 3;
            a.audio.infiniteLoop = false;
            a.audio.rate = 1.25;
            a.audio.preservePitch = true;
            a.audio.envelope.enabled = true;
            a.audio.envelope.linear = false;
            a.audio.envelope.lockToTrim = true;
            a.audio.envelope.points = { { 0.0, 0.0 }, { 0.1, 1.0 }, { 0.9, 1.0 }, { 1.0, 0.0 } };

            PluginSlotState slot;
            slot.name = "EQ";
            slot.fileOrIdentifier = "C:\\Plugins\\eq.vst3";
            slot.uniqueId = 42;
            slot.stateBase64 = "AAECAw==";
            slot.descriptionXml = "<PLUGIN name=\"EQ\" format=\"VST3\" uniqueId=\"2a\"/>";
            slot.bypassed = true;
            a.plugins.push_back (slot);
            p.cues().push_back (a);

            Cue b;
            b.name = "Outro";
            b.audio.infiniteLoop = true;
            p.cues().push_back (b);

            PluginSlotState master;
            master.name = "Limiter";
            master.uniqueId = 7;
            p.masterPlugins.push_back (master);

            const auto json = ProjectSerializer::toJson (p);
            expect (json.contains ("\"version\": " + juce::String (ProjectSerializer::currentVersion)) || json.contains ("\"version\":" + juce::String (ProjectSerializer::currentVersion)));
            expect (json.contains ("\"lists\""));
            expect (! json.contains ("fadeInMs"));

            Project q;
            juce::StringArray warnings;
            expect (ProjectSerializer::fromJson (json, q, &warnings).wasOk());
            expectEquals (q.name, juce::String ("Show"));
            expectEquals ((int) q.cues().size(), 2);
            expect (q.cues()[0].id == a.id);
            expectEquals (q.cues()[0].number, juce::String ("1.5"));
            expectEquals (q.cues()[0].name, juce::String ("Intro"));
            expectEquals (q.cues()[0].notes, juce::String ("house lights out"));
            expectEquals (q.cues()[0].color, 3);
            expectEquals (q.cues()[0].secondColor, 8);
            expect (q.cues()[0].useSecondColor && q.cues()[0].flagged && ! q.cues()[0].armed && q.cues()[0].skipIfDisarmed && q.cues()[0].autoLoad);
            expectWithinAbsoluteError (q.cues()[0].preWaitSeconds, 1.25, 1e-9);
            expectWithinAbsoluteError (q.cues()[0].postWaitSeconds, 0.5, 1e-9);
            expect (q.cues()[0].continueMode == ContinueMode::autoContinue);
            expectEquals (q.cues()[0].hotkey, juce::String ("F5"));
            expect (q.cues()[0].wallClock.enabled);
            expectEquals (q.cues()[0].wallClock.hour, 19);
            expectEquals (q.cues()[0].wallClock.minute, 30);
            expectEquals (q.cues()[0].wallClock.second, 5);
            expectEquals (q.cues()[0].wallClock.daysMask, 0x3e);
            expect (q.cues()[0].fadeStopOthers.enabled && q.cues()[0].fadeStopOthers.scope == FadeStopScope::all);
            expectWithinAbsoluteError (q.cues()[0].fadeStopOthers.seconds, 3.0, 1e-9);
            expect (q.cues()[0].duck.enabled);
            expectWithinAbsoluteError (q.cues()[0].duck.levelDb, -9.0, 1e-9);
            expectWithinAbsoluteError (q.cues()[0].duck.seconds, 0.75, 1e-9);
            expect (q.cues()[1].armed && q.cues()[1].continueMode == ContinueMode::none && q.cues()[1].number.isEmpty());
            expect (q.cues()[0].file == a.file);
            expectEquals (q.cues()[0].fadeOutMs, 1500);
            expectWithinAbsoluteError (q.cues()[0].gainDb, -3.5, 1e-9);
            expectWithinAbsoluteError (q.cues()[0].durationSeconds, 12.25, 1e-9);
            expectWithinAbsoluteError (q.cues()[0].audio.startSeconds, 1.5, 1e-9);
            expectWithinAbsoluteError (q.cues()[0].audio.endSeconds, 10.0, 1e-9);
            expectEquals (q.cues()[0].audio.playCount, 3);
            expect (! q.cues()[0].audio.infiniteLoop);
            expectWithinAbsoluteError (q.cues()[0].audio.rate, 1.25, 1e-9);
            expect (q.cues()[0].audio.preservePitch);
            expect (q.cues()[0].audio.envelope.enabled);
            expect (! q.cues()[0].audio.envelope.linear);
            expect (q.cues()[0].audio.envelope.lockToTrim);
            expectEquals ((int) q.cues()[0].audio.envelope.points.size(), 4);
            expectWithinAbsoluteError (q.cues()[0].audio.envelope.points[1].x, 0.1, 1e-9);
            expectWithinAbsoluteError (q.cues()[0].audio.envelope.points[3].level, 0.0, 1e-9);
            expectWithinAbsoluteError (q.cues()[0].regionLength(), 8.5, 1e-9);
            expectWithinAbsoluteError (q.cues()[0].effectiveLength(), 8.5 / 1.25 * 3.0, 1e-9);
            expectEquals ((int) q.cues()[0].plugins.size(), 1);
            expectEquals (q.cues()[0].plugins[0].name, juce::String ("EQ"));
            expectEquals (q.cues()[0].plugins[0].fileOrIdentifier, juce::String ("C:\\Plugins\\eq.vst3"));
            expectEquals (q.cues()[0].plugins[0].uniqueId, 42);
            expectEquals (q.cues()[0].plugins[0].stateBase64, juce::String ("AAECAw=="));
            expectEquals (q.cues()[0].plugins[0].descriptionXml, slot.descriptionXml);
            expect (q.cues()[0].plugins[0].bypassed);
            expect (q.cues()[1].id == b.id);
            expect (q.cues()[1].file == juce::File());
            expect (q.cues()[1].audio.infiniteLoop);
            expectWithinAbsoluteError (q.cues()[1].effectiveLength(), -1.0, 1e-12);
            expectEquals ((int) q.masterPlugins.size(), 1);
            expectEquals (q.masterPlugins[0].name, juce::String ("Limiter"));
            expectEquals (q.masterPlugins[0].uniqueId, 7);
            // intro.wav does not exist, so the loader flags it
            expect (q.cues()[0].fileMissing);
            expectEquals (warnings.size(), 1);
        }

        beginTest ("workspace settings round trip, including the new-cue template and the row size");
        {
            Project p;
            p.settings.doubleGoSeconds = 1.5;
            p.settings.panicSeconds = 3.0;
            p.settings.autoNumber = false;
            p.settings.rowSize = 2;
            p.settings.startOnClose = true;
            p.settings.startOnCloseCue = "99";
            p.settings.hasCueTemplate = true;
            p.settings.cueTemplate.gainDb = -4.5;
            p.settings.cueTemplate.fadeOutMs = 900;
            p.settings.cueTemplate.color = 6;
            p.settings.cueTemplate.duck.enabled = true;
            PluginSlotState templatePlugin;
            templatePlugin.name = "Comp";
            p.settings.cueTemplate.plugins.push_back (templatePlugin);

            Project q;
            expect (ProjectSerializer::fromJson (ProjectSerializer::toJson (p), q, nullptr).wasOk());
            expectWithinAbsoluteError (q.settings.doubleGoSeconds, 1.5, 1e-9);
            expectWithinAbsoluteError (q.settings.panicSeconds, 3.0, 1e-9);
            expect (! q.settings.autoNumber);
            expectEquals (q.settings.rowSize, 2);
            expect (q.settings.startOnClose);
            expectEquals (q.settings.startOnCloseCue, juce::String ("99"));
            expect (q.settings.hasCueTemplate);
            expectWithinAbsoluteError (q.settings.cueTemplate.gainDb, -4.5, 1e-9);
            expectEquals (q.settings.cueTemplate.fadeOutMs, 900);
            expectEquals (q.settings.cueTemplate.color, 6);
            expect (q.settings.cueTemplate.duck.enabled);
            expectEquals ((int) q.settings.cueTemplate.plugins.size(), 1);
            expectEquals (q.settings.cueTemplate.plugins[0].name, juce::String ("Comp"));

            Project r;
            expect (ProjectSerializer::fromJson ("{\"cues\":[],\"settings\":{\"rowSize\":7,\"hasCueTemplate\":true}}", r, nullptr).wasOk());
            expectEquals (r.settings.rowSize, 2);          // clamped
            expect (! r.settings.hasCueTemplate);          // flag without a template object -> off
        }

        beginTest ("patches round trip and a file without patches gets the default patch");
        {
            Project p;
            auto main = AudioPatch::makeDefault ("Main");
            main.numCueOutputs = 4;
            main.sanitise();
            main.setRouting (2, 3, -6.0);
            auto alt = AudioPatch::makeDefault ("Alt");
            alt.numCueOutputs = 2;
            alt.sanitise();
            p.patches = { main, alt };

            Cue c;
            c.patchId = alt.id;
            c.levels.resize (2, 2);
            c.levels.crosspointDb[0][1] = -9.0;
            c.trim.resize (2);
            c.trim.outputDb[1] = 1.5;
            c.numChannels = 2;
            p.cues().push_back (c);

            Project q;
            expect (ProjectSerializer::fromJson (ProjectSerializer::toJson (p), q, nullptr).wasOk());
            expectEquals ((int) q.patches.size(), 2);
            expect (q.patches[0] == main);
            expect (q.patches[1] == alt);
            expect (q.cues()[0].patchId == alt.id);
            expect (q.patchForCue (q.cues()[0]) == &q.patches[1]);
            expectWithinAbsoluteError (q.cues()[0].levels.crosspointDb[0][1], -9.0, 1e-12);
            expectWithinAbsoluteError (q.cues()[0].trim.outputDb[1], 1.5, 1e-12);
            expectEquals (q.cues()[0].numChannels, 2);

            Project r;
            expect (ProjectSerializer::fromJson ("{\"cues\":[{\"name\":\"x\"}]}", r, nullptr).wasOk());
            expectEquals ((int) r.patches.size(), 1);
            expectEquals (r.patches[0].numCueOutputs, AudioPatch::defaultCueOutputs);
            expect (r.patchForCue (r.cues()[0]) == &r.patches[0]);   // null patch id -> default
            expect (r.cues()[0].levels.numInputs() == 0);           // sized later from the file
        }

        beginTest ("missing fields fall back to defaults");
        {
            Project q;
            juce::StringArray warnings;
            expect (ProjectSerializer::fromJson ("{\"cues\":[{\"name\":\"only name\"}]}", q, &warnings).wasOk());
            expectEquals ((int) q.cues().size(), 1);
            expectEquals (q.cues()[0].name, juce::String ("only name"));
            expect (! q.cues()[0].id.isNull());
            expect (q.cues()[0].file == juce::File());
            expect (! q.cues()[0].fileMissing);
            expectEquals (q.cues()[0].fadeOutMs, 0);
            expectWithinAbsoluteError (q.cues()[0].gainDb, 0.0, 1e-12);
            expect (! q.cues()[0].audio.envelope.enabled);
            expectWithinAbsoluteError (q.cues()[0].audio.startSeconds, 0.0, 1e-12);
            expectWithinAbsoluteError (q.cues()[0].audio.endSeconds, -1.0, 1e-12);
            expectEquals (q.cues()[0].audio.playCount, 1);
            expectWithinAbsoluteError (q.cues()[0].audio.rate, 1.0, 1e-12);
            expectEquals ((int) q.cues()[0].plugins.size(), 0);
            expectEquals ((int) q.masterPlugins.size(), 0);
            expectEquals (warnings.size(), 0);
        }

        beginTest ("version-1 fadeInMs migrates to the integrated fade envelope");
        {
            Project q;
            juce::StringArray warnings;
            expect (ProjectSerializer::fromJson ("{\"version\":1,\"cues\":[{\"name\":\"old\",\"fadeInMs\":250,\"fadeOutMs\":1500,\"gainDb\":-2}]}",
                                                 q, &warnings).wasOk());
            expectEquals ((int) q.cues().size(), 1);
            const auto& c = q.cues()[0];
            expectEquals (c.fadeOutMs, 1500);
            expectWithinAbsoluteError (c.gainDb, -2.0, 1e-12);
            expect (c.audio.envelope.enabled && c.audio.envelope.linear && ! c.audio.envelope.lockToTrim);
            expectWithinAbsoluteError (c.audio.envelope.fadeInSeconds (30.0), 0.25, 1e-9);
            expectWithinAbsoluteError (c.audio.envelope.levelAt (0.125, 30.0), 0.5f, 1e-6f);
            expectEquals (warnings.size(), 0);

            Project none;
            expect (ProjectSerializer::fromJson ("{\"cues\":[{\"fadeInMs\":0}]}", none).wasOk());
            expect (! none.cues()[0].audio.envelope.enabled);
        }

        beginTest ("unknown fields are ignored; a file from a newer Enqueue is refused");
        {
            Project q;
            juce::StringArray warnings;
            expect (ProjectSerializer::fromJson ("{\"version\":" + juce::String (ProjectSerializer::currentVersion)
                                                     + ",\"future\":true,\"cues\":[{\"name\":\"x\",\"mystery\":[1,2],\"audio\":{\"later\":1}}]}",
                                                 q, &warnings).wasOk());
            expectEquals ((int) q.cues().size(), 1);
            expectEquals (warnings.size(), 0);

            // a newer file version: what this build cannot read would be dropped by the next save, so it does not open
            Project future;
            const auto result = ProjectSerializer::fromJson ("{\"version\":99,\"cues\":[{\"name\":\"x\"}]}", future, &warnings);
            expect (result.failed());
            expect (result.getErrorMessage().contains ("99"));
            expect (result.getErrorMessage().contains ("newer"));
        }

        beginTest ("malformed input fails cleanly");
        {
            Project q;
            expect (ProjectSerializer::fromJson ("not json at all", q).failed());
            expect (ProjectSerializer::fromJson ("[1,2,3]", q).failed());
            juce::StringArray warnings;
            expect (ProjectSerializer::fromJson ("{\"cues\":[1,{\"name\":\"ok\"}]}", q, &warnings).wasOk());
            expectEquals ((int) q.cues().size(), 1);
            expectEquals (warnings.size(), 1);

            // an envelope with junk points keeps the valid ones
            Project e;
            expect (ProjectSerializer::fromJson ("{\"cues\":[{\"audio\":{\"envelope\":{\"enabled\":true,\"points\":[[0,0],\"junk\",[1],[0.5,2]]}}}]}", e).wasOk());
            expectEquals ((int) e.cues()[0].audio.envelope.points.size(), 2);
            expectWithinAbsoluteError (e.cues()[0].audio.envelope.points[1].level, 1.0, 1e-12);
        }

        beginTest ("duplicate cue ids are replaced so cues never share a player or chain");
        {
            Project q;
            juce::StringArray warnings;
            const juce::String json = "{\"cues\":[{\"id\":\"1b4e28ba2fa14d0180b0f55d9ef3b8c4\",\"name\":\"a\"},"
                                      "{\"id\":\"1b4e28ba2fa14d0180b0f55d9ef3b8c4\",\"name\":\"b\"}]}";
            expect (ProjectSerializer::fromJson (json, q, &warnings).wasOk());
            expectEquals ((int) q.cues().size(), 2);
            expect (q.cues()[0].id != q.cues()[1].id);
            expectEquals (q.cues()[0].id.toString(), juce::String ("1b4e28ba2fa14d0180b0f55d9ef3b8c4"));
            expectEquals (warnings.size(), 1);
        }

        beginTest ("out-of-range values are sanitised");
        {
            Project q;
            expect (ProjectSerializer::fromJson ("{\"cues\":[{\"fadeInMs\":-100,\"fadeOutMs\":99999999,\"gainDb\":200,\"durationSeconds\":-1,"
                                                 "\"audio\":{\"start\":-4,\"end\":-9,\"playCount\":0,\"rate\":1000}}]}", q).wasOk());
            expect (! q.cues()[0].audio.envelope.enabled);
            expectEquals (q.cues()[0].fadeOutMs, Cue::maxFadeMs);
            expectWithinAbsoluteError (q.cues()[0].gainDb, Cue::maxGainDb, 1e-12);
            expectWithinAbsoluteError (q.cues()[0].durationSeconds, 0.0, 1e-12);
            expectWithinAbsoluteError (q.cues()[0].audio.startSeconds, 0.0, 1e-12);
            expectWithinAbsoluteError (q.cues()[0].audio.endSeconds, -1.0, 1e-12);
            expectEquals (q.cues()[0].audio.playCount, 1);
            expectWithinAbsoluteError (q.cues()[0].audio.rate, AudioCueData::maxRate, 1e-12);

            Project r;   // an end before the start plays to the end of the file instead of nothing
            expect (ProjectSerializer::fromJson ("{\"cues\":[{\"durationSeconds\":10,\"audio\":{\"start\":5,\"end\":3}}]}", r).wasOk());
            expectWithinAbsoluteError (r.cues()[0].audio.endSeconds, -1.0, 1e-12);
            expectWithinAbsoluteError (r.cues()[0].regionLength(), 5.0, 1e-12);
        }

        beginTest ("a moved project resolves files through the project-relative path");
        {
            const auto audioDir = tempRoot.getChildFile ("audio");
            expect (audioDir.createDirectory().wasOk());
            const auto tone = audioDir.getChildFile ("tone.wav");
            expect (tone.replaceWithText ("not really audio"));

            const juce::String json = "{\"cues\":[{\"name\":\"t\",\"file\":\"C:\\\\definitely\\\\missing\\\\tone.wav\",\"fileRelative\":\"audio\\\\tone.wav\"},"
                                      "{\"name\":\"u\",\"file\":\"C:\\\\definitely\\\\missing\\\\other.wav\",\"fileRelative\":\"audio\\\\other.wav\"}]}";
            Project q;
            juce::StringArray warnings;
            expect (ProjectSerializer::fromJson (json, q, &warnings, tempRoot).wasOk());
            expect (q.cues()[0].file == tone);
            expect (! q.cues()[0].fileMissing);
            expect (q.cues()[1].fileMissing);
            expectEquals (warnings.size(), 1);
        }

        beginTest ("save and load through a file");
        {
            Project p;
            p.name = "Disk";
            Cue c;
            c.name = "cue";
            c.file = tempRoot.getChildFile ("audio").getChildFile ("tone.wav");
            p.cues().push_back (c);

            const auto projectFile = tempRoot.getChildFile ("show.gocue");
            expect (ProjectSerializer::save (p, projectFile).wasOk());
            expect (projectFile.existsAsFile());

            const auto text = projectFile.loadFileAsString();
            expect (text.contains ("\"fileRelative\""));
            expect (text.contains ("\"app\": \"Enqueue\"") || text.contains ("\"app\":\"Enqueue\""));

            Project q;
            juce::StringArray warnings;
            expect (ProjectSerializer::load (projectFile, q, &warnings).wasOk());
            expectEquals ((int) q.cues().size(), 1);
            expect (q.cues()[0].file == c.file);
            expect (! q.cues()[0].fileMissing);
            expect (ProjectSerializer::load (tempRoot.getChildFile ("missing.gocue"), q).failed());
        }

        beginTest ("group cues and parent links round-trip");
        {
            Project p;
            Cue g;
            g.name = "group";
            g.type = CueType::group;
            g.group.mode = GroupMode::playlist;
            g.group.collapsed = true;
            g.group.shuffle = true;
            g.group.loop = true;
            g.group.crossfade = true;
            g.group.crossfadeSeconds = 3.5;
            Cue child;
            child.name = "child";
            child.parentId = g.id;
            p.cues().push_back (g);
            p.cues().push_back (child);

            const auto projectFile = tempRoot.getChildFile ("group.gocue");
            expect (ProjectSerializer::save (p, projectFile).wasOk());
            Project q;
            expect (ProjectSerializer::load (projectFile, q).wasOk());
            expectEquals ((int) q.cues().size(), 2);
            expect (q.cues()[0].isGroup());
            expect (q.cues()[0].group.mode == GroupMode::playlist);
            expect (q.cues()[0].group.collapsed && q.cues()[0].group.shuffle && q.cues()[0].group.loop && q.cues()[0].group.crossfade);
            expectWithinAbsoluteError (q.cues()[0].group.crossfadeSeconds, 3.5, 1e-9);
            expect (q.cues()[1].parentId == g.id);
            expect (q.cues()[0].parentId.isNull());
        }

        beginTest ("control cues round-trip");
        {
            Project p;
            Cue a;
            a.name = "a";
            Cue ctl;
            ctl.name = "ctl";
            ctl.type = CueType::control;
            ctl.control.kind = ControlKind::target;
            ctl.control.targetId = a.id;
            ctl.control.secondTargetId = ctl.id;
            ctl.control.seconds = 1.25;
            p.cues().push_back (a);
            p.cues().push_back (ctl);
            const auto projectFile = tempRoot.getChildFile ("control.gocue");
            expect (ProjectSerializer::save (p, projectFile).wasOk());
            Project q;
            expect (ProjectSerializer::load (projectFile, q).wasOk());
            expectEquals ((int) q.cues().size(), 2);
            expect (q.cues()[1].isControl());
            expect (q.cues()[1].control.kind == ControlKind::target);
            expect (q.cues()[1].control.targetId == a.id);
            expect (q.cues()[1].control.secondTargetId == ctl.id);
            expectWithinAbsoluteError (q.cues()[1].control.seconds, 1.25, 1e-9);
        }

        beginTest ("cue lists and carts round-trip; a flat legacy file becomes one main list");
        {
            Project p;
            p.ensureMainList();
            Cue a;
            a.name = "a";
            p.cues().push_back (a);
            CueContainer cart;
            cart.name = "cart";
            cart.isCart = true;
            cart.cartRows = 3;
            cart.cartCols = 5;
            Cue b;
            b.name = "b";
            cart.cues.push_back (b);
            p.lists.push_back (cart);
            p.activeList = 1;

            const auto json = ProjectSerializer::toJson (p);
            Project q;
            expect (ProjectSerializer::fromJson (json, q).wasOk());
            expectEquals ((int) q.lists.size(), 2);
            expect (q.lists[1].isCart);
            expectEquals (q.lists[1].cartRows, 3);
            expectEquals (q.lists[1].cartCols, 5);
            expectEquals (q.lists[1].name, juce::String ("cart"));
            expectEquals ((int) q.lists[1].cues.size(), 1);
            expect (q.lists[1].cues[0].id == b.id);
            expectEquals (q.activeList, 1);
            expect (q.findCue (b.id) != nullptr);
            expect (q.cues()[0].id == a.id);   // the main list is still "cues"

            // a version-4 file (no "lists") reads into one main list
            Project legacy;
            expect (ProjectSerializer::fromJson ("{\"app\":\"GoCue\",\"version\":4,\"cues\":[{\"name\":\"old\"}]}", legacy).wasOk());
            expectEquals ((int) legacy.lists.size(), 1);
            expectEquals ((int) legacy.cues().size(), 1);
            expectEquals (legacy.cues()[0].name, juce::String ("old"));
            expect (! legacy.lists[0].isCart);
        }

        expect (tempRoot.deleteRecursively());
    }
};

static ProjectSerializerTests projectSerializerTests;

} // namespace gocue::tests
