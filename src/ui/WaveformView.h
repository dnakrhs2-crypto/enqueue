#pragma once

#include "model/Cue.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue
{

/** The cue's waveform (QLab "Time & Loops"): trim handles, the integrated fade envelope, a time
    ruler, zoom / scroll and the live playhead.

    Edits are reported through callbacks; the owner writes them to the document (undoably) and
    pushes them to the running player. The view itself only holds a copy of the cue. */
class WaveformView : public juce::Component,
                     private juce::ChangeListener,
                     private juce::ScrollBar::Listener
{
public:
    WaveformView (juce::AudioFormatManager& formats, juce::AudioThumbnailCache& cache);
    ~WaveformView() override;

    /** Shows this cue (null clears). Reloads the thumbnail only when the file changed. */
    void setCue (const Cue* cue);
    /** Live playhead in file seconds (negative hides it). */
    void setPlayhead (double filePositionSeconds, bool isPlaying);
    /** -1 = all channels overlaid, otherwise one channel. */
    void setViewChannel (int channel);
    int getViewChannel() const noexcept { return viewChannel; }
    int getNumFileChannels() const noexcept { return thumbnail.getNumChannels(); }

    void zoomIn();
    void zoomOut();
    void zoomToFit();
    void zoomToRegion();
    /** The click cursor, in file seconds (used by Shift+I / Shift+O / M). */
    double getCursorSeconds() const noexcept { return cursor; }

    /** Trim edited by dragging a handle or Shift+I / Shift+O. 'finished' is false while a drag is in progress. */
    std::function<void (double startSeconds, double endSeconds, bool finished)> onTrimChanged;
    /** Envelope edited (point added / moved / removed). 'finished' is false while a drag is in progress. */
    std::function<void (const Envelope& envelope, bool finished)> onEnvelopeChanged;
    /** Right-click. The owner shows the menu (it knows about editors / the explorer). */
    std::function<void (juce::Point<int> screenPosition)> onContextMenu;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed (const juce::KeyPress& key) override;

    static constexpr double minRegionSeconds = 0.01;

private:
    enum class Drag { none, startHandle, endHandle, envelopePoint };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;

    double fileLength() const noexcept;
    double regionStart() const noexcept { return cue.regionStart(); }
    double regionEnd() const noexcept;
    double regionLength() const noexcept { return juce::jmax (0.0, regionEnd() - regionStart()); }
    bool envelopeEditable() const noexcept { return hasCue && cue.audio.envelope.enabled && regionLength() > 0.0; }

    float xForTime (double seconds) const noexcept;
    double timeForX (float x) const noexcept;
    float yForLevel (double level) const noexcept;
    double levelForY (float y) const noexcept;
    juce::Point<float> pointPosition (const EnvelopePoint& p) const noexcept;
    int findPointNear (juce::Point<float> position, float radius) const noexcept;
    float distanceToEnvelope (juce::Point<float> position) const noexcept;
    void setView (double start, double end);
    void zoomAround (double factor, double anchorSeconds);
    void moveSelectedPoint (double deltaSeconds, double deltaLevel);
    void commitEnvelope (bool finished);
    void drawRuler (juce::Graphics& g) const;
    void drawEnvelope (juce::Graphics& g) const;
    void drawHandles (juce::Graphics& g) const;

    juce::AudioFormatManager& formats;
    juce::AudioThumbnail thumbnail;
    juce::ScrollBar scrollbar { false };

    Cue cue;
    bool hasCue = false;
    juce::File loadedFile;
    double viewStart = 0.0, viewEnd = 1.0;
    double cursor = 0.0;
    double playhead = -1.0;
    bool playing = false;
    int viewChannel = -1;

    Drag drag = Drag::none;
    int selectedPoint = -1;
    int hoverPoint = -1;
    bool hoverStart = false, hoverEnd = false;
    bool envelopeDirty = false;

    juce::Rectangle<int> rulerArea, waveArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};

} // namespace gocue
