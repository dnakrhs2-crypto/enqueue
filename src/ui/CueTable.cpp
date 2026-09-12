#include "ui/CueTable.h"

#include <algorithm>

#include "app/Commands.h"
#include "audio/CueFileInfo.h"
#include "model/CueColors.h"
#include "ui/GroupModeLabels.h"
#include "ui/UiUtils.h"

namespace gocue
{

namespace
{
    const juce::String rowDragDescription ("gocue-rows");
    constexpr int indentPerLevel = Palette::childIndent;
    constexpr int disclosureWidth = 16;
}

juce::Colour CueTable::groupModeColour (GroupMode mode)
{
    switch (mode)
    {
        case GroupMode::timeline:        return Palette::playing;
        case GroupMode::playlist:        return Palette::fadingOut;
        case GroupMode::startFirstEnter:
        case GroupMode::startFirst:      return Palette::accent;
        case GroupMode::random:          return Palette::selectionRing;
    }

    return Palette::muted;
}

//==============================================================================
/** A text editor floating over one cell; commits on Return / focus loss, cancels on Escape. */
class CueTable::CellEditor : public juce::TextEditor
{
public:
    CellEditor (CueTable& o, int r, ColumnId c, const juce::String& initial)
        : owner (o), row (r), column (c), cueId (o.cues.get (r).id), generation (++o.editGeneration)
    {
        setText (initial, false);
        setSelectAllWhenFocused (true);
        setJustification (column == colName || column == colNumber ? juce::Justification::centredLeft : juce::Justification::centredRight);
        setFont (column == colName ? Palette::font() : Palette::monoFont (Palette::timeSize));
        onReturnKey = [this] { commit(); };
        onEscapeKey = [this] { cancel(); };
        onFocusLost = [this] { commit(); };
    }

    void commit()
    {
        if (done)
            return;

        done = true;
        const auto text = getText();
        juce::Component::SafePointer<CueTable> safeOwner (&owner);
        const ColumnId c = column;
        const auto id = cueId;
        const int gen = generation;

        juce::MessageManager::callAsync ([safeOwner, id, gen, c, text]
        {
            if (safeOwner == nullptr)
                return;

            if (safeOwner->editGeneration == gen)   // still our editor (a newer one may have replaced it)
                safeOwner->cellEditor.reset();

            const int currentRow = safeOwner->cues.indexOf (id);   // the row may have moved meanwhile

            if (currentRow >= 0)
                safeOwner->commitCellEdit (currentRow, c, text);

            if (safeOwner->editGeneration == gen)   // a newer editor keeps its focus
                safeOwner->focusTable();
        });
    }

    const juce::Uuid& getCueId() const noexcept { return cueId; }
    ColumnId getColumn() const noexcept { return column; }
    void markDone() noexcept { done = true; }

    void cancel()
    {
        if (done)
            return;

        done = true;
        juce::Component::SafePointer<CueTable> safeOwner (&owner);
        const int gen = generation;
        juce::MessageManager::callAsync ([safeOwner, gen]
        {
            if (safeOwner == nullptr)
                return;

            if (safeOwner->editGeneration == gen)
                safeOwner->cellEditor.reset();

            safeOwner->focusTable();
           #if ! JUCE_WINDOWS
            safeOwner->commands.invokeDirectly (CommandIDs::panicAll, true);   // Esc is the panic key, editing or not (Windows: the keyboard hook does it)
           #endif
        });
    }

private:
    CueTable& owner;
    const int row;
    const ColumnId column;
    const juce::Uuid cueId;
    const int generation;
    bool done = false;
};

//==============================================================================
bool CueTable::TableBox::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();

    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
        return false;   // e.g. Ctrl+Up/Down = move cue, handled by the application commands

    if (onKey && onKey (key))
        return true;

    return juce::TableListBox::keyPressed (key);
}

CueTable::CueTable (CueList& c, juce::AudioFormatManager& f, juce::ApplicationCommandManager& cm)
    : cues (c), formats (f), commands (cm)
{
    const int columnFlags = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable;

    auto& header = table.getHeader();
    header.addColumn ("",                  colStatus,   Palette::statusColumnWidth, Palette::statusColumnWidth, Palette::statusColumnWidth, juce::TableHeaderComponent::visible);
    header.addColumn (ko ("번호"),         colNumber,   Palette::numberColumnWidth, 40, 120, columnFlags);
    header.addColumn (ko ("이름"),         colName,     260, 80,  -1,  columnFlags);
    header.addColumn (ko ("파일"),         colFile,     Palette::fileColumnWidth, 80, -1, columnFlags);
    header.addColumn (ko ("프리웨이트"),   colPreWait,  Palette::preWaitColumnWidth, 60, 120, columnFlags);
    header.addColumn (ko ("길이"),         colDuration, Palette::durationColumnWidth, 60, 140, columnFlags);
    header.addColumn (ko ("포스트웨이트"), colPostWait, Palette::postWaitColumnWidth, 60, 120, columnFlags);
    header.addColumn (ko ("진행"),         colContinue, Palette::continueColumnWidth, Palette::continueColumnWidth, Palette::continueColumnWidth, juce::TableHeaderComponent::visible);
    header.setStretchToFitActive (false);   // only the name flexes; the CSS widths stay exact

    table.setModel (this);
    table.setRowHeight (Palette::rowHeights[1]);
    table.setHeaderHeight (Palette::tableHeaderHeight);
    table.setMultipleSelectionEnabled (true);
    table.setClickingTogglesRowSelection (false);
    table.setColour (juce::ListBox::backgroundColourId, Palette::panel);
    table.setColour (juce::ListBox::outlineColourId, Palette::outline);
    table.setOutlineThickness (0);
    table.getViewport()->setScrollBarThickness (Palette::scrollBarWidth);
    table.onKey = [this] (const juce::KeyPress& key) { return handleQuickEditKey (key); };
    addAndMakeVisible (table);

    cues.addListener (this);
    rebuildVisible();
    syncSelectionFromModel();
}

void CueTable::rebuildVisible()
{
    visible.clear();

    for (int i = 0; i < cues.size(); ++i)
        if (cues.isRowVisible (i))
            visible.push_back (i);
}

int CueTable::rowOf (int index) const noexcept
{
    const auto it = std::lower_bound (visible.begin(), visible.end(), index);
    return it != visible.end() && *it == index ? (int) (it - visible.begin()) : -1;
}

