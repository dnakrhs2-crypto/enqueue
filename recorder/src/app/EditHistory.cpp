#include "EditHistory.h"

namespace gocue::recorder
{
void EditHistory::push(EditSnapshot before, EditSnapshot after, const juce::String& name, const EditOptions& options)
{
    bool merge = false;
    if (mergeAllowed && redos.empty() && !undos.empty())
    {
        const auto& last = undos.back().options;
        const auto elapsed = options.nowMs - last.nowMs;
        merge = options.gestureId.isNotEmpty() ? options.gestureId == last.gestureId
            : last.gestureId.isEmpty() && options.mergeKey.isNotEmpty() && options.mergeKey == last.mergeKey
              && elapsed >= 0 && elapsed <= mergeWindowMs;
    }
    redos.clear();
    if (merge) { auto& last = undos.back(); last.after = std::move(after); last.name = name; last.options = options; }
    else
    {
        undos.push_back({std::move(before), std::move(after), name, options});
        if (undos.size() > maxDepth) undos.erase(undos.begin());
    }
    mergeAllowed = true;
}
const EditHistory::Entry* EditHistory::undoEntry() const { return undos.empty() ? nullptr : &undos.back(); }
const EditHistory::Entry* EditHistory::redoEntry() const { return redos.empty() ? nullptr : &redos.back(); }
void EditHistory::commitUndo(EditSnapshot current)
{ if (!undos.empty()) { undos.back().after = std::move(current); redos.push_back(std::move(undos.back())); undos.pop_back(); } endGesture(); }
void EditHistory::commitRedo(EditSnapshot current)
{ if (!redos.empty()) { redos.back().before = std::move(current); undos.push_back(std::move(redos.back())); redos.pop_back(); } endGesture(); }
void EditHistory::endGesture() { mergeAllowed = false; }
void EditHistory::clear() { undos.clear(); redos.clear(); endGesture(); }
}
