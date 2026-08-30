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

        beginTest ("round trip preserves cues, master chain and plugin states");
        {
            Project p;
            p.name = "Show";

            Cue a;
            a.name = "Intro";
            a.file = tempRoot.getChildFile ("intro.wav");
            a.fadeInMs = 250;
            a.fadeOutMs = 1500;
            a.gainDb = -3.5;
            a.durationSeconds = 12.25;

            PluginSlotState slot;
            slot.name = "EQ";
            slot.fileOrIdentifier = "C:\\Plugins\\eq.vst3";
            slot.uniqueId = 42;
            slot.stateBase64 = "AAECAw==";
            slot.bypassed = true;
            a.plugins.push_back (slot);
            p.cues.push_back (a);

            Cue b;
            b.name = "Outro";
            p.cues.push_back (b);

            PluginSlotState master;
            master.name = "Limiter";
            master.uniqueId = 7;
            p.masterPlugins.push_back (master);

            const auto json = ProjectSerializer::toJson (p);
            expect (json.contains ("\"version\": 1") || json.contains ("\"version\":1"));

            Project q;
            juce::StringArray warnings;
            expect (ProjectSerializer::fromJson (json, q, &warnings).wasOk());
            expectEquals (q.name, juce::String ("Show"));
            expectEquals ((int) q.cues.size(), 2);
            expect (q.cues[0].id == a.id);
            expectEquals (q.cues[0].name, juce::String ("Intro"));
            expect (q.cues[0].file == a.file);
            expectEquals (q.cues[0].fadeInMs, 250);
            expectEquals (q.cues[0].fadeOutMs, 1500);
            expectWithinAbsoluteError (q.cues[0].gainDb, -3.5, 1e-9);
            expectWithinAbsoluteError (q.cues[0].durationSeconds, 12.25, 1e-9);
            expectEquals ((int) q.cues[0].plugins.size(), 1);
            expectEquals (q.cues[0].plugins[0].name, juce::String ("EQ"));
            expectEquals (q.cues[0].plugins[0].fileOrIdentifier, juce::String ("C:\\Plugins\\eq.vst3"));
            expectEquals (q.cues[0].plugins[0].uniqueId, 42);
            expectEquals (q.cues[0].plugins[0].stateBase64, juce::String ("AAECAw=="));
            expect (q.cues[0].plugins[0].bypassed);
            expect (q.cues[1].id == b.id);
            expect (q.cues[1].file == juce::File());
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
            expectEquals (q.cues[0].fadeInMs, 0);
            expectEquals (q.cues[0].fadeOutMs, 0);
            expectWithinAbsoluteError (q.cues[0].gainDb, 0.0, 1e-12);
            expectEquals ((int) q.cues[0].plugins.size(), 0);
            expectEquals ((int) q.masterPlugins.size(), 0);
            expectEquals (warnings.size(), 0);
        }

        beginTest ("unknown fields are ignored and a newer version only warns");
        {
            Project q;
            juce::StringArray warnings;
            expect (ProjectSerializer::fromJson ("{\"version\":99,\"future\":true,\"cues\":[{\"name\":\"x\",\"mystery\":[1,2]}]}",
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
        }

        beginTest ("out-of-range values are sanitised");
        {
            Project q;
            expect (ProjectSerializer::fromJson ("{\"cues\":[{\"fadeInMs\":-100,\"fadeOutMs\":99999999,\"gainDb\":200,\"durationSeconds\":-1}]}", q).wasOk());
            expectEquals (q.cues[0].fadeInMs, 0);
            expectEquals (q.cues[0].fadeOutMs, Cue::maxFadeMs);
            expectWithinAbsoluteError (q.cues[0].gainDb, Cue::maxGainDb, 1e-12);
            expectWithinAbsoluteError (q.cues[0].durationSeconds, 0.0, 1e-12);
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
