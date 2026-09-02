#include "ui/CueCartView.h"

#include "audio/CueFileInfo.h"
#include "model/CueColors.h"
#include "ui/UiUtils.h"

namespace gocue
{

CueCartView::CueCartView (CueList& c, juce::AudioFormatManager& f) : cues (c), formats (f)
{
    setWantsKeyboardFocus (true);
    cues.addListener (this);
}

CueCartView::~CueCartView()
{
    cues.removeListener (this);
}

void CueCartView::setGrid (int newRows, int newCols)
{
    rows = juce::jlimit (1, 15, newRows);
    cols = juce::jlimit (1, 15, newCols);
    repaint();
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
    const int gap = 6;
    const auto area = getLocalBounds().reduced (gap);
    const int cellW = (area.getWidth() - gap * (cols - 1)) / cols;
    const int cellH = (area.getHeight() - gap * (rows - 1)) / rows;
    const int r = slot / cols, c = slot % cols;
    return { area.getX() + c * (cellW + gap), area.getY() + r * (cellH + gap), cellW, cellH };
}

int CueCartView::slotAt (juce::Point<int> p) const
{
    for (int slot = 0; slot < rows * cols; ++slot)
        if (cellBounds (slot).contains (p))
            return slot;

    return -1;
}

void CueCartView::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);
    const int slots = rows * cols;

    for (int slot = 0; slot < slots; ++slot)
    {
        const auto r = cellBounds (slot);
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

        if (slot == pressedSlot)
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
    grabKeyboardFocus();
    const int slot = slotAt (e.getPosition());

    if (slot < 0 || ! cues.isValidIndex (slot))
        return;

    cues.setSelectedIndex (slot);   // the inspector follows the button

    if (e.mods.isPopupMenu())
    {
        const auto& cue = cues.get (slot);

        if (findPlaying (cue.id) != nullptr && onStop)
            onStop (cue.id);

        return;
    }

    pressedSlot = slot;
    repaint();
}

void CueCartView::mouseUp (const juce::MouseEvent& e)
{
    const int slot = pressedSlot;
    pressedSlot = -1;
    repaint();

    if (slot < 0 || e.mods.isPopupMenu() || ! cues.isValidIndex (slot))
        return;

    if (slotAt (e.getPosition()) == slot && onTrigger)
        onTrigger (cues.get (slot));   // release inside the same button fires it
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
