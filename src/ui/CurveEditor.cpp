#include "ui/CurveEditor.h"

#include "ui/UiUtils.h"

#include <cmath>

namespace gocue
{

CurveEditor::CurveEditor()
{
    auto label = [this] (juce::Label& l, const char* text)
    {
        l.setText (ko (text), juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, Palette::dimText);
        l.setFont (juce::Font (juce::FontOptions (13.0f)));
        addAndMakeVisible (l);
    };

    label (shapeLabel, "모양");
    label (intensityLabel, "강도");
    label (domainLabel, "오디오 도메인");
    label (hint, "S커브 = 부드러운 시작·끝 · 파라메트릭 = 강도(1 = 직선, 클수록 늦게 출발; 대칭 켜면 양끝 완만) · 커스텀 = 캔버스 클릭으로 점 추가, 드래그 이동, 더블클릭/Delete 삭제. 이퀄파워 = 파라메트릭 0.5 + 리니어, 이퀄게인 = 직선 + 리니어");
    hint.setFont (juce::Font (juce::FontOptions (11.0f)));

    shapeBox.addItem (ko ("S커브"), 1);
    shapeBox.addItem (ko ("파라메트릭"), 2);
    shapeBox.addItem (ko ("직선"), 3);
    shapeBox.addItem (ko ("커스텀"), 4);
    shapeBox.setWantsKeyboardFocus (false);
    shapeBox.onChange = [this]
    {
        if (refreshing || shapeBox.getSelectedId() == 0)
            return;

        curve.shape = (CurveShape) (shapeBox.getSelectedId() - 1);

        if (curve.shape == CurveShape::custom && curve.points.size() < 2)
            curve.setDefaultPoints();

        curve.sanitise();
        updateControls();
        notify (true);
    };
    addAndMakeVisible (shapeBox);

    domainBox.addItem (ko ("슬라이더 (페이더 느낌)"), 1);
    domainBox.addItem (ko ("데시벨"), 2);
    domainBox.addItem (ko ("리니어 (진폭)"), 3);
    domainBox.setWantsKeyboardFocus (false);
    domainBox.onChange = [this]
    {
        if (refreshing || domainBox.getSelectedId() == 0)
            return;

        curve.domain = (AudioDomain) (domainBox.getSelectedId() - 1);
        notify (true);
    };
    addAndMakeVisible (domainBox);

    intensitySlider.setRange (FadeCurve::minIntensity, FadeCurve::maxIntensity, 0.1);
    intensitySlider.setSkewFactorFromMidPoint (1.0);
    intensitySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);
    intensitySlider.setDoubleClickReturnValue (true, 2.0);
    intensitySlider.setWantsKeyboardFocus (false);
    intensitySlider.onValueChange = [this]
    {
        if (refreshing)
            return;

        curve.intensity = intensitySlider.getValue();
        canvas.repaint();
        notify (! intensitySlider.isMouseButtonDown());
    };
    intensitySlider.onDragEnd = [this] { notify (true); };
    addAndMakeVisible (intensitySlider);

    mirrorToggle.setButtonText (ko ("대칭"));
    mirrorToggle.setColour (juce::ToggleButton::textColourId, Palette::text);
    mirrorToggle.setColour (juce::ToggleButton::tickColourId, Palette::standby);
    mirrorToggle.setWantsKeyboardFocus (false);
    mirrorToggle.onClick = [this]
    {
        if (refreshing)
            return;

        curve.mirror = mirrorToggle.getToggleState();
        curve.sanitise();
        canvas.repaint();
        notify (true);
    };
    addAndMakeVisible (mirrorToggle);

    resetButton.setButtonText (ko ("기본 모양으로"));
    resetButton.setWantsKeyboardFocus (false);
    resetButton.onClick = [this]
    {
        curve.setDefaultPoints();
        curve.intensity = 2.0;
        curve.sanitise();
        updateControls();
        notify (true);
    };
    addAndMakeVisible (resetButton);

    addAndMakeVisible (canvas);
    updateControls();
}

CurveEditor::~CurveEditor() = default;

void CurveEditor::setCurve (const FadeCurve& newCurve)
{
    curve = newCurve;
    curve.sanitise();
    updateControls();
}

void CurveEditor::setEditable (bool shouldBeEditable)
{
    editable = shouldBeEditable;

    for (auto* c : std::initializer_list<juce::Component*> { &shapeBox, &domainBox, &intensitySlider, &mirrorToggle, &resetButton, &canvas })
        c->setEnabled (editable);
}

void CurveEditor::updateControls()
{
    const juce::ScopedValueSetter<bool> guard (refreshing, true);
    shapeBox.setSelectedId ((int) curve.shape + 1, juce::dontSendNotification);
    domainBox.setSelectedId ((int) curve.domain + 1, juce::dontSendNotification);
    intensitySlider.setValue (curve.intensity, juce::dontSendNotification);
    intensitySlider.setEnabled (editable && curve.shape == CurveShape::parametric);
    mirrorToggle.setToggleState (curve.mirror, juce::dontSendNotification);
    mirrorToggle.setEnabled (editable && (curve.shape == CurveShape::parametric || curve.shape == CurveShape::custom));
    resetButton.setEnabled (editable && (curve.shape == CurveShape::custom || curve.shape == CurveShape::parametric));
    canvas.repaint();
}

void CurveEditor::notify (bool finished)
{
    if (onChange)
        onChange (curve, finished);
}