bool CueTable::isGroupRunning (int index) const
{
    if (! cues.isValidIndex (index) || ! cues.get (index).isGroup())
        return false;

    const auto id = cues.get (index).id;

    for (const auto& p : playing)
        if (! p.loaded)
            if (const int i = cues.indexOf (p.id); i >= 0 && cues.isDescendantOf (i, id))
                return true;

    return false;
}

CueTable::~CueTable()
{
    cellEditor.reset();
    cues.removeListener (this);
    table.setModel (nullptr);
}

void CueTable::setPlayingCues (std::vector<AudioEngine::PlayingCue> newPlaying)
{
    const bool wasEmpty = playing.empty();
    playing = std::move (newPlaying);

    if (! (wasEmpty && playing.empty()))
        table.repaint();
}

void CueTable::setEditable (bool shouldBeEditable)
{
    editable = shouldBeEditable;

    if (! editable)
        cellEditor.reset();

    table.repaint();
}

void CueTable::setRowSize (int size)
{
    table.setRowHeight (Palette::rowHeights[juce::jlimit (0, 2, size)]);
}

void CueTable::focusTable()
{
    table.grabKeyboardFocus();
}

void CueTable::finishEditing()
{
    if (cellEditor == nullptr)
        return;

    const auto text = cellEditor->getText();
    const auto id = cellEditor->getCueId();
    const auto column = cellEditor->getColumn();
    cellEditor->markDone();   // its own async commit / cancel must not run afterwards
    cellEditor.reset();

    if (const int row = cues.indexOf (id); row >= 0)
        commitCellEdit (row, column, text);
}

void CueTable::resized()
{
    table.setBounds (getLocalBounds());
    auto& header = table.getHeader();
    int otherWidth = 0;
    for (const int column : { colStatus, colNumber, colFile, colPreWait, colDuration, colPostWait, colContinue })
        otherWidth += header.getColumnWidth (column);
    header.setColumnWidth (colName, juce::jmax (140, getWidth() - Palette::scrollBarWidth - otherWidth));

    if (cellEditor != nullptr)
        cellEditor.reset();
}

void CueTable::paint (juce::Graphics& g)
{
    if (dragOver)
    {
        g.setColour (Palette::standby.withAlpha (0.25f));
        g.fillAll();
    }
}

void CueTable::paintOverChildren (juce::Graphics& g)
{
    if (dropGroupIndex >= 0)   // files or rows hovering a group: the whole row lights up
    {
        const auto it = std::find (visible.begin(), visible.end(), dropGroupIndex);

        if (it != visible.end())
        {
            const auto r = table.getRowPosition ((int) (it - visible.begin()), true).translated (table.getX(), table.getY());
            g.setColour (Palette::standby);
            g.drawRoundedRectangle (r.toFloat().reduced (1.5f), Palette::colourBarRadius, Palette::selectionWidth);
        }
    }

    if (! rowDragOver || rowDropIndex < 0)
        return;

    // insertion line between rows for a row drag
    const int rowHeight = table.getRowHeight();
    const int headerHeight = table.getHeader().getHeight();
    const int rowY = table.getY() + headerHeight + rowDropIndex * rowHeight - table.getViewport()->getViewPositionY();
    g.setColour (Palette::standby);
    g.fillRect (table.getX() + 2, rowY - 1, table.getWidth() - 4, 3);
}

//==============================================================================
int CueTable::getNumRows()
{
    return (int) visible.size();
}

const AudioEngine::PlayingCue* CueTable::findPlaying (const juce::Uuid& id) const
{
    const AudioEngine::PlayingCue* best = nullptr;

    for (const auto& p : playing)
        if (p.id == id && (best == nullptr || (best->loaded && ! p.loaded)))   // prefer a running instance over a loaded one
            best = &p;

    return best;
}

void CueTable::paintRowBackground (juce::Graphics& g, int rowNumber, int width, int height, bool)
{
    const int index = modelIndex (rowNumber);
    if (! cues.isValidIndex (index))
        return;

    const auto& cue = cues.get (index);
    const auto* running = findPlaying (cue.id);
    const bool active = running != nullptr && ! running->loaded;
    auto background = Palette::panel;
    if (active)
        background = running->paused ? Palette::pausedRow : running->fadingOut ? Palette::fadingRow : Palette::playingRow;
    else if (isGroupRunning (index))
        background = Palette::playingRow;

    if (cues.isSelected (index))
        background = Palette::selected;
    if (! cue.armed)
        background = Palette::panel.interpolatedWith (background, Palette::disabledAlpha);
    g.fillAll (background);
    g.setColour (Palette::outline.withAlpha (Palette::rowBorderAlpha));
    g.fillRect (0, height - 1, width, 1);

    if (active && ! running->paused && running->progress >= 0.0)
    {
        g.setColour ((running->fadingOut ? Palette::fadingOut : Palette::playing).withMultipliedAlpha (cue.armed ? 1.0f : Palette::disabledAlpha));
        g.fillRect (0, height - Palette::rowProgressHeight,
                    juce::roundToInt (width * juce::jlimit (0.0, 1.0, running->progress)), Palette::rowProgressHeight);
    }

    if (index == cues.getPlayheadIndex())
    {
        g.setColour (Palette::accent.withMultipliedAlpha (cue.armed ? 1.0f : Palette::disabledAlpha));
        g.fillRect (0, 0, Palette::playheadWidth, height);
    }

    // Last: selection encloses even a running cue or the playhead.
    if (cues.isSelected (index))
    {
        g.setColour (Palette::selectionRing.withMultipliedAlpha (cue.armed ? 1.0f : Palette::disabledAlpha));
        g.drawRect (0, 0, width, height, (int) Palette::selectionWidth);
    }
}

CueTable::Badge CueTable::badgeFor (int index) const
{
    const auto& cue = cues.get (index);
    const auto* p = findPlaying (cue.id);
    if (p != nullptr && ! p->loaded)
        return p->paused ? Badge { ko ("일시정지"), Palette::paused }
                         : p->fadingOut ? Badge { ko ("페이드 아웃"), Palette::fadingOut }
                                        : Badge { ko ("재생 중"), Palette::playing };
    if (cue.isAudio() && (cue.fileMissing || cue.file == juce::File()))
        return { ko ("누락 파일"), Palette::missing };
    if (cue.hasTarget() && (cue.targetId().isNull() || cues.indexOf (cue.targetId()) < 0))
        return { ko ("대상 없음"), Palette::missing };
    if (! cue.armed)
        return { ko ("비활성"), Palette::muted };
    if (cues.isSelected (index))
        return { ko ("선택"), Palette::selectionRing, true };
    if (index == cues.getPlayheadIndex())
        return { ko ("다음 큐"), Palette::accent };
    if (cue.isGroup())
        return { ko ("자식 ") + juce::String ((int) cues.childrenOf (index).size()) + ko ("개 · ") + groupPresetName (cue.group), Palette::muted };
    if (cue.wallClock.enabled)
        return { wallClockText (cue), Palette::muted };
    if (cue.hotkey.isNotEmpty())
        return { ko ("핫키 ") + cue.hotkey, Palette::muted };
    if (cue.isControl() && cue.control.kind == ControlKind::wait)
        return { ko ("대기 ") + juce::String (cue.control.seconds, std::abs (cue.control.seconds - std::round (cue.control.seconds)) < 0.001 ? 0 : 2) + ko ("초"), Palette::muted };
    return {};
}

