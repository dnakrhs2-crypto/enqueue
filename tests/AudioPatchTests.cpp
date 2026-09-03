#include "model/AudioPatch.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

namespace gocue::tests
{

class AudioPatchTests : public juce::UnitTest
{
public:
    AudioPatchTests() : juce::UnitTest ("AudioPatch", "Enqueue") {}

    void runTest() override
    {
        beginTest ("default patch: 16 cue outputs routed diagonally, unit gain");
        {
            const auto p = AudioPatch::makeDefault();
            expect (! p.id.isNull());
            expect (p.name.isNotEmpty());
            expectEquals (p.numCueOutputs, 16);
            expectWithinAbsoluteError (p.routingGain (0, 0), 1.0f, 1e-6f);
            expectWithinAbsoluteError (p.routingGain (3, 3), 1.0f, 1e-6f);
            expectWithinAbsoluteError (p.routingGain (0, 1), 0.0f, 1e-6f);
            expectWithinAbsoluteError (p.routingGain (7, 9), 0.0f, 1e-6f);   // beyond stored columns: default (silent)
            expectWithinAbsoluteError (p.routingGain (9, 9), 1.0f, 1e-6f);   // still diagonal inside the 16
            expectWithinAbsoluteError (p.routingGain (16, 16), 0.0f, 1e-6f);  // no such cue output
            expectEquals (p.cueOutputName (2), juce::String::fromUTF8 ("\xEC\xB6\x9C\xEB\xA0\xA5 3"));
        }

        beginTest ("setRouting grows the table, main level and silence apply");
        {
            auto p = AudioPatch::makeDefault();
            p.setRouting (1, 5, -6.0);
            expectEquals (p.numStoredDeviceOutputs(), 6);
            expectWithinAbsoluteError (p.routing (1, 5), -6.0, 1e-12);
            expectWithinAbsoluteError (p.routing (0, 5), LevelMatrix::silentDb, 1e-12);   // default for the new column
            expectWithinAbsoluteError (p.routing (5, 5), 0.0, 1e-12);                     // diagonal default

            p.mainDb = -6.0;
            expectWithinAbsoluteError (p.routingGain (1, 5), juce::Decibels::decibelsToGain (-12.0f), 1e-5f);
            p.mainDb = LevelMatrix::silentDb;
            expectWithinAbsoluteError (p.routingGain (1, 5), 0.0f, 1e-6f);

            p.setRouting (0, 200, 0.0);   // out of range: ignored
            expectEquals (p.numStoredDeviceOutputs(), 6);
        }

        beginTest ("stereo pairs cannot overlap and the last output cannot pair");
        {
            auto p = AudioPatch::makeDefault();
            p.numCueOutputs = 4;
            p.sanitise();
            p.cueOutputStereoWithNext = { 1, 1, 0, 1 };
            p.sanitise();
            expect (p.isFirstOfPair (0));
            expect (p.isSecondOfPair (1));
            expect (! p.isFirstOfPair (1));    // overlapped: cleared
            expect (! p.isFirstOfPair (3));    // nothing after it
            expect (! p.isSecondOfPair (0));
        }

        beginTest ("sanitise clamps the output count and levels, resizes the tables");
        {
            AudioPatch p;
            p.numCueOutputs = 500;
            p.mainDb = 100.0;
            p.routingDb = { { 5.0, -300.0 } };
            p.sanitise();
            expectEquals (p.numCueOutputs, AudioPatch::maxCueOutputs);
            expectWithinAbsoluteError (p.mainDb, LevelMatrix::maxDb, 1e-12);
            expectEquals ((int) p.routingDb.size(), AudioPatch::maxCueOutputs);
            expectEquals ((int) p.cueOutputInserts.size(), AudioPatch::maxCueOutputs);
            expectEquals (p.cueOutputNames.size(), AudioPatch::maxCueOutputs);
            expect (LevelMatrix::isSilent (p.routingDb[0][1]));
            expect (! p.id.isNull());

            p.numCueOutputs = 0;
            p.sanitise();
            expectEquals (p.numCueOutputs, 1);
        }

        beginTest ("JSON round trip incl. inserts, names, pairs and -inf");
        {
            auto p = AudioPatch::makeDefault ("Main");
            p.numCueOutputs = 4;
            p.sanitise();
            p.cueOutputNames.set (0, "L");
            p.cueOutputNames.set (1, "R");
            p.setRouting (2, 3, -3.5);
            p.setRouting (3, 3, LevelMatrix::silentDb);
            p.mainDb = -1.0;
            p.cueOutputStereoWithNext[0] = 1;
            PluginSlotState eq;
            eq.name = "EQ";
            eq.uniqueId = 42;
            eq.bypassed = true;
            p.cueOutputInserts[0].push_back (eq);
            p.deviceOutputInserts.resize (2);
            p.deviceOutputInserts[1].push_back (eq);

            const auto json = juce::JSON::toString (p.toVar());
            expect (json.contains ("\"-inf\""));
            const auto back = AudioPatch::fromVar (juce::JSON::parse (json));
            expect (back == p);
            expectEquals (back.cueOutputName (0), juce::String ("L"));
            expectEquals (back.cueOutputName (2), juce::String::fromUTF8 ("\xEC\xB6\x9C\xEB\xA0\xA5 3"));
            expect (back.isFirstOfPair (0));
            expectEquals ((int) back.cueOutputInserts[0].size(), 1);
            expect (back.cueOutputInserts[0][0].bypassed);
            expectEquals ((int) back.deviceOutputInserts[1].size(), 1);

            const auto fallback = AudioPatch::fromVar (juce::var());
            expectEquals (fallback.numCueOutputs, AudioPatch::defaultCueOutputs);
            expect (fallback.name.isNotEmpty());
        }
    }
};

static AudioPatchTests audioPatchTests;

} // namespace gocue::tests
