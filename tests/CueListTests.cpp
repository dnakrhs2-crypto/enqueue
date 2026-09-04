#include "model/CueList.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

class CueListTests : public juce::UnitTest
{
public:
    CueListTests() : juce::UnitTest ("CueList", "Enqueue") {}

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

        beginTest ("tree: depth, subtree, children, siblings, visibility");
        {
            CueList list;
            Cue g = make ("G");
            g.type = CueType::group;
            list.add (g);                                   // 0 G
            Cue a = make ("a"); a.parentId = g.id;
            Cue inner = make ("H"); inner.type = CueType::group; inner.parentId = g.id;
            Cue b = make ("b"); b.parentId = inner.id;
            list.add (a);                                   // 1 a (G)
            list.add (inner);                               // 2 H (G)
            list.add (b);                                   // 3 b (H)
            list.add (make ("c"));                          // 4 c
            expectEquals (list.depthOf (0), 0);
            expectEquals (list.depthOf (1), 1);
            expectEquals (list.depthOf (3), 2);
            expectEquals (list.parentIndexOf (3), 2);
            expectEquals (list.parentIndexOf (4), -1);
            expect (list.isDescendantOf (3, g.id));
            expect (! list.isDescendantOf (4, g.id));
            expectEquals (list.subtreeEnd (0), 4);
            expectEquals (list.subtreeEnd (2), 4);
            expectEquals (list.subtreeEnd (1), 2);
            expectEquals (indices (list.childrenOf (0)), juce::String ("1,2"));
            expectEquals (indices (list.descendantsOf (0)), juce::String ("1,2,3"));
            expectEquals (list.nextSibling (1), 2);
            expectEquals (list.nextSibling (2), -1);        // H is the last child of G
            expectEquals (list.nextSibling (0), 4);
            expectEquals (list.nextSibling (4), -1);
            expect (list.parentForInsertion (1) == g.id);
            expect (list.parentForInsertion (4).isNull());
            expect (list.parentForInsertion (5).isNull());

            // collapse G: its rows disappear from the visible order, the playhead skips them
            list.setCollapsed (0, true);
            expect (! list.isRowVisible (1) && ! list.isRowVisible (3) && list.isRowVisible (0) && list.isRowVisible (4));
            expectEquals (list.nextVisible (0), 4);
            expectEquals (list.previousVisible (4), 0);
            list.setPlayheadIndex (0);
            expect (list.advancePlayhead());
            expectEquals (list.getPlayheadIndex(), 4);
            expect (list.retreatPlayhead());
            expectEquals (list.getPlayheadIndex(), 0);
            list.setCollapsed (0, false);
            expect (list.advancePlayhead());
            expectEquals (list.getPlayheadIndex(), 1);

            // addAfter inserts a sibling after the subtree
            const int afterA = list.addAfter (make ("a2"), 1);
            expectEquals (afterA, 2);
            expect (list.get (2).parentId == g.id);
            const int afterG = list.addAfter (make ("G2"), 0);
            expectEquals (afterG, 5);                        // G a a2 H b | G2 c
            expect (list.get (5).parentId.isNull());
            expectEquals (list.addAfter (make ("z"), -1), 7);
        }

        beginTest ("tree: remove / move / duplicate keep subtrees together");
        {
            CueList list;
            Cue g = make ("G"); g.type = CueType::group;
            list.add (g);
            Cue a = make ("a"); a.parentId = g.id;
            Cue b = make ("b"); b.parentId = g.id;
            list.add (a);
            list.add (b);
            list.add (make ("c"));
            list.add (make ("d"));                          // G a b c d

            // moving c in front of a (inside G's range) makes it G's child
            expect (list.moveSubtrees ({ 3 }, 1));
            expectEquals (names (list), juce::String ("G,c,a,b,d"));
            expect (list.get (1).parentId == g.id);

            // moving the group moves its children; dropping at the end leaves the group at the top level
            expect (list.moveSubtrees ({ 0 }, 5));
            expectEquals (names (list), juce::String ("d,G,c,a,b"));
            expect (list.get (1).parentId.isNull() && list.get (2).parentId == g.id && list.get (4).parentId == g.id);

            // duplicating the group copies the subtree with new ids and re-pointed parents
            const int copyAt = list.duplicate (1);
            expectEquals (copyAt, 5);
            expectEquals (names (list), juce::String ("d,G,c,a,b,G,c,a,b"));
            expect (list.get (5).id != g.id);
            expect (list.get (6).parentId == list.get (5).id && list.get (8).parentId == list.get (5).id);

            // removing a group removes its children
            list.removeIndices ({ 5 });
            expectEquals (names (list), juce::String ("d,G,c,a,b"));
            list.remove (1);
            expectEquals (names (list), juce::String ("d"));
        }