juce::String CueTable::wallClockText (const Cue& cue)
{
    const auto& t = cue.wallClock;
    juce::String days;
    if ((t.daysMask & 0x7f) == 0x7f)
        days = ko ("매일");
    else
    {
        static const char* const names[] = { "일", "월", "화", "수", "목", "금", "토" };
        for (int d = 0; d < 7; ++d)
            if ((t.daysMask & (1 << d)) != 0)
                days += ko (names[d]);
    }
    return juce::String::formatted ("%02d:%02d:%02d", t.hour, t.minute, t.second)
             + (days.isEmpty() ? juce::String() : ko (" · ") + days);
}

juce::String CueTable::getCellTooltip (int rowNumber, int columnId)
{
    const int index = modelIndex (rowNumber);
    if (! cues.isValidIndex (index))
        return {};
    const auto& cue = cues.get (index);
    juce::StringArray parts;
    parts.add ((cue.number + " " + cue.name).trim());
    if (columnId == colFile && cue.file != juce::File())
        parts.add (cue.file.getFullPathName());
    parts.addIfNotAlreadyThere (badgeFor (index).text);
    if (cue.isGroup())
        parts.addIfNotAlreadyThere (ko ("자식 ") + juce::String ((int) cues.childrenOf (index).size()) + ko ("개 · ") + groupPresetName (cue.group));
    if (cue.hotkey.isNotEmpty())
        parts.addIfNotAlreadyThere (ko ("핫키 ") + cue.hotkey);
    if (cue.wallClock.enabled)
        parts.addIfNotAlreadyThere (wallClockText (cue));
    if (cue.flagged)
        parts.add (ko ("깃발"));
    parts.removeEmptyStrings();
    return parts.joinIntoString ("\n");
}

