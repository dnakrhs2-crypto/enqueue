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
        int structure = 0, changed = 0, selection = 0, playhead = 0, lastSelection = -2, lastChanged = -1, lastPlayhead = -2;

        void cueListStructureChanged() override { ++structure; }
        void cueChanged (int index) override { ++changed; lastChanged = index; }
        void cueSelectionChanged (int index) override { ++selection; lastSelection = index; }
        void playheadChanged (int index) override { ++playhead; lastPlayhead = index; }
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

    static juce::String indices (const std::vector<int>& v)
    {
        juce::StringArray result;

        for (int i : v)
            result.add (juce::String (i));

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
            expectEquals (list.getPlayheadIndex(), 0);
            expectEquals (counter.lastSelection, 0);
            expectEquals (counter.lastPlayhead, 0);
            expectEquals (list.add (make ("c")), 1);
            expectEquals (list.add (make ("b"), 1), 1);
            expectEquals (names (list), juce::String ("a,b,c"));
            expectEquals (counter.structure, 3);

            list.setSelectedIndex (2);
            list.add (make ("x"), 0);
            expectEquals (names (list), juce::String ("x,a,b,c"));
            expectEquals (list.getSelectedIndex(), 3);
            expectEquals (list.getPlayheadIndex(), 3);
            expectEquals (list.getSelected()->name, juce::String ("c"));
            list.removeListener (&counter);
        }

        beginTest ("remove keeps a sensible selection and playhead");
        {
            CueList list;
            list.add (make ("a"));
            list.add (make ("b"));
            list.add (make ("c"));

            list.setSelectedIndex (2);
            list.remove (2);
            expectEquals (names (list), juce::String ("a,b"));
            expectEquals (list.getSelectedIndex(), 1);
            expectEquals (list.getPlayheadIndex(), 1);

            list.setSelectedIndex (0);
            list.remove (1);
            expectEquals (list.getSelectedIndex(), 0);
            expectEquals (list.getSelected()->name, juce::String ("a"));

            list.remove (0);
            expectEquals (list.size(), 0);
            expectEquals (list.getSelectedIndex(), -1);
            expectEquals (list.getPlayheadIndex(), -1);
            expect (list.getSelected() == nullptr);

            list.remove (5);   // out of range: no-op
            expectEquals (list.size(), 0);
        }

        beginTest ("removeIndices drops several rows and remaps the cursors");
        {
            CueList list;
            for (auto* n : { "a", "b", "c", "d", "e" })
                list.add (make (n));

            list.setSelection ({ 1, 3 }, 3);
            list.setLockPlayheadToSelection (false);
            list.setPlayheadIndex (4);
            list.removeIndices ({ 3, 1, 1 });
            expectEquals (names (list), juce::String ("a,c,e"));
            expectEquals (list.getPlayheadIndex(), 2);            // 'e'
            expectEquals (list.getSelectedIndex(), 2);            // the row that took the removed primary's place
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

        beginTest ("move reorders and the cursors follow the right cue");
        {
            CueList list;
            for (auto* n : { "a", "b", "c", "d" })
                list.add (make (n));

            list.setSelectedIndex (1);              // b
            expect (list.move (1, 3));
            expectEquals (names (list), juce::String ("a,c,d,b"));
            expectEquals (list.getSelectedIndex(), 3);
            expectEquals (list.getPlayheadIndex(), 3);

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

        beginTest ("moveIndices moves a block and keeps the selection on the same cues");
        {
            CueList list;
            for (auto* n : { "a", "b", "c", "d", "e" })
                list.add (make (n));

            list.setSelection ({ 0, 1 }, 1);
            expect (list.moveIndices ({ 0, 1 }, 3));           // a,b after c,d,e minus the block -> c,d,e then insert at 3
            expectEquals (names (list), juce::String ("c,d,e,a,b"));
            expectEquals (indices (list.getSelectedIndices()), juce::String ("3,4"));
            expectEquals (list.getSelectedIndex(), 4);
            expectEquals (list.getPlayheadIndex(), 4);

            expect (list.moveIndices ({ 4, 3 }, 0));
            expectEquals (names (list), juce::String ("a,b,c,d,e"));
            expectEquals (indices (list.getSelectedIndices()), juce::String ("0,1"));
        }

        beginTest ("advancePlayhead moves on and stops at the end; the selection follows when locked");
        {
            CueList list;
            expect (! list.advancePlayhead());

            list.add (make ("a"));
            list.add (make ("b"));
            expectEquals (list.getPlayheadIndex(), 0);
            expect (list.advancePlayhead());
            expectEquals (list.getPlayheadIndex(), 1);
            expectEquals (list.getSelectedIndex(), 1);
            expect (! list.advancePlayhead());
            expectEquals (list.getPlayheadIndex(), 1);
            expect (list.retreatPlayhead());
            expectEquals (list.getPlayheadIndex(), 0);
            expectEquals (list.getSelectedIndex(), 0);
            expect (list.selectNext());
            expectEquals (list.getPlayheadIndex(), 1);
        }

        beginTest ("unlocking lets the playhead and the selection move independently");
        {
            CueList list;
            Counter counter;
            for (auto* n : { "a", "b", "c" })
                list.add (make (n));

            list.addListener (&counter);
            list.setLockPlayheadToSelection (false);
            list.setSelectedIndex (2);
            expectEquals (list.getSelectedIndex(), 2);
            expectEquals (list.getPlayheadIndex(), 0);
            expectEquals (counter.playhead, 0);

            list.setPlayheadIndex (1);
            expectEquals (list.getPlayheadIndex(), 1);
            expectEquals (list.getSelectedIndex(), 2);
            expectEquals (counter.playhead, 1);
            expectEquals (counter.lastPlayhead, 1);

            list.advancePlayhead();
            expectEquals (list.getPlayheadIndex(), 2);
            expectEquals (list.getSelectedIndex(), 2);

            list.setLockPlayheadToSelection (true);
            list.setSelectedIndex (0);
            expectEquals (list.getPlayheadIndex(), 0);
            list.removeListener (&counter);
        }

        beginTest ("multi selection keeps a primary row and setSelectedIndex only notifies on change");
        {
            CueList list;
            Counter counter;
            for (auto* n : { "a", "b", "c", "d" })
                list.add (make (n));

            list.addListener (&counter);
            list.setSelection ({ 3, 1, 1 }, 1);
            expectEquals (indices (list.getSelectedIndices()), juce::String ("1,3"));
            expectEquals (list.getSelectedIndex(), 1);
            expect (list.isSelected (3) && ! list.isSelected (2));
            expectEquals (list.getPlayheadIndex(), 1);
            expectEquals (counter.selection, 1);

            list.setSelection ({ 0, 2 }, 9);                      // bad primary -> last of the set
            expectEquals (list.getSelectedIndex(), 2);

            list.selectAll();
            expectEquals (indices (list.getSelectedIndices()), juce::String ("0,1,2,3"));
            expectEquals (list.getSelectedIndex(), 2);            // unchanged primary

            list.setSelectedIndex (99);
            expectEquals (list.getSelectedIndex(), 3);
            expectEquals (indices (list.getSelectedIndices()), juce::String ("3"));
            const int before = counter.selection;
            list.setSelectedIndex (3);
            expectEquals (counter.selection, before);

            list.setSelectedIndex (-1);
            expectEquals (list.getSelectedIndex(), -1);
            expectEquals ((int) list.getSelectedIndices().size(), 0);
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
                c.audio.rate = 100.0;
                c.gainDb = 100.0;
                c.preWaitSeconds = -3.0;
            });

            expectEquals (list.get (0).name, juce::String ("renamed"));
            expectWithinAbsoluteError (list.get (0).audio.rate, AudioCueData::maxRate, 1e-12);
            expectEquals (list.get (0).gainDb, Cue::maxGainDb);
            expectWithinAbsoluteError (list.get (0).preWaitSeconds, 0.0, 1e-12);
            expectEquals (counter.changed, 1);
            expectEquals (counter.lastChanged, 0);

            list.update (7, [] (Cue& c) { c.name = "nope"; });
            expectEquals (counter.changed, 1);
            list.removeListener (&counter);
        }

        beginTest ("replaceAll / clear reset the cursors");
        {
            CueList list;
            list.add (make ("old"));
            list.setSelectedIndex (0);

            list.replaceAll ({ make ("n1"), make ("n2") });
            expectEquals (names (list), juce::String ("n1,n2"));
            expectEquals (list.getSelectedIndex(), 0);
            expectEquals (list.getPlayheadIndex(), 0);

            list.clear();
            expectEquals (list.size(), 0);
            expectEquals (list.getSelectedIndex(), -1);
            expectEquals (list.getPlayheadIndex(), -1);
        }

        beginTest ("isNumberTaken sees other cues' numbers only");
        {
            CueList list;
            Cue x, y;
            x.number = "1";
            y.number = "2";
            list.add (x);
            list.add (y);
            expect (list.isNumberTaken ("1", y.id));
            expect (! list.isNumberTaken ("1", x.id));
            expect (! list.isNumberTaken ("3", y.id));
            expect (! list.isNumberTaken ("", y.id));
        }

        beginTest ("indexOf / findById find cues by id");
        {
            CueList list;
            list.add (make ("a"));
            list.add (make ("b"));
            expectEquals (list.indexOf (list.get (1).id), 1);
            expectEquals (list.indexOf (juce::Uuid()), -1);
            expect (list.findById (list.get (0).id) == &list.get (0));
            expect (list.findById (juce::Uuid()) == nullptr);
        }
    }
};

static CueListTests cueListTests;

} // namespace gocue::tests
