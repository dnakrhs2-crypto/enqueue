#include "model/LevelMatrix.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue::tests
{

class LevelMatrixTests : public juce::UnitTest
{
public:
    LevelMatrixTests() : juce::UnitTest ("LevelMatrix", "Enqueue") {}

    void runTest() override
    {
        beginTest ("default routing: stereo diagonal, mono to both outputs, extra channels silent");
        {
            LevelMatrix stereo;
            stereo.resize (2, 2);
            expectWithinAbsoluteError (stereo.gainFor (0, 0), 1.0f, 1e-6f);
            expectWithinAbsoluteError (stereo.gainFor (1, 1), 1.0f, 1e-6f);
            expectWithinAbsoluteError (stereo.gainFor (0, 1), 0.0f, 1e-6f);
            expectWithinAbsoluteError (stereo.gainFor (1, 0), 0.0f, 1e-6f);

            LevelMatrix mono;
            mono.resize (1, 4);
            expectWithinAbsoluteError (mono.gainFor (0, 0), 1.0f, 1e-6f);
            expectWithinAbsoluteError (mono.gainFor (0, 1), 1.0f, 1e-6f);
            expectWithinAbsoluteError (mono.gainFor (0, 2), 0.0f, 1e-6f);

            LevelMatrix quad;
            quad.resize (4, 2);
            expectWithinAbsoluteError (quad.gainFor (2, 0), 0.0f, 1e-6f);
            expectWithinAbsoluteError (quad.gainFor (3, 1), 0.0f, 1e-6f);
            expectWithinAbsoluteError (quad.gainFor (1, 1), 1.0f, 1e-6f);

            expectWithinAbsoluteError (stereo.gainFor (5, 0), 0.0f, 1e-6f);   // out of range
        }

        beginTest ("gainFor sums input, crosspoint and output; any silent part gives 0");
        {
            LevelMatrix m;
            m.resize (2, 2);
            m.inputDb[0] = -6.0;
            m.crosspointDb[0][0] = -6.0;
            m.outputDb[0] = 6.0;
            expectWithinAbsoluteError (m.gainFor (0, 0), juce::Decibels::decibelsToGain (-6.0f), 1e-5f);

            m.outputDb[0] = LevelMatrix::silentDb;
            expectWithinAbsoluteError (m.gainFor (0, 0), 0.0f, 1e-6f);
            m.outputDb[0] = 0.0;
            m.inputDb[0] = -200.0;
            expectWithinAbsoluteError (m.gainFor (0, 0), 0.0f, 1e-6f);
        }

        beginTest ("resize keeps existing levels and adds default crosspoints for new outputs");
        {
            LevelMatrix m;
            m.resize (2, 2);
            m.crosspointDb[0][1] = -3.0;
            m.outputDb[1] = -1.0;
            m.resize (2, 4);
            expectEquals (m.numOutputs(), 4);
            expectWithinAbsoluteError (m.crosspointDb[0][1], -3.0, 1e-12);
            expectWithinAbsoluteError (m.outputDb[1], -1.0, 1e-12);
            expect (LevelMatrix::isSilent (m.crosspointDb[0][2]));
            expect (LevelMatrix::isSilent (m.crosspointDb[1][3]));
            expectWithinAbsoluteError (m.outputDb[3], 0.0, 1e-12);

            m.resize (1, 4);   // becomes mono: routing re-derived
            expectWithinAbsoluteError (m.crosspointDb[0][0], 0.0, 1e-12);
            expectWithinAbsoluteError (m.crosspointDb[0][1], 0.0, 1e-12);
            expect (LevelMatrix::isSilent (m.crosspointDb[0][2]));

            m.resize (30, 200);   // clamped to the maxima
            expectEquals (m.numInputs(), LevelMatrix::maxInputs);
            expectEquals (m.numOutputs(), LevelMatrix::maxOutputs);
        }

        beginTest ("setDefaults / silenceCrosspoints");
        {
            LevelMatrix m;
            m.resize (2, 2);
            m.inputDb[1] = -20.0;
            m.crosspointDb[0][0] = -20.0;
            m.silenceCrosspoints();
            expect (LevelMatrix::isSilent (m.crosspointDb[1][1]));
            expectWithinAbsoluteError (m.inputDb[1], -20.0, 1e-12);
            m.setDefaults();
            expectWithinAbsoluteError (m.inputDb[1], 0.0, 1e-12);
            expectWithinAbsoluteError (m.crosspointDb[0][0], 0.0, 1e-12);
            expect (LevelMatrix::isSilent (m.crosspointDb[0][1]));
        }

        beginTest ("sanitise clamps and rounds, gangs limited to 0..8");
        {
            LevelMatrix m;
            m.resize (2, 2);
            m.inputDb[0] = 100.0;
            m.outputDb[0] = -500.0;
            m.crosspointDb[1][1] = -3.14159;
            m.crosspointDb[0].resize (1);   // short row
            m.inputGang[0] = 42;
            m.mainGang = -3;
            m.sanitise();
            expectWithinAbsoluteError (m.inputDb[0], LevelMatrix::maxDb, 1e-12);
            expect (LevelMatrix::isSilent (m.outputDb[0]));
            expectWithinAbsoluteError (m.crosspointDb[1][1], -3.1, 1e-12);
            expectEquals ((int) m.crosspointDb[0].size(), 2);
            expectEquals (m.inputGang[0], 8);
            expectEquals (m.mainGang, 0);
        }

        beginTest ("JSON round trip with -inf");
        {
            LevelMatrix m;
            m.resize (2, 3);
            m.inputDb[1] = -6.5;
            m.outputDb[2] = LevelMatrix::silentDb;
            m.crosspointDb[0][2] = -12.0;
            m.crosspointGang[1][1] = 3;
            m.mainGang = 1;

            const auto json = juce::JSON::toString (m.toVar());
            expect (json.contains ("\"-inf\""));

            const auto back = LevelMatrix::fromVar (juce::JSON::parse (json));
            expect (back == m);

            // a crosspoint row shorter than the outputs is padded with the default routing
            const auto padded = LevelMatrix::fromVar (juce::JSON::parse ("{\"inputs\":[0,0],\"outputs\":[0,0,0],\"crosspoints\":[[0],[\"-inf\",0]]}"));
            expectEquals (padded.numInputs(), 2);
            expectEquals (padded.numOutputs(), 3);
            expect (LevelMatrix::isSilent (padded.crosspointDb[0][1]));
            expect (LevelMatrix::isSilent (padded.crosspointDb[0][2]));
            expectWithinAbsoluteError (padded.crosspointDb[1][1], 0.0, 1e-12);

            expect (LevelMatrix::fromVar (juce::var()) == LevelMatrix());
            expectWithinAbsoluteError (dbFromVar (juce::var ("-INF")), LevelMatrix::silentDb, 1e-12);
            expectWithinAbsoluteError (dbFromVar (juce::var ("abc"), 5.0), 5.0, 1e-12);   // unparsable string -> the caller's default, never 0 dB
            expectWithinAbsoluteError (dbFromVar (juce::var ("-6.5"), 5.0), -6.5, 1e-12);
            expectWithinAbsoluteError (dbFromVar (juce::var ("3dB"), 5.0), 5.0, 1e-12);
        }

        beginTest ("trim levels");
        {
            TrimLevels t;
            t.resize (2);
            t.mainDb = 3.0;
            t.outputDb[1] = -3.0;
            expectWithinAbsoluteError (t.gainForOutput (0), juce::Decibels::decibelsToGain (3.0f), 1e-5f);
            expectWithinAbsoluteError (t.gainForOutput (1), 1.0f, 1e-5f);
            expectWithinAbsoluteError (t.gainForOutput (7), juce::Decibels::decibelsToGain (3.0f), 1e-5f);   // missing output = 0 dB trim

            t.mainDb = -1000.0;
            t.sanitise();
            expectWithinAbsoluteError (t.mainDb, -60.0, 1e-12);

            const auto back = TrimLevels::fromVar (juce::JSON::parse (juce::JSON::toString (t.toVar())));
            expect (back == t);
        }
    }
};

static LevelMatrixTests levelMatrixTests;

} // namespace gocue::tests
