#pragma once

#include "model/LevelMatrix.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace gocue
{

/** QLab-style level grid: a main cell (top-left), one level cell per output (top row), one per input
    (left column) and the crosspoints. Drag vertically to change a level (Shift = 0.1 dB steps), double-click
    to reset, type a value (no sign = negative, empty = silence), right-click for gangs. Ganged cells move
    together. Levels are clamped to [minDb, maxDb]; below minDb is silence (-inf).
    Also used for a patch's routing (inputs = cue outputs, outputs = device outputs, main = patch main). */
class LevelMatrixComponent : public juce::Component
{
public:
    LevelMatrixComponent();
    ~LevelMatrixComponent() override;

    /** Replaces the displayed levels. 'outputConnected[k]' dims an output column that reaches no device output. */
    void setLevels (double mainDb, const LevelMatrix& matrix);
    void setLabels (const juce::StringArray& inputNames, const juce::StringArray& outputNames);
    void setOutputConnected (const std::vector<bool>& connected);
    void setLimits (double minDb, double maxDb);
    void setMainVisible (bool visible);
    /** Fade-cue mode: every cell carries an active flag (inactive = the fade leaves it alone). Alt+click or the
        right-click menu toggles it; inactive cells are drawn hatched. Pass nullptr to leave the mode. */
    void setActiveFlags (const bool* mainActive, const std::vector<char>* inputs, const std::vector<char>* outputs,
                         const std::vector<std::vector<char>>* crosspoints);
    /** Fired when an active flag is toggled: kind 0 = main, 1 = input, 2 = output, 3 = crosspoint. */
    std::function<void (int kind, int input, int output, bool active)> onActiveToggled;
    /** Hides the input / output level cells (a patch's routing has only crosspoints and a main). */
    void setEdgeLevelsVisible (bool visible);
    void setEditable (bool editable);

    double getMainDb() const noexcept { return mainDb; }
    const LevelMatrix& getMatrix() const noexcept { return matrix; }

    /** Every change (drag steps included). 'finished' is false while dragging: coalesce those. */
    std::function<void (double mainDb, const LevelMatrix& matrix, bool finished)> onChange;

    /** Preferred size for the current matrix (the parent usually wraps this in a viewport). */
    int getPreferredWidth() const noexcept;
    int getPreferredHeight() const noexcept;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void focusLost (FocusChangeType) override;

    static constexpr int cellWidth = 58;
    static constexpr int cellHeight = 22;
    static constexpr int gap = 3;
    static constexpr int headerWidth = 64;
    static constexpr int headerHeight = 20;

private:
    enum class Kind { none, main, input, output, cross };

    struct CellRef
    {
        Kind kind = Kind::none;
        int in = -1, out = -1;
        bool operator== (const CellRef& o) const noexcept { return kind == o.kind && in == o.in && out == o.out; }
        bool operator!= (const CellRef& o) const noexcept { return ! (*this == o); }
        bool isValid() const noexcept { return kind != Kind::none; }
    };

    CellRef cellAt (juce::Point<int> p) const noexcept;
    juce::Rectangle<int> boundsOf (const CellRef& c) const noexcept;
    double getValue (const CellRef& c) const noexcept;
    void setValue (const CellRef& c, double db) noexcept;
    int gangOf (const CellRef& c) const noexcept;
    void setGang (const CellRef& c, int gang) noexcept;
    double defaultValue (const CellRef& c) const noexcept;
    double clampLevel (double db) const noexcept;
    void forEachCell (const std::function<void (const CellRef&)>& fn) const;
    void applyDelta (double deltaDb, bool finished);
    void beginTyping (const juce::String& initial);
    void commitTyping();
    /** Applies 'fn' to the cell and, when it is ganged, to every cell of the same gang. */
    void forEachInGang (const CellRef& cell, const std::function<void (const CellRef&)>& fn) const;
    void showGangMenu (const CellRef& c);
    juce::String textFor (double db) const;
    void notify (bool finished);

    double mainDb = 0.0;
    LevelMatrix matrix;
    juce::StringArray inputNames, outputNames;
    std::vector<bool> outputConnected;
    double minDb = -60.0, maxDb = 12.0;
    bool mainVisible = true;
    bool edgeLevelsVisible = true;
    bool activeMode = false;
    bool mainActive = true;
    std::vector<char> inputActive, outputActive;
    std::vector<std::vector<char>> crosspointActive;
    bool isActive (const CellRef& c) const noexcept;
    void toggleActive (const CellRef& c);
    bool editable = true;

    CellRef selected, dragCell;
    std::vector<std::pair<CellRef, double>> dragStartValues;
    double lastDragDelta = 0.0;
    bool dragging = false;
    std::unique_ptr<juce::TextEditor> typingEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMatrixComponent)
};

} // namespace gocue
