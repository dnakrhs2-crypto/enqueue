#include "model/CueList.h"
#include "model/CueNumbering.h"

#include <algorithm>
#include <map>

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
int CueList::depthOf (int index) const noexcept
{
    int depth = 0;

    for (int p = parentIndexOf (index); p >= 0 && depth < 64; p = parentIndexOf (p))
        ++depth;

    return depth;
}

int CueList::parentIndexOf (int index) const noexcept
{
    if (! isValidIndex (index) || cues[(size_t) index].parentId.isNull())
        return -1;

    return indexOf (cues[(size_t) index].parentId);
}

bool CueList::isDescendantOf (int index, const juce::Uuid& ancestorId) const noexcept
{
    if (ancestorId.isNull())
        return false;

    int guard = 0;

    for (int p = parentIndexOf (index); p >= 0 && guard < 64; p = parentIndexOf (p), ++guard)
        if (cues[(size_t) p].id == ancestorId)
            return true;

    return false;
}

int CueList::subtreeEnd (int index) const noexcept
{
    if (! isValidIndex (index))
        return size();

    const auto id = cues[(size_t) index].id;
    int end = index + 1;

    while (end < size() && isDescendantOf (end, id))
        ++end;

    return end;
}

std::vector<int> CueList::childrenOf (int index) const
{
    std::vector<int> result;

    if (! isValidIndex (index))
        return result;

    const auto id = cues[(size_t) index].id;
    const int end = subtreeEnd (index);

    for (int i = index + 1; i < end; ++i)
        if (cues[(size_t) i].parentId == id)
            result.push_back (i);

    return result;
}

std::vector<int> CueList::descendantsOf (int index) const
{
    std::vector<int> result;

    if (! isValidIndex (index))
        return result;

    for (int i = index + 1, end = subtreeEnd (index); i < end; ++i)
        result.push_back (i);

    return result;
}

int CueList::nextSibling (int index) const noexcept
{
    if (! isValidIndex (index))
        return -1;

    const int next = subtreeEnd (index);

    if (next >= size() || cues[(size_t) next].parentId != cues[(size_t) index].parentId)
        return -1;

    return next;
}

bool CueList::isRowVisible (int index) const noexcept
{
    int guard = 0;

    for (int p = parentIndexOf (index); p >= 0 && guard < 64; p = parentIndexOf (p), ++guard)
        if (cues[(size_t) p].group.collapsed)
            return false;

    return true;
}

int CueList::nextVisible (int index) const noexcept
{
    for (int i = index + 1; i < size(); ++i)
        if (isRowVisible (i))
            return i;

    return -1;
}

int CueList::previousVisible (int index) const noexcept
{
    for (int i = std::min (index, size()) - 1; i >= 0; --i)
        if (isRowVisible (i))
            return i;

    return -1;
}

juce::Uuid CueList::parentForInsertion (int insertAt) const noexcept
{
    return isValidIndex (insertAt) ? cues[(size_t) insertAt].parentId : juce::Uuid::null();
}

int CueList::addAfter (Cue cue, int index)
{
    if (! isValidIndex (index))
    {
        cue.parentId = juce::Uuid::null();
        return add (std::move (cue), -1);
    }

    cue.parentId = cues[(size_t) index].parentId;
    return add (std::move (cue), subtreeEnd (index));
}

double CueList::effectiveLengthOf (int index) const noexcept
{
    if (! isValidIndex (index))
        return 0.0;

    const auto& cue = cues[(size_t) index];

    if (! cue.isGroup())
        return cue.effectiveLength();

    double total = 0.0;

    for (int child : childrenOf (index))
    {
        const double length = effectiveLengthOf (child);

        if (length < 0.0)
            return -1.0;

        const auto& c = cues[(size_t) child];

        if (cue.group.mode == GroupMode::timeline)
            total = std::max (total, c.preWaitSeconds + length);
        else if (cue.group.mode == GroupMode::playlist)
            total += c.preWaitSeconds + length;
        else
            return 0.0;   // "start first" / random: unknown
    }

    return cue.group.mode == GroupMode::playlist && cue.group.loop && ! childrenOf (index).empty() ? -1.0 : total;
}

