#include "VolumeCueUiTestSupport.h"
#include "app/CueController.h"
#include "ui/ContainerTabs.h"
#include "ui/UiUtils.h"

#include <utility>
#include <vector>

namespace gocue::tests
{
namespace
{
using namespace volume_ui;

/** gom 2026-10-18: the tabs above the cue list (lists A, B, C ... each with its own songs) can be put in another order.
    A tab dragged sideways lands in the gap it is dropped in (the cue table's purple bar marks it), the tab menu moves
    it one step, and the active list, every list's cues, undo and the saved file follow the new order. */
class TabReorderTests final : public juce::UnitTest
{
public:
    TabReorderTests() : UnitTest ("Tab reorder", "Enqueue") {}

    static Cue make (const juce::String& name)
    {
        Cue c;
        c.name = name;
        return c;
    }

    static juce::String order (const ProjectDocument& doc)
    {
        juce::StringArray names;
        for (int i = 0; i < doc.getNumContainers(); ++i)
            names.add (doc.getContainerInfo (i).name);
        return names.joinIntoString (",");
    }

    /** The cue names of list 'index' (switching to it and back is not an edit). */
    static juce::String cuesOf (ProjectDocument& doc, int index)
    {
        const int was = doc.getActiveContainer();
        doc.setActiveContainer (index);
        juce::StringArray names;
        for (const auto& c : doc.cues.getAll())
            names.add (c.name);
        doc.setActiveContainer (was);
        return names.joinIntoString (",");
    }

    /** Lists A, B, C, D holding cues a, b, c, d; A active. */
    static void fill (ProjectDocument& doc)
    {
        doc.renameContainer (0, "A");
        doc.cues.add (make ("a"));
        for (const auto* name : { "B", "C", "D" })
        {
            doc.setActiveContainer (doc.addContainer (name, false));
            doc.cues.add (make (juce::String (name).toLowerCase()));
        }
        doc.setActiveContainer (0);
    }

    static juce::MouseEvent mouse (juce::Component& c, juce::Point<int> at, juce::Point<int> down, int clicks = 1, bool dragged = false)
    {
        const auto now = juce::Time::getCurrentTime();
        return { juce::Desktop::getInstance().getMainMouseSource(), at.toFloat(), juce::ModifierKeys::leftButtonModifier,
                 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, down.toFloat(), now, clicks, dragged };
    }

    /** A tab strip on 'doc' wired like the main window: a click selects (and the strip is rebuilt), moves are recorded. */
    struct Strip
    {
        Strip (ProjectDocument& d, int width) : doc (d), tabs (d)
        {
            tabs.setSize (width, ContainerTabs::height);
            tabs.onSelect = [this] (int index) { doc.setActiveContainer (index); tabs.refresh(); };
            tabs.onMoveTab = [this] (int from, int to) { moves.push_back ({ from, to }); };
            tabs.onRename = [this] (int index) { renamed.push_back (index); };
        }

        juce::Point<int> centre (int index) const { return tabs.getTabBounds (index).getCentre(); }

        /** Press the middle of 'tab', drag to x (one drag event), release there. Returns the marker seen before release. */
        juce::Rectangle<int> drag (int tab, int x)
        {
            const auto down = centre (tab);
            tabs.mouseDown (mouse (tabs, down, down));
            tabs.mouseDrag (mouse (tabs, { x, down.y }, down, 1, true));
            const auto marker = tabs.getDropMarkerBounds();
            tabs.mouseUp (mouse (tabs, { x, down.y }, down, 1, true));
            return marker;
        }

        ProjectDocument& doc;
        ContainerTabs tabs;
        std::vector<std::pair<int, int>> moves;
        std::vector<int> renamed;
    };

    /** Opt-in review artifacts: the real strip painting mid-drag, at 100 / 125 / 150 %, no window opened. */
    void screenshot (ContainerTabs& tabs, const juce::String& imageName)
    {
        const auto path = juce::SystemStats::getEnvironmentVariable ("ENQ_TABMOVE_SCREENSHOT_DIR", {});
        if (path.isEmpty())
            return;
        const juce::File directory (path);
        expect (directory.createDirectory().wasOk());
        for (const float scale : { 1.0f, 1.25f, 1.5f })
        {
            const auto file = directory.getChildFile (imageName + "_" + juce::String (juce::roundToInt (scale * 100)) + ".png");
            file.deleteFile();
            juce::FileOutputStream stream (file);
            expect (stream.openedOk());
            if (stream.openedOk())
                expect (juce::PNGImageFormat().writeImageToStream (tabs.createComponentSnapshot (tabs.getLocalBounds(), true, scale), stream));
        }
    }

