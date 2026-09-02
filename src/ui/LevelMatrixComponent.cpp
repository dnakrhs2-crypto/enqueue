#include "ui/LevelMatrixComponent.h"

#include "ui/UiUtils.h"

#include <cmath>

namespace gocue
{

namespace
{
    const juce::Colour gangColours[] = {
        juce::Colour (0xff5a3d8b), juce::Colour (0xff3d6f8b), juce::Colour (0xff3d8b5a), juce::Colour (0xff8b8b3d),
        juce::Colour (0xff8b5a3d), juce::Colour (0xff8b3d5a), juce::Colour (0xff3d8b8b), juce::Colour (0xff6b6b6b)
    };
}

LevelMatrixComponent::LevelMatrixComponent()
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (true);
}

LevelMatrixComponent::~LevelMatrixComponent() = default;

void LevelMatrixComponent::setLevels (double newMainDb, const LevelMatrix& newMatrix)
{
    if (dragging || typingEditor != nullptr)
        return;   // the user is editing: keep the local state

    mainDb = newMainDb;
    matrix = newMatrix;
    outputConnected.resize ((size_t) matrix.numOutputs(), true);

    if (selected.isValid() && ! boundsOf (selected).isEmpty())
    {
        // keep the selection if it still exists
    }
    else
    {
        selected = {};
    }

    setSize (getPreferredWidth(), getPreferredHeight());
    repaint();
}

void LevelMatrixComponent::setLabels (const juce::StringArray& inputs, const juce::StringArray& outputs)
{
    inputNames = inputs;
    outputNames = outputs;
    repaint();
}

void LevelMatrixComponent::setOutputConnected (const std::vector<bool>& connected)
{
    outputConnected = connected;
    outputConnected.resize ((size_t) matrix.numOutputs(), true);
    repaint();
}

void LevelMatrixComponent::setLimits (double newMinDb, double newMaxDb)
{
    minDb = juce::jmin (newMinDb, newMaxDb - 1.0);
    maxDb = newMaxDb;
    repaint();
}

void LevelMatrixComponent::setMainVisible (bool visible)
{
    mainVisible = visible;
    repaint();
}

void LevelMatrixComponent::setActiveFlags (const bool* main, const std::vector<char>* inputs, const std::vector<char>* outputs,
                                           const std::vector<std::vector<char>>* crosspoints)
{
    activeMode = main != nullptr;

    if (activeMode)
    {
        mainActive = *main;
        inputActive = inputs != nullptr ? *inputs : std::vector<char>();
        outputActive = outputs != nullptr ? *outputs : std::vector<char>();
        crosspointActive = crosspoints != nullptr ? *crosspoints : std::vector<std::vector<char>>();
    }

    repaint();
}

bool LevelMatrixComponent::isActive (const CellRef& c) const noexcept
{
    if (! activeMode)
        return true;

    switch (c.kind)
    {
        case Kind::main:   return mainActive;
        case Kind::input:  return c.in >= 0 && c.in < (int) inputActive.size() && inputActive[(size_t) c.in] != 0;
        case Kind::output: return c.out >= 0 && c.out < (int) outputActive.size() && outputActive[(size_t) c.out] != 0;
        case Kind::cross:  return c.in >= 0 && c.in < (int) crosspointActive.size() && c.out >= 0
                               && c.out < (int) crosspointActive[(size_t) c.in].size() && crosspointActive[(size_t) c.in][(size_t) c.out] != 0;
        case Kind::none:   break;
    }

    return false;
}

void LevelMatrixComponent::toggleActive (const CellRef& c)
{
    if (! activeMode || ! c.isValid())
        return;

    const bool on = ! isActive (c);

    switch (c.kind)
    {
        case Kind::main:   mainActive = on; break;
        case Kind::input:  if (c.in < (int) inputActive.size()) inputActive[(size_t) c.in] = on ? 1 : 0; break;
        case Kind::output: if (c.out < (int) outputActive.size()) outputActive[(size_t) c.out] = on ? 1 : 0; break;
        case Kind::cross:  if (c.in < (int) crosspointActive.size() && c.out < (int) crosspointActive[(size_t) c.in].size()) crosspointActive[(size_t) c.in][(size_t) c.out] = on ? 1 : 0; break;
        case Kind::none:   break;
    }

    repaint();

    if (onActiveToggled)
        onActiveToggled (c.kind == Kind::main ? 0 : c.kind == Kind::input ? 1 : c.kind == Kind::output ? 2 : 3, c.in, c.out, on);
}

