#include "app/ProjectDocument.h"
#include "app/ProjectHistory.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

class ProjectHistoryTests : public juce::UnitTest
{
public:
    ProjectHistoryTests() : juce::UnitTest ("ProjectHistory", "GoCue") {}

    static ProjectSnapshot snapshotNamed (const juce::String& cueName)
    {
        ProjectSnapshot s;
        Cue c;
        c.name = cueName;
        s.project.cues.push_back (c);
        return s;
    }

    static juce::String firstName (const ProjectSnapshot& s)
    {
        return s.project.cues.empty() ? juce::String() : s.project.cues[0].name;
    }

    void runTest() override
    {
        beginTest ("undo restores the recorded state and redo brings the current one back");
        {
            ProjectHistory h;
            expect (! h.canUndo() && ! h.canRedo());

            expect (h.push (snapshotNamed ("v1"), "rename", {}, 0.0));
            expect (h.canUndo());
            expectEquals (h.getUndoName(), juce::String ("rename"));

            auto restored = h.undo ([] (bool) { return snapshotNamed ("v2"); });
            expect (restored.has_value());
            expectEquals (firstName (*restored), juce::String ("v1"));
            expect (! h.canUndo());
            expect (h.canRedo());
            expectEquals (h.getRedoName(), juce::String ("rename"));

            auto redone = h.redo ([] (bool) { return snapshotNamed ("v1 again"); });
            expect (redone.has_value());
            expectEquals (firstName (*redone), juce::String ("v2"));
            expect (h.canUndo() && ! h.canRedo());
            expectEquals (h.getUndoName(), juce::String ("rename"));

            expect (! h.redo ([] (bool) { return ProjectSnapshot(); }).has_value());
        }

        beginTest ("a new edit after undo drops the redo stack");
        {
            ProjectHistory h;
            h.push (snapshotNamed ("a"), "one", {}, 0.0);
            h.push (snapshotNamed ("b"), "two", {}, 1.0);
            h.undo ([] (bool) { return snapshotNamed ("c"); });
            expect (h.canRedo());

            h.push (snapshotNamed ("b2"), "three", {}, 2.0);
            expect (! h.canRedo());
            expectEquals (h.getUndoDepth(), 2);
            expectEquals (h.getUndoName(), juce::String ("three"));
        }

        beginTest ("same-key pushes within the window coalesce into one step");
        {
            ProjectHistory h;
            expect (h.push (snapshotNamed ("g0"), "gain", "gain:1", 1000.0));
            expect (! h.push (snapshotNamed ("g1"), "gain", "gain:1", 1500.0));   // merged
            expect (! h.push (snapshotNamed ("g2"), "gain", "gain:1", 2100.0));   // still within 700 ms of the last push
            expectEquals (h.getUndoDepth(), 1);

            expect (h.push (snapshotNamed ("g3"), "gain", "gain:1", 3000.0));     // too late: new step
            expect (h.push (snapshotNamed ("g4"), "gain", "gain:2", 3001.0));     // other cue: new step
            expect (h.push (snapshotNamed ("g5"), "name", {}, 3002.0));           // no key: always a new step
            expect (h.push (snapshotNamed ("g6"), "name", {}, 3003.0));
            expectEquals (h.getUndoDepth(), 5);

            auto first = h.undo ([] (bool) { return ProjectSnapshot(); });
            expectEquals (firstName (*first), juce::String ("g6"));
        }

        beginTest ("the stack keeps at most maxDepth steps");
        {
            ProjectHistory h;

            for (int i = 0; i < ProjectHistory::maxDepth + 25; ++i)
                h.push (snapshotNamed (juce::String (i)), "edit", {}, (double) i);

            expectEquals (h.getUndoDepth(), ProjectHistory::maxDepth);
            auto top = h.undo ([] (bool) { return ProjectSnapshot(); });
            expectEquals (firstName (*top), juce::String (ProjectHistory::maxDepth + 24));
        }

        beginTest ("ProjectDocument::perform records a step and undo/redo replay the model");
        {
            ProjectDocument doc;
            double now = 0.0;
            doc.clock = [&now] { return now; };

            Cue a;
            a.name = "a";
            doc.perform ("add", [&] { doc.cues.add (a); });
            expect (doc.isDirty());
            expect (doc.canUndo());
            expectEquals (doc.getUndoName(), juce::String ("add"));
            expectEquals (doc.cues.size(), 1);

            now = 100.0;
            doc.perform ("rename", [&] { doc.cues.update (0, [] (Cue& c) { c.name = "renamed"; }); });
            expectEquals (doc.cues.get (0).name, juce::String ("renamed"));

            int restoredCalls = 0;
            doc.onSnapshotRestored = [&restoredCalls] (const ProjectSnapshot&) { ++restoredCalls; };

            expect (doc.undo());
            expectEquals (doc.cues.get (0).name, juce::String ("a"));
            expectEquals (restoredCalls, 1);
            expect (doc.canRedo());
            expectEquals (doc.getRedoName(), juce::String ("rename"));

            expect (doc.undo());
            expectEquals (doc.cues.size(), 0);
            expect (! doc.canUndo());
            expect (! doc.undo());

            expect (doc.redo());
            expectEquals (doc.cues.size(), 1);
            expectEquals (doc.cues.get (0).name, juce::String ("a"));
            expect (doc.redo());
            expectEquals (doc.cues.get (0).name, juce::String ("renamed"));
            expect (! doc.canRedo());
            expectEquals (restoredCalls, 4);
        }

        beginTest ("gain drags coalesce and the selection survives undo");
        {
            ProjectDocument doc;
            double now = 0.0;
            doc.clock = [&now] { return now; };

            for (auto* n : { "x", "y", "z" })
            {
                Cue c;
                c.name = n;
                doc.perform ("add", [&] { doc.cues.add (c); });
                now += 1000.0;
            }

            doc.cues.setSelectedIndex (1);
            const auto selectedId = doc.cues.get (1).id;
            const juce::String key = "gain:" + selectedId.toString();

            for (int i = 1; i <= 5; ++i)
            {
                now += 50.0;
                doc.perform ("gain", [&, i] { doc.cues.update (1, [i] (Cue& c) { c.gainDb = -1.0 * i; }); }, { key });
            }

            expectWithinAbsoluteError (doc.cues.get (1).gainDb, -5.0, 1e-9);
            expectEquals (doc.getHistory().getUndoDepth(), 4);    // 3 adds + 1 coalesced gain step

            doc.cues.setSelectedIndex (2);
            expect (doc.undo());
            expectWithinAbsoluteError (doc.cues.get (1).gainDb, 0.0, 1e-9);
            expectEquals (doc.cues.getSelectedIndex(), 1);          // restored to the cue that was selected before the edit
        }

        beginTest ("snapshotDecorator adds live state only when plugin states are captured");
        {
            ProjectDocument doc;
            doc.clock = [] { return 0.0; };
            int decorated = 0;
            doc.snapshotDecorator = [&decorated] (Project& p)
            {
                ++decorated;
                PluginSlotState s;
                s.name = "live";
                p.masterPlugins.push_back (s);
            };

            Cue c;
            doc.perform ("plain", [&] { doc.cues.add (c); });
            expectEquals (decorated, 0);

            doc.perform ("structural", [&] { doc.cues.add (c); }, { {}, true });
            expectEquals (decorated, 1);

            expect (doc.undo());                                    // restores the decorated snapshot
            expectEquals ((int) doc.masterPlugins.size(), 1);
            expectEquals (doc.masterPlugins[0].name, juce::String ("live"));
        }

        beginTest ("newProject and adopt clear the history");
        {
            ProjectDocument doc;
            Cue c;
            doc.perform ("add", [&] { doc.cues.add (c); });
            expect (doc.canUndo());
            doc.newProject();
            expect (! doc.canUndo());

            doc.perform ("add", [&] { doc.cues.add (c); });
            doc.adopt (Project(), juce::File());
            expect (! doc.canUndo() && ! doc.canRedo());
        }
    }
};

static ProjectHistoryTests projectHistoryTests;

} // namespace gocue::tests