void CurveEditor::resized()
{
    auto area = getLocalBounds().reduced (12, 6);
    auto row = area.removeFromTop (24);
    shapeLabel.setBounds (row.removeFromLeft (36));
    shapeBox.setBounds (row.removeFromLeft (120));
    row.removeFromLeft (12);
    intensityLabel.setBounds (row.removeFromLeft (36));
    intensitySlider.setBounds (row.removeFromLeft (200));
    row.removeFromLeft (8);
    mirrorToggle.setBounds (row.removeFromLeft (64));
    row.removeFromLeft (12);
    domainLabel.setBounds (row.removeFromLeft (90));
    domainBox.setBounds (row.removeFromLeft (170));
    row.removeFromLeft (12);
    resetButton.setBounds (row.removeFromLeft (110));
    area.removeFromTop (4);
    hint.setBounds (area.removeFromTop (16));
    area.removeFromTop (4);
    canvas.setBounds (area);
}

//==============================================================================
juce::Point<float> CurveEditor::Canvas::toScreen (double x, double y) const
{
    const auto r = getLocalBounds().toFloat().reduced (10.0f);
    return { r.getX() + (float) x * r.getWidth(), r.getBottom() - (float) y * r.getHeight() };
}

juce::Point<double> CurveEditor::Canvas::toCurve (juce::Point<float> p) const
{
    const auto r = getLocalBounds().toFloat().reduced (10.0f);
    return { juce::jlimit (0.0, 1.0, (double) ((p.x - r.getX()) / juce::jmax (1.0f, r.getWidth()))),
             juce::jlimit (0.0, 1.0, (double) ((r.getBottom() - p.y) / juce::jmax (1.0f, r.getHeight()))) };
}

int CurveEditor::Canvas::hitPoint (juce::Point<float> p) const
{
    const auto& points = owner.curve.points;

    for (int i = 0; i < (int) points.size(); ++i)
        if (toScreen (points[(size_t) i].x, points[(size_t) i].y).getDistanceFrom (p) < 9.0f)
            return i;

    return -1;
}

void CurveEditor::Canvas::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (Palette::rowOdd);
    g.fillRoundedRectangle (bounds, 4.0f);
    const auto r = bounds.reduced (10.0f);

    g.setColour (Palette::outline);

    for (int i = 1; i < 4; ++i)
    {
        const float fx = r.getX() + r.getWidth() * (float) i / 4.0f;
        const float fy = r.getY() + r.getHeight() * (float) i / 4.0f;
        g.drawLine (fx, r.getY(), fx, r.getBottom(), 0.5f);
        g.drawLine (r.getX(), fy, r.getRight(), fy, 0.5f);
    }

    g.drawRect (r, 1.0f);

    // the curve
    juce::Path path;
    const int steps = juce::jmax (16, (int) r.getWidth());

    for (int i = 0; i <= steps; ++i)
    {
        const double t = (double) i / (double) steps;
        const auto p = toScreen (t, owner.curve.completion (t));

        if (i == 0)
            path.startNewSubPath (p);
        else
            path.lineTo (p);
    }

    g.setColour (isEnabled() ? juce::Colours::yellow.withAlpha (0.9f) : Palette::dimText);
    g.strokePath (path, juce::PathStrokeType (2.0f));

    if (owner.curve.shape == CurveShape::custom)
    {
        const auto& points = owner.curve.points;

        for (int i = 0; i < (int) points.size(); ++i)
        {
            const auto p = toScreen (points[(size_t) i].x, points[(size_t) i].y);
            g.setColour (i == selected ? Palette::standby : juce::Colours::yellow);
            g.fillEllipse (p.x - 5.0f, p.y - 5.0f, 10.0f, 10.0f);
        }
    }

    g.setColour (Palette::dimText);
    g.setFont (juce::Font (juce::FontOptions (11.0f)));
    g.drawText (ko ("시간 →"), r.withTrimmedTop (r.getHeight() - 14.0f).withTrimmedLeft (r.getWidth() - 50.0f).toNearestInt(), juce::Justification::centredRight, false);
    g.drawText (ko ("완료 ↑"), juce::Rectangle<int> ((int) r.getX() + 2, (int) r.getY() + 2, 44, 14), juce::Justification::centredLeft, false);
}

void CurveEditor::Canvas::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();

    if (! isEnabled() || owner.curve.shape != CurveShape::custom)
        return;

    const auto p = e.position;
    selected = hitPoint (p);

    if (selected < 0 && ! e.mods.isPopupMenu())
    {
        const auto c = toCurve (p);
        owner.curve.addPoint (c.x, c.y);

        // find the point we just added (sanitise may have re-sorted / mirrored)
        selected = hitPoint (p);
    }

    dragging = selected > 0 && selected < (int) owner.curve.points.size() - 1;
    repaint();
}

void CurveEditor::Canvas::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging || ! isEnabled())
        return;

    const auto c = toCurve (e.position);
    owner.curve.movePoint (selected, c.x, c.y);
    selected = hitPoint (e.position) >= 0 ? hitPoint (e.position) : selected;
    repaint();
    owner.notify (false);
}

void CurveEditor::Canvas::mouseUp (const juce::MouseEvent&)
{
    if (! isEnabled() || owner.curve.shape != CurveShape::custom)
        return;

    dragging = false;
    owner.notify (true);
}

void CurveEditor::Canvas::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! isEnabled() || owner.curve.shape != CurveShape::custom)
        return;

    const int hit = hitPoint (e.position);

    if (hit > 0 && hit < (int) owner.curve.points.size() - 1)
    {
        owner.curve.removePoint (hit);
        selected = -1;
        repaint();
        owner.notify (true);
    }
}

bool CurveEditor::Canvas::keyPressed (const juce::KeyPress& key)
{
    if (! isEnabled() || owner.curve.shape != CurveShape::custom || selected <= 0 || selected >= (int) owner.curve.points.size() - 1)
        return false;

    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        owner.curve.removePoint (selected);
        selected = -1;
        repaint();
        owner.notify (true);
        return true;
    }

    return false;
}

} // namespace gocue