void LevelMatrixComponent::setEdgeLevelsVisible (bool visible)
{
    edgeLevelsVisible = visible;
    repaint();
}

void LevelMatrixComponent::setEditable (bool shouldBeEditable)
{
    editable = shouldBeEditable;

    if (! editable)
    {
        typingEditor.reset();
        dragging = false;
    }

    repaint();
}

int LevelMatrixComponent::getPreferredWidth() const noexcept
{
    return headerWidth + gap + (matrix.numOutputs() + 1) * (cellWidth + gap);
}

int LevelMatrixComponent::getPreferredHeight() const noexcept
{
    return headerHeight + gap + (matrix.numInputs() + 1) * (cellHeight + gap);
}

//==============================================================================
juce::Rectangle<int> LevelMatrixComponent::boundsOf (const CellRef& c) const noexcept
{
    const int x0 = headerWidth + gap;
    const int y0 = headerHeight + gap;

    switch (c.kind)
    {
        case Kind::main:   return { x0, y0, cellWidth, cellHeight };
        case Kind::output: return c.out >= 0 && c.out < matrix.numOutputs() ? juce::Rectangle<int> (x0 + (c.out + 1) * (cellWidth + gap), y0, cellWidth, cellHeight) : juce::Rectangle<int>();
        case Kind::input:  return c.in >= 0 && c.in < matrix.numInputs() ? juce::Rectangle<int> (x0, y0 + (c.in + 1) * (cellHeight + gap), cellWidth, cellHeight) : juce::Rectangle<int>();
        case Kind::cross:  return c.in >= 0 && c.in < matrix.numInputs() && c.out >= 0 && c.out < matrix.numOutputs()
                                  ? juce::Rectangle<int> (x0 + (c.out + 1) * (cellWidth + gap), y0 + (c.in + 1) * (cellHeight + gap), cellWidth, cellHeight)
                                  : juce::Rectangle<int>();
        case Kind::none:   break;
    }

    return {};
}

LevelMatrixComponent::CellRef LevelMatrixComponent::cellAt (juce::Point<int> p) const noexcept
{
    const int x0 = headerWidth + gap;
    const int y0 = headerHeight + gap;

    if (p.x < x0 || p.y < y0)
        return {};

    const int col = (p.x - x0) / (cellWidth + gap);
    const int row = (p.y - y0) / (cellHeight + gap);

    if ((p.x - x0) % (cellWidth + gap) >= cellWidth || (p.y - y0) % (cellHeight + gap) >= cellHeight)
        return {};   // in a gap

    if (col == 0 && row == 0)
        return mainVisible ? CellRef { Kind::main, -1, -1 } : CellRef();

    if (row == 0)
        return edgeLevelsVisible && col - 1 < matrix.numOutputs() ? CellRef { Kind::output, -1, col - 1 } : CellRef();

    if (col == 0)
        return edgeLevelsVisible && row - 1 < matrix.numInputs() ? CellRef { Kind::input, row - 1, -1 } : CellRef();

    if (row - 1 < matrix.numInputs() && col - 1 < matrix.numOutputs())
        return { Kind::cross, row - 1, col - 1 };

    return {};
}

double LevelMatrixComponent::getValue (const CellRef& c) const noexcept
{
    switch (c.kind)
    {
        case Kind::main:   return mainDb;
        case Kind::input:  return matrix.inputDb[(size_t) c.in];
        case Kind::output: return matrix.outputDb[(size_t) c.out];
        case Kind::cross:  return matrix.crosspointDb[(size_t) c.in][(size_t) c.out];
        case Kind::none:   break;
    }

    return 0.0;
}

void LevelMatrixComponent::setValue (const CellRef& c, double db) noexcept
{
    switch (c.kind)
    {
        case Kind::main:   mainDb = db; break;
        case Kind::input:  matrix.inputDb[(size_t) c.in] = db; break;
        case Kind::output: matrix.outputDb[(size_t) c.out] = db; break;
        case Kind::cross:  matrix.crosspointDb[(size_t) c.in][(size_t) c.out] = db; break;
        case Kind::none:   break;
    }
}

int LevelMatrixComponent::gangOf (const CellRef& c) const noexcept
{
    switch (c.kind)
    {
        case Kind::main:   return matrix.mainGang;
        case Kind::input:  return matrix.inputGang[(size_t) c.in];
        case Kind::output: return matrix.outputGang[(size_t) c.out];
        case Kind::cross:  return matrix.crosspointGang[(size_t) c.in][(size_t) c.out];
        case Kind::none:   break;
    }

    return 0;
}

