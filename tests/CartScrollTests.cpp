#include "ui/CueCartView.h"
#include "ui/UiUtils.h"

namespace gocue::tests
{
namespace
{
/** A cart's rows x cols grid fills its view; more cues than that continue below in rows of the same size, reached
    with a scrollbar like the cue list's (user request 11/14: "카트로 하면 전체큐가 보여지지않고 16개까지밖에 안보이는데
    스크롤도 없어서요"). */
class CartScrollTests : public juce::UnitTest
{
public:
    CartScrollTests() : UnitTest ("Cart scroll", "Enqueue") {}

    void runTest() override
    {
        fitsWithoutScroll();
        morePastTheGrid();
        wheel();
        selectionComesIntoView();
        clickDoesNotMoveTheView();
        gridChangeShowsSelection();
        shrinkBack();
    }

private:
    static constexpr int gap = 6, width = 800, height = 600;

    struct Cart
    {
        Cart (int numCues, int gridRows, int gridCols)
        {
            for (int i = 0; i < numCues; ++i)
            {
                Cue cue;
                cue.number = juce::String (i + 1);
                cue.name = "cue " + juce::String (i + 1);
                cues.add (cue);
            }

            view.setGrid (gridRows, gridCols);
            view.setSize (width, height);
            view.onTrigger = [this] (const Cue& cue) { fired.push_back (cues.indexOf (cue.id)); };
            rows = gridRows;
            cols = gridCols;
        }

        /** The centre of grid cell (row, col) on screen, rows counted from the first cue. */
        juce::Point<int> centre (int row, int col) const
        {
            const int areaW = width - 2 * gap - (view.isScrollable() ? Palette::scrollBarWidth : 0);
            const int cellW = (areaW - gap * (cols - 1)) / cols;
            const int cellH = (height - 2 * gap - gap * (rows - 1)) / rows;
            return { gap + col * (cellW + gap) + cellW / 2, gap + row * (cellH + gap) + cellH / 2 - view.getScrollPosition() };
        }

        int cellHeight() const { return (height - 2 * gap - gap * (rows - 1)) / rows; }
        int maxScroll (int totalRows) const { return gap + totalRows * (cellHeight() + gap) - height; }

        void click (juce::Point<int> p)
        {
            const auto now = juce::Time::getCurrentTime();
            const juce::MouseEvent e { juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(), juce::ModifierKeys::leftButtonModifier,
                                       1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &view, &view, now, p.toFloat(), now, 1, false };
            view.mouseDown (e);
            view.mouseUp (e);
        }

        CueList cues;
        juce::AudioFormatManager formats;
        CueCartView view { cues, formats };
        std::vector<int> fired;
        int rows = 4, cols = 4;
    };

    static juce::MouseWheelDetails notches (float n)
    {
        juce::MouseWheelDetails wheel;
        wheel.deltaX = 0.0f;
        wheel.deltaY = n * 120.0f / 256.0f;   // one Windows wheel notch, as JUCE reports it
        wheel.isReversed = false;
        wheel.isSmooth = false;
        wheel.isInertial = false;
        return wheel;
    }

    void fitsWithoutScroll()
    {
        beginTest ("a cart whose cues fit its grid has no scrollbar, and its buttons stay where they were");
        Cart cart (16, 4, 4);
        expect (! cart.view.isScrollable());
        expectEquals (cart.view.getScrollPosition(), 0);
        cart.click (cart.centre (3, 3));
        expect (cart.fired == std::vector<int> { 15 }, "the last button fires the 16th cue");
        cart.view.setScrollPosition (100);
        expectEquals (cart.view.getScrollPosition(), 0, "nothing to scroll");
    }

    void morePastTheGrid()
    {
        beginTest ("cues past the grid continue below in rows of the same size, reached with the cue list's scrollbar");
        Cart cart (20, 4, 4);
        expect (cart.view.isScrollable());
        auto* bar = dynamic_cast<juce::ScrollBar*> (cart.view.getChildComponent (0));
        expect (bar != nullptr && bar->isVisible(), "a visible scrollbar");

        if (bar != nullptr)
        {
            expectEquals (bar->getWidth(), Palette::scrollBarWidth, "as thick as the cue list's");
            expectEquals (bar->getRight(), width, "along the right edge");
        }

        cart.click (cart.centre (3, 0));
        expect (cart.fired == std::vector<int> { 12 }, "the view starts at the first row");

        cart.view.setScrollPosition (100000);
        expectEquals (cart.view.getScrollPosition(), cart.maxScroll (5), "the scroll stops with the last row a gap above the bottom");
        cart.click (cart.centre (4, 0));
        cart.click (cart.centre (4, 3));
        cart.click (cart.centre (1, 0));
        expect (cart.fired == std::vector<int> { 12, 16, 19, 4 }, "the fifth row holds cues 17 to 20; the rows above moved up with it");
    }

