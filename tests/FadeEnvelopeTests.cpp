#include "audio/FadeEnvelope.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

class FadeEnvelopeTests : public juce::UnitTest
{
public:
    FadeEnvelopeTests() : juce::UnitTest ("FadeEnvelope", "GoCue") {}

    static void fill (juce::AudioBuffer<float>& buffer, float value)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), value, buffer.getNumSamples());
    }

    void runTest() override
    {
        juce::AudioBuffer<float> buffer (2, 64);

        beginTest ("fade-in ramps linearly across block boundaries");
        {
            FadeEnvelope env;
            env.prepare (1000.0);          // 1 ms == 1 sample
            env.setLevel (0.0f);
            env.fadeIn (100.0);            // 100-sample ramp
            expect (env.isRamping());
            expectEquals (env.getRemainingSamples(), 100);

            fill (buffer, 1.0f);
            env.applyToBuffer (buffer, 0, 64);
            expectWithinAbsoluteError (buffer.getSample (0, 0), 0.0f, 1e-6f);
            expectWithinAbsoluteError (buffer.getSample (1, 50), 0.5f, 1e-4f);
            expectWithinAbsoluteError (env.getLevel(), 0.64f, 1e-4f);

            fill (buffer, 1.0f);
            env.applyToBuffer (buffer, 0, 64);   // samples 64..127: ramp ends at 100
            expectWithinAbsoluteError (buffer.getSample (0, 0), 0.64f, 1e-4f);
            expectWithinAbsoluteError (buffer.getSample (0, 40), 1.0f, 1e-6f);
            expectWithinAbsoluteError (buffer.getSample (1, 63), 1.0f, 1e-6f);
            expect (! env.isRamping());
            expectEquals (env.getLevel(), 1.0f);
            expect (! env.isFadingOut());
        }

        beginTest ("zero-length fades jump immediately");
        {
            FadeEnvelope env;
            env.prepare (48000.0);
            env.setLevel (0.0f);
            env.fadeIn (0.0);
            expect (! env.isRamping());
            expectEquals (env.getLevel(), 1.0f);

            env.fadeOut (0.0);
            expect (env.hasReachedSilence());
        }

        beginTest ("fade-out reaches silence and reports it");
        {
            FadeEnvelope env;
            env.prepare (1000.0);
            env.setLevel (1.0f);
            env.fadeOut (10.0);            // 10-sample ramp
            expect (env.isFadingOut());
            expect (! env.hasReachedSilence());

            fill (buffer, 1.0f);
            env.applyToBuffer (buffer, 0, 64);
            expectWithinAbsoluteError (buffer.getSample (0, 0), 1.0f, 1e-6f);
            expectWithinAbsoluteError (buffer.getSample (0, 5), 0.5f, 1e-4f);
            expectWithinAbsoluteError (buffer.getSample (0, 10), 0.0f, 1e-6f);
            expectWithinAbsoluteError (buffer.getSample (1, 63), 0.0f, 1e-6f);
            expect (env.hasReachedSilence());
        }

        beginTest ("a fade-out started mid fade-in continues from the current level");
        {
            FadeEnvelope env;
            env.prepare (1000.0);
            env.setLevel (0.0f);
            env.fadeIn (100.0);

            fill (buffer, 1.0f);
            env.applyToBuffer (buffer, 0, 50);
            expectWithinAbsoluteError (env.getLevel(), 0.5f, 1e-4f);

            env.fadeOut (50.0);
            fill (buffer, 1.0f);
            env.applyToBuffer (buffer, 0, 64);
            expectWithinAbsoluteError (buffer.getSample (0, 0), 0.5f, 1e-4f);
            expectWithinAbsoluteError (buffer.getSample (0, 25), 0.25f, 1e-4f);
            expectWithinAbsoluteError (buffer.getSample (0, 60), 0.0f, 1e-6f);
            expect (env.hasReachedSilence());
        }

        beginTest ("levels are clamped to [0, 1]");
        {
            FadeEnvelope env;
            env.setLevel (7.0f);
            expectEquals (env.getLevel(), 1.0f);
            env.fadeTo (-3.0f, 0.0);
            expectEquals (env.getLevel(), 0.0f);
        }
    }
};

static FadeEnvelopeTests fadeEnvelopeTests;

} // namespace gocue::tests