void LevelMatrixComponent::setGang (const CellRef& c, int gang) noexcept
{
    switch (c.kind)
    {
        case Kind::main:   matrix.mainGang = gang; break;
        case Kind::input:  matrix.inputGang[(size_t) c.in] = gang; break;
        case Kind::output: matrix.outputGang[(size_t) c.out] = gang; break;
        case Kind::cross:  matrix.crosspointGang[(size_t) c.in][(size_t) c.out] = gang; break;
        case Kind::none:   break;
    }
}

double LevelMatrixComponent::defaultValue (const CellRef& c) const noexcept
{
    if (c.kind == Kind::cross)
    {
        if (matrix.numInputs() == 1)
            return c.out < 2 ? 0.0 : LevelMatrix::silentDb;

        return c.in == c.out ? 0.0 : LevelMatrix::silentDb;
    }

    return 0.0;
}

double LevelMatrixComponent::clampLevel (double db) const noexcept
{
    if (! std::isfinite (db) || db < minDb)
        return LevelMatrix::silentDb;

    return juce::jlimit (minDb, maxDb, std::round (db * 10.0) / 10.0);
}

void LevelMatrixComponent::forEachCell (const std::function<void (const CellRef&)>& fn) const
{
    if (mainVisible)
        fn ({ Kind::main, -1, -1 });

    if (edgeLevelsVisible)
        for (int o = 0; o < matrix.numOutputs(); ++o)
            fn ({ Kind::output, -1, o });

    for (int i = 0; i < matrix.numInputs(); ++i)
    {
        if (edgeLevelsVisible)
            fn ({ Kind::input, i, -1 });

        for (int o = 0; o < matrix.numOutputs(); ++o)
            fn ({ Kind::cross, i, o });
    }
}

juce::String LevelMatrixComponent::textFor (double db) const
{
    if (LevelMatrix::isSilent (db) || db < minDb)
        return juce::String::fromUTF8 ("-\xE2\x88\x9E");   // -∞

    return juce::String (db, 1);
}

//==============================================================================
void LevelMatrixComponent::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));

    const int x0 = headerWidth + gap;
    const int y0 = headerHeight + gap;

    // column headers
    for (int o = 0; o < matrix.numOutputs(); ++o)
    {
        const bool connected = o < (int) outputConnected.size() ? outputConnected[(size_t) o] : true;
        g.setColour (connected ? Palette::dimText : Palette::dimText.withAlpha (0.45f));
        const juce::Rectangle<int> r (x0 + (o + 1) * (cellWidth + gap), 0, cellWidth, headerHeight);
        const auto name = o < outputNames.size() && outputNames[o].isNotEmpty() ? outputNames[o] : juce::String (o + 1);
        g.drawFittedText (name, r, juce::Justification::centred, 1);

        if (! connected)   // "dog ear": this cue output reaches no device output
        {
            juce::Path ear;
            ear.addTriangle ((float) r.getRight() - 8.0f, (float) r.getY() + 2.0f, (float) r.getRight() - 1.0f, (float) r.getY() + 2.0f, (float) r.getRight() - 1.0f, (float) r.getY() + 9.0f);
            g.setColour (Palette::fadingOut);
            g.fillPath (ear);
        }
    }

    g.setColour (Palette::dimText);

    if (mainVisible)
        g.drawFittedText (ko ("메인"), juce::Rectangle<int> (0, y0, headerWidth - 4, cellHeight), juce::Justification::centredRight, 1);

    for (int i = 0; i < matrix.numInputs(); ++i)
    {
        const juce::Rectangle<int> r (0, y0 + (i + 1) * (cellHeight + gap), headerWidth - 4, cellHeight);
        const auto name = i < inputNames.size() && inputNames[i].isNotEmpty() ? inputNames[i] : ko ("입력 ") + juce::String (i + 1);
        g.drawFittedText (name, r, juce::Justification::centredRight, 1);
    }

    forEachCell ([&] (const CellRef& c)
    {
        const auto r = boundsOf (c);

        if (r.isEmpty())
            return;

        const double value = getValue (c);
        const bool silent = LevelMatrix::isSilent (value) || value < minDb;
        const int gang = gangOf (c);
        juce::Colour fill = c.kind == Kind::cross ? (silent ? Palette::rowOdd : Palette::rowEven.brighter (0.25f)) : Palette::rowEven.brighter (0.1f);

        if (gang > 0)
            fill = gangColours[(gang - 1) % 8].withAlpha (silent ? 0.35f : 0.75f);

        if (! editable)
            fill = fill.withAlpha (0.5f);

        const bool active = isActive (c);

        if (! active)
            fill = fill.withAlpha (0.35f);

        g.setColour (fill);
        g.fillRoundedRectangle (r.toFloat(), 3.0f);

        if (! active)   // hatched: this cell is not part of the fade
        {
            g.setColour (Palette::dimText.withAlpha (0.35f));

            for (int hx = r.getX() - r.getHeight(); hx < r.getRight(); hx += 6)
                g.drawLine ((float) hx, (float) r.getBottom(), (float) hx + (float) r.getHeight(), (float) r.getY(), 1.0f);
        }
        else if (activeMode)
        {
            g.setColour (juce::Colours::yellow.withAlpha (0.8f));
            g.fillRect (r.getX() + 2, r.getY() + 2, 4, 4);
        }

        if (c == selected)
        {
            g.setColour (Palette::standby);
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 1.5f);
        }

        g.setColour (silent ? Palette::dimText : Palette::text);
        g.drawFittedText (textFor (value), r.reduced (3, 0), juce::Justification::centred, 1);
    });
}

