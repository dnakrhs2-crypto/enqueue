#include "ui/CueCartView.h"

#include "audio/CueFileInfo.h"
#include "model/CueColors.h"
#include "ui/UiUtils.h"

namespace gocue
{

namespace
{
    constexpr int cellGap = 6;   // around and between the buttons
}

CueCartView::CueCartView (CueList& c, juce::AudioFormatManager& f) : cues (c), formats (f)
{
    setWantsKeyboardFocus (true);
    scrollBar.setAutoHide (false);   // shown exactly when the cues need more rows than the grid
    scrollBar.setSingleStepSize (14.0 * Palette::rowHeights[1] / 10.0);   // a wheel over the bar moves as over the buttons
    scrollBar.addListener (this);
    addChildComponent (scrollBar);
    cues.addListener (this);
}

CueCartView::~CueCartView()
{
    cues.removeListener (this);
    scrollBar.removeListener (this);
}

void CueCartView::setGrid (int newRows, int newCols)
{
    const bool changed = rows != juce::jlimit (1, 15, newRows) || cols != juce::jlimit (1, 15, newCols);

    if (changed)
    {
        pressedCueId = juce::Uuid::null();
        pressedSlot = -1;
    }
    rows = juce::jlimit (1, 15, newRows);
    cols = juce::jlimit (1, 15, newCols);
    updateScrollRange();

    if (changed)
        scrollToShow (cues.getSelectedIndex());   // another cart's grid (a tab switch) or a new size: its selected cue in view

    repaint();
}

void CueCartView::resized()
{
    updateScrollRange();
}

void CueCartView::cueListStructureChanged()
{
    if (cues.findById (pressedCueId) == nullptr)
    {
        pressedCueId = juce::Uuid::null();
        pressedSlot = -1;
    }
    updateScrollRange();

    if (! clicking)
        scrollToShow (cues.getSelectedIndex());   // as the cue list: an added cue below the view comes into it

    repaint();
}

void CueCartView::cueSelectionChanged (int)
{
    if (! clicking)
        scrollToShow (cues.getSelectedIndex());   // as the cue list: the selected cue comes into view

    repaint();
}

int CueCartView::totalRows() const
{
    return juce::jmax (rows, (cues.size() + cols - 1) / cols);
}

int CueCartView::cellHeight() const
{
    // the grid's rows fill the view as before; the rows beyond them have the same height
    return (getHeight() - 2 * cellGap - cellGap * (rows - 1)) / rows;
}

juce::Rectangle<int> CueCartView::gridArea() const
{
    auto area = getLocalBounds().reduced (cellGap);

    if (scrollBar.isVisible())
        area.removeFromRight (scrollBar.getWidth());   // the buttons end a gap before the bar

    return area;
}

void CueCartView::updateScrollRange()
{
    const int cellH = cellHeight();
    const int content = cellGap + totalRows() * (cellH + cellGap);
    const bool scrolls = cellH > 0 && content > getHeight();

    scrollBar.setBounds (getLocalBounds().removeFromRight (Palette::scrollBarWidth));
    scrollBar.setRangeLimits (0.0, (double) juce::jmax (content, getHeight()), juce::dontSendNotification);
    scrollBar.setCurrentRange (scrolls ? scrollBar.getCurrentRangeStart() : 0.0, (double) getHeight(), juce::dontSendNotification);
    scrollBar.setVisible (scrolls);
}

int CueCartView::getScrollPosition() const
{
    return scrollBar.isVisible() ? juce::roundToInt (scrollBar.getCurrentRangeStart()) : 0;
}

void CueCartView::setScrollPosition (int pixels)
{
    if (! scrollBar.isVisible())
        return;

    scrollBar.setCurrentRangeStart ((double) pixels, juce::dontSendNotification);   // clamped by the bar
    repaint();
}

void CueCartView::scrollToShow (int slot)
{
    if (! scrollBar.isVisible() || ! cues.isValidIndex (slot))
        return;

    const auto cell = cellBounds (slot);

    if (cell.getY() < cellGap)
        setScrollPosition (getScrollPosition() + cell.getY() - cellGap);
    else if (cell.getBottom() > getHeight() - cellGap)
        setScrollPosition (getScrollPosition() + cell.getBottom() - (getHeight() - cellGap));
}

void CueCartView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! scrollBar.isVisible() || juce::approximatelyEqual (wheel.deltaY, 0.0f))
    {
        juce::Component::mouseWheelMove (e, wheel);   // nothing to scroll: the parent may use it
        return;
    }

    // the cue list's wheel step: juce::Viewport moves 14 x its single step (a list row) per unit of wheel
    const float distance = wheel.deltaY * 14.0f * (float) Palette::rowHeights[1];
    setScrollPosition (getScrollPosition() - juce::roundToInt (distance < 0.0f ? juce::jmin (distance, -1.0f)
                                                                                : juce::jmax (distance, 1.0f)));
}

