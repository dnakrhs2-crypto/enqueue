#include "ui/WaveformView.h"

#include "ui/UiUtils.h"

#include <cmath>

namespace gocue
{

namespace
{
    constexpr int rulerHeight = 18;
    constexpr int scrollbarHeight = 10;
    constexpr int handleSize = 9;
    constexpr float pointRadius = 5.0f;
    constexpr float hitRadius = 7.0f;

    const juce::Colour waveColour   { 0xff7fa7d9 };
    const juce::Colour waveDim      { 0xff44546a };
    const juce::Colour envelopeCol  { 0xffe6c229 };
    const juce::Colour handleCol    { 0xffb0b0b8 };
    const juce::Colour cursorCol    { 0xffd0d0d8 };
    const juce::Colour playheadCol  { 0xffff5555 };

    double niceTickInterval (double secondsPerPixel, int minPixels)
    {
        static const double candidates[] = { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0 };

        for (const double c : candidates)
            if (c / secondsPerPixel >= minPixels)
                return c;

        return 600.0;
    }
}

WaveformView::WaveformView (juce::AudioFormatManager& f, juce::AudioThumbnailCache& cache)
    : formats (f), thumbnail (512, f, cache)
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (true);
    thumbnail.addChangeListener (this);

    scrollbar.setAutoHide (false);
    scrollbar.addListener (this);
    addAndMakeVisible (scrollbar);
}

WaveformView::~WaveformView()
{
    scrollbar.removeListener (this);
    thumbnail.removeChangeListener (this);
    thumbnail.setSource (nullptr);
}

//==============================================================================
void WaveformView::setCue (const Cue* newCue)
{
    const bool fileChanged = newCue == nullptr ? loadedFile != juce::File() : newCue->file != loadedFile;
    const bool cueChanged = newCue == nullptr ? hasCue : (! hasCue || newCue->id != cue.id);

    if (newCue == nullptr)
    {
        hasCue = false;
        cue = Cue();
    }
    else
    {
        hasCue = true;
        cue = *newCue;
    }

    if (fileChanged)
    {
        loadedFile = hasCue ? cue.file : juce::File();

        if (loadedFile.existsAsFile())
            thumbnail.setSource (new juce::FileInputSource (loadedFile));
        else
            thumbnail.setSource (nullptr);

        zoomToFit();
    }

    if (cueChanged)
    {
        selectedPoint = -1;
        hoverPoint = -1;
        cursor = juce::jlimit (0.0, fileLength(), regionStart());

        if (! fileChanged)
            zoomToFit();
    }

    if (selectedPoint >= (int) cue.audio.envelope.points.size())
        selectedPoint = -1;

    repaint();
}

void WaveformView::setPlayhead (double filePositionSeconds, bool isPlaying)
{
    if (juce::approximatelyEqual (playhead, filePositionSeconds) && playing == isPlaying)
        return;

    playhead = filePositionSeconds;
    playing = isPlaying;
    repaint (waveArea);
}

void WaveformView::setViewChannel (int channel)
{
    viewChannel = channel;
    repaint();
}

void WaveformView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (thumbnail.getTotalLength() > 0.0 && viewEnd <= viewStart + 1e-9)
        zoomToFit();

    repaint();
}

//==============================================================================
double WaveformView::fileLength() const noexcept
{
    if (hasCue && cue.durationSeconds > 0.0)
        return cue.durationSeconds;

    return juce::jmax (0.0, thumbnail.getTotalLength());
}

double WaveformView::regionEnd() const noexcept
{
    const double length = fileLength();

    if (cue.audio.endSeconds >= 0.0)
        return length > 0.0 ? juce::jmin (cue.audio.endSeconds, length) : cue.audio.endSeconds;

    return length;
}

float WaveformView::xForTime (double seconds) const noexcept
{
    const double span = juce::jmax (1e-9, viewEnd - viewStart);
    return (float) waveArea.getX() + (float) ((seconds - viewStart) / span * waveArea.getWidth());
}

double WaveformView::timeForX (float x) const noexcept
{
    const double span = juce::jmax (1e-9, viewEnd - viewStart);
    return viewStart + (double) ((x - (float) waveArea.getX()) / (float) juce::jmax (1, waveArea.getWidth())) * span;
}

