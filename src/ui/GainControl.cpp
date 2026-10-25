#include "ui/GainControl.h"

#include "model/Cue.h"
#include "ui/UiUtils.h"

#include <cmath>

namespace gocue
{

namespace GainUnits
{
    double toPercent (double db) noexcept
    {
        if (db <= Cue::minGainDb)
            return 0.0;   // the floor is silence (Cue::gainLinear)

        return 100.0 * std::pow (10.0, juce::jmin (db, Cue::maxGainDb) / 20.0);
    }

    double fromPercent (double percent) noexcept
    {
        if (! (percent > 0.0))
            return Cue::minGainDb;

        return juce::jlimit (Cue::minGainDb, Cue::maxGainDb, 20.0 * std::log10 (percent / 100.0));
    }

    double maxPercent() noexcept
    {
        return toPercent (Cue::maxGainDb);
    }

    juce::String format (double db, bool percent)
    {
        return percent ? juce::String (toPercent (db), 1) + " %" : juce::String (db, 1) + " dB";
    }
}

namespace
{
    /** "-12", "3.5", "0" (no sign but a leading minus, one point at most, a digit somewhere). */
    bool isPlainNumber (const juce::String& t)
    {
        int digits = 0, points = 0;

        for (int i = 0; i < t.length(); ++i)
        {
            const auto c = t[i];

            if (c >= '0' && c <= '9')
                ++digits;
            else if (c == '.')
                ++points;
            else if (! (c == '-' && i == 0))
                return false;
        }

        return digits > 0 && points <= 1;
    }

    /** "1,200" or "-12,345.5": the commas only between groups of three digits (a thousands separator). */
    bool isGroupedThousands (juce::String t)
    {
        if (t.startsWithChar ('-'))
            t = t.substring (1);

        juce::StringArray groups;
        groups.addTokens (t.upToFirstOccurrenceOf (".", false, false), ",", "");

        if (groups.size() < 2 || groups[0].isEmpty() || groups[0].length() > 3)
            return false;

        for (int i = 0; i < groups.size(); ++i)
            if (! groups[i].containsOnly ("0123456789") || (i > 0 && groups[i].length() != 3))
                return false;

        return ! t.fromFirstOccurrenceOf (".", false, false).containsChar (',');
    }
}

//==============================================================================
GainSlider::GainSlider()
{
    setSliderStyle (juce::Slider::LinearHorizontal);
    // no interval: snapValue keeps the 0.1 dB / 0.1 % steps, so a value set in % is not moved onto the dB grid while dB shows
    setRange (Cue::minGainDb, Cue::maxGainDb, 0.0);
    setTextBoxStyle (juce::Slider::TextBoxRight, false, 90, Palette::fieldHeight);
    setDoubleClickReturnValue (true, 0.0);
    setWantsKeyboardFocus (false);
    follower.onChange = [this] { applyUnit(); };
    unit.addListener (&follower);
    applyUnit();
}

GainSlider::~GainSlider()
{
    unit.removeListener (&follower);
}

void GainSlider::bindUnit (juce::Value& percent)
{
    unit.referTo (percent);
    applyUnit();
}

bool GainSlider::showsPercent() const
{
    return static_cast<bool> (unit.getValue());
}

void GainSlider::applyUnit()
{
    setTextValueSuffix (showsPercent() ? " %" : " dB");
    updateText();
}

juce::String GainSlider::getTextFromValue (double value)
{
    return GainUnits::format (value, showsPercent());
}

double GainSlider::getValueFromText (const juce::String& text)
{
    auto t = text.trim().removeCharacters (" ");
    bool percent = showsPercent();
    bool unitTyped = false;

    if (t.endsWithChar ('%'))
    {
        percent = unitTyped = true;
        t = t.dropLastCharacters (1);
    }
    else if (t.endsWithIgnoreCase ("db"))
    {
        percent = false;
        unitTyped = true;
        t = t.dropLastCharacters (2);
    }

    const bool signedNumber = t.startsWithChar ('+') || t.startsWithChar ('-');

    while (t.startsWithChar ('+'))
        t = t.substring (1);

    if (t.containsChar (','))
    {
        if (! isGroupedThousands (t))
            return getValue();   // "1,5": no thousands separator, a typo - the gain stays

        t = t.removeCharacters (",");
    }

    if (! isPlainNumber (t))
        return getValue();   // not a number: the gain stays (a typo must not jump to 0 dB, or to silence in %)

    const double number = t.getDoubleValue();

    if (percent && ! unitTyped && signedNumber)
        percent = false;   // a share has no sign: "+6" or "-6" typed in % is dB

    return percent ? GainUnits::fromPercent (number) : number;
}

double GainSlider::onGrid (double db) const
{
    db = juce::jlimit (Cue::minGainDb, Cue::maxGainDb, db);

    if (showsPercent())
        return GainUnits::fromPercent (std::round (GainUnits::toPercent (db) * 10.0) / 10.0);

    // the 0.1 dB interval the slider had, by the same rule JUCE snapped it with
    return juce::jlimit (Cue::minGainDb, Cue::maxGainDb, Cue::minGainDb + 0.1 * std::floor ((db - Cue::minGainDb) / 0.1 + 0.5));
}

double GainSlider::snapValue (double attemptedValue, DragMode)
{
    const double snapped = onGrid (attemptedValue);

    if (! wheeling || juce::approximatelyEqual (attemptedValue, wheelFrom) || ! juce::approximatelyEqual (snapped, onGrid (wheelFrom)))
        return snapped;

    // JUCE moves a wheel notch by a share of the range (about 5 dB); near silence in %, or with a touchpad's small
    // steps in dB, that lands back on the step it started from and the wheel would do nothing: one step on instead
    const double direction = attemptedValue > wheelFrom ? 1.0 : -1.0;

    if (showsPercent())
        return GainUnits::fromPercent (juce::jlimit (0.0, GainUnits::maxPercent(),
                                                     std::round (GainUnits::toPercent (wheelFrom) * 10.0) / 10.0 + 0.1 * direction));

    return onGrid (onGrid (wheelFrom) + 0.1 * direction);
}

void GainSlider::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const juce::ScopedValueSetter<bool> notch (wheeling, true);
    wheelFrom = getValue();
    juce::Slider::mouseWheelMove (e, wheel);
}

