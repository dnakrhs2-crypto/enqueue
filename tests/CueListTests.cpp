#include "model/CueList.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

class CueListTests : public juce::UnitTest
{
public:
    CueListTests() : juce::UnitTest ("CueList", "GoCue") {}

    struct Counter : public CueList::Listener
    {
        int structure = 0, changed = 0, selection = 0, lastSelection = -2, lastChanged = -1;

        void cueListStructureChanged() override { ++structure; }
        void cueChanged (int index) override { ++changed; lastChanged = index; }
        void cueSelectionChanged (int index) override { ++selection; lastSelection = index; }
    };

    static Cue make (const juce::String& name)
    {
        Cue c;
        c.name = name;
        return c;
    }

    static juce::String names (const CueList& list)
    {
        juce::StringArray result;

        for (const auto& c : list.getAll())
            result.add (c.name);

        return result.joinIntoString (",");
    }

    void runTest() override
    {
        beginTest ("add appends / inserts and auto-selects the first cue");
        {
            CueList list;
            Counter counter;
            list.addListener (&counter);

            expectEquals (list.add (make ("a")), 0);
            expectEquals (list.getSelectedIndex(), 0);
            expectEquals (counter.lastSelection, 0);
            expectEquals (list.add (make ("c")), 1);
            expectEquals (list.add (make ("b"), 1), 1);
            expectEquals (names (list), juce::String ("a,b,c"));
            expectEquals (counter.structure, 3);

            list.setSelectedIndex (2);
            list.add (make ("x"), 0);
            expectEquals (names (list), juce::String ("x,a,b,c"));
            expectEquals (list.getSelectedIndex(), 3);
            expectEquals (list.getSelected()->name, juce::String ("c"));
            list.removeListener (&counter);
        }

        beginTest ("remove keeps a sensible selection");
        {
            CueList list;
            list.add (make ("a"));
            list.add (make ("b"));
            list.add (make ("c"));

            list.setSelectedIndex (2);
            list.remove (2);
            expectEquals (names (list), juce::String ("a,b"));
            expectEquals (list.getSelectedIndex(), 1);

            list.setSelectedIndex (0);
            list.remove (1);
            expectEquals (list.getSelectedIndex(), 0);
            expectEquals (list.getSelected()->name, juce::String ("a"));

            list.remove (0);
            expectEquals (list.size(), 0);
            expectEquals (list.getSelectedIndex(), -1);
            expect (list.getSelected() == nullptr);

            list.remove (5);   // out of range: no-op
            expectEquals (list.size(), 0);
        }

        beginTest ("duplicate inserts a copy with a fresh id right after the original");
        {
            CueList list;
            list.add (make ("a"));
            list.add (make ("b"));

            const auto originalId = list.get (0).id;
            expectEquals (list.duplicate (0), 1);
            expectEquals (names (list), juce::String ("a,a,b"));
            expect (list.get (1).id != originalId);
            expect (list.get (0).id == originalId);
            expectEquals (list.duplicate (9), -1);
        }

        beginTest ("move reorders and the selection follows the right cue");
        {
            CueList list;
            for (auto* n : { "a", "b", "c", "d" })
                list.add (make (n));

            list.setSelectedIndex (1);              // b
            expect (list.move (1, 3));
            expectEquals (names (list), juce::String ("a,c,d,b"));
            expectEquals (list.getSelectedIndex(), 3);

            list.setSelectedIndex (1);              // c
            expect (list.move (3, 0));              // b to the front
            expectEquals (names (list), juce::String ("b,a,c,d"));
            expectEquals (list.getSelectedIndex(), 2);
            expectEquals (list.getSelected()->name, juce::String ("c"));

            list.setSelectedIndex (3);              // d
            expect (list.move (0, 2));              // b down past the selection
            expectEquals (names (list), juce::String ("a,c,b,d"));
            expectEquals (list.getSelectedIndex(), 3);

            expect (! list.move (0, 0));
            expect (! list.move (9, 0));
            expect (! list.move (0, 9));
        }

        beginTest ("selectNext advances and stops at the end");
        {
            CueList list;
            expect (! list.selectNext());

            list.add (make ("a"));
            list.add (make ("b"));
            expectEquals (list.getSelectedIndex(), 0);
            expect (list.selectNext());
            expectEquals (list.getSelectedIndex(), 1);
            expect (! list.selectNext());
            expectEquals (list.getSelectedIndex(), 1);
        }

        beginTest ("setSelectedIndex clamps, clears and only notifies on change");
        {
            CueList list;
            Counter counter;
            list.add (make ("a"));
            list.add (make ("b"));
            list.addListener (&counter);

            list.setSelectedIndex (99);
            expectEquals (list.getSelectedIndex(), 1);
            expectEquals (counter.selection, 1);

            list.setSelectedIndex (1);
            expectEquals (counter.selection, 1);

            list.setSelectedIndex (-1);
            expectEquals (list.getSelectedIndex(), -1);
            expectEquals (counter.lastSelection, -1);
            list.removeListener (&counter);
        }

        beginTest ("update mutates in place, sanitises and notifies");
        {
            CueList list;
            Counter counter;
            list.add (make ("a"));
            list.addListener (&counter);

            list.update (0, [] (Cue& c)
            {
                c.name = "renamed";
                c.fadeInMs = -5;
                c.gainDb = 100.0;
            });

            expectEquals (list.get (0).name, juce::String ("renamed"));
            expectEquals (list.get (0).fadeInMs, 0);
            expectEquals (list.get (0).gainDb, Cue::maxGainDb);
            expectEquals (counter.changed, 1);
            expectEquals (counter.lastChanged, 0);

            list.update (7, [] (Cue& c) { c.name = "nope"; });
            expectEquals (counter.changed, 1);
            list.removeListener (&counter);
        }

        beginTest ("replaceAll / clear reset the selection");
        {
            CueList list;
            list.add (make ("old"));
            list.setSelectedIndex (0);

            list.replaceAll ({ make ("n1"), make ("n2") });
            expectEquals (names (list), juce::String ("n1,n2"));
            expectEquals (list.getSelectedIndex(), 0);

            list.clear();
            expectEquals (list.size(), 0);
            expectEquals (list.getSelectedIndex(), -1);
        }

        beginTest ("indexOf finds cues by id");
        {
            CueList list;
            list.add (make ("a"));
            list.add (make ("b"));
            expectEquals (list.indexOf (list.get (1).id), 1);
            expectEquals (list.indexOf (juce::Uuid()), -1);
        }
    }
};

static CueListTests cueListTests;

} // namespace gocue::tests
