#pragma once

#include "model/Cue.h"

#include <functional>
#include <vector>

namespace gocue
{

/** Ordered list of cues plus two cursors:
    - the selection (one or more rows; the primary one is what the inspector edits), and
    - the playhead: the cue the next GO fires (QLab's "standby" cue).
    With lockPlayheadToSelection (default) the playhead follows the selection and vice versa.
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

        /** The selection changed (index = the primary selected cue, may be -1). */
        virtual void cueSelectionChanged (int index) { juce::ignoreUnused (index); }

        /** The playhead moved (index may be -1). */
        virtual void playheadChanged (int index) { juce::ignoreUnused (index); }
    };

    int size() const noexcept { return (int) cues.size(); }
    bool isEmpty() const noexcept { return cues.empty(); }
    bool isValidIndex (int index) const noexcept { return index >= 0 && index < size(); }

    const Cue& get (int index) const { return cues[(size_t) index]; }
    const std::vector<Cue>& getAll() const noexcept { return cues; }
    int indexOf (const juce::Uuid& id) const noexcept;
    const Cue* findById (const juce::Uuid& id) const noexcept;

    /** Inserts a cue (insertAt == -1 appends). Returns the index it landed on. */
    int add (Cue cue, int insertAt = -1);
    void remove (int index);
    /** Removes several cues at once (indices in any order). */
    void removeIndices (std::vector<int> indices);
    /** Inserts a copy (new id) right after 'index'. Returns the new index or -1. */
    int duplicate (int index);
    /** Moves the cue at 'from' so that it ends up at index 'to'. */
    bool move (int from, int to);
    /** Moves several cues (sorted indices) so that the block starts at 'to' (an index in the list after removal). */
    bool moveIndices (std::vector<int> indices, int to);
    /** Replaces everything (project load). Selection and playhead reset to the first cue. */
    void replaceAll (std::vector<Cue> newCues);
    void clear();
    /** Mutates one cue in place and notifies cueChanged(). */
    void update (int index, const std::function<void (Cue&)>& mutator);
    /** Replaces the saved plugin snapshot without notifying (used after a save). */
    void setPluginStatesQuietly (int index, std::vector<PluginSlotState> states);

    //==========================================================================
    // Selection (inspector) — 'getSelectedIndex' is the primary selected row

    int getSelectedIndex() const noexcept { return primary; }
    const Cue* getSelected() const noexcept { return isValidIndex (primary) ? &cues[(size_t) primary] : nullptr; }
    const std::vector<int>& getSelectedIndices() const noexcept { return selection; }
    bool isSelected (int index) const noexcept;
    /** Single selection (clamped; -1 clears). Moves the playhead too when locked. */
    void setSelectedIndex (int index);
    /** Multi selection; 'primaryIndex' must be one of them (or -1 = last). Moves the playhead to the primary when locked. */
    void setSelection (std::vector<int> indices, int primaryIndex = -1);
    void selectAll();

    //==========================================================================
    // Playhead (GO)

    int getPlayheadIndex() const noexcept { return playhead; }
    const Cue* getPlayhead() const noexcept { return isValidIndex (playhead) ? &cues[(size_t) playhead] : nullptr; }
    /** Clamped; -1 clears. Moves the selection too when locked. */
    void setPlayheadIndex (int index);
    /** Moves the playhead to the next cue. Returns false when already on the last cue (unchanged). */
    bool advancePlayhead();
    /** Same as advancePlayhead(): kept for the older call sites. */
    bool selectNext() { return advancePlayhead(); }
    /** Moves the playhead to the previous cue. */
    bool retreatPlayhead();

    /** Turning the lock on snaps the playhead to the current primary selection. */
    void setLockPlayheadToSelection (bool shouldLock);
    /** True when another cue (not 'exceptId') already carries this non-empty number. */
    bool isNumberTaken (const juce::String& number, const juce::Uuid& exceptId) const noexcept;
    bool isPlayheadLockedToSelection() const noexcept { return lockPlayhead; }

    void addListener (Listener* l) { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

private:
    void notifyStructureChanged();
    void notifySelectionChanged();
    void notifyPlayheadChanged();
    void setSelectionInternal (std::vector<int> indices, int primaryIndex, bool syncPlayhead);
    void setPlayheadInternal (int index, bool syncSelection);
    void clampCursors();

    std::vector<Cue> cues;
    std::vector<int> selection;   // sorted, unique
    int primary = -1;
    int playhead = -1;
    bool lockPlayhead = true;
    juce::ListenerList<Listener> listeners;
};

} // namespace gocue
