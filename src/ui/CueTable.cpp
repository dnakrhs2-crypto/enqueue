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
    constexpr int rowHeights[] = { 24, 30, 38 };
}

//==============================================================================
/** A text editor floating over one cell; commits on Return / focus loss, cancels on Escape. */
class CueTable::CellEditor : public juce::TextEditor
{
public:
    CellEditor (CueTable& o, int r, ColumnId c, const juce::String& initial)
        : owner (o), row (r), column (c)
    {
        setText (initial, false);
        setSelectAllWhenFocused (true);
        setJustification (column == colName || column == colNumber ? juce::Justification::centredLeft : juce::Justification::centredRight);
        setFont (juce::Font (juce::FontOptions (14.0f)));
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
        const int r = row;
        const ColumnId c = column;

        juce::MessageManager::callAsync ([safeOwner, r, c, text]
        {
            if (safeOwner != nullptr)
            {
                safeOwner->cellEditor.reset();
                safeOwner->commitCellEdit (r, c, text);
                safeOwner->focusTable();
            }
        });
    }

    void cancel()
    {
        if (done)
            return;

        done = true;
        juce::Component::SafePointer<CueTable> safeOwner (&owner);
        juce::MessageManager::callAsync ([safeOwner]
        {
            if (safeOwner != nullptr)
            {
                safeOwner->cellEditor.reset();
                safeOwner->focusTable();
            }
        });
    }

private:
    CueTable& owner;
    const int row;
    const ColumnId column;
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
    syncSelectionFromModel();
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
    return cues.size();
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
    if (! cues.isValidIndex (rowNumber))
        return;

    const auto& cue = cues.get (rowNumber);
    const auto* running = findPlaying (cue.id);
    const bool isRunning = running != nullptr && ! running->loaded;

    juce::Colour background = (rowNumber % 2 == 0) ? Palette::rowEven : Palette::rowOdd;

    if (cue.color > 0)
        background = background.interpolatedWith (CueColors::get (cue.color), 0.35f);

    if (isRunning)
        background = running->paused ? Palette::paused : (running->fadingOut ? Palette::fadingOut : Palette::playing);

    g.fillAll (background);

    if (isRunning && running->lengthSeconds > 0.0)
    {
        const double fraction = juce::jlimit (0.0, 1.0, running->positionSeconds / running->lengthSeconds);
        g.setColour (juce::Colours::white.withAlpha (0.22f));
        g.fillRect (0, 0, juce::roundToInt (width * fraction), height);
    }

    if (! cue.armed)
    {
        g.setColour (juce::Colours::black.withAlpha (0.28f));

        for (int x = -height; x < width; x += 10)
            g.drawLine ((float) x, (float) height, (float) (x + height), 0.0f, 2.0f);
    }

    if (cues.isSelected (rowNumber))
    {
        g.setColour (Palette::standby.withAlpha (isRunning ? 0.15f : 0.28f));
        g.fillRect (0, 0, width, height);
    }

    if (rowNumber == cues.getPlayheadIndex())
    {
        g.setColour (Palette::standby);
        g.drawRect (0, 0, width, height, 2);
    }
}

void CueTable::paintCell (juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool)
{
    if (! cues.isValidIndex (rowNumber))
        return;

    const auto& cue = cues.get (rowNumber);
    const auto* running = findPlaying (cue.id);
    const bool isRunning = running != nullptr && ! running->loaded;

    if (columnId == colStatus)
    {
        const float cy = height * 0.5f;
        float x = 6.0f;

        if (rowNumber == cues.getPlayheadIndex())
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
                g.setColour (running->fadingOut ? juce::Colours::orange : juce::Colours::lightgreen);
                g.fillPath (tri);
            }
        }
        else if (running != nullptr && running->loaded)
        {
            g.setColour (juce::Colours::yellow.withAlpha (0.85f));
            g.fillEllipse (x, cy - 5.0f, 10.0f, 10.0f);
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

    switch (columnId)
    {
        case colNumber:
            text = cue.number;
            colour = Palette::dimText;
            break;

        case colName:
            text = cue.name.isNotEmpty() ? cue.name : ko ("(이름 없음)");
            break;

        case colFile:
            if (cue.file == juce::File())
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
            justification = juce::Justification::centredRight;
            break;

        case colPostWait:
            text = cue.postWaitSeconds > 0.0 ? formatTimeMs (cue.postWaitSeconds) : juce::String();
            colour = cue.continueMode == ContinueMode::autoContinue ? Palette::text : Palette::dimText;
            justification = juce::Justification::centredRight;
            break;

        case colDuration:
            if (isRunning && running->lengthSeconds != 0.0)
            {
                if (running->lengthSeconds < 0.0)
                    text = juce::String::fromUTF8 ("\xE2\x88\x9E ") + formatSeconds (running->positionSeconds);
                else
                    text = "-" + formatSeconds (juce::jmax (0.0, running->lengthSeconds - running->positionSeconds));

                colour = juce::Colours::white;
            }
            else
            {
                const double effective = cue.effectiveLength();
                text = effective < 0.0 ? juce::String::fromUTF8 ("\xE2\x88\x9E") : formatSeconds (effective > 0.0 ? effective : cue.durationSeconds);
            }

            justification = juce::Justification::centredRight;
            break;

        default:
            break;
    }

    g.setColour (colour);
    g.setFont (juce::Font (juce::FontOptions (14.0f, columnId == colName ? juce::Font::bold : juce::Font::plain)));
    g.drawText (text, 8, 0, width - 16, height, justification, true);
}