//==============================================================================
GainUnitButton::GainUnitButton() : juce::Button ("gainUnit")
{
    setWantsKeyboardFocus (false);
    follower.onChange = [this] { applyUnit(); };
    unit.addListener (&follower);
    applyUnit();
}

GainUnitButton::~GainUnitButton()
{
    unit.removeListener (&follower);
}

void GainUnitButton::bindUnit (juce::Value& percent)
{
    unit.referTo (percent);
    applyUnit();
}

void GainUnitButton::applyUnit()
{
    setButtonText (static_cast<bool> (unit.getValue()) ? ko ("게인 (%)") : ko ("게인 (dB)"));
    repaint();
}

void GainUnitButton::clicked()
{
    unit = ! static_cast<bool> (unit.getValue());
    unit.getValueSource().sendChangeMessage (true);   // both tabs and the saved setting now, not on the next message
}

void GainUnitButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    const bool percent = static_cast<bool> (unit.getValue());
    const float alpha = isEnabled() ? 1.0f : Palette::disabledAlpha;
    const auto bounds = getLocalBounds();
    const auto labelFont = Palette::font (Palette::fieldLabelSize);
    constexpr int labelInset = 5;   // juce::Label's default border: the text starts where the other labels' text does
    const auto widthOf = [] (const juce::Font& font, const juce::String& text)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (font, text));
    };

    // the word as the other labels draw theirs, then a key with the unit it shows (gom's pick of three, 2026-10-25)
    const auto word = ko ("게인");
    const int wordWidth = widthOf (labelFont, word);
    g.setColour (Palette::dimText.withMultipliedAlpha (alpha));
    g.setFont (labelFont);
    g.drawText (word, bounds.withTrimmedLeft (labelInset).withWidth (wordWidth), juce::Justification::centredLeft, false);

    const auto unitFont = Palette::monoFont (Palette::fileSize);
    const float keyHeight = 22.0f;
    const auto key = juce::Rectangle<float> ((float) (labelInset + wordWidth + 5), ((float) bounds.getHeight() - keyHeight) * 0.5f,
                                             (float) (widthOf (unitFont, "dB") + 12), keyHeight).reduced (0.5f);
    const float radius = Palette::fieldRadius * keyHeight / (float) Palette::fieldHeight;   // the number box's corners, scaled
    auto fill = Palette::field;

    if (down)
        fill = fill.darker (Palette::pressedDarken);
    else if (over)
        fill = fill.brighter (Palette::hoverBrighten);

    g.setColour (fill.withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (key, radius);
    g.setColour ((over ? Palette::accent : Palette::outline).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (key, radius, Palette::borderWidth);
    g.setColour (Palette::text.withMultipliedAlpha (alpha));
    g.setFont (unitFont);
    g.drawText (percent ? "%" : "dB", key, juce::Justification::centred, false);
}

} // namespace gocue