void CueTable::paintCell (juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool)
{
    const int index = modelIndex (rowNumber);
    if (! cues.isValidIndex (index))
        return;

    const auto& cue = cues.get (index);
    const auto* running = findPlaying (cue.id);
    const bool isRunning = running != nullptr && ! running->loaded;
    const auto stateColour = isRunning ? (running->paused ? Palette::paused : running->fadingOut ? Palette::fadingOut : Palette::playing) : Palette::muted;
    const float alpha = cue.armed ? 1.0f : Palette::disabledAlpha;
    auto setColour = [&] (juce::Colour colour) { g.setColour (colour.withMultipliedAlpha (alpha)); };

    if (columnId == colStatus)
    {
        const float cy = height * 0.5f;
        const float x = (float) (width - Palette::statusIconSize) * 0.5f + 2.0f;
        const bool broken = (cue.isAudio() && (cue.fileMissing || cue.file == juce::File()))
                              || (cue.hasTarget() && (cue.targetId().isNull() || cues.indexOf (cue.targetId()) < 0));
        if (isRunning)
        {
            setColour (stateColour);
            juce::Path icon;
            if (running->paused)
            {
                icon.addRectangle (x + 1.0f, cy - 5.0f, 3.0f, 10.0f);
                icon.addRectangle (x + 7.0f, cy - 5.0f, 3.0f, 10.0f);
                g.fillPath (icon);
            }
            else if (running->fadingOut)
            {
                icon.startNewSubPath (x, cy - 5.0f);
                icon.lineTo (x + 4.0f, cy + 1.0f);
                icon.lineTo (x + 11.0f, cy + 5.0f);
                icon.lineTo (x, cy + 5.0f);
                icon.closeSubPath();
                g.strokePath (icon, juce::PathStrokeType (1.5f));
            }
            else
            {
                icon.addTriangle (x + 1.0f, cy - 5.0f, x + 1.0f, cy + 5.0f, x + 10.0f, cy);
                g.fillPath (icon);
            }
        }
        else if (broken)
        {
            setColour (Palette::missing);
            juce::Path warning;
            warning.addTriangle (x + 6.0f, cy - 6.0f, x, cy + 5.0f, x + 12.0f, cy + 5.0f);
            g.strokePath (warning, juce::PathStrokeType (1.5f));
            g.fillRect (x + 5.3f, cy - 2.5f, 1.4f, 4.0f);
            g.fillEllipse (x + 5.2f, cy + 2.7f, 1.6f, 1.6f);
        }
        else if (! cue.armed)
        {
            setColour (Palette::muted);
            g.drawLine (x, cy, x + 12.0f, cy, 1.5f);
        }
        else if (cue.isGroup())
        {
            setColour (Palette::accent);
            juce::Path folder;
            folder.addRoundedRectangle (x, cy - 3.0f, 12.0f, 9.0f, 1.5f);
            folder.addRectangle (x, cy - 5.0f, 5.0f, 3.0f);
            g.fillPath (folder);
        }
        else if (index == cues.getPlayheadIndex())
        {
            setColour (Palette::accent);
            juce::Path playhead;
            playhead.addTriangle (x, cy - 5.0f, x, cy + 5.0f, x + 8.0f, cy);
            playhead.addRectangle (x + 10.0f, cy - 5.0f, 1.5f, 10.0f);
            g.fillPath (playhead);
        }
        else if (running != nullptr && running->loaded)
        {
            setColour (Palette::warn);
            g.fillEllipse (x + 1.0f, cy - 5.0f, 10.0f, 10.0f);
        }
        else if (cue.wallClock.enabled || (cue.isControl() && cue.control.kind == ControlKind::wait))
        {
            setColour (Palette::muted);
            juce::Path clock;
            clock.addEllipse (x, cy - 6.0f, 12.0f, 12.0f);
            clock.startNewSubPath (x + 6.0f, cy - 4.0f);
            clock.lineTo (x + 6.0f, cy);
            clock.lineTo (x + 9.0f, cy + 2.0f);
            g.strokePath (clock, juce::PathStrokeType (1.4f));
        }
        else if (cue.hotkey.isNotEmpty())
        {
            setColour (Palette::muted);
            juce::Path key;
            key.addRoundedRectangle (x - 1.0f, cy - 5.0f, 14.0f, 10.0f, Palette::keyRadius);
            key.startNewSubPath (x + 2.0f, cy + 2.0f);
            key.lineTo (x + 10.0f, cy + 2.0f);
            g.strokePath (key, juce::PathStrokeType (1.2f));
        }
        else if (cue.isFade())
        {
            // a fade: a slope; red when the target is missing
            const bool broken = cue.fade.targetId.isNull() || cues.indexOf (cue.fade.targetId) < 0;
            setColour (broken ? Palette::missing : Palette::dimText);
            juce::Path slope;

            if (cue.fade.mode == FadeMode::fadeOut)
            {
                slope.startNewSubPath (x, cy - 5.0f);
                slope.lineTo (x + 10.0f, cy + 5.0f);
                slope.lineTo (x, cy + 5.0f);
            }
            else
            {
                slope.startNewSubPath (x, cy + 5.0f);
                slope.lineTo (x + 10.0f, cy - 5.0f);
                slope.lineTo (x + 10.0f, cy + 5.0f);
            }

            slope.closeSubPath();
            g.fillPath (slope);
        }
        else if (cue.isDevamp())
        {
            // a devamp: a loop arc with a bar (the loop point)
            const bool broken = cue.devamp.targetId.isNull() || cues.indexOf (cue.devamp.targetId) < 0;
            setColour (broken ? Palette::missing : Palette::dimText);
            juce::Path arc;
            arc.addCentredArc (x + 5.0f, cy, 4.5f, 4.5f, 0.0f, 0.4f, 5.9f, true);
            g.strokePath (arc, juce::PathStrokeType (1.8f));
            g.fillRect (x + 9.0f, cy - 6.0f, 2.0f, 12.0f);
        }
        else if (cue.isControl())
        {
            // control cues: one small glyph per kind; red when a needed target is missing
            const bool broken = cue.control.needsTarget() && (cue.control.targetId.isNull() || cues.indexOf (cue.control.targetId) < 0);
            setColour (broken ? Palette::missing : Palette::dimText);
            juce::Path p;

            switch (cue.control.kind)
            {
                case ControlKind::start:   p.addTriangle (x, cy - 5.0f, x, cy + 5.0f, x + 9.0f, cy); g.strokePath (p, juce::PathStrokeType (1.5f)); break;
                case ControlKind::stop:    g.drawRect (x, cy - 4.5f, 9.0f, 9.0f, 1.5f); break;
                case ControlKind::pause:   g.drawRect (x, cy - 5.0f, 3.0f, 10.0f, 1.5f); g.drawRect (x + 5.5f, cy - 5.0f, 3.0f, 10.0f, 1.5f); break;
                case ControlKind::load:    g.drawEllipse (x, cy - 4.5f, 9.0f, 9.0f, 1.5f); g.fillEllipse (x + 3.0f, cy - 1.5f, 3.0f, 3.0f); break;
                case ControlKind::reset:   p.addCentredArc (x + 5.0f, cy, 4.5f, 4.5f, 0.0f, 0.9f, 6.0f, true); g.strokePath (p, juce::PathStrokeType (1.5f));
                                           g.fillRect (x + 8.0f, cy - 6.5f, 3.0f, 3.0f); break;
                case ControlKind::gotoCue: g.drawLine (x, cy - 5.0f, x, cy + 2.0f, 1.5f); g.drawLine (x, cy + 2.0f, x + 8.0f, cy + 2.0f, 1.5f);
                                           p.addTriangle (x + 7.0f, cy - 2.0f, x + 7.0f, cy + 6.0f, x + 11.0f, cy + 2.0f); g.fillPath (p); break;
                case ControlKind::wait:    g.drawEllipse (x, cy - 5.0f, 10.0f, 10.0f, 1.5f); g.drawLine (x + 5.0f, cy - 3.0f, x + 5.0f, cy, 1.5f);
                                           g.drawLine (x + 5.0f, cy, x + 7.5f, cy + 1.5f, 1.5f); break;
                case ControlKind::memo:    g.drawRect (x, cy - 5.0f, 9.0f, 10.0f, 1.2f); g.drawLine (x + 2.0f, cy - 2.0f, x + 7.0f, cy - 2.0f, 1.0f);
                                           g.drawLine (x + 2.0f, cy + 0.5f, x + 7.0f, cy + 0.5f, 1.0f); g.drawLine (x + 2.0f, cy + 3.0f, x + 5.0f, cy + 3.0f, 1.0f); break;
                case ControlKind::arm:     g.drawLine (x, cy - 6.0f, x, cy + 6.0f, 1.5f); p.addTriangle (x, cy - 6.0f, x + 8.0f, cy - 3.0f, x, cy); g.fillPath (p); break;
                case ControlKind::disarm:  g.drawLine (x, cy - 6.0f, x, cy + 6.0f, 1.5f); p.addTriangle (x, cy - 6.0f, x + 8.0f, cy - 3.0f, x, cy); g.strokePath (p, juce::PathStrokeType (1.2f));
                                           g.drawLine (x - 1.0f, cy + 6.0f, x + 10.0f, cy - 7.0f, 1.5f); break;
                case ControlKind::target:  g.drawEllipse (x, cy - 5.0f, 10.0f, 10.0f, 1.5f); g.drawLine (x + 5.0f, cy - 7.0f, x + 5.0f, cy + 7.0f, 1.0f);
                                           g.drawLine (x - 2.0f, cy, x + 12.0f, cy, 1.0f); break;
            }
        }
        else if (cue.isMic())
        {
            // a mic: a capsule on a stand
            setColour (Palette::dimText);
            g.fillRoundedRectangle (x + 2.5f, cy - 7.0f, 5.0f, 9.0f, 2.5f);
            juce::Path stand;
            stand.addCentredArc (x + 5.0f, cy, 4.5f, 4.5f, 0.0f, 1.6f, 4.7f, true);
            g.strokePath (stand, juce::PathStrokeType (1.4f));
            g.drawLine (x + 5.0f, cy + 4.5f, x + 5.0f, cy + 7.0f, 1.4f);
            g.drawLine (x + 2.0f, cy + 7.0f, x + 8.0f, cy + 7.0f, 1.4f);
        }
        else
        {
            setColour (Palette::muted);
            juce::Path wave;
            const float lengths[] = { 5.0f, 9.0f, 13.0f, 8.0f, 4.0f };
            for (int i = 0; i < 5; ++i)
                wave.addRoundedRectangle (x + (float) i * 3.0f, cy - lengths[i] * 0.5f, 1.5f, lengths[i], 0.7f);
            g.fillPath (wave);
        }

        // Flags and preloaded status remain visible as small corner marks beside the main icon.
        if (cue.flagged)
        {
            setColour (Palette::warn);
            juce::Path flag;
            flag.startNewSubPath ((float) width - 6.0f, 3.0f);
            flag.lineTo ((float) width - 6.0f, 11.0f);
            g.strokePath (flag, juce::PathStrokeType (1.0f));
            flag.addTriangle ((float) width - 6.0f, 3.0f, (float) width - 2.0f, 5.0f, (float) width - 6.0f, 7.0f);
            g.fillPath (flag);
        }
        if (running != nullptr && running->loaded && index == cues.getPlayheadIndex())
        {
            setColour (Palette::warn);
            g.fillEllipse (2.0f, (float) height - 7.0f, 4.0f, 4.0f);
        }
        return;
    }

    if (columnId == colContinue)
    {
        setColour (Palette::accent);
        g.setFont (Palette::font (Palette::bodySize, true));
        g.drawText (cue.continueMode == ContinueMode::autoContinue ? ko ("→") : cue.continueMode == ContinueMode::autoFollow ? ko ("↓") : juce::String(),
                    juce::Rectangle<int> (width, height), juce::Justification::centred, false);
        return;
    }

    if (columnId == colName)
    {
        auto area = juce::Rectangle<int> (width, height).reduced (6, 0);
        area.removeFromLeft (juce::jmin (area.getWidth(), cues.depthOf (index) * Palette::childIndent));
        if (cue.isGroup())
        {
            const auto disclosure = area.removeFromLeft (disclosureWidth).toFloat();
            const auto c = disclosure.getCentre();
            juce::Path triangle;
            if (cue.group.collapsed)
                triangle.addTriangle (c.x - 2.0f, c.y - 4.0f, c.x - 2.0f, c.y + 4.0f, c.x + 3.0f, c.y);
            else
                triangle.addTriangle (c.x - 4.0f, c.y - 2.0f, c.x + 4.0f, c.y - 2.0f, c.x, c.y + 3.0f);
            setColour (Palette::muted);
            g.fillPath (triangle);
        }

        const int colourIndex = cue.useSecondColor && cue.secondColor > 0 && hasPlayed && hasPlayed (cue.id) ? cue.secondColor : cue.color;
        if (colourIndex > 0)
        {
            setColour (CueColors::get (colourIndex));
            g.fillRoundedRectangle (area.withWidth (Palette::colourBarWidth).withSizeKeepingCentre (Palette::colourBarWidth, Palette::colourBarHeight).toFloat(),
                                    Palette::colourBarRadius);
        }
        area.removeFromLeft (Palette::colourBarWidth + 8);
        const auto badge = badgeFor (index);
        const auto badgeFont = Palette::font (Palette::pillSize, true);
        if (badge.text.isNotEmpty())
        {
            const int wanted = juce::GlyphArrangement::getStringWidthInt (badgeFont, badge.text) + 14;
            const int badgeWidth = juce::jmin (wanted, juce::jmax (0, area.getWidth() - 32));
            if (badgeWidth > 0)
            {
                const auto pill = area.removeFromRight (badgeWidth).withSizeKeepingCentre (badgeWidth, Palette::pillHeight);
                Palette::drawPill (g, pill, badge.colour.withMultipliedAlpha (alpha), badge.text, badgeFont, badge.filled);
                area.removeFromRight (8);
            }
        }
        const auto name = cue.name.isNotEmpty() ? cue.name : ko ("(이름 없음)");
        const auto font = Palette::font (Palette::bodySize, true);
        setColour (Palette::text);
        g.setFont (font);
        if (cue.isGroup())
            Palette::drawHeavyText (g, name, area, font, juce::Justification::centredLeft, Palette::groupTextStroke);
        else
            g.drawText (name, area, juce::Justification::centredLeft, true);
        if (! cue.armed)
        {
            const int length = juce::jmin (area.getWidth(), juce::GlyphArrangement::getStringWidthInt (font, name));
            g.drawLine ((float) area.getX(), (float) area.getCentreY(), (float) (area.getX() + length), (float) area.getCentreY(), Palette::borderWidth);
        }
        return;
    }

    juce::String text;
    auto colour = Palette::muted;
    auto font = Palette::monoFont (Palette::timeSize);
    auto justification = juce::Justification::centredRight;
    switch (columnId)
    {
        case colNumber:
            text = cue.number;
            font = Palette::monoFont (Palette::bodySize).boldened();
            justification = juce::Justification::centredLeft;
            break;
        case colFile:
            font = Palette::monoFont (Palette::fileSize);
            justification = juce::Justification::centredLeft;
            if (cue.isGroup())
            {
                text = groupPresetName (cue.group) + " · " + juce::String ((int) cues.childrenOf (index).size()) + ko ("개");
                colour = Palette::muted;
                font = Palette::font (Palette::fileSize);
                break;
            }
            if (cue.isMic())
                text = ko ("입력 ") + juce::String (cue.mic.firstInput + 1) + (cue.mic.numInputs > 1 ? "-" + juce::String (cue.mic.firstInput + cue.mic.numInputs) : juce::String());
            else if (cue.isControl() && ! cue.control.needsTarget())
                text = cue.control.kind == ControlKind::wait ? ko ("대기 ") + formatTimeMs (cue.control.seconds) : ko ("메모");
            else if (cue.hasTarget())
            {
                const int target = cue.targetId().isNull() ? -1 : cues.indexOf (cue.targetId());
                if (target < 0)
                {
                    text = ko ("대상 없음");
                    colour = Palette::missing;
                }
                else
                {
                    const auto& t = cues.get (target);
                    text = ko ("→ ") + (t.number.isNotEmpty() ? t.number + " " : juce::String()) + t.name;
                }
            }
            else if (cue.file == juce::File())
            {
                text = ko ("파일 없음");
                colour = Palette::missing;
            }
            else
            {
                text = cue.file.getFileName();
                if (cue.fileMissing)
                {
                    text = ko ("[없음] ") + text;
                    colour = Palette::missing;
                }
            }
            break;
        case colPreWait:
            text = cue.preWaitSeconds > 0.0 ? formatTimeMs (cue.preWaitSeconds) : juce::String();
            break;
        case colPostWait:
            text = cue.postWaitSeconds > 0.0 ? formatTimeMs (cue.postWaitSeconds) : juce::String();
            break;
        case colDuration:
            colour = Palette::text;
            if (isRunning && running->lengthSeconds != 0.0)
            {
                text = running->lengthSeconds < 0.0 ? ko ("∞ ") + formatSeconds (running->positionSeconds)
                                                   : "-" + formatSeconds (juce::jmax (0.0, running->remainingSeconds));
                colour = stateColour;
            }
            else
            {
                const double effective = cues.effectiveLengthOf (index);
                text = effective < 0.0 ? ko ("∞") : formatSeconds (effective > 0.0 ? effective : cue.durationSeconds);
            }
            font = font.boldened();
            break;
        default: break;
    }
    setColour (colour);
    g.setFont (font);
    g.drawText (text, 6, 0, juce::jmax (0, width - 12), height, justification, true);
}