    bool itemEnabled (ContainerTabs& tabs, int index, int itemId, juce::String* text = nullptr)
    {
        const auto menu = tabs.createTabMenu (index);
        juce::PopupMenu::MenuItemIterator it (menu);
        while (it.next())
            if (it.getItem().itemID == itemId)
            {
                if (text != nullptr)
                    *text = it.getItem().text;
                return it.getItem().isEnabled;
            }
        expect (false, "the tab menu has item " + juce::String (itemId));
        return false;
    }

    void runTest() override
    {
        beginTest ("moveContainer puts a tab at the new index; the active list and every list's cues stay with their list");
        {
            ProjectDocument doc;
            fill (doc);
            doc.setContainerCart (2, true, 3, 5);   // C is a cart: its grid travels with it
            doc.markClean();

            expect (doc.moveContainer (0, 2), "the active A after C");
            expectEquals (order (doc), juce::String ("B,C,A,D"));
            expectEquals (doc.getActiveContainer(), 2);
            expectEquals (doc.cues.get (0).name, juce::String ("a"));
            expect (doc.isDirty(), "a new order is an edit (it is saved with the project)");

            expect (doc.moveContainer (3, 0), "D to the front: the active A moves along one place");
            expectEquals (order (doc), juce::String ("D,B,C,A"));
            expectEquals (doc.getActiveContainer(), 3);
            expectEquals (doc.cues.get (0).name, juce::String ("a"));

            expect (doc.moveContainer (1, 2), "one step right");
            expectEquals (order (doc), juce::String ("D,C,B,A"));
            expectEquals (doc.getActiveContainer(), 3);

            for (int i = 0; i < doc.getNumContainers(); ++i)
                expectEquals (cuesOf (doc, i), doc.getContainerInfo (i).name.toLowerCase(), "list " + juce::String (i) + " kept its cues");
            const auto cart = doc.getContainerInfo (1);
            expect (cart.isCart && cart.cartRows == 3 && cart.cartCols == 5, "C's cart grid moved with it");

            expect (! doc.moveContainer (2, 2) && ! doc.moveContainer (-1, 0) && ! doc.moveContainer (0, 4) && ! doc.moveContainer (4, 0),
                    "the same place or a bad index changes nothing");
            expectEquals (order (doc), juce::String ("D,C,B,A"));
        }

        beginTest ("a move is one undo step; undo and redo bring back the order, the active list and its cues");
        {
            ProjectDocument doc;
            fill (doc);
            doc.setActiveContainer (1);   // B
            doc.perform (ko ("탭 순서 변경"), [&doc] { doc.moveContainer (1, 3); });
            expectEquals (order (doc), juce::String ("A,C,D,B"));
            expectEquals (doc.getContainerInfo (doc.getActiveContainer()).name, juce::String ("B"));

            expect (doc.undo());
            expectEquals (order (doc), juce::String ("A,B,C,D"));
            expectEquals (doc.getActiveContainer(), 1);
            expectEquals (doc.cues.get (0).name, juce::String ("b"));

            expect (doc.redo());
            expectEquals (order (doc), juce::String ("A,C,D,B"));
            expectEquals (doc.getContainerInfo (doc.getActiveContainer()).name, juce::String ("B"));
            expectEquals (doc.cues.get (0).name, juce::String ("b"));
            for (int i = 0; i < doc.getNumContainers(); ++i)
                expectEquals (cuesOf (doc, i), doc.getContainerInfo (i).name.toLowerCase());
        }

        beginTest ("the new order and the active list are saved with the project and come back when it is opened");
        {
            Scratch scratch;
            ProjectDocument doc;
            fill (doc);
            doc.moveContainer (0, 3);   // the active A goes last
            const auto file = scratch.folder.getChildFile ("order.enqueue");
            const auto saved = doc.save (file);
            expect (saved.wasOk(), saved.getErrorMessage());

            ProjectDocument reopened;
            const auto loaded = reopened.load (file);
            expect (loaded.wasOk(), loaded.getErrorMessage());
            expectEquals (order (reopened), juce::String ("B,C,D,A"));
            expectEquals (reopened.getActiveContainer(), 3);
            expect (! reopened.cues.isEmpty() && reopened.cues.get (0).name == "a", "it opens on A with A's cue");
        }

        beginTest ("dragging a tab sideways moves it to the gap it is dropped in; the bar marks that gap");
        {
            ProjectDocument doc;
            fill (doc);
            doc.addContainer ("E", false);
            Strip s (doc, 900);

            const auto marker = s.drag (2, s.tabs.getTabBounds (1).getX() + 3);   // C just right of the A|B gap
            expect (s.moves.size() == 1 && s.moves.back() == std::make_pair (2, 1), "C lands in front of B");
            expectEquals (marker.getX(), s.tabs.getTabBounds (1).getX() - 2, "the bar stands in the A|B gap");
            expectEquals (marker.getWidth(), 3);
            expectEquals (marker.getY(), s.tabs.getTabBounds (1).getY());
            expect (! s.tabs.isDraggingTab() && s.tabs.getDropMarkerBounds().isEmpty(), "the drop ends the drag");
            expectEquals (doc.getActiveContainer(), 2, "pressing C selected it, as a click does");

            const auto endMarker = s.drag (0, 890);   // past the last tab (and the + button)
            expect (s.moves.size() == 2 && s.moves.back() == std::make_pair (0, 4), "A goes last");
            expectEquals (endMarker.getX(), s.tabs.getTabBounds (4).getRight(), "the bar stands after the last tab");

            s.drag (4, 0);   // left of the first tab
            expect (s.moves.size() == 3 && s.moves.back() == std::make_pair (4, 0), "E goes first");

            const auto beside = s.drag (1, s.tabs.getTabBounds (2).getX() - 1);   // B dropped in the gap right next to itself
            expect (beside.isEmpty() && s.moves.size() == 3, "a drop either side of the tab itself moves nothing and shows no bar");
        }

        beginTest ("a press that moves a few px is a click; show mode drags nothing");
        {
            ProjectDocument doc;
            fill (doc);
            Strip s (doc, 900);
            const auto down = s.centre (1);
            s.tabs.mouseDown (mouse (s.tabs, down, down));
            s.tabs.mouseDrag (mouse (s.tabs, down.translated (3, 1), down, 1, true));
            expect (! s.tabs.isDraggingTab(), "3 px is still a click");
            s.tabs.mouseUp (mouse (s.tabs, down.translated (3, 1), down, 1, true));
            expect (s.moves.empty());
            expectEquals (doc.getActiveContainer(), 1, "the click selected B");

            s.tabs.setEditable (false);
            s.drag (3, 0);
            expect (s.moves.empty() && ! s.tabs.isDraggingTab(), "show mode: no drag");
            expectEquals (doc.getActiveContainer(), 1, "show mode: a click does not change the list either (unchanged rule)");
            s.tabs.setEditable (true);
        }

        beginTest ("right after a drag, a click and a double-click on another tab select and rename that tab");
        {
            ProjectDocument doc;
            fill (doc);
            Strip s (doc, 900);
            s.drag (0, 890);
            expect (s.moves.size() == 1);

            const auto p = s.centre (2);
            s.tabs.mouseDown (mouse (s.tabs, p, p, 1));
            s.tabs.mouseUp (mouse (s.tabs, p, p, 1));
            s.tabs.mouseDown (mouse (s.tabs, p, p, 2));
            s.tabs.mouseDoubleClick (mouse (s.tabs, p, p, 2));
            s.tabs.mouseUp (mouse (s.tabs, p, p, 2));
            expectEquals (doc.getActiveContainer(), 2);
            expect (s.renamed.size() == 1 && s.renamed.back() == 2, "the double-click renames C");
            expect (s.moves.size() == 1, "no second move");
        }

        beginTest ("on a strip too narrow for every tab, holding a dragged tab at the right end scrolls there and drops it last");
        {
            ProjectDocument doc;
            doc.renameContainer (0, "T1");
            for (int i = 2; i <= 12; ++i)
                doc.addContainer ("T" + juce::String (i), false);
            Strip s (doc, 420);   // tabs end at x 372 here (14 + 26 + 2 x 8 px are kept for the + button)
            expect (s.tabs.getTabBounds (11).getX() > 372, "the last tab starts out of view");

            const auto down = s.centre (0);
            const juce::Point<int> edge { 360, down.y };
            s.tabs.mouseDown (mouse (s.tabs, down, down));
            for (int i = 0; i < 80; ++i)   // the drag auto-repeat while the mouse rests at the right end
                s.tabs.mouseDrag (mouse (s.tabs, edge, down, 1, true));
            expect (s.tabs.getTabBounds (11).getRight() <= 372, "the strip scrolled to its end: " + s.tabs.getTabBounds (11).toString());
            expect (! s.tabs.getDropMarkerBounds().isEmpty(), "the bar shows where it will go");
            s.tabs.mouseUp (mouse (s.tabs, edge, down, 1, true));
            expect (s.moves.size() == 1 && s.moves.back() == std::make_pair (0, 11), "T1 goes last");
        }

        beginTest ("on a crowded strip the tab just moved stays in sight: after a drag that scrolled, and after 오른쪽으로 이동");
        {
            const auto crowded = [] (ProjectDocument& doc)
            {
                doc.renameContainer (0, "T1");
                for (int i = 2; i <= 12; ++i)
                    doc.addContainer ("T" + juce::String (i), false);
            };
            const auto inSight = [] (juce::Rectangle<int> r) { return r.getX() >= 8 && r.getRight() <= 372; };   // 420 px strip

            ProjectDocument doc;
            crowded (doc);
            doc.setActiveContainer (11);
            Strip s (doc, 420);
            s.tabs.onMoveTab = [&] (int from, int to) { s.moves.push_back ({ from, to }); doc.moveContainer (from, to); s.tabs.refresh(); };
            expect (inSight (s.tabs.getTabBounds (11)), "T12 is active and in sight");

            const auto down = s.centre (11);
            s.tabs.mouseDown (mouse (s.tabs, down, down));
            for (int i = 0; i < 80; ++i)   // held at the left end: the strip scrolls back to T1
                s.tabs.mouseDrag (mouse (s.tabs, { 10, down.y }, down, 1, true));
            expectEquals (s.tabs.getTabBounds (0).getX(), 8);
            s.tabs.mouseDrag (mouse (s.tabs, { 340, down.y }, down, 1, true));   // back right, into the T5|T6 gap at the visible end
            s.tabs.mouseUp (mouse (s.tabs, { 340, down.y }, down, 1, true));
            expect (s.moves.size() == 1 && s.moves.back() == std::make_pair (11, 5), "T12 lands after T5");
            expectEquals (doc.getContainerInfo (5).name, juce::String ("T12"));
            expect (inSight (s.tabs.getTabBounds (5)), "the moved (current) tab is in sight: " + s.tabs.getTabBounds (5).toString());
            screenshot (s.tabs, "after_drop_crowded");

            ProjectDocument doc2;
            crowded (doc2);
            Strip m (doc2, 420);
            m.tabs.onMoveTab = [&] (int from, int to) { doc2.moveContainer (from, to); m.tabs.refresh(); };
            expect (! inSight (m.tabs.getTabBounds (5)), "T6 is cut at the right end");
            m.tabs.performTabMenuItem (5, ContainerTabs::moveRightItem);
            expectEquals (doc2.getContainerInfo (6).name, juce::String ("T6"));
            expect (inSight (m.tabs.getTabBounds (6)), "T6 moved right and is in sight: " + m.tabs.getTabBounds (6).toString());
        }

        beginTest ("the tab menu moves a tab one step left or right; the first and last tab cannot go further");
        {
            ProjectDocument doc;
            fill (doc);
            Strip s (doc, 900);
            juce::String left, right;
            expect (! itemEnabled (s.tabs, 0, ContainerTabs::moveLeftItem, &left), "the first tab: no left");
            expect (itemEnabled (s.tabs, 0, ContainerTabs::moveRightItem, &right));
            expectEquals (left, ko ("왼쪽으로 이동"));
            expectEquals (right, ko ("오른쪽으로 이동"));
            expect (itemEnabled (s.tabs, 3, ContainerTabs::moveLeftItem));
            expect (! itemEnabled (s.tabs, 3, ContainerTabs::moveRightItem), "the last tab: no right");
            expect (itemEnabled (s.tabs, 0, ContainerTabs::removeItem) && itemEnabled (s.tabs, 0, ContainerTabs::renameItem), "the old items stay");

            s.tabs.performTabMenuItem (2, ContainerTabs::moveLeftItem);
            s.tabs.performTabMenuItem (2, ContainerTabs::moveRightItem);
            s.tabs.performTabMenuItem (0, ContainerTabs::moveLeftItem);
            s.tabs.performTabMenuItem (3, ContainerTabs::moveRightItem);
            expect (s.moves.size() == 2 && s.moves[0] == std::make_pair (2, 1) && s.moves[1] == std::make_pair (2, 3));
        }

        if (juce::SystemStats::getEnvironmentVariable ("ENQ_TABMOVE_SCREENSHOT_DIR", {}).isNotEmpty())
        {
            beginTest ("opt-in screenshots of a tab drag");
            GoCueLookAndFeel theme;
            struct RestoreLookAndFeel
            {
                juce::LookAndFeel* previous = &juce::LookAndFeel::getDefaultLookAndFeel();
                ~RestoreLookAndFeel() { juce::LookAndFeel::setDefaultLookAndFeel (previous); }
            } restore;
            juce::LookAndFeel::setDefaultLookAndFeel (&theme);
            {
                ProjectDocument doc;
                doc.renameContainer (0, ko ("오프닝"));
                for (const auto* name : { "BGM", "코너", "효과음", "엔딩" })
                    doc.addContainer (juce::String::fromUTF8 (name), juce::String::fromUTF8 (name) == ko ("효과음"));
                Strip s (doc, 900);
                const auto down = s.centre (2);
                s.tabs.mouseDown (mouse (s.tabs, down, down));
                s.tabs.mouseDrag (mouse (s.tabs, { s.tabs.getTabBounds (1).getX() + 6, down.y }, down, 1, true));
                screenshot (s.tabs, "drag_between");
                s.tabs.mouseUp (mouse (s.tabs, { s.tabs.getTabBounds (1).getX() + 6, down.y }, down, 1, true));
            }
            {
                ProjectDocument doc;
                doc.renameContainer (0, "T1");
                for (int i = 2; i <= 12; ++i)
                    doc.addContainer ("T" + juce::String (i), false);
                Strip s (doc, 420);
                const auto down = s.centre (0);
                s.tabs.mouseDown (mouse (s.tabs, down, down));
                for (int i = 0; i < 80; ++i)
                    s.tabs.mouseDrag (mouse (s.tabs, { 360, down.y }, down, 1, true));
                screenshot (s.tabs, "drag_scrolled_end");
                s.tabs.mouseUp (mouse (s.tabs, { 360, down.y }, down, 1, true));
            }
        }

        beginTest ("main window: a tab moved while another list's cue plays - it keeps playing and stops by itself, GO stays put");
        {
            Fixture f;
            auto& doc = f.document();
            const auto firstName = doc.getContainerInfo (0).name;
            const auto a = f.addSound ("a");
            doc.cues.setPlayheadIndex (0);
            expect (f.command (CommandIDs::go));
            f.settle();
            expect (f.engine.isPlaying (a.id), "a plays");

            doc.setActiveContainer (doc.addContainer ("BGM", false));
            const auto b = f.addSound ("b");
            doc.cues.setPlayheadIndex (0);
            dispatch();

            auto* tabs = child<ContainerTabs> (*f.main);
            expect (tabs != nullptr);
            if (tabs != nullptr)
            {
                tabs->performTabMenuItem (1, ContainerTabs::moveLeftItem);   // BGM to the front
                dispatch();
                expectEquals (order (doc), "BGM," + firstName);
                expectEquals (doc.getActiveContainer(), 0, "BGM is still the list on screen");
                expectEquals (doc.getUndoName(), ko ("탭 순서 변경"));
                expect (f.engine.isPlaying (a.id), "a keeps playing");
                expectEquals (doc.containerOf (a.id), 1);
                expect (doc.cues.getPlayhead() != nullptr && doc.cues.getPlayhead()->id == b.id, "the next GO is still BGM's cue");

                ReopenLastProjectTestAccess::controller (*f.main).stopCue (a.id);
                f.settle();
                expect (! f.engine.isPlaying (a.id), "a stops by itself from the other list");

                expect (doc.undo());
                dispatch();
                expectEquals (order (doc), firstName + ",BGM");
                expectEquals (doc.getContainerInfo (doc.getActiveContainer()).name, juce::String ("BGM"));

                f.command (CommandIDs::toggleShowMode);
                tabs->performTabMenuItem (1, ContainerTabs::moveLeftItem);
                dispatch();
                expectEquals (order (doc), firstName + ",BGM", "show mode: tabs do not move");
                f.command (CommandIDs::toggleShowMode);
            }
            ReopenLastProjectTestAccess::controller (*f.main).hardStopAll();
            f.settle();
        }
    }
};

static TabReorderTests tabReorderTests;
}
} // namespace gocue::tests
