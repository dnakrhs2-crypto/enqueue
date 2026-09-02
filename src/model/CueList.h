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

    //==========================================================================
    // Tree: a group cue's children are the cues that follow it (pre-order) with parentId == the group's id.
    // Every operation keeps that contiguity (a group moves / duplicates / deletes with its subtree).

    /** 0 at the top level, 1 for a child of a top-level group ... */
    int depthOf (int index) const noexcept;
    /** Index of the cue's group, -1 at the top level. */
    int parentIndexOf (int index) const noexcept;
    bool isDescendantOf (int index, const juce::Uuid& ancestorId) const noexcept;
    /** First index after 'index' that is not inside its subtree (index + 1 for a leaf). */
    int subtreeEnd (int index) const noexcept;
    /** Direct children of the cue at 'index' (empty unless it is a group). */
    std::vector<int> childrenOf (int index) const;
    /** All cues inside the subtree of 'index' (not the cue itself). */
    std::vector<int> descendantsOf (int index) const;
    /** The next cue with the same parent, or -1 (end of the parent's children / of the list). */
    int nextSibling (int index) const noexcept;
    /** False when one of the ancestors is collapsed (the row is hidden in the list view). */
    bool isRowVisible (int index) const noexcept;
    /** The next / previous visible index after 'index' (-1 when there is none). */
    int nextVisible (int index) const noexcept;
    int previousVisible (int index) const noexcept;
    /** Parent id a cue gets when it is inserted at 'insertAt': the parent of the cue that will follow it (null at the end). */
    juce::Uuid parentForInsertion (int insertAt) const noexcept;
    /** Inserts 'cue' as the next sibling of the cue at 'index' (after its subtree); index -1 appends at the top level. */
    int addAfter (Cue cue, int index);
    /** Length of a cue for the list (a group: the longest child start + length for a timeline, the sum for a playlist; -1 = endless). */
    double effectiveLengthOf (int index) const noexcept;
    /** Shows / hides the children of a group in the list view (no cueChanged: only the visibility changes). */
    void setCollapsed (int index, bool collapsed);
    /** Expands indices to whole subtrees (sorted, unique). */
    std::vector<int> withSubtrees (std::vector<int> indices) const;
    /** Puts the cues (with their subtrees) into a new group inserted where the first of them was; the group takes the
        first cue's parent. Returns the group's index (-1 when nothing valid was given). Selects the group. */
    int wrapInGroup (std::vector<int> indices, Cue group);
    /** Removes a group cue and hands its children to the group's parent (they stay where they are). Returns false
        when 'index' is not a group. Selects the first former child (or the row that took the group's place). */
    bool ungroup (int index);

    /** Inserts a cue (insertAt == -1 appends). Returns the index it landed on. */
    int add (Cue cue, int insertAt = -1);
    void remove (int index);
    /** Removes several cues at once (indices in any order). */
    void removeIndices (std::vector<int> indices);
    /** Inserts a copy (new id) right after 'index'. Returns the new index or -1. */
    int duplicate (int index);
    /** Moves the cue at 'from' so that it ends up at index 'to'. */
    bool move (int from, int to);
    /** Moves several cues (sorted indices) so that the block starts at 'to' (an index in the list after removal).
        Raw rows: use moveSubtrees() from the UI so groups travel with their children. */
    bool moveIndices (std::vector<int> indices, int to);
    /** Moves the cues (with their subtrees) so that the block lands in front of the cue currently at 'insertIndex'
        (size() = the end). The moved top-level rows join the group at that point. */
    bool moveSubtrees (std::vector<int> indices, int insertIndex);
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
    /** Nulls parent ids that do not point at a group directly enclosing the cue (keeps the pre-order invariant). */
    void sanitiseTree();

    std::vector<Cue> cues;
    std::vector<int> selection;   // sorted, unique
    int primary = -1;
    int playhead = -1;
    bool lockPlayhead = true;
    juce::ListenerList<Listener> listeners;
};

} // namespace gocue