void CueTable::cellClicked (int rowNumber, int columnId, const juce::MouseEvent& e)
{
    const int index = modelIndex (rowNumber);

    if (! cues.isValidIndex (index))
        return;

    table.grabKeyboardFocus();   // the quick-edit keys (N Q E W C O D) need the table focused

    if (e.mods.isPopupMenu())
    {
        if (! cues.isSelected (index))
            cues.setSelectedIndex (index);

        showContextMenu (index, e.getScreenPosition());
        return;
    }

    if (columnId == colStatus)
        cues.setPlayheadIndex (index);
    else if (columnId == colContinue && editable)
        cycleContinueMode (index);
    else if (columnId == colName && cues.get (index).isGroup() && onToggleCollapse
             && e.x - table.getHeader().getColumnPosition (table.getHeader().getIndexOfColumnId (colName, true)).getX()
                          < 6 + cues.depthOf (index) * indentPerLevel + disclosureWidth)
        onToggleCollapse (index, ! cues.get (index).group.collapsed);
}

void CueTable::cellDoubleClicked (int rowNumber, int columnId, const juce::MouseEvent&)
{
    const int index = modelIndex (rowNumber);

    if (! editable || ! cues.isValidIndex (index))
        return;

    switch (columnId)
    {
        case colNumber:
        case colName:
        case colPreWait:
        case colPostWait:
            beginCellEdit (index, (ColumnId) columnId);
            break;

        case colDuration:
            if (onEditDuration)
                onEditDuration (index);
            break;

        default:
            break;
    }
}