        beginTest ("tree: addIntoGroup appends a new last child (files dropped onto a group)");
        {
            CueList list;
            list.add (make ("a"));
            Cue g = make ("G");
            g.type = CueType::group;
            list.add (g);
            list.add (make ("b"));
            const int gi = 1;
            const int c1 = list.addIntoGroup (make ("c1"), gi);
            const int c2 = list.addIntoGroup (make ("c2"), gi);
            expectEquals (c1, 2);
            expectEquals (c2, 3);
            expect (list.get (c1).parentId == list.get (gi).id && list.get (c2).parentId == list.get (gi).id);
            expectEquals ((int) list.childrenOf (gi).size(), 2);
            expectEquals (list.subtreeEnd (gi), 4);
            expect (list.get (4).parentId.isNull());   // b stays outside the group
            expectEquals (list.addIntoGroup (make ("x"), 0), 5);   // not a group: appended at the end, top level
            expect (list.get (5).parentId.isNull());
        }

        beginTest ("tree: moveSubtreesInto puts rows at the end of a group (rows dropped onto a group row)");
        {
            CueList list;
            Cue g = make ("G"); g.type = CueType::group;
            list.add (g);
            Cue a = make ("a"); a.parentId = g.id;
            list.add (a);
            list.add (make ("b"));
            list.add (make ("c"));
            Cue h = make ("H"); h.type = CueType::group;
            list.add (h);                                    // G a | b c H

            expect (list.moveSubtreesInto ({ 3 }, 0));       // c into G, from after it
            expectEquals (names (list), juce::String ("G,a,c,b,H"));
            expect (list.get (2).parentId == g.id);
            expectEquals (list.subtreeEnd (0), 3);

            expect (list.moveSubtreesInto ({ 1 }, 0));       // a, already inside, goes to the end of G
            expectEquals (names (list), juce::String ("G,c,a,b,H"));
            expect (list.get (2).parentId == g.id);

            expect (list.moveSubtreesInto ({ 0 }, 4));       // the group G (with its children) into H, from before it
            expectEquals (names (list), juce::String ("b,H,G,c,a"));
            expect (list.get (2).parentId == list.get (1).id && list.get (3).parentId == g.id && list.get (4).parentId == g.id);
            expectEquals (list.depthOf (4), 2);

            expect (! list.moveSubtreesInto ({ 1 }, 1));     // H onto itself
            expect (! list.moveSubtreesInto ({ 1 }, 2));     // H into G, which sits inside H
            expect (! list.moveSubtreesInto ({ 0 }, 0));     // b is not a group
            expectEquals (names (list), juce::String ("b,H,G,c,a"));
        }

        beginTest ("tree: a move that would nest deeper than maxDepth is refused");
        {
            CueList list;
            int parent = -1;

            for (int i = 0; i < CueList::maxDepth; ++i)   // g0 > g1 > ... > g31: depths 0..31
            {
                Cue g = make ("g" + juce::String (i));
                g.type = CueType::group;
                parent = parent < 0 ? list.add (g) : list.addIntoGroup (g, parent);
            }

            expectEquals (list.depthOf (parent), CueList::maxDepth - 1);

            Cue h = make ("H");
            h.type = CueType::group;
            const int hi = list.add (h);                    // top level, after the chain
            const int ci = list.addIntoGroup (make ("c"), hi);
            expectEquals (list.subtreeDepthBelow (hi), 1);
            expectEquals (list.subtreeDepthBelow (ci), 0);

            expect (! list.moveSubtreesInto ({ hi }, parent));   // H would sit at 32 and c at 33: refused
            expect (list.get (hi).parentId.isNull());
            expect (list.moveSubtreesInto ({ ci }, parent));     // c alone at 32: the limit itself is fine
            expectEquals (list.depthOf (parent + 1), CueList::maxDepth);   // c sits right after g31, at the limit

            expect (! list.moveSubtrees ({ hi }, parent + 1));   // between-rows into the deepest group: the same limit
        }