float WaveformView::yForLevel (double level) const noexcept
{
    return (float) waveArea.getBottom() - (float) (juce::jlimit (0.0, 1.0, level) * waveArea.getHeight());
}

double WaveformView::levelForY (float y) const noexcept
{
    return juce::jlimit (0.0, 1.0, (double) ((float) waveArea.getBottom() - y) / (double) juce::jmax (1, waveArea.getHeight()));
}

juce::Point<float> WaveformView::pointPosition (const EnvelopePoint& p) const noexcept
{
    const double seconds = regionStart() + cue.audio.envelope.toSeconds (p.x, regionLength());
    return { xForTime (seconds), yForLevel (p.level) };
}

int WaveformView::findPointNear (juce::Point<float> position, float radius) const noexcept
{
    int best = -1;
    float bestDistance = radius;
    const auto& points = cue.audio.envelope.points;

    for (int i = 0; i < (int) points.size(); ++i)
    {
        const float d = pointPosition (points[(size_t) i]).getDistanceFrom (position);

        if (d <= bestDistance)
        {
            best = i;
            bestDistance = d;
        }
    }

    return best;
}

float WaveformView::distanceToEnvelope (juce::Point<float> position) const noexcept
{
    if (! envelopeEditable())
        return 1.0e9f;

    const double seconds = timeForX (position.x) - regionStart();
    const float y = yForLevel (cue.audio.envelope.levelAt (seconds, regionLength()));
    return std::abs (y - position.y);
}

//==============================================================================
void WaveformView::setView (double start, double end)
{
    const double length = juce::jmax (0.001, fileLength());
    double span = juce::jlimit (0.02, length, end - start);
    start = juce::jlimit (0.0, length - span, start);
    viewStart = start;
    viewEnd = start + span;

    scrollbar.setRangeLimits ({ 0.0, length }, juce::dontSendNotification);
    scrollbar.setCurrentRange ({ viewStart, viewEnd }, juce::dontSendNotification);
    repaint();
}

void WaveformView::zoomAround (double factor, double anchorSeconds)
{
    const double span = (viewEnd - viewStart) * factor;
    const double fraction = (viewEnd - viewStart) > 0.0 ? (anchorSeconds - viewStart) / (viewEnd - viewStart) : 0.5;
    setView (anchorSeconds - fraction * span, anchorSeconds + (1.0 - fraction) * span);
}

void WaveformView::zoomIn()  { zoomAround (0.5, (viewStart + viewEnd) * 0.5); }
void WaveformView::zoomOut() { zoomAround (2.0, (viewStart + viewEnd) * 0.5); }
void WaveformView::zoomToFit() { setView (0.0, juce::jmax (0.001, fileLength())); }

void WaveformView::zoomToRegion()
{
    if (regionLength() > 0.0)
    {
        const double margin = regionLength() * 0.05;
        setView (regionStart() - margin, regionEnd() + margin);
    }
}

void WaveformView::scrollBarMoved (juce::ScrollBar*, double newRangeStart)
{
    const double span = viewEnd - viewStart;
    viewStart = newRangeStart;
    viewEnd = newRangeStart + span;
    repaint();
}

void WaveformView::resized()
{
    auto area = getLocalBounds();
    rulerArea = area.removeFromTop (rulerHeight);
    scrollbar.setBounds (area.removeFromBottom (scrollbarHeight));
    area.removeFromBottom (handleSize + 2);
    waveArea = area;
    setView (viewStart, viewEnd);
}