void CueList::setCollapsed (int index, bool collapsed)
{
    if (! isValidIndex (index) || ! cues[(size_t) index].isGroup() || cues[(size_t) index].group.collapsed == collapsed)
        return;

    cues[(size_t) index].group.collapsed = collapsed;
    listeners.call ([] (Listener& l) { l.cueListStructureChanged(); });

    // a playhead hidden inside a collapsed group would fire what nobody sees: it moves to the group row
    if (collapsed && isValidIndex (playhead) && playhead > index && playhead < subtreeEnd (index))
        setPlayheadInternal (index, true);
}

void CueList::replaceAllWithCursors (std::vector<Cue> newCues, std::vector<int> selectedIndices, int primaryIndex, int playheadIndex)
{
    cues = std::move (newCues);

    for (auto& c : cues)
        c.sanitise();

    sanitiseTree();
    selection.clear();
    primary = -1;
    playhead = -1;

    std::sort (selectedIndices.begin(), selectedIndices.end());
    selectedIndices.erase (std::unique (selectedIndices.begin(), selectedIndices.end()), selectedIndices.end());
    selectedIndices.erase (std::remove_if (selectedIndices.begin(), selectedIndices.end(), [this] (int i) { return ! isValidIndex (i); }), selectedIndices.end());
    selection = std::move (selectedIndices);
    primary = isValidIndex (primaryIndex) && std::binary_search (selection.begin(), selection.end(), primaryIndex) ? primaryIndex
            : (selection.empty() ? -1 : selection.back());
    playhead = isValidIndex (playheadIndex) ? playheadIndex : primary;

    if (primary < 0 && ! cues.empty())
    {
        primary = 0;
        selection.push_back (0);

        if (playhead < 0)
            playhead = 0;
    }

    notifyStructureChanged();
    notifySelectionChanged();
    notifyPlayheadChanged();
}

void CueList::sanitiseTree()
{
    std::vector<juce::Uuid> open;   // the chain of groups the previous cue sits in (outermost first)

    for (auto& c : cues)
    {
        if (! c.parentId.isNull())
        {
            int found = -1;

            for (int i = (int) open.size() - 1; i >= 0; --i)
                if (open[(size_t) i] == c.parentId)
                {
                    found = i;
                    break;
                }

            if (found < 0)
                c.parentId = juce::Uuid::null();   // not directly inside that group (or it is not a group / does not exist)

            open.resize (found < 0 ? 0 : (size_t) found + 1);

            if ((int) open.size() > maxDepth)
            {
                c.parentId = juce::Uuid::null();   // too deep: the tree helpers stop searching at this depth
                open.clear();
            }
        }
        else
        {
            open.clear();
        }

        if (c.isGroup() && (int) open.size() < maxDepth)
            open.push_back (c.id);
    }
}

std::vector<int> CueList::withSubtrees (std::vector<int> indices) const
{
    std::vector<int> result;

    for (int i : indices)
    {
        if (! isValidIndex (i))
            continue;

        for (int j = i, end = subtreeEnd (i); j < end; ++j)
            result.push_back (j);
    }

    std::sort (result.begin(), result.end());
    result.erase (std::unique (result.begin(), result.end()), result.end());
    return result;
}

int CueList::wrapInGroup (std::vector<int> indices, Cue group)
{
    indices = withSubtrees (std::move (indices));

    if (indices.empty())
        return -1;

    group.sanitise();
    group.type = CueType::group;
    const int first = indices.front();
    group.parentId = cues[(size_t) first].parentId;
    std::vector<juce::Uuid> movedIds;

    for (int i : indices)
        movedIds.push_back (cues[(size_t) i].id);

    std::vector<Cue> block;

    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
    {
        block.insert (block.begin(), std::move (cues[(size_t) *it]));
        cues.erase (cues.begin() + *it);
    }

    for (auto& c : block)
        if (std::find (movedIds.begin(), movedIds.end(), c.parentId) == movedIds.end())
            c.parentId = group.id;   // the top-level rows of the block become the group's children

    const auto groupId = group.id;
    const juce::Uuid playheadId = isValidIndex (playhead) ? cues[(size_t) playhead].id : juce::Uuid::null();
    cues.insert (cues.begin() + first, std::move (group));
    cues.insert (cues.begin() + first + 1, std::make_move_iterator (block.begin()), std::make_move_iterator (block.end()));
    sanitiseTree();
    selection.clear();
    primary = -1;
    playhead = -1;
    notifyStructureChanged();
    setSelectedIndex (indexOf (groupId));

    // with the lock off the playhead stays where it was (the group itself when it was on a wrapped row)
    if (! lockPlayhead && ! playheadId.isNull())
    {
        const int kept = indexOf (playheadId);
        setPlayheadInternal (kept >= 0 && std::find (movedIds.begin(), movedIds.end(), playheadId) == movedIds.end() ? kept : indexOf (groupId), false);
    }

    return indexOf (groupId);
}