void CueTable::selectedRowsChanged (int lastRowSelected)
{
    if (syncingSelection)
        return;

    std::vector<int> rows;
    const auto selected = table.getSelectedRows();

    for (int i = 0; i < selected.size(); ++i)
        if (const int index = modelIndex (selected[i]); index >= 0)
            rows.push_back (index);

    if (rows.empty())
    {
        syncSelectionFromModel();   // keep the model's selection: clicking empty space must not clear the next cue
        return;
    }

    cues.setSelection (rows, modelIndex (lastRowSelected));
}

void CueTable::deleteKeyPressed (int)
{
    if (editable)
        commands.invokeDirectly (CommandIDs::removeCue, true);
}

void CueTable::backgroundClicked (const juce::MouseEvent& e)
{
    syncSelectionFromModel();

    if (e.mods.isPopupMenu())
        showAddMenu (e.getScreenPosition());   // empty space: the add-cue items
}

void CueTable::addCueItems (juce::PopupMenu& menu)
{
    for (auto id : { CommandIDs::addCue, CommandIDs::addFadeCue, CommandIDs::addFadeOutCue, CommandIDs::addDevampCue, CommandIDs::addGroupCue,
                     CommandIDs::addControlCue, CommandIDs::addWaitCue, CommandIDs::addMemoCue, CommandIDs::addMicCue })
        menu.addCommandItem (&commands, id);
}

void CueTable::showAddMenu (juce::Point<int> screenPosition)
{
    juce::PopupMenu menu;
    addCueItems (menu);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ screenPosition.x, screenPosition.y, 1, 1 }));
}

juce::var CueTable::getDragSourceDescription (const juce::SparseSet<int>& rowsToDescribe)
{
    if (! editable || rowsToDescribe.isEmpty())
        return {};

    draggedIds.clear();   // what moves is what the drag started with, not the selection at the drop

    for (int i = 0; i < rowsToDescribe.size(); ++i)
        if (const int index = modelIndex (rowsToDescribe[i]); cues.isValidIndex (index))
            draggedIds.push_back (cues.get (index).id);

    if (onStatus)
        onStatus (ko ("행 드래그 시작: ") + juce::String (rowsToDescribe.size()) + ko ("개 (놓을 자리에 선이 표시됩니다)"));

    return rowDragDescription;
}

//==============================================================================
void CueTable::cueListStructureChanged()
{
    cellEditor.reset();
    rebuildVisible();
    table.updateContent();
    syncSelectionFromModel();
    table.repaint();
}

void CueTable::cueChanged (int index)
{
    if (const int row = rowOf (index); row >= 0)
        table.repaintRow (row);

    // a child's length / pre-wait changes what its group shows
    for (int p = cues.parentIndexOf (index), guard = 0; p >= 0 && guard < 32; p = cues.parentIndexOf (p), ++guard)
        if (const int row = rowOf (p); row >= 0)
            table.repaintRow (row);
}

void CueTable::cueSelectionChanged (int)
{
    syncSelectionFromModel();
}

void CueTable::playheadChanged (int index)
{
    table.repaint();

    if (const int row = rowOf (index); row >= 0)
        table.scrollToEnsureRowIsOnscreen (row);
}

void CueTable::syncSelectionFromModel()
{
    const juce::ScopedValueSetter<bool> guard (syncingSelection, true);
    juce::SparseSet<int> rows;

    for (int i : cues.getSelectedIndices())
        if (const int row = rowOf (i); row >= 0)
            rows.addRange ({ row, row + 1 });

    table.setSelectedRows (rows, juce::dontSendNotification);

    if (const int row = rowOf (cues.getSelectedIndex()); row >= 0)
        table.scrollToEnsureRowIsOnscreen (row);

    table.repaint();
}