void LevelMatrixComponent::resized()
{
    if (typingEditor != nullptr)
        typingEditor->setBounds (boundsOf (selected));
}

//==============================================================================
void LevelMatrixComponent::mouseDown (const juce::MouseEvent& e)
{
    if (typingEditor != nullptr)
        commitTyping();

    const auto cell = cellAt (e.getPosition());
    selected = cell;
    repaint();

    if (! cell.isValid() || ! editable)
        return;

    if (e.mods.isPopupMenu())
    {
        showGangMenu (cell);
        return;
    }

    if (activeMode && e.mods.isAltDown())
    {
        toggleActive (cell);
        return;
    }

    dragCell = cell;
    dragStartValues.clear();
    const int gang = gangOf (cell);

    forEachCell ([&] (const CellRef& c)
    {
        if (c == cell || (gang > 0 && gangOf (c) == gang))
            dragStartValues.push_back ({ c, getValue (c) });
    });

    lastDragDelta = 0.0;
    dragging = false;
}

void LevelMatrixComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragCell.isValid() || ! editable || e.mods.isPopupMenu())
        return;

    const double pixelsPerDb = e.mods.isShiftDown() ? 40.0 : 4.0;
    const double delta = -e.getDistanceFromDragStartY() / pixelsPerDb;

    if (! dragging && std::abs (e.getDistanceFromDragStartY()) < 2)
        return;

    dragging = true;
    applyDelta (delta, false);
}

void LevelMatrixComponent::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
    {
        dragging = false;
        notify (true);
    }

    dragCell = {};
    dragStartValues.clear();
}

void LevelMatrixComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto cell = cellAt (e.getPosition());

    if (! cell.isValid() || ! editable)
        return;

    selected = cell;
    setValue (cell, defaultValue (cell));
    repaint();
    notify (true);
}

void LevelMatrixComponent::applyDelta (double deltaDb, bool finished)
{
    for (const auto& [cell, start] : dragStartValues)
    {
        const double base = LevelMatrix::isSilent (start) ? minDb : start;
        double value = base + deltaDb;

        if (LevelMatrix::isSilent (start) && deltaDb <= 0.0)
            value = LevelMatrix::silentDb;   // silence stays silent when dragged down

        setValue (cell, clampLevel (value));
    }

    lastDragDelta = deltaDb;
    repaint();
    notify (finished);
}