bool CueList::ungroup (int index)
{
    if (! isValidIndex (index) || ! cues[(size_t) index].isGroup())
        return false;

    const auto groupId = cues[(size_t) index].id;
    const auto parent = cues[(size_t) index].parentId;
    const int end = subtreeEnd (index);
    const juce::Uuid playheadId = isValidIndex (playhead) ? cues[(size_t) playhead].id : juce::Uuid::null();

    for (int i = index + 1; i < end; ++i)
        if (cues[(size_t) i].parentId == groupId)
            cues[(size_t) i].parentId = parent;

    cues.erase (cues.begin() + index);
    sanitiseTree();
    selection.clear();
    primary = -1;
    playhead = -1;
    notifyStructureChanged();
    setSelectedIndex (juce::jmin (index, size() - 1));

    if (! lockPlayhead && ! playheadId.isNull())
    {
        const int kept = indexOf (playheadId);
        setPlayheadInternal (kept >= 0 ? kept : juce::jmin (index, size() - 1), false);
    }

    return true;
}

//==============================================================================
int CueList::add (Cue cue, int insertAt)
{
    cue.sanitise();

    if (insertAt < 0 || insertAt > size())
        insertAt = size();

    cues.insert (cues.begin() + insertAt, std::move (cue));
    sanitiseTree();

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
    indices = withSubtrees (std::move (indices));   // a group goes with its children

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

    // the whole subtree is copied with fresh ids; the copies' parent links point at the copied groups
    const int end = subtreeEnd (index);
    std::vector<Cue> copies;
    std::map<juce::Uuid, juce::Uuid> newIds;

    for (int i = index; i < end; ++i)
    {
        Cue copy = cues[(size_t) i].duplicated();
        newIds[cues[(size_t) i].id] = copy.id;
        copies.push_back (std::move (copy));
    }

    for (size_t i = 0; i < copies.size(); ++i)
    {
        auto& copy = copies[i];

        if (const auto it = newIds.find (copy.parentId); i > 0 && it != newIds.end())
            copy.parentId = it->second;

        // references inside the copied subtree follow the copies; the rest keep pointing outside
        if (const auto it = newIds.find (copy.targetId()); ! copy.targetId().isNull() && it != newIds.end())
            copy.setTargetId (it->second);

        if (const auto it = newIds.find (copy.control.secondTargetId); copy.isControl() && it != newIds.end())
            copy.control.secondTargetId = it->second;

        copy.hotkey.clear();   // a hotkey stays unique
    }

    const int at = end;
    cues.insert (cues.begin() + at, std::make_move_iterator (copies.begin()), std::make_move_iterator (copies.end()));
    const int count = end - index;

    for (auto& s : selection)
        if (s >= at)
            s += count;

    if (primary >= at)
        primary += count;

    if (playhead >= at)
        playhead += count;

    sanitiseTree();
    notifyStructureChanged();
    return at;
}

bool CueList::move (int from, int to)
{
    if (! isValidIndex (from) || ! isValidIndex (to) || from == to)
        return false;

    if (const int count = subtreeEnd (from) - from; count > 1)
        return moveSubtrees ({ from }, to > from ? to + count : to);   // the root ends up at 'to'

    Cue moved = std::move (cues[(size_t) from]);
    cues.erase (cues.begin() + from);
    cues.insert (cues.begin() + to, std::move (moved));
    cues[(size_t) to].parentId = to + 1 < size() ? cues[(size_t) to + 1].parentId : juce::Uuid::null();

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

    sanitiseTree();
    notifyStructureChanged();
    return true;
}

