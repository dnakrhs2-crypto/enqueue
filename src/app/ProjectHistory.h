#pragma once

#include "model/ProjectSerializer.h"

#include <functional>
#include <optional>
#include <vector>

namespace gocue
{

/** What one undo step restores: the whole project plus the standby selection.
    Copies are cheap: juce::String (plugin states) is copy-on-write. */
struct ProjectSnapshot
{
    Project project;
    juce::Uuid selectedId;                 // primary selection
    std::vector<juce::Uuid> selectedIds;   // the whole selection
    juce::Uuid playheadId;
    bool pluginStatesCaptured = false;   // 'project' carries the live plugin chain states (taken for structural edits)
};

/** Snapshot-based undo / redo stack. Message thread only. */
class ProjectHistory
{
public:
    static constexpr int maxDepth = 200;
    static constexpr double coalesceWindowMs = 700.0;

    /** Records the state before an edit and clears the redo stack.
        Returns false when the step was merged into the previous one: same non-empty coalesceKey
        within coalesceWindowMs of the previous push (a slider drag becomes one step). */
    bool push (ProjectSnapshot before, const juce::String& name, const juce::String& coalesceKey, double nowMs)
    {
        redoStack.clear();

        if (coalesceKey.isNotEmpty() && ! undoStack.empty())
        {
            auto& top = undoStack.back();

            if (top.coalesceKey == coalesceKey && nowMs - top.timeMs <= coalesceWindowMs)
            {
                top.timeMs = nowMs;
                return false;
            }
        }

        undoStack.push_back ({ std::move (before), name, coalesceKey, nowMs });

        if ((int) undoStack.size() > maxDepth)
            undoStack.erase (undoStack.begin(), undoStack.begin() + ((int) undoStack.size() - maxDepth));

        return true;
    }

    bool canUndo() const noexcept { return ! undoStack.empty(); }
    bool canRedo() const noexcept { return ! redoStack.empty(); }
    juce::String getUndoName() const { return undoStack.empty() ? juce::String() : undoStack.back().name; }
    juce::String getRedoName() const { return redoStack.empty() ? juce::String() : redoStack.back().name; }
    int getUndoDepth() const noexcept { return (int) undoStack.size(); }
    int getRedoDepth() const noexcept { return (int) redoStack.size(); }

    /** Pops the last recorded state. 'makeCurrent' captures the present state for redo; it is told
        whether the step being undone carried plugin states so the redo step matches. */
    std::optional<ProjectSnapshot> undo (const std::function<ProjectSnapshot (bool capturePluginStates)>& makeCurrent)
    {
        if (undoStack.empty())
            return std::nullopt;

        Entry entry = std::move (undoStack.back());
        undoStack.pop_back();

        Entry redoEntry { makeCurrent (entry.snapshot.pluginStatesCaptured), entry.name, {}, 0.0 };
        redoEntry.snapshot.pluginStatesCaptured = entry.snapshot.pluginStatesCaptured;
        redoStack.push_back (std::move (redoEntry));

        return std::move (entry.snapshot);
    }

    std::optional<ProjectSnapshot> redo (const std::function<ProjectSnapshot (bool capturePluginStates)>& makeCurrent)
    {
        if (redoStack.empty())
            return std::nullopt;

        Entry entry = std::move (redoStack.back());
        redoStack.pop_back();

        Entry undoEntry { makeCurrent (entry.snapshot.pluginStatesCaptured), entry.name, {}, 0.0 };
        undoEntry.snapshot.pluginStatesCaptured = entry.snapshot.pluginStatesCaptured;
        undoStack.push_back (std::move (undoEntry));

        return std::move (entry.snapshot);
    }

    void clear()
    {
        undoStack.clear();
        redoStack.clear();
    }

private:
    struct Entry
    {
        ProjectSnapshot snapshot;
        juce::String name;
        juce::String coalesceKey;
        double timeMs = 0.0;
    };

    std::vector<Entry> undoStack, redoStack;
};

} // namespace gocue