//==============================================================================
void WaveformView::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);

    if (! hasCue || loadedFile == juce::File())
    {
        g.setColour (Palette::dimText);
        g.setFont (juce::Font (juce::FontOptions (14.0f)));
        g.drawText (hasCue ? ko ("파일 없음") : ko ("선택된 큐 없음"), getLocalBounds(), juce::Justification::centred);
        return;
    }

    drawRuler (g);

    // waveform: the trimmed-away parts are dimmed
    const float xs = xForTime (regionStart());
    const float xe = xForTime (regionEnd());
    const auto region = juce::Rectangle<float> (xs, (float) waveArea.getY(), juce::jmax (0.0f, xe - xs), (float) waveArea.getHeight())
                            .getIntersection (waveArea.toFloat());

    g.setColour (Palette::rowEven);
    g.fillRect (region);

    if (thumbnail.getTotalLength() > 0.0)
    {
        const int numChannels = thumbnail.getNumChannels();

        auto drawChannels = [&] (const juce::Colour& colour, juce::Rectangle<int> clip)
        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (clip);
            g.setColour (colour);

            if (viewChannel >= 0 && viewChannel < numChannels)
            {
                thumbnail.drawChannel (g, waveArea, viewStart, viewEnd, viewChannel, 1.0f);
            }
            else
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    thumbnail.drawChannel (g, waveArea, viewStart, viewEnd, ch, 1.0f);
            }
        };

        drawChannels (waveDim, waveArea);
        drawChannels (waveColour, region.getSmallestIntegerContainer());
    }
    else if (loadedFile.existsAsFile())
    {
        g.setColour (Palette::dimText);
        g.drawText (ko ("파형 읽는 중..."), waveArea, juce::Justification::centred);
    }
    else
    {
        g.setColour (Palette::missing);
        g.drawText (ko ("[없음] ") + loadedFile.getFullPathName(), waveArea, juce::Justification::centred);
    }

    // centre line
    g.setColour (Palette::outline);
    g.drawHorizontalLine (waveArea.getCentreY(), (float) waveArea.getX(), (float) waveArea.getRight());

    // cursor
    if (cursor >= viewStart && cursor <= viewEnd)
    {
        g.setColour (cursorCol.withAlpha (0.6f));
        const float x = xForTime (cursor);
        g.drawLine (x, (float) waveArea.getY(), x, (float) waveArea.getBottom(), 1.0f);
    }

    drawEnvelope (g);
    drawHandles (g);

    // playhead
    if (playhead >= 0.0 && playhead >= viewStart && playhead <= viewEnd)
    {
        g.setColour (playing ? playheadCol : playheadCol.withAlpha (0.5f));
        const float x = xForTime (playhead);
        g.drawLine (x, (float) waveArea.getY(), x, (float) waveArea.getBottom(), 2.0f);
    }

    g.setColour (Palette::outline);
    g.drawRect (waveArea);

    if (hasKeyboardFocus (true))
    {
        g.setColour (Palette::standby.withAlpha (0.8f));
        g.drawRect (getLocalBounds(), 1);
    }
}

void WaveformView::drawRuler (juce::Graphics& g) const
{
    g.setColour (Palette::panel);
    g.fillRect (rulerArea);

    const double secondsPerPixel = (viewEnd - viewStart) / juce::jmax (1, waveArea.getWidth());
    const double interval = niceTickInterval (secondsPerPixel, 70);
    const double minor = interval / 5.0;

    g.setFont (juce::Font (juce::FontOptions (10.0f)));

    for (double t = std::floor (viewStart / minor) * minor; t <= viewEnd + 1e-9; t += minor)
    {
        const float x = xForTime (t);

        if (x < (float) waveArea.getX() - 1.0f || x > (float) waveArea.getRight() + 1.0f)
            continue;

        const bool major = std::abs (t / interval - std::round (t / interval)) < 1e-6;
        g.setColour (major ? Palette::dimText : Palette::outline);
        g.drawLine (x, (float) (rulerArea.getBottom() - (major ? 8 : 4)), x, (float) rulerArea.getBottom(), 1.0f);

        if (major)
        {
            g.setColour (Palette::dimText);
            g.drawText (formatTimeMs (t, interval < 1.0), (int) x + 3, rulerArea.getY(), 80, rulerArea.getHeight() - 2,
                        juce::Justification::centredLeft, false);
        }
    }
}

