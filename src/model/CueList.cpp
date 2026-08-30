#include "model/CueList.h"

namespace gocue
{

int CueList::indexOf (const juce::Uuid& id) const noexcept
{
    for (size_t i = 0; i < cues.size(); ++i)
        if (cues[i].id == id)
            return (int) i;

    return -1;
}

int CueList::add (Cue cue, int insertAt)
{
    cue.sanitise();

    if (insertAt < 0 || insertAt > size())
        insertAt = size();

    cues.insert (cues.begin() + insertAt, std::move (cue));

    if (selected >= insertAt)
        ++selected;

    notifyStructureChanged();

    if (selected < 0)
        setSelectedIndex (insertAt);

    return insertAt;
}

void CueList::remove (int index)
{
    if (! isValidIndex (index))
        return;

    cues.erase (cues.begin() + index);

    const int oldSelected = selected;

    if (selected > index)
        --selected;

    if (selected >= size())
        selected = size() - 1;

    notifyStructureChanged();

    if (selected != oldSelected || selected == index)
        notifySelectionChanged();
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

    if (selected == from)
        selected = to;
    else if (from < selected && selected <= to)
        --selected;
    else if (to <= selected && selected < from)
        ++selected;

    notifyStructureChanged();
    return true;
}

void CueList::replaceAll (std::vector<Cue> newCues)
{
    cues = std::move (newCues);

    for (auto& c : cues)
        c.sanitise();

    selected = cues.empty() ? -1 : 0;
    notifyStructureChanged();
    notifySelectionChanged();
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

void CueList::setSelectedIndex (int index)
{
    int newIndex = -1;

    if (! cues.empty() && index >= 0)
        newIndex = juce::jmin (index, size() - 1);

    if (newIndex == selected)
        return;

    selected = newIndex;
    notifySelectionChanged();
}

bool CueList::selectNext()
{
    if (! isValidIndex (selected) || selected + 1 >= size())
        return false;

    setSelectedIndex (selected + 1);
    return true;
}

void CueList::notifyStructureChanged()
{
    listeners.call ([] (Listener& l) { l.cueListStructureChanged(); });
}

void CueList::notifySelectionChanged()
{
    const int index = selected;
    listeners.call ([index] (Listener& l) { l.cueSelectionChanged (index); });
}

} // namespace gocue