void CueCartView::setPlayingCues (std::vector<AudioEngine::PlayingCue> newPlaying)
{
    const bool wasEmpty = playing.empty();
    playing = std::move (newPlaying);

    if (! (wasEmpty && playing.empty()))
        repaint();
}

void CueCartView::setEditable (bool shouldBeEditable)
{
    editable = shouldBeEditable;
    repaint();
}

const AudioEngine::PlayingCue* CueCartView::findPlaying (const juce::Uuid& id) const
{
    const AudioEngine::PlayingCue* best = nullptr;

    for (const auto& p : playing)
        if (p.id == id && (best == nullptr || (best->loaded && ! p.loaded)))
            best = &p;

    return best;
}

juce::Rectangle<int> CueCartView::cellBounds (int slot) const
{
    const auto area = gridArea();
    const int cellW = (area.getWidth() - cellGap * (cols - 1)) / cols;
    const int cellH = cellHeight();
    const int r = slot / cols, c = slot % cols;
    return { area.getX() + c * (cellW + cellGap), area.getY() + r * (cellH + cellGap) - getScrollPosition(), cellW, cellH };
}

int CueCartView::slotAt (juce::Point<int> p) const
{
    if (! getLocalBounds().contains (p))   // a release below the view is not on the hidden part of a button
        return -1;

    for (int slot = 0; slot < totalRows() * cols; ++slot)
        if (cellBounds (slot).contains (p))
            return slot;

    return -1;
}

void CueCartView::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);
    const int slots = totalRows() * cols;

    for (int slot = 0; slot < slots; ++slot)
    {
        const auto r = cellBounds (slot);

        if (! r.intersects (getLocalBounds()))
            continue;   // scrolled out of view

        const bool hasCue = cues.isValidIndex (slot);

        if (! hasCue)
        {
            g.setColour (slot == dropSlot ? Palette::standby : Palette::outline);
            const float dash[] = { 4.0f, 4.0f };
            juce::Path outline, dashed;
            outline.addRoundedRectangle (r.toFloat().reduced (1.0f), 6.0f);
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dash, 2);
            g.fillPath (dashed);   // an empty slot
            continue;
        }

        const auto& cue = cues.get (slot);
        const auto* running = findPlaying (cue.id);
        const bool isRunning = running != nullptr && ! running->loaded;
        juce::Colour fill = Palette::rowEven;

        if (cue.color > 0)
            fill = fill.interpolatedWith (CueColors::get (cue.color).darker (2.0f), 0.55f);   // the text keeps reading

        if (isRunning)
            fill = running->paused ? Palette::pausedRow : (running->fadingOut ? Palette::fadingRow : Palette::playingRow);

        if (cue.id == pressedCueId)
            fill = fill.brighter (0.3f);

        g.setColour (fill);
        g.fillRoundedRectangle (r.toFloat(), 6.0f);

        if (isRunning && running->progress >= 0.0)
        {
            const float reached = (float) r.getWidth() * (float) juce::jlimit (0.0, 1.0, running->progress);
            g.setColour (juce::Colours::white.withAlpha (0.16f));
            g.fillRoundedRectangle (r.toFloat().withWidth (reached), 6.0f);
            g.setColour (running->paused ? Palette::paused : (running->fadingOut ? Palette::fadingOut : Palette::playing));
            g.fillRect (r.toFloat().removeFromBottom (4.0f).withWidth (reached));
        }

        if (! cue.armed)
        {
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRoundedRectangle (r.toFloat(), 6.0f);
        }

        g.setColour (cues.isSelected (slot) ? Palette::standby : (slot == dropSlot ? Palette::standby : Palette::outline));
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 6.0f, cues.isSelected (slot) ? 2.0f : 1.0f);

        auto inner = r.reduced (8, 6);
        g.setColour (Palette::text);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (cue.number, inner.removeFromTop (14), juce::Justification::topLeft, true);

        if (isRunning)
        {
            const auto remaining = running->remainingSeconds < 0.0 ? juce::String::fromUTF8 ("\xE2\x88\x9E") : "-" + formatSeconds (juce::jmax (0.0, running->remainingSeconds));
            g.setColour (juce::Colours::white);
            g.drawText (remaining, inner.removeFromBottom (14), juce::Justification::bottomRight, true);
        }
        else if (cue.fileMissing || (cue.isAudio() && cue.file == juce::File()))
        {
            g.setColour (Palette::missing);
            g.drawText (juce::String::fromUTF8 ("파일 없음"), inner.removeFromBottom (14), juce::Justification::bottomRight, true);
        }

        g.setColour (Palette::text);
        g.setFont (juce::Font (juce::FontOptions (r.getHeight() > 70 ? 15.0f : 13.0f, juce::Font::bold)));
        g.drawFittedText (cue.name.isNotEmpty() ? cue.name : juce::String::fromUTF8 ("(이름 없음)"), inner, juce::Justification::centred, 3, 0.8f);
    }
}