void WaveformView::drawEnvelope (juce::Graphics& g) const
{
    if (! hasCue || ! cue.audio.envelope.enabled || regionLength() <= 0.0)
        return;

    const auto& envelope = cue.audio.envelope;
    const double length = regionLength();
    const float xs = juce::jmax ((float) waveArea.getX(), xForTime (regionStart()));
    const float xe = juce::jmin ((float) waveArea.getRight(), xForTime (regionEnd()));

    if (xe <= xs)
        return;

    juce::Path path;
    bool started = false;

    for (float x = xs; x <= xe; x += 1.0f)
    {
        const double seconds = timeForX (x) - regionStart();
        const float y = yForLevel (envelope.levelAt (seconds, length));

        if (! started)
        {
            path.startNewSubPath (x, y);
            started = true;
        }
        else
        {
            path.lineTo (x, y);
        }
    }

    g.setColour (envelopeCol.withAlpha (0.9f));
    g.strokePath (path, juce::PathStrokeType (2.0f));

    for (int i = 0; i < (int) envelope.points.size(); ++i)
    {
        const auto position = pointPosition (envelope.points[(size_t) i]);

        if (position.x < (float) waveArea.getX() - pointRadius || position.x > (float) waveArea.getRight() + pointRadius)
            continue;

        const bool selected = i == selectedPoint;
        const bool hovered = i == hoverPoint;
        const float radius = selected ? pointRadius + 1.5f : pointRadius;

        g.setColour (selected ? juce::Colours::white : (hovered ? envelopeCol.brighter (0.4f) : envelopeCol));
        g.fillEllipse (position.x - radius, position.y - radius, radius * 2.0f, radius * 2.0f);
        g.setColour (Palette::background);
        g.drawEllipse (position.x - radius, position.y - radius, radius * 2.0f, radius * 2.0f, 1.0f);
    }
}

void WaveformView::drawHandles (juce::Graphics& g) const
{
    if (! hasCue)
        return;

    auto drawHandle = [&] (double seconds, bool isStart, bool hovered)
    {
        const float x = xForTime (seconds);

        if (x < (float) waveArea.getX() - handleSize || x > (float) waveArea.getRight() + handleSize)
            return;

        g.setColour (hovered ? juce::Colours::white : handleCol);
        g.drawLine (x, (float) waveArea.getY(), x, (float) waveArea.getBottom(), hovered ? 2.0f : 1.0f);

        juce::Path triangle;
        const float top = (float) waveArea.getBottom() + 1.0f;
        const float bottom = top + (float) handleSize;
        const float dir = isStart ? 1.0f : -1.0f;
        triangle.addTriangle (x, top, x, bottom, x + dir * (float) handleSize, bottom);
        g.fillPath (triangle);
    };

    drawHandle (regionStart(), true, hoverStart || drag == Drag::startHandle);
    drawHandle (regionEnd(), false, hoverEnd || drag == Drag::endHandle);
}

//==============================================================================
void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    if (! hasCue)
        return;

    const auto position = e.position;
    const bool wasStart = hoverStart, wasEnd = hoverEnd;
    const int wasPoint = hoverPoint;

    hoverPoint = envelopeEditable() ? findPointNear (position, hitRadius) : -1;
    hoverStart = hoverPoint < 0 && std::abs (position.x - xForTime (regionStart())) <= hitRadius;
    hoverEnd = hoverPoint < 0 && ! hoverStart && std::abs (position.x - xForTime (regionEnd())) <= hitRadius;

    if (hoverStart || hoverEnd)
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else if (hoverPoint >= 0)
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    else if (envelopeEditable() && distanceToEnvelope (position) <= hitRadius)
        setMouseCursor (juce::MouseCursor::CrosshairCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);

    if (wasStart != hoverStart || wasEnd != hoverEnd || wasPoint != hoverPoint)
        repaint();
}