void CueTable::cellClicked (int rowNumber, int columnId, const juce::MouseEvent& e)
{
    if (! cues.isValidIndex (rowNumber))
        return;

    if (e.mods.isPopupMenu())
    {
        if (! cues.isSelected (rowNumber))
            cues.setSelectedIndex (rowNumber);

        showContextMenu (rowNumber, e.getScreenPosition());
        return;
    }

    if (columnId == colStatus)
        cues.setPlayheadIndex (rowNumber);
    else if (columnId == colContinue && editable)
        cycleContinueMode (rowNumber);
}

void CueTable::cellDoubleClicked (int rowNumber, int columnId, const juce::MouseEvent&)
{
    if (! editable || ! cues.isValidIndex (rowNumber))
        return;

    switch (columnId)
    {
        case colNumber:
        case colName:
        case colPreWait:
        case colPostWait:
            beginCellEdit (rowNumber, (ColumnId) columnId);
            break;

        case colDuration:
            if (onEditDuration)
                onEditDuration (rowNumber);
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
        rows.push_back (selected[i]);

    if (rows.empty())
    {
        syncSelectionFromModel();   // keep the model's selection: clicking empty space must not clear the next cue
        return;
    }

    cues.setSelection (rows, lastRowSelected);
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
    table.updateContent();
    syncSelectionFromModel();
    table.repaint();
}

void CueTable::cueChanged (int index)
{
    table.repaintRow (index);
}

void CueTable::cueSelectionChanged (int)
{
    syncSelectionFromModel();
}

void CueTable::playheadChanged (int index)
{
    table.repaint();

    if (index >= 0)
        table.scrollToEnsureRowIsOnscreen (index);
}

void CueTable::syncSelectionFromModel()
{
    const juce::ScopedValueSetter<bool> guard (syncingSelection, true);
    juce::SparseSet<int> rows;

    for (int i : cues.getSelectedIndices())
        rows.addRange ({ i, i + 1 });

    table.setSelectedRows (rows, juce::dontSendNotification);

    if (const int primary = cues.getSelectedIndex(); primary >= 0)
        table.scrollToEnsureRowIsOnscreen (primary);

    table.repaint();
}

//==============================================================================
bool CueTable::handleQuickEditKey (const juce::KeyPress& key)
{
    const int row = cues.getSelectedIndex();

    if (! editable || ! cues.isValidIndex (row) || key.getModifiers().isAnyModifierKeyDown())
        return false;

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

    table.scrollToEnsureRowIsOnscreen (row);
    auto cell = table.getCellPosition (column, row, true);

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

            if (number != cue.number)
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

int CueTable::insertionIndexForY (int y) const
{
    const auto local = table.getLocalPoint (this, juce::Point<int> (0, y));
    int insertIndex = table.getInsertionIndexForPosition (0, local.y);

    if (insertIndex < 0 || insertIndex > cues.size())
        insertIndex = cues.size();

    return insertIndex;
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
    rowDropIndex = insertionIndexForY (details.localPosition.y);
    repaint();
}

void CueTable::itemDragMove (const SourceDetails& details)
{
    const int index = insertionIndexForY (details.localPosition.y);

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
