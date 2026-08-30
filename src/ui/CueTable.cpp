#include "ui/CueTable.h"

#include "app/Commands.h"
#include "audio/CueFileInfo.h"
#include "ui/UiUtils.h"

namespace gocue
{

CueTable::CueTable (CueList& c, juce::AudioFormatManager& f, juce::ApplicationCommandManager& cm)
    : cues (c), formats (f), commands (cm)
{
    const int columnFlags = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable;

    auto& header = table.getHeader();
    header.addColumn ("#",                 colNumber,   50,  40,  70,  columnFlags);
    header.addColumn (ko ("이름"),         colName,     260, 80,  -1,  columnFlags);
    header.addColumn (ko ("파일"),         colFile,     300, 80,  -1,  columnFlags);
    header.addColumn (ko ("페이드인"),     colFadeIn,   90,  60,  140, columnFlags);
    header.addColumn (ko ("페이드아웃"),   colFadeOut,  90,  60,  140, columnFlags);
    header.addColumn (ko ("길이"),         colDuration, 90,  60,  140, columnFlags);
    header.setStretchToFitActive (true);

    table.setModel (this);
    table.setRowHeight (30);
    table.setMultipleSelectionEnabled (false);
    table.setClickingTogglesRowSelection (false);
    table.setColour (juce::ListBox::backgroundColourId, Palette::background);
    table.setColour (juce::ListBox::outlineColourId, Palette::outline);
    table.setOutlineThickness (1);
    addAndMakeVisible (table);

    cues.addListener (this);
    syncSelectionFromModel();
}

CueTable::~CueTable()
{
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

void CueTable::focusTable()
{
    table.grabKeyboardFocus();
}

void CueTable::resized()
{
    table.setBounds (getLocalBounds());
}

void CueTable::paint (juce::Graphics& g)
{
    if (dragOver)
    {
        g.setColour (Palette::standby.withAlpha (0.25f));
        g.fillAll();
    }
}

//==============================================================================
int CueTable::getNumRows()
{
    return cues.size();
}

const AudioEngine::PlayingCue* CueTable::findPlaying (const juce::Uuid& id) const
{
    for (const auto& p : playing)
        if (p.id == id)
            return &p;

    return nullptr;
}

void CueTable::paintRowBackground (juce::Graphics& g, int rowNumber, int width, int height, bool rowIsSelected)
{
    if (! cues.isValidIndex (rowNumber))
        return;

    const auto& cue = cues.get (rowNumber);
    const auto* running = findPlaying (cue.id);

    juce::Colour background = (rowNumber % 2 == 0) ? Palette::rowEven : Palette::rowOdd;

    if (running != nullptr)
        background = running->fadingOut ? Palette::fadingOut : Palette::playing;

    g.fillAll (background);

    if (running != nullptr && running->lengthSeconds > 0.0)
    {
        const double fraction = juce::jlimit (0.0, 1.0, running->positionSeconds / running->lengthSeconds);
        g.setColour (juce::Colours::white.withAlpha (0.22f));
        g.fillRect (0, 0, juce::roundToInt (width * fraction), height);
    }

    if (rowIsSelected)
    {
        g.setColour (Palette::standby.withAlpha (running != nullptr ? 0.15f : 0.28f));
        g.fillRect (0, 0, width, height);
        g.setColour (Palette::standby);
        g.drawRect (0, 0, width, height, 2);
    }
}

void CueTable::paintCell (juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool)
{
    if (! cues.isValidIndex (rowNumber))
        return;

    const auto& cue = cues.get (rowNumber);
    juce::String text;
    auto colour = Palette::text;
    auto justification = juce::Justification::centredLeft;

    switch (columnId)
    {
        case colNumber:
            text = juce::String (rowNumber + 1);
            justification = juce::Justification::centred;
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

        case colFadeIn:
            text = juce::String (cue.fadeInMs) + " ms";
            justification = juce::Justification::centredRight;
            break;

        case colFadeOut:
            text = juce::String (cue.fadeOutMs) + " ms";
            justification = juce::Justification::centredRight;
            break;

        case colDuration:
            text = formatSeconds (cue.durationSeconds);
            justification = juce::Justification::centredRight;
            break;

        default:
            break;
    }

    g.setColour (colour);
    g.setFont (juce::Font (juce::FontOptions (14.0f, columnId == colName ? juce::Font::bold : juce::Font::plain)));
    g.drawText (text, 8, 0, width - 16, height, justification, true);
}

void CueTable::selectedRowsChanged (int lastRowSelected)
{
    if (syncingSelection)
        return;

    cues.setSelectedIndex (lastRowSelected);
}

void CueTable::deleteKeyPressed (int)
{
    commands.invokeDirectly (CommandIDs::removeCue, true);
}

void CueTable::backgroundClicked (const juce::MouseEvent&)
{
    // Keep the standby selection: clicking empty space must not clear the next cue.
    syncSelectionFromModel();
}

//==============================================================================
bool CueTable::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
        if (isSupportedAudioFile (formats, juce::File (path)))
            return true;

    return false;
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

void CueTable::filesDropped (const juce::StringArray& files, int x, int y)
{
    dragOver = false;
    repaint();

    juce::StringArray audioFiles;

    for (const auto& path : files)
        if (isSupportedAudioFile (formats, juce::File (path)))
            audioFiles.add (path);

    if (audioFiles.isEmpty() || ! onFilesDropped)
        return;

    const auto local = table.getLocalPoint (this, juce::Point<int> (x, y));
    int insertIndex = table.getInsertionIndexForPosition (local.x, local.y);

    if (insertIndex < 0 || insertIndex > cues.size())
        insertIndex = cues.size();

    onFilesDropped (audioFiles, insertIndex);
}

//==============================================================================
void CueTable::cueListStructureChanged()
{
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

void CueTable::syncSelectionFromModel()
{
    const juce::ScopedValueSetter<bool> guard (syncingSelection, true);
    const int index = cues.getSelectedIndex();

    if (index >= 0)
    {
        if (table.getSelectedRow() != index)
            table.selectRow (index);
    }
    else
    {
        table.deselectAllRows();
    }
}

} // namespace gocue
