#include "model/CueNumbering.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

class CueNumberingTests : public juce::UnitTest
{
public:
    CueNumberingTests() : juce::UnitTest ("CueNumbering", "Enqueue") {}

    static Cue numbered (const juce::String& number)
    {
        Cue c;
        c.number = number;
        return c;
    }

    void runTest() override
    {
        beginTest ("format trims trailing zeros and keeps up to three decimals");
        {
            expectEquals (CueNumbering::format (1.0), juce::String ("1"));
            expectEquals (CueNumbering::format (1.5), juce::String ("1.5"));
            expectEquals (CueNumbering::format (2.25), juce::String ("2.25"));
            expectEquals (CueNumbering::format (0.1 + 0.2), juce::String ("0.3"));
            expectEquals (CueNumbering::format (10.0005), juce::String ("10.001"));
        }

        beginTest ("isNumeric accepts plain decimals only");
        {
            expect (CueNumbering::isNumeric ("12"));
            expect (CueNumbering::isNumeric ("1.5"));
            expect (! CueNumbering::isNumeric ("A1"));
            expect (! CueNumbering::isNumeric ("1.2.3"));
            expect (! CueNumbering::isNumeric (""));
            expect (! CueNumbering::isNumeric ("."));
        }

        beginTest ("next continues from the cue above and skips taken numbers");
        {
            std::vector<Cue> cues { numbered ("1"), numbered ("2"), numbered ("3") };
            expectEquals (CueNumbering::next (cues, 3, 1.0), juce::String ("4"));
            expectEquals (CueNumbering::next (cues, 1, 1.0), juce::String ("4"));        // 2 is taken -> 3 taken -> 4
            expectEquals (CueNumbering::next (cues, 1, 0.5), juce::String ("1.5"));
            expectEquals (CueNumbering::next (cues, 0, 1.0), juce::String ("4"));        // nothing above: largest + 1

            std::vector<Cue> mixed { numbered ("A"), numbered ("7"), numbered ("") };
            expectEquals (CueNumbering::next (mixed, 1, 1.0), juce::String ("8"));       // above is "A": largest numeric + 1
            expectEquals (CueNumbering::next (mixed, 3, 1.0), juce::String ("8"));

            std::vector<Cue> none;
            expectEquals (CueNumbering::next (none, 0, 1.0), juce::String ("1"));
            expectEquals (CueNumbering::next (none, 0, 0.0), juce::String());
        }

        beginTest ("isUnique ignores the cue itself and empty numbers");
        {
            std::vector<Cue> cues { numbered ("1"), numbered ("2") };
            expect (! CueNumbering::isUnique (cues, "1"));
            expect (CueNumbering::isUnique (cues, "1", cues[0].id));
            expect (CueNumbering::isUnique (cues, "3"));
            expect (CueNumbering::isUnique (cues, ""));
        }

        beginTest ("generate applies start, increment, prefix and suffix");
        {
            CueNumbering::RenumberOptions options;
            options.start = 10.0;
            options.increment = 0.5;
            options.prefix = "S";
            options.suffix = "a";
            const auto numbers = CueNumbering::generate (3, options);
            expectEquals ((int) numbers.size(), 3);
            expectEquals (numbers[0], juce::String ("S10a"));
            expectEquals (numbers[1], juce::String ("S10.5a"));
            expectEquals (numbers[2], juce::String ("S11a"));
        }
    }
};

static CueNumberingTests cueNumberingTests;

} // namespace gocue::tests