void WaveformView::mouseExit (const juce::MouseEvent&)
{
    hoverStart = hoverEnd = false;
    hoverPoint = -1;
    repaint();
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();

    if (! hasCue)
        return;

    if (e.mods.isPopupMenu())
    {
        if (onContextMenu)
            onContextMenu (e.getScreenPosition());

        return;
    }

    if (! waveArea.expanded (0, handleSize + 2).contains (e.getPosition()) && ! rulerArea.contains (e.getPosition()))
        return;

    mouseMove (e);

    if (hoverStart)
    {
        drag = Drag::startHandle;
        return;
    }

    if (hoverEnd)
    {
        drag = Drag::endHandle;
        return;
    }

    if (envelopeEditable())
    {
        if (hoverPoint >= 0)
        {
            selectedPoint = hoverPoint;
            drag = Drag::envelopePoint;
            repaint();
            return;
        }

        if (distanceToEnvelope (e.position) <= hitRadius)
        {
            // click on the curve: add a point there
            const double seconds = juce::jlimit (0.0, regionLength(), timeForX (e.position.x) - regionStart());
            EnvelopePoint p;
            p.x = cue.audio.envelope.toX (seconds, regionLength());
            p.level = levelForY (e.position.y);
            cue.audio.envelope.points.push_back (p);
            cue.audio.envelope.sanitise();

            selectedPoint = -1;

            for (int i = 0; i < (int) cue.audio.envelope.points.size(); ++i)
                if (juce::approximatelyEqual (cue.audio.envelope.points[(size_t) i].x, p.x))
                    selectedPoint = i;

            drag = Drag::envelopePoint;
            commitEnvelope (false);
            return;
        }
    }

    cursor = juce::jlimit (0.0, fileLength(), timeForX (e.position.x));
    selectedPoint = -1;
    repaint();
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (! hasCue || drag == Drag::none)
        return;

    const double t = timeForX (e.position.x);

    if (drag == Drag::startHandle)
    {
        const double newStart = juce::jlimit (0.0, juce::jmax (0.0, regionEnd() - minRegionSeconds), t);
        cue.audio.startSeconds = newStart;

        if (onTrimChanged)
            onTrimChanged (cue.audio.startSeconds, cue.audio.endSeconds, false);
    }
    else if (drag == Drag::endHandle)
    {
        const double length = fileLength();
        const double newEnd = juce::jlimit (regionStart() + minRegionSeconds, length > 0.0 ? length : t, t);
        cue.audio.endSeconds = (length > 0.0 && newEnd >= length - 1e-6) ? -1.0 : newEnd;

        if (onTrimChanged)
            onTrimChanged (cue.audio.startSeconds, cue.audio.endSeconds, false);
    }
    else if (drag == Drag::envelopePoint && selectedPoint >= 0)
    {
        auto& points = cue.audio.envelope.points;
        const auto& envelope = cue.audio.envelope;
        const double length = regionLength();
        auto& p = points[(size_t) selectedPoint];

        double seconds = juce::jlimit (0.0, length, t - regionStart());
        double x = envelope.toX (seconds, length);

        if (selectedPoint > 0)
            x = juce::jmax (x, points[(size_t) selectedPoint - 1].x);

        if (selectedPoint + 1 < (int) points.size())
            x = juce::jmin (x, points[(size_t) selectedPoint + 1].x);

        p.x = x;
        p.level = levelForY (e.position.y);
        commitEnvelope (false);
    }

    repaint();
}

void WaveformView::mouseUp (const juce::MouseEvent&)
{
    if (drag == Drag::startHandle || drag == Drag::endHandle)
    {
        if (onTrimChanged)
            onTrimChanged (cue.audio.startSeconds, cue.audio.endSeconds, true);
    }
    else if (drag == Drag::envelopePoint)
    {
        commitEnvelope (true);
    }

    drag = Drag::none;
    repaint();
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! hasCue)
        return;

    if (e.mods.isAltDown() || e.mods.isCtrlDown())
    {
        zoomAround (wheel.deltaY > 0.0f ? 0.8 : 1.25, timeForX (e.position.x));
    }
    else
    {
        const double span = viewEnd - viewStart;
        const float delta = wheel.deltaX != 0.0f ? wheel.deltaX : wheel.deltaY;
        setView (viewStart - delta * span * 0.5, viewEnd - delta * span * 0.5);
    }
}

void WaveformView::commitEnvelope (bool finished)
{
    if (onEnvelopeChanged)
        onEnvelopeChanged (cue.audio.envelope, finished);
}

