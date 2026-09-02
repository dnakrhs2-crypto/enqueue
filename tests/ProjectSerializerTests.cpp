#include "model/ProjectSerializer.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

class ProjectSerializerTests : public juce::UnitTest
{
public:
    ProjectSerializerTests() : juce::UnitTest ("ProjectSerializer", "GoCue") {}

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
            a.name = "Intro";
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
            p.cues.push_back (a);

            Cue b;
            b.name = "Outro";
            b.audio.infiniteLoop = true;
            p.cues.push_back (b);

            PluginSlotState master;
            master.name = "Limiter";
            master.uniqueId = 7;
            p.masterPlugins.push_back (master);

            const auto json = ProjectSerializer::toJson (p);
            expect (json.contains ("\"version\": 2") || json.contains ("\"version\":2"));
            expect (! json.contains ("fadeInMs"));

            Project q;
            juce::StringArray warnings;
            expect (ProjectSerializer::fromJson (json, q, &warnings).wasOk());
            expectEquals (q.name, juce::String ("Show"));
            expectEquals ((int) q.cues.size(), 2);
            expect (q.cues[0].id == a.id);
            expectEquals (q.cues[0].name, juce::String ("Intro"));
            expect (q.cues[0].file == a.file);
            expectEquals (q.cues[0].fadeOutMs, 1500);
            expectWithinAbsoluteError (q.cues[0].gainDb, -3.5, 1e-9);
            expectWithinAbsoluteError (q.cues[0].durationSeconds, 12.25, 1e-9);
            expectWithinAbsoluteError (q.cues[0].audio.startSeconds, 1.5, 1e-9);
            expectWithinAbsoluteError (q.cues[0].audio.endSeconds, 10.0, 1e-9);
            expectEquals (q.cues[0].audio.playCount, 3);
            expect (! q.cues[0].audio.infiniteLoop);
            expectWithinAbsoluteError (q.cues[0].audio.rate, 1.25, 1e-9);
            expect (q.cues[0].audio.preservePitch);
            expect (q.cues[0].audio.envelope.enabled);
            expect (! q.cues[0].audio.envelope.linear);
            expect (q.cues[0].audio.envelope.lockToTrim);
            expectEquals ((int) q.cues[0].audio.envelope.points.size(), 4);
            expectWithinAbsoluteError (q.cues[0].audio.envelope.points[1].x, 0.1, 1e-9);
            expectWithinAbsoluteError (q.cues[0].audio.envelope.points[3].level, 0.0, 1e-9);
            expectWithinAbsoluteError (q.cues[0].regionLength(), 8.5, 1e-9);
            expectWithinAbsoluteError (q.cues[0].effectiveLength(), 8.5 / 1.25 * 3.0, 1e-9);
            expectEquals ((int) q.cues[0].plugins.size(), 1);
            expectEquals (q.cues[0].plugins[0].name, juce::String ("EQ"));
            expectEquals (q.cues[0].plugins[0].fileOrIdentifier, juce::String ("C:\\Plugins\\eq.vst3"));
            expectEquals (q.cues[0].plugins[0].uniqueId, 42);
            expectEquals (q.cues[0].plugins[0].stateBase64, juce::String ("AAECAw=="));
            expectEquals (q.cues[0].plugins[0].descriptionXml, slot.descriptionXml);
            expect (q.cues[0].plugins[0].bypassed);
            expect (q.cues[1].id == b.id);
            expect (q.cues[1].file == juce::File());
            expect (q.cues[1].audio.infiniteLoop);
            expectWithinAbsoluteError (q.cues[1].effectiveLength(), -1.0, 1e-12);
            expectEquals ((int) q.masterPlugins.size(), 1);
            expectEquals (q.masterPlugins[0].name, juce::String ("Limiter"));
            expectEquals (q.masterPlugins[0].uniqueId, 7);
            // intro.wav does not exist, so the loader flags it
            expect (q.cues[0].fileMissing);
            expectEquals (warnings.size(), 1);
        }

        beginTest ("missing fields fall back to defaults");
        {
            Project q;
            juce::StringArray warnings;
            expect (ProjectSerializer::fromJson ("{\"cues\":[{\"name\":\"only name\"}]}", q, &warnings).wasOk());
            expectEquals ((int) q.cues.size(), 1);
            expectEquals (q.cues[0].name, juce::String ("only name"));
            expect (! q.cues[0].id.isNull());
            expect (q.cues[0].file == juce::File());
            expect (! q.cues[0].fileMissing);
            expectEquals (q.cues[0].fadeOutMs, 0);
            expectWithinAbsoluteError (q.cues[0].gainDb, 0.0, 1e-12);
            expect (! q.cues[0].audio.envelope.enabled);
            expectWithinAbsoluteError (q.cues[0].audio.startSeconds, 0.0, 1e-12);
            expectWithinAbsoluteError (q.cues[0].audio.endSeconds, -1.0, 1e-12);
            expectEquals (q.cues[0].audio.playCount, 1);
            expectWithinAbsoluteError (q.cues[0].audio.rate, 1.0, 1e-12);
            expectEquals ((int) q.cues[0].plugins.size(), 0);
            expectEquals ((int) q.masterPlugins.size(), 0);
            expectEquals (warnings.size(), 0);
        }

        beginTest ("version-1 fadeInMs migrates to the integrated fade envelope");
        {
            Project q;
            juce::StringArray warnings;
            expect (ProjectSerializer::fromJson ("{\"version\":1,\"cues\":[{\"name\":\"old\",\"fadeInMs\":250,\"fadeOutMs\":1500,\"gainDb\":-2}]}",
                                                 q, &warnings).wasOk());
            expectEquals ((int) q.cues.size(), 1);
            const auto& c = q.cues[0];
            expectEquals (c.fadeOutMs, 1500);
            expectWithinAbsoluteError (c.gainDb, -2.0, 1e-12);
            expect (c.audio.envelope.enabled && c.audio.envelope.linear && ! c.audio.envelope.lockToTrim);
            expectWithinAbsoluteError (c.audio.envelope.fadeInSeconds (30.0), 0.25, 1e-9);
            expectWithinAbsoluteError (c.audio.envelope.levelAt (0.125, 30.0), 0.5f, 1e-6f);
            expectEquals (warnings.size(), 0);

            Project none;
            expect (ProjectSerializer::fromJson ("{\"cues\":[{\"fadeInMs\":0}]}", none).wasOk());
            expect (! none.cues[0].audio.envelope.enabled);
        }

        beginTest ("unknown fields are ignored and a newer version only warns");
        {
            Project q;
            juce::StringArray warnings;
            expect (ProjectSerializer::fromJson ("{\"version\":99,\"future\":true,\"cues\":[{\"name\":\"x\",\"mystery\":[1,2],\"audio\":{\"later\":1}}]}",
                                                 q, &warnings).wasOk());
            expectEquals ((int) q.cues.size(), 1);
            expectEquals (warnings.size(), 1);
            expect (warnings[0].contains ("99"));
        }

        beginTest ("malformed input fails cleanly");
        {
            Project q;
            expect (ProjectSerializer::fromJson ("not json at all", q).failed());
            expect (ProjectSerializer::fromJson ("[1,2,3]", q).failed());
            juce::StringArray warnings;
            expect (ProjectSerializer::fromJson ("{\"cues\":[1,{\"name\":\"ok\"}]}", q, &warnings).wasOk());
            expectEquals ((int) q.cues.size(), 1);
            expectEquals (warnings.size(), 1);

            // an envelope with junk points keeps the valid ones
            Project e;
            expect (ProjectSerializer::fromJson ("{\"cues\":[{\"audio\":{\"envelope\":{\"enabled\":true,\"points\":[[0,0],\"junk\",[1],[0.5,2]]}}}]}", e).wasOk());
            expectEquals ((int) e.cues[0].audio.envelope.points.size(), 2);
            expectWithinAbsoluteError (e.cues[0].audio.envelope.points[1].level, 1.0, 1e-12);
        }

        beginTest ("duplicate cue ids are replaced so cues never share a player or chain");
        {
            Project q;
            juce::StringArray warnings;
            const juce::String json = "{\"cues\":[{\"id\":\"1b4e28ba2fa14d0180b0f55d9ef3b8c4\",\"name\":\"a\"},"
                                      "{\"id\":\"1b4e28ba2fa14d0180b0f55d9ef3b8c4\",\"name\":\"b\"}]}";
            expect (ProjectSerializer::fromJson (json, q, &warnings).wasOk());
            expectEquals ((int) q.cues.size(), 2);
            expect (q.cues[0].id != q.cues[1].id);
            expectEquals (q.cues[0].id.toString(), juce::String ("1b4e28ba2fa14d0180b0f55d9ef3b8c4"));
            expectEquals (warnings.size(), 1);
        }

        beginTest ("out-of-range values are sanitised");
        {
            Project q;
            expect (ProjectSerializer::fromJson ("{\"cues\":[{\"fadeInMs\":-100,\"fadeOutMs\":99999999,\"gainDb\":200,\"durationSeconds\":-1,"
                                                 "\"audio\":{\"start\":-4,\"end\":-9,\"playCount\":0,\"rate\":1000}}]}", q).wasOk());
            expect (! q.cues[0].audio.envelope.enabled);
            expectEquals (q.cues[0].fadeOutMs, Cue::maxFadeMs);
            expectWithinAbsoluteError (q.cues[0].gainDb, Cue::maxGainDb, 1e-12);
            expectWithinAbsoluteError (q.cues[0].durationSeconds, 0.0, 1e-12);
            expectWithinAbsoluteError (q.cues[0].audio.startSeconds, 0.0, 1e-12);
            expectWithinAbsoluteError (q.cues[0].audio.endSeconds, -1.0, 1e-12);
            expectEquals (q.cues[0].audio.playCount, 1);
            expectWithinAbsoluteError (q.cues[0].audio.rate, AudioCueData::maxRate, 1e-12);

            Project r;   // an end before the start plays to the end of the file instead of nothing
            expect (ProjectSerializer::fromJson ("{\"cues\":[{\"durationSeconds\":10,\"audio\":{\"start\":5,\"end\":3}}]}", r).wasOk());
            expectWithinAbsoluteError (r.cues[0].audio.endSeconds, -1.0, 1e-12);
            expectWithinAbsoluteError (r.cues[0].regionLength(), 5.0, 1e-12);
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
            expect (q.cues[0].file == tone);
            expect (! q.cues[0].fileMissing);
            expect (q.cues[1].fileMissing);
            expectEquals (warnings.size(), 1);
        }

        beginTest ("save and load through a file");
        {
            Project p;
            p.name = "Disk";
            Cue c;
            c.name = "cue";
            c.file = tempRoot.getChildFile ("audio").getChildFile ("tone.wav");
            p.cues.push_back (c);

            const auto projectFile = tempRoot.getChildFile ("show.gocue");
            expect (ProjectSerializer::save (p, projectFile).wasOk());
            expect (projectFile.existsAsFile());

            const auto text = projectFile.loadFileAsString();
            expect (text.contains ("\"fileRelative\""));
            expect (text.contains ("\"app\": \"GoCue\"") || text.contains ("\"app\":\"GoCue\""));

            Project q;
            juce::StringArray warnings;
            expect (ProjectSerializer::load (projectFile, q, &warnings).wasOk());
            expectEquals ((int) q.cues.size(), 1);
            expect (q.cues[0].file == c.file);
            expect (! q.cues[0].fileMissing);
            expect (ProjectSerializer::load (tempRoot.getChildFile ("missing.gocue"), q).failed());
        }

        expect (tempRoot.deleteRecursively());
    }
};

static ProjectSerializerTests projectSerializerTests;

} // namespace gocue::tests
