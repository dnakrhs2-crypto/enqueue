#include "app/ProjectDocument.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

/** Cue lists and carts inside the document: switching, lookup across lists, undo, file round trip. */
class ProjectDocumentTests : public juce::UnitTest
{
public:
    ProjectDocumentTests() : juce::UnitTest ("ProjectDocument", "GoCue") {}

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
        beginTest ("a new document has one main list; adding a cart keeps the active list's cues in place");
        {
            ProjectDocument document;
            expectEquals (document.getNumContainers(), 1);
            expectEquals (document.getActiveContainer(), 0);
            expect (! document.isActiveCart());
            document.cues.add (make ("a"));
            document.cues.add (make ("b"));

            const int cart = document.addContainer ("cart", true);
            expectEquals (cart, 1);
            expectEquals (document.getNumContainers(), 2);
            expect (document.getContainerInfo (1).isCart);
            expectEquals (document.getActiveContainer(), 0);
            expectEquals (names (document.cues), juce::String ("a,b"));   // still the main list
        }

        beginTest ("switching the active container swaps the cues and keeps each list's cursors");
        {
            ProjectDocument document;
            const auto aId = document.cues.get (document.cues.add (make ("a"))).id;
            document.cues.add (make ("b"));
            document.cues.setPlayheadIndex (1);
            document.addContainer ("second", false);

            document.setActiveContainer (1);
            expectEquals (document.getActiveContainer(), 1);
            expect (document.cues.isEmpty());
            document.cues.add (make ("x"));
            const auto xId = document.cues.get (0).id;

            // cues of the inactive list are still found, in their list
            int index = -1;
            auto* mainList = document.listContaining (aId, &index);
            expect (mainList != nullptr && mainList != &document.cues);
            expectEquals (index, 0);
            expectEquals (document.containerOf (aId), 0);
            expectEquals (document.containerOf (xId), 1);
            expect (document.findCueAnywhere (aId) != nullptr && document.findCueAnywhere (aId)->name == "a");
            expect (document.findCueAnywhere (juce::Uuid()) == nullptr);

            document.setActiveContainer (0);
            expectEquals (names (document.cues), juce::String ("a,b"));
            expectEquals (document.cues.getPlayheadIndex(), 1);   // the playhead came back with the list
            expectEquals (document.containerOf (xId), 1);

            int lists = 0;
            document.forEachList ([&lists] (CueList&) { ++lists; });
            expectEquals (lists, 2);
        }

        beginTest ("removing a container: the last one stays, removing the active one activates a neighbour");
        {
            ProjectDocument document;
            document.cues.add (make ("a"));
            document.addContainer ("second", false);
            document.addContainer ("third", true);
            expect (! document.removeContainer (5));
            document.setActiveContainer (2);
            expect (document.removeContainer (2));
            expectEquals (document.getNumContainers(), 2);
            expectEquals (document.getActiveContainer(), 1);
            expect (document.removeContainer (1));
            expectEquals (document.getActiveContainer(), 0);
            expectEquals (names (document.cues), juce::String ("a"));
            expect (! document.removeContainer (0));   // the last list cannot go
            document.renameContainer (0, "renamed");
            expectEquals (document.getContainerInfo (0).name, juce::String ("renamed"));
            document.setContainerCart (0, true, 3, 99);
            expect (document.isActiveCart());
            expectEquals (document.getContainerInfo (0).cartCols, CueContainer::maxGrid);
        }

        beginTest ("undo restores lists, the active list and its cues");
        {
            ProjectDocument document;
            document.clock = [] { return 0.0; };
            document.cues.add (make ("a"));
            document.perform ("add cart", [&] { document.addContainer ("cart", true); });
            document.perform ("switch", [&] { document.setActiveContainer (1); });
            document.perform ("add x", [&] { document.cues.add (make ("x")); });
            expectEquals (document.getActiveContainer(), 1);
            expectEquals (names (document.cues), juce::String ("x"));

            expect (document.undo());   // add x
            expect (document.cues.isEmpty());
            expect (document.undo());   // switch
            expectEquals (document.getActiveContainer(), 0);
            expectEquals (names (document.cues), juce::String ("a"));
            expect (document.undo());   // add cart
            expectEquals (document.getNumContainers(), 1);
            expect (document.redo());
            expectEquals (document.getNumContainers(), 2);
            expect (document.redo());
            expectEquals (document.getActiveContainer(), 1);
            expect (document.redo());
            expectEquals (names (document.cues), juce::String ("x"));
        }

        beginTest ("toProject / adopt carry every list and the active one");
        {
            ProjectDocument document;
            document.cues.add (make ("a"));
            document.addContainer ("cart", true);
            document.setActiveContainer (1);
            document.cues.add (make ("x"));
            document.setContainerCart (1, true, 2, 6);

            const auto project = document.toProject();
            expectEquals ((int) project.lists.size(), 2);
            expectEquals (project.activeList, 1);
            expectEquals ((int) project.lists[0].cues.size(), 1);
            expectEquals ((int) project.lists[1].cues.size(), 1);
            expectEquals (project.lists[1].cartCols, 6);

            ProjectDocument other;
            other.adopt (project, juce::File());
            expectEquals (other.getNumContainers(), 2);
            expectEquals (other.getActiveContainer(), 1);
            expectEquals (names (other.cues), juce::String ("x"));
            expect (other.isActiveCart());
            other.setActiveContainer (0);
            expectEquals (names (other.cues), juce::String ("a"));
            expect (! other.isDirty());   // switching lists is not an edit

            other.newProject();
            expectEquals (other.getNumContainers(), 1);
            expect (other.cues.isEmpty());
        }
    }
};

static ProjectDocumentTests projectDocumentTests;

} // namespace gocue::tests
