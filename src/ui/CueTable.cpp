#include "ui/CueTable.h"

#include "app/Commands.h"
#include "audio/CueFileInfo.h"
#include "model/CueColors.h"
#include "ui/UiUtils.h"

namespace gocue
{

namespace
{
    const juce::String rowDragDescription ("gocue-rows");
    constexpr int rowHeights[] = { 28, 34, 42 };
    constexpr int indentPerLevel = 18;
    constexpr int disclosureWidth = 16;
}

juce::Colour CueTable::groupModeColour (GroupMode mode)
{
    switch (mode)
    {
        case GroupMode::timeline:        return juce::Colour (0xff3fa860);
        case GroupMode::playlist:        return juce::Colour (0xffd8902c);
        case GroupMode::startFirstEnter:
        case GroupMode::startFirst:      return juce::Colour (0xff4a86d8);
        case GroupMode::random:          return juce::Colour (0xffa462d8);
    }

    return juce::Colours::grey;
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
        setFont (juce::Font (juce::FontOptions (16.0f)));
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
            safeOwner->commands.invokeDirectly (CommandIDs::panicAll, true);   // Esc is the panic key, editing or not
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
    header.addColumn ("",                  colStatus,   44,  36,  60,  juce::TableHeaderComponent::visible);
    header.addColumn (ko ("번호"),         colNumber,   64,  40,  120, columnFlags);
    header.addColumn (ko ("이름"),         colName,     260, 80,  -1,  columnFlags);
    header.addColumn (ko ("파일"),         colFile,     240, 80,  -1,  columnFlags);
    header.addColumn (ko ("프리웨이트"),   colPreWait,  84,  60,  120, columnFlags);
    header.addColumn (ko ("길이"),         colDuration, 90,  60,  140, columnFlags);
    header.addColumn (ko ("포스트웨이트"), colPostWait, 92,  60,  120, columnFlags);
    header.addColumn (ko ("진행"),         colContinue, 48,  40,  60,  juce::TableHeaderComponent::visible);
    header.setStretchToFitActive (true);

    table.setModel (this);
    table.setRowHeight (rowHeights[1]);
    table.setMultipleSelectionEnabled (true);
    table.setClickingTogglesRowSelection (false);
    table.setColour (juce::ListBox::backgroundColourId, Palette::background);
    table.setColour (juce::ListBox::outlineColourId, Palette::outline);
    table.setOutlineThickness (1);
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
    table.setRowHeight (rowHeights[juce::jlimit (0, 2, size)]);
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
    const bool isRunning = running != nullptr && ! running->loaded;
    const bool groupRunning = isGroupRunning (index);

    juce::Colour background = (rowNumber % 2 == 0) ? Palette::rowEven : Palette::rowOdd;

    const int colourIndex = cue.useSecondColor && cue.secondColor > 0 && hasPlayed && hasPlayed (cue.id) ? cue.secondColor : cue.color;

    if (colourIndex > 0)
        background = background.interpolatedWith (CueColors::get (colourIndex), 0.35f);

    if (isRunning)
        background = running->paused ? Palette::pausedRow : (running->fadingOut ? Palette::fadingRow : Palette::playingRow);
    else if (groupRunning)
        background = background.interpolatedWith (Palette::playingRow, 0.6f);

    g.fillAll (background);

    // group rows and their children carry the group's mode colour down the left edge
    {
        const int depth = cues.depthOf (index);

        if (cue.isGroup())
        {
            g.setColour (groupModeColour (cue.group.mode));
            g.fillRect (depth * indentPerLevel, 0, 4, height);
        }

        for (int level = 0, p = cues.parentIndexOf (index); p >= 0 && level < 32; p = cues.parentIndexOf (p), ++level)
        {
            g.setColour (groupModeColour (cues.get (p).group.mode).withAlpha (0.55f));
            g.fillRect (cues.depthOf (p) * indentPerLevel, 0, 4, height);
        }
    }

    if (isRunning && running->progress >= 0.0)
    {
        const double fraction = juce::jlimit (0.0, 1.0, running->progress);
        g.setColour (juce::Colours::white.withAlpha (0.22f));
        g.fillRect (0, 0, juce::roundToInt (width * fraction), height);
    }

    if (! cue.armed)
    {
        g.setColour (juce::Colours::black.withAlpha (0.28f));

        for (int x = -height; x < width; x += 10)
            g.drawLine ((float) x, (float) height, (float) (x + height), 0.0f, 2.0f);
    }

    if (cues.isSelected (index))
    {
        g.setColour (Palette::standby.withAlpha (isRunning ? 0.15f : 0.28f));
        g.fillRect (0, 0, width, height);
    }

    if (index == cues.getPlayheadIndex())
    {
        g.setColour (Palette::standby);
        g.drawRect (0, 0, width, height, 2);
    }
}

void CueTable::paintCell (juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool)
{
    const int index = modelIndex (rowNumber);

    if (! cues.isValidIndex (index))
        return;

    const auto& cue = cues.get (index);
    const auto* running = findPlaying (cue.id);
    const bool isRunning = running != nullptr && ! running->loaded;

    if (columnId == colStatus)
    {
        const float cy = height * 0.5f;
        float x = 6.0f;

        if (index == cues.getPlayheadIndex())
        {
            juce::Path playhead;
            playhead.addTriangle (x, cy - 6.0f, x, cy + 6.0f, x + 9.0f, cy);
            g.setColour (Palette::standby.brighter (0.5f));
            g.fillPath (playhead);
        }

        x += 14.0f;

        if (isRunning)
        {
            if (running->paused)
            {
                g.setColour (juce::Colours::yellow.withAlpha (0.9f));
                g.fillRect (x, cy - 6.0f, 3.0f, 12.0f);
                g.fillRect (x + 5.0f, cy - 6.0f, 3.0f, 12.0f);
            }
            else
            {
                juce::Path tri;
                tri.addTriangle (x, cy - 6.0f, x, cy + 6.0f, x + 10.0f, cy);
                g.setColour (running->fadingOut ? Palette::fadingOut : Palette::playing);
                g.fillPath (tri);
            }
        }
        else if (running != nullptr && running->loaded)
        {
            g.setColour (juce::Colours::yellow.withAlpha (0.85f));
            g.fillEllipse (x, cy - 5.0f, 10.0f, 10.0f);
        }
        else if (cue.isFade())
        {
            // a fade: a slope; red when the target is missing
            const bool broken = cue.fade.targetId.isNull() || cues.indexOf (cue.fade.targetId) < 0;
            g.setColour (broken ? Palette::missing : Palette::dimText);
            juce::Path slope;
            slope.startNewSubPath (x, cy + 5.0f);
            slope.lineTo (x + 10.0f, cy - 5.0f);
            slope.lineTo (x + 10.0f, cy + 5.0f);
            slope.closeSubPath();
            g.fillPath (slope);
        }
        else if (cue.isDevamp())
        {
            // a devamp: a loop arc with a bar (the loop point)
            const bool broken = cue.devamp.targetId.isNull() || cues.indexOf (cue.devamp.targetId) < 0;
            g.setColour (broken ? Palette::missing : Palette::dimText);
            juce::Path arc;
            arc.addCentredArc (x + 5.0f, cy, 4.5f, 4.5f, 0.0f, 0.4f, 5.9f, true);
            g.strokePath (arc, juce::PathStrokeType (1.8f));
            g.fillRect (x + 9.0f, cy - 6.0f, 2.0f, 12.0f);
        }
        else if (cue.isControl())
        {
            // control cues: one small glyph per kind; red when a needed target is missing
            const bool broken = cue.control.needsTarget() && (cue.control.targetId.isNull() || cues.indexOf (cue.control.targetId) < 0);
            g.setColour (broken ? Palette::missing : Palette::dimText);
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
            g.setColour (Palette::dimText);
            g.fillRoundedRectangle (x + 2.5f, cy - 7.0f, 5.0f, 9.0f, 2.5f);
            juce::Path stand;
            stand.addCentredArc (x + 5.0f, cy, 4.5f, 4.5f, 0.0f, 1.6f, 4.7f, true);
            g.strokePath (stand, juce::PathStrokeType (1.4f));
            g.drawLine (x + 5.0f, cy + 4.5f, x + 5.0f, cy + 7.0f, 1.4f);
            g.drawLine (x + 2.0f, cy + 7.0f, x + 8.0f, cy + 7.0f, 1.4f);
        }
        else if (cue.isGroup())
        {
            // a group: a folder tab in the mode colour (running children = filled)
            g.setColour (groupModeColour (cue.group.mode));
            juce::Path folder;
            folder.addRoundedRectangle (x, cy - 4.0f, 11.0f, 9.0f, 1.5f);
            folder.addRectangle (x, cy - 6.0f, 5.0f, 3.0f);

            if (isGroupRunning (index))
                g.fillPath (folder);
            else
                g.strokePath (folder, juce::PathStrokeType (1.4f));
        }
        else if (cue.fileMissing || cue.file == juce::File())
        {
            g.setColour (Palette::missing);
            g.drawLine (x, cy - 5.0f, x + 10.0f, cy + 5.0f, 2.0f);
            g.drawLine (x, cy + 5.0f, x + 10.0f, cy - 5.0f, 2.0f);
        }

        if (cue.flagged)
        {
            x += 14.0f;
            g.setColour (juce::Colours::yellow);
            g.drawLine (x, cy - 7.0f, x, cy + 7.0f, 1.5f);
            juce::Path flag;
            flag.addTriangle (x, cy - 7.0f, x + 8.0f, cy - 4.0f, x, cy - 1.0f);
            g.fillPath (flag);
        }

        return;
    }

    if (columnId == colContinue)
    {
        if (cue.continueMode == ContinueMode::none)
            return;

        const float cy = height * 0.5f;
        const float x0 = 8.0f, x1 = (float) width - 10.0f;
        g.setColour (Palette::text);
        g.drawLine (x0, cy, x1, cy, 2.0f);
        juce::Path head;
        head.addTriangle (x1, cy - 5.0f, x1, cy + 5.0f, x1 + 6.0f, cy);
        g.fillPath (head);

        if (cue.continueMode == ContinueMode::autoFollow)   // a bar: "when the cue has finished"
            g.fillRect (x0 - 3.0f, cy - 6.0f, 2.5f, 12.0f);

        return;
    }

    juce::String text;
    auto colour = Palette::text;
    auto justification = juce::Justification::centredLeft;
    int indent = 0;

    switch (columnId)
    {
        case colNumber:
            text = cue.number;
            break;   // the number reads as strongly as the name

        case colName:
            text = cue.name.isNotEmpty() ? cue.name : ko ("(이름 없음)");
            indent = cues.depthOf (index) * indentPerLevel;

            if (cue.isGroup())
            {
                // disclosure triangle: right = collapsed, down = expanded
                const float cy = height * 0.5f;
                const float tx = 8.0f + (float) indent;
                juce::Path tri;

                if (cue.group.collapsed)
                    tri.addTriangle (tx, cy - 5.0f, tx, cy + 5.0f, tx + 7.0f, cy);
                else
                    tri.addTriangle (tx, cy - 3.0f, tx + 9.0f, cy - 3.0f, tx + 4.5f, cy + 4.0f);

                g.setColour (Palette::text);
                g.fillPath (tri);
                indent += disclosureWidth;
            }
            break;

        case colFile:
            if (cue.isGroup())
            {
                const int count = (int) cues.childrenOf (index).size();
                const char* mode = cue.group.mode == GroupMode::timeline ? "타임라인"
                                 : cue.group.mode == GroupMode::playlist ? "플레이리스트"
                                 : cue.group.mode == GroupMode::startFirstEnter ? "첫 큐 시작 후 진입"
                                 : cue.group.mode == GroupMode::startFirst ? "첫 큐 시작" : "랜덤";
                text = ko (mode) + ko (" · ") + juce::String (count) + ko ("개");
                colour = Palette::dimText;
            }
            else if (cue.isMic())
            {
                text = ko ("입력 ") + juce::String (cue.mic.firstInput + 1) + (cue.mic.numInputs > 1 ? "-" + juce::String (cue.mic.firstInput + cue.mic.numInputs) : juce::String());
                colour = Palette::dimText;
            }
            else if (cue.isControl() && ! cue.control.needsTarget())
            {
                text = cue.control.kind == ControlKind::wait ? ko ("대기 ") + formatTimeMs (cue.control.seconds) : ko ("메모");
                colour = Palette::dimText;
            }
            else if (cue.isFade() || cue.isDevamp() || cue.isControl())
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
                    text = juce::String::fromUTF8 ("\xE2\x86\x92 ") + (t.number.isNotEmpty() ? t.number + " " : juce::String()) + t.name;   // → target
                    colour = Palette::dimText;
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
            justification = juce::Justification::centred;   // centred like its header
            break;

        case colPostWait:
            text = cue.postWaitSeconds > 0.0 ? formatTimeMs (cue.postWaitSeconds) : juce::String();
            colour = cue.continueMode == ContinueMode::autoContinue ? Palette::text : Palette::dimText;
            justification = juce::Justification::centred;   // centred like its header
            break;

        case colDuration:
            if (isRunning && running->lengthSeconds != 0.0)
            {
                if (running->lengthSeconds < 0.0)
                    text = juce::String::fromUTF8 ("\xE2\x88\x9E ") + formatSeconds (running->positionSeconds);
                else
                    text = "-" + formatSeconds (juce::jmax (0.0, running->remainingSeconds));

                colour = juce::Colours::white;
            }
            else
            {
                const double effective = cues.effectiveLengthOf (index);
                text = effective < 0.0 ? juce::String::fromUTF8 ("\xE2\x88\x9E") : formatSeconds (effective > 0.0 ? effective : cue.durationSeconds);
            }

            justification = juce::Justification::centred;   // centred like its header
            break;

        default:
            break;
    }

    g.setColour (colour);
    const bool strong = columnId == colName || columnId == colNumber;
    g.setFont (juce::Font (juce::FontOptions (strong ? 16.0f : 15.0f, strong ? juce::Font::bold : juce::Font::plain)));
    g.drawText (text, 8 + indent, 0, width - 16 - indent, height, justification, true);
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
             && e.x < 8 + cues.depthOf (index) * indentPerLevel + disclosureWidth)
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

void CueTable::backgroundClicked (const juce::MouseEvent&)
{
    syncSelectionFromModel();
}

juce::var CueTable::getDragSourceDescription (const juce::SparseSet<int>& rowsToDescribe)
{
    if (! editable || rowsToDescribe.isEmpty())
        return {};

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

    juce::PopupMenu colours;
    colours.addItem (100, ko ("없음"), true, cue.color == 0);

    for (int i = 1; i <= CueColors::numColors; ++i)
        colours.addItem (100 + i, juce::String::fromUTF8 (CueColors::name (i)), true, cue.color == i);

    menu.addSubMenu (ko ("색상"), colours, editable);
    menu.addItem (1, cue.flagged ? ko ("깃발 해제") : ko ("깃발"), editable);
    menu.addItem (2, cue.armed ? ko ("비활성화 (아밍 해제)") : ko ("활성화 (아밍)"), editable);

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
            self.onEditCues (rows, armed ? ko ("활성화") : ko ("비활성화"), [armed] (Cue& c) { c.armed = armed; });
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

void CueTable::fileDragEnter (const juce::StringArray&, int, int)
{
    dragOver = true;
    repaint();
}

void CueTable::fileDragExit (const juce::StringArray&)
{
    dragOver = false;
    repaint();
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
    repaint();

    const auto audioFiles = collectAudioFiles (formats, files);

    if (audioFiles.isEmpty() || ! onFilesDropped)
        return;

    onFilesDropped (audioFiles, insertionIndexForY (y));
}

bool CueTable::isInterestedInDragSource (const SourceDetails& details)
{
    return editable && details.description == rowDragDescription;
}

void CueTable::itemDragEnter (const SourceDetails& details)
{
    rowDragOver = true;
    rowDropIndex = insertionRowForY (details.localPosition.y);
    repaint();
}

void CueTable::itemDragMove (const SourceDetails& details)
{
    const int index = insertionRowForY (details.localPosition.y);

    if (index != rowDropIndex)
    {
        rowDropIndex = index;
        repaint();
    }
}

void CueTable::itemDragExit (const SourceDetails&)
{
    rowDragOver = false;
    rowDropIndex = -1;
    repaint();
}

void CueTable::itemDropped (const SourceDetails& details)
{
    rowDragOver = false;
    const int index = insertionIndexForY (details.localPosition.y);
    rowDropIndex = -1;
    repaint();

    if (onMoveRows)
        onMoveRows (cues.getSelectedIndices(), index);
}

} // namespace gocue