        beginTest ("tree: wrapInGroup / ungroup");
        {
            CueList list;
            list.add (make ("a"));
            list.add (make ("b"));
            list.add (make ("c"));
            list.add (make ("d"));
            Cue g = make ("G");
            const int gi = list.wrapInGroup ({ 1, 2 }, g);
            expectEquals (gi, 1);
            expectEquals (names (list), juce::String ("a,G,b,c,d"));
            expect (list.get (1).isGroup() && list.get (1).parentId.isNull());
            expect (list.get (2).parentId == list.get (1).id && list.get (3).parentId == list.get (1).id);
            expect (list.get (4).parentId.isNull());
            expectEquals (list.getSelectedIndex(), 1);

            // a nested wrap inside the group keeps the outer parent
            const int inner = list.wrapInGroup ({ 3 }, make ("H"));
            expectEquals (inner, 3);
            expect (list.get (3).parentId == list.get (1).id);
            expect (list.get (4).parentId == list.get (3).id);
            expectEquals (list.subtreeEnd (1), 5);

            expect (list.ungroup (3));
            expectEquals (names (list), juce::String ("a,G,b,c,d"));
            expect (list.get (3).parentId == list.get (1).id);
            expect (list.ungroup (1));
            expectEquals (names (list), juce::String ("a,b,c,d"));
            expect (list.get (1).parentId.isNull() && list.get (2).parentId.isNull());
            expect (! list.ungroup (0));
        }

        beginTest ("tree: a drop onto a moved row is a no-op; the parent is decided before the move; copies keep inner targets");
        {
            CueList list;
            Cue g = make ("G"); g.type = CueType::group;
            list.add (g);
            Cue h = make ("H"); h.type = CueType::group; h.parentId = g.id;
            list.add (h);
            Cue x = make ("x"); x.parentId = h.id;
            list.add (x);
            list.add (make ("Top"));                        // G H x Top
            expect (! list.moveSubtrees ({ 1 }, 1));         // H dropped on itself: nothing happens
            expect (! list.moveSubtrees ({ 1 }, 2));         // ... or inside its own subtree
            expectEquals (names (list), juce::String ("G,H,x,Top"));
            expect (list.get (1).parentId == g.id);

            // moving Top in front of x (inside H) makes it H's child, decided before the rows move
            expect (list.moveSubtrees ({ 3 }, 2));
            expectEquals (names (list), juce::String ("G,H,Top,x"));
            expect (list.get (2).parentId == h.id);

            // duplicating a group whose child targets a sibling: the copy targets the copied sibling
            CueList list2;
            Cue g2 = make ("G2"); g2.type = CueType::group;
            list2.add (g2);
            Cue audio = make ("a"); audio.parentId = g2.id; audio.hotkey = "F5";
            Cue fade = make ("f"); fade.type = CueType::fade; fade.parentId = g2.id; fade.fade.targetId = audio.id;
            list2.add (audio);
            list2.add (fade);
            const int copyAt = list2.duplicate (0);
            expectEquals (copyAt, 3);
            expect (list2.get (5).fade.targetId == list2.get (4).id);   // the copied fade -> the copied audio
            expect (list2.get (4).hotkey.isEmpty());                    // hotkeys stay unique
        }

        beginTest ("tree: broken parent links are nulled on load");
        {
            CueList list;
            Cue g = make ("G"); g.type = CueType::group;
            Cue a = make ("a"); a.parentId = g.id;
            Cue x = make ("x");                              // top level between G's children
            Cue b = make ("b"); b.parentId = g.id;           // no longer directly inside G: detached
            Cue orphan = make ("o"); orphan.parentId = juce::Uuid();   // unknown parent
            Cue leafChild = make ("l"); leafChild.parentId = x.id;      // x is not a group
            list.replaceAll ({ g, a, x, b, orphan, leafChild });
            expect (list.get (1).parentId == g.id);
            expect (list.get (3).parentId.isNull());
            expect (list.get (4).parentId.isNull());
            expect (list.get (5).parentId.isNull());
        }

        beginTest ("tree: effective length of groups");
        {
            CueList list;
            Cue g = make ("G"); g.type = CueType::group;
            list.add (g);
            Cue a = make ("a"); a.parentId = g.id; a.durationSeconds = 2.0; a.preWaitSeconds = 1.0;
            Cue b = make ("b"); b.parentId = g.id; b.durationSeconds = 5.0;
            list.add (a);
            list.add (b);
            expectWithinAbsoluteError (list.effectiveLengthOf (0), 5.0, 1e-9);   // timeline: max (1 + 2, 0 + 5)
            list.update (0, [] (Cue& c) { c.group.mode = GroupMode::playlist; });
            expectWithinAbsoluteError (list.effectiveLengthOf (0), 8.0, 1e-9);   // playlist: sum
            list.update (0, [] (Cue& c) { c.group.loop = true; });
            expectWithinAbsoluteError (list.effectiveLengthOf (0), -1.0, 1e-9);
            list.update (0, [] (Cue& c) { c.group.loop = false; c.group.mode = GroupMode::random; });
            expectWithinAbsoluteError (list.effectiveLengthOf (0), 0.0, 1e-9);
        }
    }
};

static CueListTests cueListTests;

} // namespace gocue::tests
