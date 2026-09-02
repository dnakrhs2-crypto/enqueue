#include "model/CueList.h"
#include "model/CueNumbering.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

class CueOrderTests : public juce::UnitTest
{
public:
    CueOrderTests() : juce::UnitTest ("CueOrder", "GoCue") {}

    static Cue make (const juce::String& number)
    {
        Cue c;
        c.number = number;
        c.name = number;
        return c;
    }

    static juce::String numbers (const CueList& list)
    {
        juce::StringArray r;

        for (const auto& c : list.getAll())
            r.add (c.number);

        return r.joinIntoString (",");
    }

    void runTest() override
    {
        beginTest ("compare: numeric when both are numbers, natural order otherwise");
        {
            using namespace CueNumbering;
            expect (compare ("2", "12") < 0);
            expect (compare ("1.5", "1.25") > 0);
            expect (compare ("3", "3.0") == 0);
            expect (compare ("A2", "A10") < 0);
            expect (compare ("", "1") < 0);
        }

        beginTest ("placeByNumber moves a renumbered cue to where its number belongs among its siblings");
        {
            CueList list;

            for (auto n : { "1", "2", "3", "4", "5" })
                list.add (make (n));

            list.update (3, [] (Cue& c) { c.number = "12"; });
            expectEquals (list.placeByNumber (3), 4);
            expectEquals (numbers (list), juce::String ("1,2,3,5,12"));

            list.update (4, [] (Cue& c) { c.number = "0.5"; });
            expectEquals (list.placeByNumber (4), 0);
            expectEquals (numbers (list), juce::String ("0.5,1,2,3,5"));

            list.update (2, [] (Cue& c) { c.number = "2.5"; });   // already in place: nothing moves
            expectEquals (list.placeByNumber (2), 2);
            expectEquals (numbers (list), juce::String ("0.5,1,2.5,3,5"));

            list.update (1, [] (Cue& c) { c.number = ""; });      // no number: stays put
            expectEquals (list.placeByNumber (1), 1);
            expectEquals (numbers (list), juce::String ("0.5,,2.5,3,5"));
        }

        beginTest ("placeByNumber leaves hand-ordered siblings alone and moves only the renumbered cue");
        {
            CueList list;

            for (auto n : { "3", "1", "2" })
                list.add (make (n));

            list.update (2, [] (Cue& c) { c.number = "2.5"; });   // still right after "1", the last smaller sibling
            expectEquals (list.placeByNumber (2), 2);
            expectEquals (numbers (list), juce::String ("3,1,2.5"));

            list.update (1, [] (Cue& c) { c.number = "5"; });     // past every sibling: to the end
            expectEquals (list.placeByNumber (1), 2);
            expectEquals (numbers (list), juce::String ("3,2.5,5"));

            list.update (2, [] (Cue& c) { c.number = "0.1"; });   // smaller than all: in front of the first greater
            expectEquals (list.placeByNumber (2), 0);
            expectEquals (numbers (list), juce::String ("0.1,3,2.5"));
        }

        beginTest ("placeByNumber keeps a child inside its group and moves a group with its children");
        {
            CueList list;

            for (auto n : { "1", "2", "3", "4" })
                list.add (make (n));

            Cue g = make ("2");
            g.type = CueType::group;
            const int groupIndex = list.wrapInGroup ({ 1, 2 }, g);   // 1, [2: 2, 3], 4
            expectEquals (groupIndex, 1);
            expectEquals (numbers (list), juce::String ("1,2,2,3,4"));

            // the first child gets a number past the second: it moves after it, still inside the group
            list.update (2, [] (Cue& c) { c.number = "3.5"; });
            expectEquals (list.placeByNumber (2), 3);
            expectEquals (numbers (list), juce::String ("1,2,3,3.5,4"));
            expectEquals (list.parentIndexOf (3), 1);

            // the group itself gets a number past the last top-level cue: the whole subtree moves to the end
            list.update (1, [] (Cue& c) { c.number = "9"; });
            expectEquals (list.placeByNumber (1), 2);
            expectEquals (numbers (list), juce::String ("1,4,9,3,3.5"));
            expectEquals (list.parentIndexOf (3), 2);
            expectEquals (list.parentIndexOf (4), 2);
        }
    }
};

static CueOrderTests cueOrderTests;

} // namespace gocue::tests