bool LevelMatrixComponent::keyPressed (const juce::KeyPress& key)
{
    if (! selected.isValid() || ! editable)
        return false;

    const auto moveTo = [this] (CellRef c)
    {
        if (c.isValid() && ! boundsOf (c).isEmpty())
        {
            selected = c;
            repaint();
        }
    };

    if (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey || key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
    {
        const int dx = key == juce::KeyPress::leftKey ? -1 : key == juce::KeyPress::rightKey ? 1 : 0;
        const int dy = key == juce::KeyPress::upKey ? -1 : key == juce::KeyPress::downKey ? 1 : 0;

        // grid coordinates: col 0 = main / inputs, row 0 = main / outputs
        int col = selected.kind == Kind::main || selected.kind == Kind::input ? 0 : selected.out + 1;
        int row = selected.kind == Kind::main || selected.kind == Kind::output ? 0 : selected.in + 1;
        col = juce::jlimit (0, matrix.numOutputs(), col + dx);
        row = juce::jlimit (0, matrix.numInputs(), row + dy);

        if (col == 0 && row == 0)
            moveTo (mainVisible ? CellRef { Kind::main, -1, -1 } : selected);
        else if (row == 0)
            moveTo (edgeLevelsVisible ? CellRef { Kind::output, -1, col - 1 } : selected);
        else if (col == 0)
            moveTo (edgeLevelsVisible ? CellRef { Kind::input, row - 1, -1 } : selected);
        else
            moveTo ({ Kind::cross, row - 1, col - 1 });

        return true;
    }

    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        setValue (selected, LevelMatrix::silentDb);
        repaint();
        notify (true);
        return true;
    }

    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::F2Key)
    {
        beginTyping (textFor (getValue (selected)));
        return true;
    }

    const auto ch = key.getTextCharacter();

    if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '+')
    {
        beginTyping (juce::String::charToString (ch));
        return true;
    }

    return false;
}

void LevelMatrixComponent::focusLost (FocusChangeType)
{
    if (typingEditor != nullptr && ! typingEditor->hasKeyboardFocus (true))
        commitTyping();
}

void LevelMatrixComponent::beginTyping (const juce::String& initial)
{
    if (! selected.isValid())
        return;

    typingEditor = std::make_unique<juce::TextEditor>();
    typingEditor->setInputRestrictions (8, "0123456789.-+");
    typingEditor->setJustification (juce::Justification::centred);
    typingEditor->setText (initial.startsWithChar ('-') && initial.length() > 1 && ! juce::CharacterFunctions::isDigit (initial[1]) ? juce::String() : initial, false);
    typingEditor->setCaretPosition (typingEditor->getText().length());
    typingEditor->onReturnKey = [this] { commitTyping(); };
    typingEditor->onEscapeKey = [this] { typingEditor.reset(); grabKeyboardFocus(); };
    typingEditor->onFocusLost = [this] { commitTyping(); };
    addAndMakeVisible (*typingEditor);
    typingEditor->setBounds (boundsOf (selected));
    typingEditor->grabKeyboardFocus();
}

void LevelMatrixComponent::commitTyping()
{
    if (typingEditor == nullptr)
        return;

    const auto text = typingEditor->getText().trim();
    typingEditor.reset();

    if (! selected.isValid())
        return;

    double value;

    if (text.isEmpty() || text == "-" || text.containsIgnoreCase ("inf"))
        value = LevelMatrix::silentDb;
    else
    {
        value = text.getDoubleValue();

        if (! text.startsWithChar ('-') && ! text.startsWithChar ('+') && value > 0.0)
            value = -value;   // QLab: a bare number in the matrix means below unity
    }

    setValue (selected, clampLevel (value));
    repaint();
    grabKeyboardFocus();
    notify (true);
}

void LevelMatrixComponent::showGangMenu (const CellRef& cell)
{
    juce::PopupMenu menu;
    const int current = gangOf (cell);
    menu.addItem (100, ko ("겡 없음"), true, current == 0);
    menu.addSeparator();

    for (int g = 1; g <= LevelMatrix::maxGang; ++g)
        menu.addItem (100 + g, ko ("겡 ") + juce::String (g), true, current == g);

    menu.addSeparator();
    menu.addItem (200, ko ("기본값으로 (더블클릭)"));
    menu.addItem (201, ko ("무음 (-\xE2\x88\x9E)"));

    if (activeMode)
    {
        menu.addSeparator();
        menu.addItem (300, isActive (cell) ? ko ("이 칸 비활성 (페이드에서 제외)") : ko ("이 칸 활성 (페이드에 포함)  — Alt+클릭"));
    }

    juce::Component::SafePointer<LevelMatrixComponent> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (boundsOf (cell))),
                        [safeThis, cell] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;

        if (result >= 100 && result <= 100 + LevelMatrix::maxGang)
            safeThis->setGang (cell, result - 100);
        else if (result == 200)
            safeThis->setValue (cell, safeThis->defaultValue (cell));
        else if (result == 201)
            safeThis->setValue (cell, LevelMatrix::silentDb);
        else if (result == 300)
        {
            safeThis->toggleActive (cell);
            return;
        }

        safeThis->repaint();
        safeThis->notify (true);
    });
}

void LevelMatrixComponent::notify (bool finished)
{
    if (onChange)
        onChange (mainDb, matrix, finished);
}

} // namespace gocue