void CueCartView::mouseDown (const juce::MouseEvent& e)
{
    pressedCueId = juce::Uuid::null();
    pressedSlot = -1;
    grabKeyboardFocus();
    const int slot = slotAt (e.getPosition());

    if (slot < 0 || ! cues.isValidIndex (slot))
        return;

    // Selection listeners can reorder or remove rows. Keep the pressed cue's
    // identity across that notification.
    const auto id = cues.get (slot).id;
    {
        const juce::ScopedValueSetter<bool> pressing (clicking, true);   // the button stays where it was pressed
        cues.setSelectedIndex (slot);   // the inspector follows the button
    }

    if (cues.findById (id) == nullptr)
        return;

    if (e.mods.isPopupMenu())
    {
        if (onStop)
            onStop (id);

        return;
    }

    pressedCueId = id;
    pressedSlot = slot;
    repaint();
}

void CueCartView::mouseUp (const juce::MouseEvent& e)
{
    const auto id = pressedCueId;
    const int downSlot = pressedSlot;
    pressedCueId = juce::Uuid::null();
    pressedSlot = -1;
    repaint();

    const int slot = slotAt (e.getPosition());

    if (id.isNull() || e.mods.isPopupMenu() || slot != downSlot)
        return;

    if (const auto* cue = cues.findById (id); cue != nullptr && onTrigger)
        onTrigger (*cue);   // release in the pressed slot fires its surviving UUID
}

bool CueCartView::isInterestedInFileDrag (const juce::StringArray& files)
{
    return editable && containsAudioOrFolder (formats, files);
}

void CueCartView::fileDragEnter (const juce::StringArray&, int x, int y)
{
    dropSlot = slotAt ({ x, y });
    repaint();
}

void CueCartView::fileDragMove (const juce::StringArray&, int x, int y)
{
    const int slot = slotAt ({ x, y });

    if (slot != dropSlot)
    {
        dropSlot = slot;
        repaint();
    }
}

void CueCartView::fileDragExit (const juce::StringArray&)
{
    dropSlot = -1;
    repaint();
}

void CueCartView::filesDropped (const juce::StringArray& files, int x, int y)
{
    const int slot = slotAt ({ x, y });
    dropSlot = -1;
    repaint();

    const auto audioFiles = collectAudioFiles (formats, files);

    if (audioFiles.isEmpty() || ! onFilesDropped)
        return;

    // an empty slot beyond the last cue appends; a slot on a cue inserts in front of it
    onFilesDropped (audioFiles, slot < 0 ? cues.size() : juce::jmin (slot, cues.size()));
}

} // namespace gocue