void WaveformView::moveSelectedPoint (double deltaSeconds, double deltaLevel)
{
    auto& points = cue.audio.envelope.points;

    if (selectedPoint < 0 || selectedPoint >= (int) points.size())
        return;

    const auto& envelope = cue.audio.envelope;
    const double length = regionLength();
    auto& p = points[(size_t) selectedPoint];

    double seconds = juce::jlimit (0.0, length, envelope.toSeconds (p.x, length) + deltaSeconds);
    double x = envelope.toX (seconds, length);

    if (selectedPoint > 0)
        x = juce::jmax (x, points[(size_t) selectedPoint - 1].x);

    if (selectedPoint + 1 < (int) points.size())
        x = juce::jmin (x, points[(size_t) selectedPoint + 1].x);

    p.x = x;
    p.level = juce::jlimit (0.0, 1.0, p.level + deltaLevel);
    commitEnvelope (true);
    repaint();
}

bool WaveformView::keyPressed (const juce::KeyPress& key)
{
    if (! hasCue)
        return false;

    const auto mods = key.getModifiers();

    if (key.isKeyCode ('I') && mods.isShiftDown() && ! mods.isCtrlDown())
    {
        const double newStart = juce::jlimit (0.0, juce::jmax (0.0, regionEnd() - minRegionSeconds), cursor);
        cue.audio.startSeconds = newStart;

        if (onTrimChanged)
            onTrimChanged (cue.audio.startSeconds, cue.audio.endSeconds, true);

        repaint();
        return true;
    }

    if (key.isKeyCode ('O') && mods.isShiftDown() && ! mods.isCtrlDown())
    {
        const double length = fileLength();
        const double newEnd = juce::jmax (regionStart() + minRegionSeconds, cursor);
        cue.audio.endSeconds = (length > 0.0 && newEnd >= length - 1e-6) ? -1.0 : newEnd;

        if (onTrimChanged)
            onTrimChanged (cue.audio.startSeconds, cue.audio.endSeconds, true);

        repaint();
        return true;
    }

    if (key == juce::KeyPress ('=', juce::ModifierKeys::ctrlModifier, 0) || key == juce::KeyPress ('+', juce::ModifierKeys::ctrlModifier, 0))
    {
        zoomIn();
        return true;
    }

    if (key == juce::KeyPress ('-', juce::ModifierKeys::ctrlModifier, 0))
    {
        zoomOut();
        return true;
    }

    if (! envelopeEditable())
        return false;

    const int numPoints = (int) cue.audio.envelope.points.size();

    if (mods.isAltDown() && numPoints > 0 && selectedPoint >= 0)
    {
        const double timeStep = mods.isShiftDown() ? 0.001 : 0.01;
        const double levelStep = mods.isShiftDown() ? 0.005 : 0.02;

        if (key.isKeyCode (juce::KeyPress::leftKey))  { moveSelectedPoint (-timeStep, 0.0); return true; }
        if (key.isKeyCode (juce::KeyPress::rightKey)) { moveSelectedPoint (timeStep, 0.0); return true; }
        if (key.isKeyCode (juce::KeyPress::upKey))    { moveSelectedPoint (0.0, levelStep); return true; }
        if (key.isKeyCode (juce::KeyPress::downKey))  { moveSelectedPoint (0.0, -levelStep); return true; }
    }

    if (! mods.isAnyModifierKeyDown() && numPoints > 0)
    {
        if (key.isKeyCode (juce::KeyPress::leftKey))
        {
            selectedPoint = selectedPoint < 0 ? numPoints - 1 : juce::jmax (0, selectedPoint - 1);
            repaint();
            return true;
        }

        if (key.isKeyCode (juce::KeyPress::rightKey))
        {
            selectedPoint = selectedPoint < 0 ? 0 : juce::jmin (numPoints - 1, selectedPoint + 1);
            repaint();
            return true;
        }

        if ((key.isKeyCode (juce::KeyPress::deleteKey) || key.isKeyCode (juce::KeyPress::backspaceKey)) && selectedPoint >= 0)
        {
            cue.audio.envelope.points.erase (cue.audio.envelope.points.begin() + selectedPoint);
            selectedPoint = -1;
            commitEnvelope (true);
            repaint();
            return true;
        }
    }

    return false;
}

} // namespace gocue
