#pragma once
#include "../model/RecorderModel.h"
#include <optional>

namespace gocue::recorder
{
struct EditSnapshot
{
    EditState state;
    std::vector<Id> selection;
};
struct EditOptions
{
    juce::String mergeKey, gestureId;
    double nowMs = juce::Time::getMillisecondCounterHiRes();
};
class EditHistory
{
public:
    static constexpr size_t maxDepth = 200;
    static constexpr double mergeWindowMs = 700.0;
    struct Entry { EditSnapshot before, after; juce::String name; EditOptions options; };
    void push(EditSnapshot before, EditSnapshot after, const juce::String& name, const EditOptions&);
    const Entry* undoEntry() const;
    const Entry* redoEntry() const;
    void commitUndo(EditSnapshot current);
    void commitRedo(EditSnapshot current);
    void endGesture();
    void clear();
    size_t undoDepth() const { return undos.size(); }
    size_t redoDepth() const { return redos.size(); }
private:
    std::vector<Entry> undos, redos;
    bool mergeAllowed = false;
};
}
