#include "model/CueList.h"

#include <algorithm>

namespace gocue
{

int CueList::indexOf (const juce::Uuid& id) const noexcept
{
    for (size_t i = 0; i < cues.size(); ++i)
        if (cues[i].id == id)
            return (int) i;

    return -1;
}

const Cue* CueList::findById (const juce::Uuid& id) const noexcept
{
    const int index = indexOf (id);
    return index >= 0 ? &cues[(size_t) index] : nullptr;
}

//==============================================================================
int CueList::add (Cue cue, int insertAt)
{
    cue.sanitise();

    if (insertAt < 0 || insertAt > size())
        insertAt = size();

    cues.insert (cues.begin() + insertAt, std::move (cue));

    for (auto& s : selection)
        if (s >= insertAt)
            ++s;

    if (primary >= insertAt)
        ++primary;

    if (playhead >= insertAt)
        ++playhead;

    notifyStructureChanged();

    if (primary < 0)
        setSelectedIndex (insertAt);

    return insertAt;
}

void CueList::remove (int index)
{
    if (! isValidIndex (index))
        return;

    removeIndices ({ index });
}

void CueList::removeIndices (std::vector<int> indices)
{
    std::sort (indices.begin(), indices.end());
    indices.erase (std::unique (indices.begin(), indices.end()), indices.end());
    indices.erase (std::remove_if (indices.begin(), indices.end(), [this] (int i) { return ! isValidIndex (i); }), indices.end());

    if (indices.empty())
        return;

    const int oldPrimary = primary;
    const int oldPlayhead = playhead;

    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
    {
        const int index = *it;
        cues.erase (cues.begin() + index);

        std::vector<int> kept;

        for (int s : selection)
        {
            if (s < index)
                kept.push_back (s);
            else if (s > index)
                kept.push_back (s - 1);
        }

        selection = std::move (kept);

        if (primary > index)
            --primary;

        if (playhead > index)
            --playhead;
    }

    // a removed primary / playhead lands on the row that took its place (or the last row)
    if (primary >= size())
        primary = size() - 1;

    if (playhead >= size())
        playhead = size() - 1;

    if (primary >= 0 && ! isSelected (primary))
        selection.push_back (primary);

    std::sort (selection.begin(), selection.end());
    notifyStructureChanged();

    if (primary != oldPrimary || std::binary_search (indices.begin(), indices.end(), oldPrimary))
        notifySelectionChanged();

    if (playhead != oldPlayhead || std::binary_search (indices.begin(), indices.end(), oldPlayhead))
        notifyPlayheadChanged();
}

int CueList::duplicate (int index)
{
    if (! isValidIndex (index))
        return -1;

    return add (cues[(size_t) index].duplicated(), index + 1);
}

bool CueList::move (int from, int to)
{
    if (! isValidIndex (from) || ! isValidIndex (to) || from == to)
        return false;

    Cue moved = std::move (cues[(size_t) from]);
    cues.erase (cues.begin() + from);
    cues.insert (cues.begin() + to, std::move (moved));

    auto remap = [from, to] (int i)
    {
        if (i == from)
            return to;

        if (from < i && i <= to)
            return i - 1;

        if (to <= i && i < from)
            return i + 1;

        return i;
    };

    for (auto& s : selection)
        s = remap (s);

    std::sort (selection.begin(), selection.end());
    primary = remap (primary);
    playhead = remap (playhead);

    notifyStructureChanged();
    return true;
}

bool CueList::moveIndices (std::vector<int> indices, int to)
{
    std::sort (indices.begin(), indices.end());
    indices.erase (std::unique (indices.begin(), indices.end()), indices.end());
    indices.erase (std::remove_if (indices.begin(), indices.end(), [this] (int i) { return ! isValidIndex (i); }), indices.end());

    if (indices.empty())
        return false;

    const juce::Uuid primaryId = isValidIndex (primary) ? cues[(size_t) primary].id : juce::Uuid::null();
    const juce::Uuid playheadId = isValidIndex (playhead) ? cues[(size_t) playhead].id : juce::Uuid::null();
    std::vector<juce::Uuid> selectedIds;

    for (int s : selection)
        if (isValidIndex (s))
            selectedIds.push_back (cues[(size_t) s].id);

    std::vector<Cue> block;

    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
    {
        block.insert (block.begin(), std::move (cues[(size_t) *it]));
        cues.erase (cues.begin() + *it);
    }

    to = juce::jlimit (0, size(), to);
    cues.insert (cues.begin() + to, std::make_move_iterator (block.begin()), std::make_move_iterator (block.end()));

    selection.clear();

    for (const auto& id : selectedIds)
        selection.push_back (indexOf (id));

    std::sort (selection.begin(), selection.end());
    primary = indexOf (primaryId);
    playhead = indexOf (playheadId);

    notifyStructureChanged();
    return true;
}

void CueList::replaceAll (std::vector<Cue> newCues)
{
    cues = std::move (newCues);

    for (auto& c : cues)
        c.sanitise();

    selection.clear();
    primary = cues.empty() ? -1 : 0;
    playhead = primary;

    if (primary >= 0)
        selection.push_back (primary);

    notifyStructureChanged();
    notifySelectionChanged();
    notifyPlayheadChanged();
}

void CueList::clear()
{
    replaceAll ({});
}

void CueList::update (int index, const std::function<void (Cue&)>& mutator)
{
    if (! isValidIndex (index) || ! mutator)
        return;

    mutator (cues[(size_t) index]);
    cues[(size_t) index].sanitise();
    listeners.call ([index] (Listener& l) { l.cueChanged (index); });
}

void CueList::setPluginStatesQuietly (int index, std::vector<PluginSlotState> states)
{
    if (isValidIndex (index))
        cues[(size_t) index].plugins = std::move (states);
}

//==============================================================================
bool CueList::isSelected (int index) const noexcept
{
    return std::binary_search (selection.begin(), selection.end(), index);
}

void CueList::clampCursors()
{
    if (cues.empty())
    {
        selection.clear();
        primary = -1;
        playhead = -1;
        return;
    }

    selection.erase (std::remove_if (selection.begin(), selection.end(), [this] (int i) { return ! isValidIndex (i); }), selection.end());
    primary = isValidIndex (primary) ? primary : -1;
    playhead = isValidIndex (playhead) ? playhead : -1;
}

void CueList::setSelectionInternal (std::vector<int> indices, int primaryIndex, bool syncPlayhead)
{
    std::sort (indices.begin(), indices.end());
    indices.erase (std::unique (indices.begin(), indices.end()), indices.end());
    indices.erase (std::remove_if (indices.begin(), indices.end(), [this] (int i) { return ! isValidIndex (i); }), indices.end());

    if (indices.empty())
        primaryIndex = -1;
    else if (! isValidIndex (primaryIndex) || ! std::binary_search (indices.begin(), indices.end(), primaryIndex))
        primaryIndex = indices.back();

    const bool changed = indices != selection || primaryIndex != primary;
    selection = std::move (indices);
    primary = primaryIndex;

    if (changed)
        notifySelectionChanged();

    if (syncPlayhead && lockPlayhead && primary >= 0)
        setPlayheadInternal (primary, false);
}

void CueList::setSelectedIndex (int index)
{
    int newIndex = -1;

    if (! cues.empty() && index >= 0)
        newIndex = juce::jmin (index, size() - 1);

    if (newIndex < 0)
        setSelectionInternal ({}, -1, true);
    else
        setSelectionInternal ({ newIndex }, newIndex, true);
}

void CueList::setSelection (std::vector<int> indices, int primaryIndex)
{
    setSelectionInternal (std::move (indices), primaryIndex, true);
}

void CueList::selectAll()
{
    std::vector<int> all;

    for (int i = 0; i < size(); ++i)
        all.push_back (i);

    setSelectionInternal (std::move (all), primary, false);
}

void CueList::setPlayheadInternal (int index, bool syncSelection)
{
    int newIndex = -1;

    if (! cues.empty() && index >= 0)
        newIndex = juce::jmin (index, size() - 1);

    if (newIndex != playhead)
    {
        playhead = newIndex;
        notifyPlayheadChanged();
    }

    if (syncSelection && lockPlayhead && playhead >= 0)
        setSelectionInternal ({ playhead }, playhead, false);
}

void CueList::setPlayheadIndex (int index)
{
    setPlayheadInternal (index, true);
}

bool CueList::advancePlayhead()
{
    if (! isValidIndex (playhead) || playhead + 1 >= size())
        return false;

    setPlayheadInternal (playhead + 1, true);
    return true;
}

bool CueList::retreatPlayhead()
{
    if (! isValidIndex (playhead) || playhead <= 0)
        return false;

    setPlayheadInternal (playhead - 1, true);
    return true;
}

//==============================================================================
void CueList::notifyStructureChanged()
{
    clampCursors();
    listeners.call ([] (Listener& l) { l.cueListStructureChanged(); });
}

void CueList::notifySelectionChanged()
{
    const int index = primary;
    listeners.call ([index] (Listener& l) { l.cueSelectionChanged (index); });
}

void CueList::notifyPlayheadChanged()
{
    const int index = playhead;
    listeners.call ([index] (Listener& l) { l.playheadChanged (index); });
}

} // namespace gocue