//==============================================================================
bool CueTable::handleQuickEditKey (const juce::KeyPress& key)
{
    const int row = cues.getSelectedIndex();

    if (! editable || ! cues.isValidIndex (row) || key.getModifiers().isAnyModifierKeyDown())
        return false;

    if (key.isKeyCode (juce::KeyPress::leftKey) || key.isKeyCode (juce::KeyPress::rightKey))
    {
        // Right opens a group, Left closes it (Left on a child goes to its group)
        const bool open = key.isKeyCode (juce::KeyPress::rightKey);
        const auto& cue = cues.get (row);

        if (cue.isGroup() && cue.group.collapsed == open && onToggleCollapse)
            onToggleCollapse (row, ! open);
        else if (! open && ! cue.isGroup() && cues.parentIndexOf (row) >= 0)
            cues.setSelectedIndex (cues.parentIndexOf (row));
        else if (! open && cue.isGroup() && cues.parentIndexOf (row) >= 0)
            cues.setSelectedIndex (cues.parentIndexOf (row));

        return true;
    }

    if (key.isKeyCode ('N')) { beginCellEdit (row, colNumber); return true; }
    if (key.isKeyCode ('Q')) { beginCellEdit (row, colName); return true; }
    if (key.isKeyCode ('E')) { beginCellEdit (row, colPreWait); return true; }
    if (key.isKeyCode ('W')) { beginCellEdit (row, colPostWait); return true; }
    if (key.isKeyCode ('C')) { cycleContinueMode (row); return true; }
    if (key.isKeyCode ('O')) { if (onEditNotes) onEditNotes (row); return true; }
    if (key.isKeyCode ('D')) { if (onEditDuration) onEditDuration (row); return true; }

    return false;
}

std::vector<int> CueTable::rowsForEdit (int row) const
{
    if (cues.isSelected (row))
        return cues.getSelectedIndices();

    return { row };
}

void CueTable::cycleContinueMode (int row)
{
    if (! cues.isValidIndex (row) || ! onEditCues)
        return;

    const auto current = cues.get (row).continueMode;
    const auto next = current == ContinueMode::none ? ContinueMode::autoContinue
                    : current == ContinueMode::autoContinue ? ContinueMode::autoFollow
                    : ContinueMode::none;

    onEditCues (rowsForEdit (row), ko ("진행 모드"), [next] (Cue& c) { c.continueMode = next; });
}

void CueTable::beginCellEdit (int row, ColumnId column)
{
    if (! editable || ! cues.isValidIndex (row))
        return;

    cellEditor.reset();
    const auto& cue = cues.get (row);
    juce::String initial;

    switch (column)
    {
        case colNumber:  initial = cue.number; break;
        case colName:    initial = cue.name; break;
        case colPreWait: initial = formatTimeMs (cue.preWaitSeconds); break;
        case colPostWait: initial = formatTimeMs (cue.postWaitSeconds); break;
        default: return;
    }

    const int visibleRow = rowOf (row);

    if (visibleRow < 0)
        return;   // inside a collapsed group

    table.scrollToEnsureRowIsOnscreen (visibleRow);
    table.scrollToEnsureColumnIsOnscreen (column);
    table.getHorizontalScrollBar().handleUpdateNowIfNeeded();   // apply the new viewport/header offset before reading the cell
    auto cell = table.getCellPosition (column, visibleRow, true);

    if (cell.isEmpty())
        return;

    cellEditor = std::make_unique<CellEditor> (*this, row, column, initial);
    addAndMakeVisible (*cellEditor);
    cellEditor->setBounds (cell.translated (table.getX(), table.getY()));
    cellEditor->grabKeyboardFocus();
}

void CueTable::commitCellEdit (int row, ColumnId column, const juce::String& text)
{
    if (! cues.isValidIndex (row) || ! onEditCues)
        return;

    const auto& cue = cues.get (row);

    switch (column)
    {
        case colNumber:
        {
            const auto number = text.trim();

            if (number == cue.number)
                break;

            if (isNumberTaken ? isNumberTaken (number, cue.id) : cues.isNumberTaken (number, cue.id))
            {
                juce::LookAndFeel::getDefaultLookAndFeel().playAlertSound();   // numbers are unique: refused
                break;
            }

            if (onSetNumber)
                onSetNumber (cue.id, number);   // the document renumbers and moves the row into numeric order
            else
                onEditCues ({ row }, ko ("번호"), [number] (Cue& c) { c.number = number; });

            break;
        }

        case colName:
        {
            const auto name = text.trim();

            if (name != cue.name)
                onEditCues ({ row }, ko ("이름 변경"), [name] (Cue& c) { c.name = name; });
            break;
        }

        case colPreWait:
        case colPostWait:
        {
            const double seconds = parseTimeText (text);

            if (seconds < 0.0)
                return;

            const bool pre = column == colPreWait;

            if (juce::approximatelyEqual (seconds, pre ? cue.preWaitSeconds : cue.postWaitSeconds))
                return;

            onEditCues (rowsForEdit (row), pre ? ko ("프리웨이트") : ko ("포스트웨이트"), [seconds, pre] (Cue& c)
            {
                if (pre)
                    c.preWaitSeconds = seconds;
                else
                    c.postWaitSeconds = seconds;
            });
            break;
        }

        default:
            break;
    }
}

void CueTable::showContextMenu (int row, juce::Point<int> screenPosition)
{
    if (! cues.isValidIndex (row))
        return;

    const auto rows = rowsForEdit (row);
    const auto& cue = cues.get (row);
    juce::PopupMenu menu;
    addCueItems (menu);   // the same items as the 큐 menu (they insert after the selection)
    menu.addSeparator();

    juce::PopupMenu colours;
    colours.addItem (100, ko ("없음"), true, cue.color == 0);

    for (int i = 1; i <= CueColors::numColors; ++i)
        colours.addItem (100 + i, juce::String::fromUTF8 (CueColors::name (i)), true, cue.color == i);

    menu.addSubMenu (ko ("색상"), colours, editable);
    menu.addItem (1, cue.flagged ? ko ("깃발 해제") : ko ("깃발"), editable);
    menu.addItem (2, cue.armed ? ko ("비활성화") : ko ("활성화"), editable);

    juce::PopupMenu continueMenu;
    continueMenu.addItem (10, ko ("계속 안 함"), true, cue.continueMode == ContinueMode::none);
    continueMenu.addItem (11, ko ("자동 계속 (포스트웨이트 뒤 다음 큐)"), true, cue.continueMode == ContinueMode::autoContinue);
    continueMenu.addItem (12, ko ("자동 팔로우 (끝나면 다음 큐)"), true, cue.continueMode == ContinueMode::autoFollow);
    menu.addSubMenu (ko ("진행 모드"), continueMenu, editable);
    menu.addSeparator();
    menu.addItem (20, ko ("플레이헤드를 여기로"));
    menu.addCommandItem (&commands, CommandIDs::preview);
    menu.addCommandItem (&commands, CommandIDs::loadCue);
    menu.addSeparator();
    menu.addCommandItem (&commands, CommandIDs::duplicateCue);
    menu.addCommandItem (&commands, CommandIDs::removeCue);

    juce::Component::SafePointer<CueTable> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ screenPosition.x, screenPosition.y, 1, 1 }),
                        [safeThis, rows, row] (int result)
    {
        if (safeThis == nullptr || result == 0 || ! safeThis->cues.isValidIndex (row))
            return;

        auto& self = *safeThis;

        if (! self.onEditCues)
            return;

        if (result >= 100 && result <= 100 + CueColors::numColors)
        {
            const int colour = result - 100;
            self.onEditCues (rows, ko ("색상"), [colour] (Cue& c) { c.color = colour; });
        }
        else if (result == 1)
        {
            const bool flagged = ! self.cues.get (row).flagged;
            self.onEditCues (rows, flagged ? ko ("깃발") : ko ("깃발 해제"), [flagged] (Cue& c) { c.flagged = flagged; });
        }
        else if (result == 2)
        {
            const bool armed = ! self.cues.get (row).armed;
            self.onEditCues (rows, armed ? ko ("활성화") : ko ("비활성화"), [armed] (Cue& c) { c.armed = armed; if (! armed) c.skipIfDisarmed = true; });   // older readers skip it too
        }
        else if (result >= 10 && result <= 12)
        {
            const auto mode = (ContinueMode) (result - 10);
            self.onEditCues (rows, ko ("진행 모드"), [mode] (Cue& c) { c.continueMode = mode; });
        }
        else if (result == 20)
        {
            self.cues.setPlayheadIndex (row);
        }
    });
}