int CueList::placeByNumber (int index)
{
    if (! isValidIndex (index) || cues[(size_t) index].number.isEmpty())
        return index;

    const juce::Uuid id = cues[(size_t) index].id;
    const int parent = parentIndexOf (index);
    const int begin = parent >= 0 ? parent + 1 : 0;
    const int end = parent >= 0 ? subtreeEnd (parent) : size();
    const auto number = cues[(size_t) index].number;

    // in front of the first sibling with a greater number, else after the last sibling
    int insertIndex = end;

    for (int i = begin; i < end; i = subtreeEnd (i))
        if (i != index && CueNumbering::compare (cues[(size_t) i].number, number) > 0)
        {
            insertIndex = i;
            break;
        }

    if (insertIndex == index || insertIndex == subtreeEnd (index))
        return index;   // already there

    const auto block = withSubtrees ({ index });
    int before = 0;

    for (int i : block)
        if (i < insertIndex)
            ++before;

    const juce::Uuid parentId = parent >= 0 ? cues[(size_t) parent].id : juce::Uuid::null();
    moveIndices (block, insertIndex - before, &parentId);
    return indexOf (id);
}

bool CueList::moveSubtrees (std::vector<int> indices, int insertIndex)
{
    indices = withSubtrees (std::move (indices));

    if (indices.empty())
        return false;

    insertIndex = juce::jlimit (0, size(), insertIndex);

    if (std::binary_search (indices.begin(), indices.end(), insertIndex))
        return false;   // dropped onto one of the moved rows (its own place, or inside its own subtree): nothing to do

    // the group the block joins: that of the first row that is not moving at / after the drop point,
    // read before anything moves (after the removal the neighbour could be a different row)
    juce::Uuid newParent = juce::Uuid::null();

    for (int i = insertIndex; i < size(); ++i)
        if (! std::binary_search (indices.begin(), indices.end(), i))
        {
            newParent = cues[(size_t) i].parentId;
            break;
        }

    int before = 0;

    for (int i : indices)
        if (i < insertIndex)
            ++before;

    return moveIndices (std::move (indices), insertIndex - before, &newParent);
}

bool CueList::moveIndices (std::vector<int> indices, int to)
{
    return moveIndices (std::move (indices), to, nullptr);
}

bool CueList::moveIndices (std::vector<int> indices, int to, const juce::Uuid* parentForMoved)
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
    const auto newParent = parentForMoved != nullptr ? *parentForMoved : parentForInsertion (to);
    std::vector<juce::Uuid> blockIds;

    for (const auto& c : block)
        blockIds.push_back (c.id);

    // the moved top-level rows join the group at the drop point; their own children keep their parents
    for (auto& c : block)
        if (std::find (blockIds.begin(), blockIds.end(), c.parentId) == blockIds.end())
            c.parentId = newParent;

    cues.insert (cues.begin() + to, std::make_move_iterator (block.begin()), std::make_move_iterator (block.end()));
    sanitiseTree();

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

    sanitiseTree();
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

void CueList::restoreCursors (std::vector<int> selectedIndices, int primaryIndex, int playheadIndex)
{
    setSelectionInternal (std::move (selectedIndices), primaryIndex, false);
    setPlayheadInternal (playheadIndex, false);
}

void CueList::setLockPlayheadToSelection (bool shouldLock)
{
    const bool wasLocked = lockPlayhead;
    lockPlayhead = shouldLock;

    if (shouldLock && ! wasLocked && isValidIndex (primary) && playhead != primary)
        setPlayheadInternal (primary, false);
}

bool CueList::isNumberTaken (const juce::String& number, const juce::Uuid& exceptId) const noexcept
{
    if (number.isEmpty())
        return false;

    for (const auto& c : cues)
        if (c.number == number && c.id != exceptId)
            return true;

    return false;
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
    if (! isValidIndex (playhead))
        return false;

    const int next = nextVisible (playhead);

    if (next < 0)
        return false;

    setPlayheadInternal (next, true);
    return true;
}

bool CueList::retreatPlayhead()
{
    if (! isValidIndex (playhead))
        return false;

    const int previous = previousVisible (playhead);

    if (previous < 0)
        return false;

    setPlayheadInternal (previous, true);
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
