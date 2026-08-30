#pragma once

#include "model/Cue.h"

#include <functional>
#include <vector>

namespace gocue
{

/** Ordered list of cues plus the "standby" selection: the cue the next GO will fire.
    Message-thread only. */
class CueList
{
public:
    struct Listener
    {
        virtual ~Listener() = default;

        /** Cues were added, removed, reordered or the whole list was replaced. */
        virtual void cueListStructureChanged() {}

        /** A single cue's properties changed. */
        virtual void cueChanged (int index) { juce::ignoreUnused (index); }

        /** The standby selection moved (index may be -1). */
        virtual void cueSelectionChanged (int index) { juce::ignoreUnused (index); }
    };

    int size() const noexcept { return (int) cues.size(); }
    bool isEmpty() const noexcept { return cues.empty(); }
    bool isValidIndex (int index) const noexcept { return index >= 0 && index < size(); }

    const Cue& get (int index) const { return cues[(size_t) index]; }
    const std::vector<Cue>& getAll() const noexcept { return cues; }
    int indexOf (const juce::Uuid& id) const noexcept;

    /** Inserts a cue (insertAt == -1 appends). Returns the index it landed on. */
    int add (Cue cue, int insertAt = -1);
    void remove (int index);
    /** Inserts a copy (new id) right after 'index'. Returns the new index or -1. */
    int duplicate (int index);
    /** Moves the cue at 'from' so that it ends up at index 'to'. */
    bool move (int from, int to);
    /** Replaces everything (project load). Selection resets to the first cue. */
    void replaceAll (std::vector<Cue> newCues);
    void clear();
    /** Mutates one cue in place and notifies cueChanged(). */
    void update (int index, const std::function<void (Cue&)>& mutator);

    int getSelectedIndex() const noexcept { return selected; }
    const Cue* getSelected() const noexcept { return isValidIndex (selected) ? &cues[(size_t) selected] : nullptr; }
    /** Clamped into range; -1 clears the selection. */
    void setSelectedIndex (int index);
    /** Advances the standby selection. Returns false when already on the last cue (selection unchanged). */
    bool selectNext();

    void addListener (Listener* l) { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

private:
    void notifyStructureChanged();
    void notifySelectionChanged();

    std::vector<Cue> cues;
    int selected = -1;
    juce::ListenerList<Listener> listeners;
};

} // namespace gocue