//==============================================================================
bool CueTable::isInterestedInFileDrag (const juce::StringArray& files)
{
    return editable && containsAudioOrFolder (formats, files);
}

void CueTable::fileDragEnter (const juce::StringArray& files, int, int)
{
    dragOver = true;
    dropGroupIndex = -1;
    repaint();

    if (onStatus)
        onStatus (ko ("파일 드래그 감지: ") + juce::String (files.size()) + ko ("개 - 놓으면 큐로 추가"));
}

void CueTable::fileDragMove (const juce::StringArray&, int, int y)
{
    const int group = groupAtY (y);

    if (group != dropGroupIndex)
    {
        dropGroupIndex = group;
        repaint();
    }
}

void CueTable::fileDragExit (const juce::StringArray&)
{
    dragOver = false;
    dropGroupIndex = -1;
    repaint();
}

int CueTable::groupAtY (int y) const
{
    const auto local = table.getLocalPoint (this, juce::Point<int> (0, y));
    const int row = table.getRowContainingPosition (0, local.y);

    if (row < 0 || row >= (int) visible.size())
        return -1;

    const int index = visible[(size_t) row];

    if (! cues.get (index).isGroup())
        return -1;

    // the middle half of the row means "into the group"; the edges keep the between-rows insertion
    const auto rowArea = table.getRowPosition (row, true);
    const int inset = rowArea.getHeight() / 4;
    return local.y >= rowArea.getY() + inset && local.y < rowArea.getBottom() - inset ? index : -1;
}

int CueTable::insertionRowForY (int y) const
{
    const auto local = table.getLocalPoint (this, juce::Point<int> (0, y));
    int row = table.getInsertionIndexForPosition (0, local.y);

    if (row < 0 || row > (int) visible.size())
        row = (int) visible.size();

    return row;
}

int CueTable::insertionIndexForY (int y) const
{
    // between two visible rows: in front of the lower one (model index), or at the end
    const int row = insertionRowForY (y);
    return row < (int) visible.size() ? visible[(size_t) row] : cues.size();
}

void CueTable::filesDropped (const juce::StringArray& files, int, int y)
{
    dragOver = false;
    const int intoGroup = groupAtY (y);   // the drop position itself, not the last hover
    dropGroupIndex = -1;
    repaint();

    const auto audioFiles = collectAudioFiles (formats, files);

    if (onStatus)
        onStatus (ko ("드롭: 오디오 ") + juce::String (audioFiles.size()) + " / " + juce::String (files.size()) + ko ("개"));

    if (audioFiles.isEmpty() || ! onFilesDropped)
        return;

    onFilesDropped (audioFiles, insertionIndexForY (y), intoGroup);
}

bool CueTable::isInterestedInDragSource (const SourceDetails& details)
{
    return editable && details.description == rowDragDescription;
}

std::vector<int> CueTable::draggedIndices() const
{
    std::vector<int> rows;

    for (const auto& id : draggedIds)
        if (const int index = cues.indexOf (id); index >= 0)
            rows.push_back (index);

    return rows;
}

int CueTable::rowDragGroupAtY (int y) const
{
    const int group = groupAtY (y);

    if (group < 0)
        return -1;

    for (int s : draggedIndices())
        if (s == group || cues.isDescendantOf (group, cues.get (s).id))
            return -1;

    return group;
}

void CueTable::updateRowDragTarget (int y)
{
    // over a group's middle band the rows go into the group (the row lights up); elsewhere between rows (a line)
    const int group = rowDragGroupAtY (y);
    const int index = group >= 0 ? -1 : insertionRowForY (y);

    if (group != dropGroupIndex || index != rowDropIndex)
    {
        dropGroupIndex = group;
        rowDropIndex = index;
        repaint();
    }
}

void CueTable::itemDragEnter (const SourceDetails& details)
{
    rowDragOver = true;
    updateRowDragTarget (details.localPosition.y);
}

void CueTable::itemDragMove (const SourceDetails& details)
{
    updateRowDragTarget (details.localPosition.y);
}

void CueTable::itemDragExit (const SourceDetails&)
{
    rowDragOver = false;
    rowDropIndex = -1;
    dropGroupIndex = -1;
    repaint();
}

void CueTable::itemDropped (const SourceDetails& details)
{
    rowDragOver = false;
    const int group = rowDragGroupAtY (details.localPosition.y);   // the drop position itself, not the last hover
    const int index = insertionIndexForY (details.localPosition.y);
    const auto rows = draggedIndices();
    rowDropIndex = -1;
    dropGroupIndex = -1;
    draggedIds.clear();
    repaint();

    if (rows.empty())
        return;   // the dragged cues are gone (the list changed under the drag)

    if (group >= 0)
    {
        if (onStatus)
            onStatus (ko ("행 이동: 그룹 '") + cues.get (group).name + ko ("' 안으로"));

        if (onMoveRowsInto)
            onMoveRowsInto (rows, group);

        return;
    }

    if (onStatus)
        onStatus (ko ("행 이동: ") + juce::String (index) + ko ("번째 자리로"));

    if (onMoveRows)
        onMoveRows (rows, index);
}

} // namespace gocue