    void wheel()
    {
        beginTest ("the wheel scrolls by the cue list's step; with nothing to scroll the parent gets it");
        struct Parent : juce::Component
        {
            void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override { ++wheels; }
            int wheels = 0;
        };

        Cart cart (40, 4, 4);
        Parent parent;
        parent.setSize (width, height);
        parent.addAndMakeVisible (cart.view);
        const auto e = juce::MouseEvent { juce::Desktop::getInstance().getMainMouseSource(), {}, {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                          &cart.view, &cart.view, juce::Time::getCurrentTime(), {}, juce::Time::getCurrentTime(), 0, false };
        const int step = juce::roundToInt (14.0f * (float) Palette::rowHeights[1] * notches (1.0f).deltaY);

        cart.view.mouseWheelMove (e, notches (-1.0f));
        expectEquals (cart.view.getScrollPosition(), step, "one notch down");
        cart.view.mouseWheelMove (e, notches (-100.0f));
        expectEquals (cart.view.getScrollPosition(), cart.maxScroll (10), "stops at the last row");
        cart.view.mouseWheelMove (e, notches (100.0f));
        expectEquals (cart.view.getScrollPosition(), 0, "back to the first row");
        expectEquals (parent.wheels, 0, "a scrolling cart keeps its wheel");

        Cart small (3, 4, 4);
        parent.addAndMakeVisible (small.view);
        small.view.mouseWheelMove (e.withNewPosition (juce::Point<float>()), notches (-1.0f));
        expectEquals (parent.wheels, 1, "a cart that fits passes the wheel on, as before");
    }

    void selectionComesIntoView()
    {
        beginTest ("a cue selected below or above the view comes into it, as in the cue list");
        Cart cart (40, 4, 4);
        cart.cues.setSelectedIndex (39);
        expectEquals (cart.view.getScrollPosition(), cart.maxScroll (10), "the last cue is shown");
        cart.click (cart.centre (9, 3));
        expect (cart.fired == std::vector<int> { 39 });

        cart.cues.setSelectedIndex (1);
        expectEquals (cart.view.getScrollPosition(), 0, "the second cue is shown from the top");

        cart.cues.setSelectedIndex (6);
        expectEquals (cart.view.getScrollPosition(), 0, "a cue already in view does not move it");

        Cue added;
        added.name = "added";
        cart.cues.add (added);
        cart.cues.setSelectedIndex (40);
        cart.click (cart.centre (10, 0));
        expect (cart.fired.back() == 40, "a cue added past the last row gets a new row and is shown");
    }

    void clickDoesNotMoveTheView()
    {
        beginTest ("pressing a button that is partly out of view fires it without moving the view under the mouse");
        Cart cart (40, 4, 4);
        const int partly = cart.cellHeight() / 2;   // the fifth row's top half shows at the bottom
        cart.view.setScrollPosition (partly);
        const auto p = cart.centre (4, 2) - juce::Point<int> (0, cart.cellHeight() / 4);
        expect (p.y < height - gap, "the press is on the part that shows");
        cart.click (p);
        expectEquals (cart.view.getScrollPosition(), partly, "the view stays");
        expect (cart.fired == std::vector<int> { 18 }, "the pressed button fires");
        expectEquals (cart.cues.getSelectedIndex(), 18, "and is selected");
    }

    void gridChangeShowsSelection()
    {
        beginTest ("another grid size (a tab switch or the grid dialog) keeps the selected cue in view");
        Cart cart (40, 4, 4);
        cart.cues.setSelectedIndex (39);
        cart.view.setGrid (2, 2);
        cart.rows = cart.cols = 2;
        expectEquals (cart.view.getScrollPosition(), cart.maxScroll (20));
        cart.click (cart.centre (19, 1));
        expect (cart.fired == std::vector<int> { 39 });

        cart.view.setGrid (15, 15);
        cart.rows = cart.cols = 15;
        expect (! cart.view.isScrollable(), "40 cues fit a 15 x 15 grid");
        expectEquals (cart.view.getScrollPosition(), 0);
    }

    void shrinkBack()
    {
        beginTest ("a cart that loses its extra cues drops the scrollbar and shows its grid from the top");
        Cart cart (20, 4, 4);
        cart.view.setScrollPosition (100000);

        while (cart.cues.size() > 16)
            cart.cues.remove (cart.cues.size() - 1);

        expect (! cart.view.isScrollable());
        expectEquals (cart.view.getScrollPosition(), 0);
        cart.click (cart.centre (0, 0));
        expect (cart.fired == std::vector<int> { 0 });
    }
};

static CartScrollTests cartScrollTests;
}
}
