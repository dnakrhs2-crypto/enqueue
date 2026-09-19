#pragma once

#include "model/Cue.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>

namespace gocue::CueIcons
{

/** Monochrome cue vocabulary, independent of commands and menu ownership.
    Artwork uses a 24-unit frame and 2.4-unit round strokes, baked into filled paths once.
    Insets are optical: the dense cart pads are smaller than the open clock / group outlines. */
inline constexpr float frameSize = 24.0f;
inline constexpr float strokeWidth = 2.4f;

struct Key
{
    CueType type;
    FadeMode fadeMode = FadeMode::custom;
    ControlKind controlKind = ControlKind::start;
};

enum class Container { cueList, cart };

namespace detail
{
    inline juce::Path filledStroke (const juce::Path& centreline)
    {
        juce::Path result;
        juce::PathStrokeType (strokeWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
            .createStrokedPath (result, centreline);
        return result;
    }

    inline juce::Path audio()
    {
        // Five symmetric waveform bars identify recorded sound without a tiny file badge.
        juce::Path path;
        const std::array<float, 5> heights { 6.0f, 12.0f, 20.0f, 12.0f, 6.0f };
        for (size_t i = 0; i < heights.size(); ++i)
            path.addRoundedRectangle (2.4f + (float) i * 4.2f, 12.0f - heights[i] * 0.5f,
                                      strokeWidth, heights[i], strokeWidth * 0.5f);
        return path;
    }

    inline juce::Path fadeIn()
    {
        // A rising ramp over a level floor reads as increasing amplitude from left to right.
        juce::Path path;
        path.addTriangle (3.0f, 20.0f, 21.0f, 4.0f, 21.0f, 20.0f);
        return filledStroke (path);
    }

    inline juce::Path fadeOut()
    {
        // The exact horizontal mirror keeps the pair recognisable while reversing its slope.
        auto path = fadeIn();
        path.applyTransform (juce::AffineTransform (-1.0f, 0.0f, frameSize, 0.0f, 1.0f, 0.0f));
        return path;
    }

    inline juce::Path customFade()
    {
        // A bent envelope distinguishes legacy/custom goals from the two directional ramps.
        juce::Path path;
        path.startNewSubPath (3.0f, 18.0f);
        path.lineTo (9.0f, 10.0f);
        path.lineTo (15.0f, 14.0f);
        path.lineTo (21.0f, 6.0f);
        auto result = filledStroke (path);
        result.addEllipse (7.0f, 8.0f, 4.0f, 4.0f);
        result.addEllipse (13.0f, 12.0f, 4.0f, 4.0f);
        return result;
    }

    inline juce::Path devamp()
    {
        // An open loop with a rightward exit means leaving the repeat and moving on.
        juce::Path path;
        path.startNewSubPath (13.0f, 4.0f);
        path.lineTo (10.0f, 4.0f);
        path.cubicTo (5.6f, 4.0f, 3.0f, 7.6f, 3.0f, 12.0f);
        path.cubicTo (3.0f, 16.4f, 5.6f, 20.0f, 10.0f, 20.0f);
        path.lineTo (13.0f, 20.0f);
        path.startNewSubPath (10.0f, 12.0f);
        path.lineTo (21.0f, 12.0f);
        path.startNewSubPath (17.0f, 8.0f);
        path.lineTo (21.0f, 12.0f);
        path.lineTo (17.0f, 16.0f);
        return filledStroke (path);
    }

    inline juce::Path group()
    {
        // Overlapping cue cards collect several cues into one group.
        juce::Path path;
        path.addRoundedRectangle (7.0f, 8.0f, 14.0f, 13.0f, 1.3f);
        path.startNewSubPath (3.0f, 16.0f);
        path.lineTo (3.0f, 5.0f);
        path.quadraticTo (3.0f, 3.0f, 5.0f, 3.0f);
        path.lineTo (16.0f, 3.0f);
        return filledStroke (path);
    }

    inline juce::Path control()
    {
        // Two offset slider knobs signify changing another cue's state.
        juce::Path path;
        for (const auto y : { 7.0f, 17.0f })
            path.addRoundedRectangle (3.0f, y - strokeWidth * 0.5f, 18.0f, strokeWidth, strokeWidth * 0.5f);
        path.addEllipse (5.0f, 4.0f, 6.0f, 6.0f);
        path.addEllipse (13.0f, 14.0f, 6.0f, 6.0f);
        return path;
    }

    inline juce::Path wait()
    {
        // A round clock and two hands communicate elapsed waiting time.
        juce::Path path;
        path.addEllipse (3.0f, 3.0f, 18.0f, 18.0f);
        path.startNewSubPath (12.0f, 6.5f);
        path.lineTo (12.0f, 12.0f);
        path.lineTo (16.0f, 14.5f);
        return filledStroke (path);
    }

    inline juce::Path memo()
    {
        // A folded sheet with two short lines reads as a note, distinct from the stacked group.
        juce::Path path;
        path.startNewSubPath (5.0f, 3.0f);
        path.lineTo (14.0f, 3.0f);
        path.lineTo (20.0f, 9.0f);
        path.lineTo (20.0f, 21.0f);
        path.lineTo (5.0f, 21.0f);
        path.closeSubPath();
        path.startNewSubPath (14.0f, 3.0f);
        path.lineTo (14.0f, 9.0f);
        path.lineTo (20.0f, 9.0f);
        path.startNewSubPath (9.0f, 13.0f);
        path.lineTo (16.0f, 13.0f);
        path.startNewSubPath (9.0f, 17.0f);
        path.lineTo (14.0f, 17.0f);
        return filledStroke (path);
    }

    inline juce::Path mic()
    {
        // A capsule in a U-shaped cradle distinguishes live input from the audio waveform.
        juce::Path path;
        path.startNewSubPath (5.5f, 11.0f);
        path.lineTo (5.5f, 12.5f);
        path.cubicTo (5.5f, 16.1f, 8.4f, 18.0f, 12.0f, 18.0f);
        path.cubicTo (15.6f, 18.0f, 18.5f, 16.1f, 18.5f, 12.5f);
        path.lineTo (18.5f, 11.0f);
        path.startNewSubPath (12.0f, 18.0f);
        path.lineTo (12.0f, 21.0f);
        path.startNewSubPath (8.0f, 21.0f);
        path.lineTo (16.0f, 21.0f);
        auto result = filledStroke (path);
        result.addRoundedRectangle (8.5f, 2.0f, 7.0f, 12.0f, 3.5f);
        return result;
    }

    inline juce::Path cueList()
    {
        // Three bullets and equal rows describe an ordered cue list.
        juce::Path path;
        for (const auto y : { 4.5f, 12.0f, 19.5f })
        {
            path.addRoundedRectangle (3.0f, y - strokeWidth * 0.5f, strokeWidth, strokeWidth, 0.5f);
            path.addRoundedRectangle (8.0f, y - strokeWidth * 0.5f, 13.0f, strokeWidth, strokeWidth * 0.5f);
        }
        return path;
    }

    inline juce::Path cart()
    {
        // Four solid pads describe the cart's button grid rather than a shopping cart.
        juce::Path path;
        for (const auto x : { 3.0f, 13.8f })
            for (const auto y : { 3.0f, 13.8f })
                path.addRoundedRectangle (x, y, 7.2f, 7.2f, 1.4f);
        return path;
    }

    struct Entry { Key key; juce::Path path; };

    inline const std::array<Entry, 10>& registry()
    {
        static const std::array<Entry, 10> entries {{
            { { CueType::audio }, audio() },
            { { CueType::fade, FadeMode::fadeIn }, fadeIn() },
            { { CueType::fade, FadeMode::fadeOut }, fadeOut() },
            { { CueType::fade, FadeMode::custom }, customFade() },
            { { CueType::devamp }, devamp() },
            { { CueType::group }, group() },
            { { CueType::control }, control() },
            { { CueType::control, FadeMode::custom, ControlKind::wait }, wait() },
            { { CueType::control, FadeMode::custom, ControlKind::memo }, memo() },
            { { CueType::mic }, mic() }
        }};
        return entries;
    }

    class FramedPath final : public juce::DrawablePath
    {
    public:
        explicit FramedPath (const juce::Path& sourcePath)
        {
            setPath (sourcePath);
            // White is the single tint token used by the popup look and feel.
            setFill (juce::Colours::white);
        }

        juce::Rectangle<float> getDrawableBounds() const override
        {
            return { 0.0f, 0.0f, frameSize, frameSize };
        }

        std::unique_ptr<juce::Drawable> createCopy() const override
        {
            // PopupMenu and the renderer copy Drawables; retain the shared frame in both cases.
            return std::make_unique<FramedPath> (*this);
        }
    };

    inline std::unique_ptr<juce::Drawable> create (const juce::Path* path)
    {
        return path != nullptr ? std::make_unique<FramedPath> (*path) : nullptr;
    }
}

/** Only fade cues use fadeMode. Control kinds other than wait / memo share the control symbol. */
inline const juce::Path* pathFor (Key key)
{
    const auto controlKind = key.controlKind == ControlKind::wait || key.controlKind == ControlKind::memo
                                 ? key.controlKind : ControlKind::start;
    for (const auto& entry : detail::registry())
        if (entry.key.type == key.type
            && (key.type != CueType::fade || entry.key.fadeMode == key.fadeMode)
            && (key.type != CueType::control || entry.key.controlKind == controlKind))
            return &entry.path;
    return nullptr;
}

inline const juce::Path* pathFor (Container container)
{
    static const auto list = detail::cueList();
    static const auto cart = detail::cart();
    switch (container)
    {
        case Container::cueList: return &list;
        case Container::cart:    return &cart;
    }
    return nullptr;
}

inline std::unique_ptr<juce::Drawable> create (Key key) { return detail::create (pathFor (key)); }
inline std::unique_ptr<juce::Drawable> create (Container container) { return detail::create (pathFor (container)); }
inline std::unique_ptr<juce::Drawable> create (const Cue& cue)
{
    return create (Key { cue.type, cue.fade.mode, cue.control.kind });
}

} // namespace gocue::CueIcons
