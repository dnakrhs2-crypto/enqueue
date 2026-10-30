#include "ui/LoudnessWindow.h"

#include "Widgets.h"

#include <cmath>
#include <limits>
#include <vector>

namespace gocue::livemix
{

namespace
{
    struct Target
    {
        double lufs;
        const char* label;
    };

    // the platforms' normalisation levels and the broadcast standards; the first is the default
    const Target targets[] = {
        { -14.0, "-14 LUFS   \xEC\x9C\xA0\xED\x8A\x9C\xEB\xB8\x8C \xC2\xB7 \xEC\x8A\xA4\xED\x8F\xAC\xED\x8B\xB0\xED\x8C\x8C\xEC\x9D\xB4" },        // 유튜브 · 스포티파이
        { -16.0, "-16 LUFS   \xEC\x95\xA0\xED\x94\x8C \xEB\xAE\xA4\xEC\xA7\x81 \xC2\xB7 \xED\x8C\x9F\xEC\xBA\x90\xEC\x8A\xA4\xED\x8A\xB8" },   // 애플 뮤직 · 팟캐스트
        { -18.0, "-18 LUFS" },
        { -20.0, "-20 LUFS" },
        { -23.0, "-23 LUFS   \xEB\xB0\xA9\xEC\x86\xA1 (EBU R128)" },                                                                       // 방송 (EBU R128)
        { -24.0, "-24 LUFS   \xEB\xB0\xA9\xEC\x86\xA1 (ATSC A/85)" },                                                                      // 방송 (ATSC A/85)
    };

    constexpr double scaleMin = -60.0, scaleMax = 0.0;   // the bar and the graph
    constexpr int historyLength = 1800;                 // three minutes of short-term values, one every 100 ms
    constexpr double truePeakCeiling = -1.0;            // dBTP: the usual limit (EBU R128, the platforms)
    const float nothing = std::numeric_limits<float>::quiet_NaN();

    juce::String valueText (const LoudnessValue& v, int decimals = 1)
    {
        return v.valid ? juce::String (v.value, decimals) : juce::String ("-");
    }

    juce::String signedText (double value)
    {
        return (value >= 0.0 ? "+" : "") + juce::String (value, 1);
    }
}

//==============================================================================
/** The layout of the loudness meter plugins (Youlean and the like): the numbers big on the left - the integrated
    value largest, then short-term and momentary, then range and true peak - a large short-term history on the right,
    the target line through it, and a bar meter along the bottom. */
class LoudnessWindow::Content : public juce::Component,
                                private juce::Timer
{
public:
    Content (MixEngine& e, LiveMixSettings& s) : engine (e), settings (s)
    {
        history.assign ((size_t) historyLength, nothing);
        lastBlocks = engine.getLoudnessMeter().getStats().getSubBlockCount();   // the curve starts now: no invented past

        resetButton.setButtonText (ko ("초기화"));
        resetButton.setTooltip (ko ("통합 라우드니스, 범위, 최대값, 트루 피크를 지금부터 다시 잽니다"));
        resetButton.setWantsKeyboardFocus (false);
        resetButton.setColour (juce::TextButton::buttonColourId, Palette::accent);
        resetButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        resetButton.onClick = [this]
        {
            engine.getLoudnessMeter().reset();
            clearHistory();
            repaint();
        };
        addAndMakeVisible (resetButton);

        styleCaption (targetCaption, ko ("목표"));
        addAndMakeVisible (targetCaption);

        for (size_t i = 0; i < std::size (targets); ++i)
            targetCombo.addItem (juce::String::fromUTF8 (targets[i].label), (int) i + 1);

        targetCombo.setWantsKeyboardFocus (false);
        selectTarget (settings.getLufsTarget());
        targetCombo.onChange = [this]
        {
            const int id = targetCombo.getSelectedId();

            if (id >= 1 && id <= (int) std::size (targets))
            {
                target = targets[(size_t) id - 1].lufs;
                settings.setLufsTarget (target);
                repaint();
            }
        };
        addAndMakeVisible (targetCombo);

        onTopToggle.setButtonText (ko ("항상 위"));
        onTopToggle.setTooltip (ko ("이 창을 다른 창 위에 둡니다 (방송 화면 옆에 두고 볼 때)"));
        onTopToggle.setWantsKeyboardFocus (false);
        onTopToggle.setToggleState (settings.getLufsAlwaysOnTop(), juce::dontSendNotification);
        onTopToggle.onClick = [this]
        {
            settings.setLufsAlwaysOnTop (onTopToggle.getToggleState());

            if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
                window->setAlwaysOnTop (onTopToggle.getToggleState());
        };
        addAndMakeVisible (onTopToggle);

        setSize (980, 620);
        startTimerHz (10);
    }

    ~Content() override
    {
        stopTimer();
    }

    bool isAlwaysOnTopWanted() const { return onTopToggle.getToggleState(); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, 12);
        auto top = area.removeFromTop (32);
        resetButton.setBounds (top.removeFromRight (96));
        top.removeFromRight (10);
        onTopToggle.setBounds (top.removeFromRight (96));
        top.removeFromRight (10);
        targetCaption.setBounds (top.removeFromLeft (40));
        targetCombo.setBounds (top.removeFromLeft (juce::jlimit (150, 300, top.getWidth() - 200)));
        noteArea = top.withTrimmedLeft (14);
        area.removeFromTop (12);

        barArea = area.removeFromBottom (64);
        area.removeFromBottom (12);
        readoutArea = area.removeFromLeft (juce::jlimit (250, 330, area.getWidth() * 2 / 5));
        area.removeFromLeft (14);
        graphArea = area;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Palette::background);

        const auto& meter = engine.getLoudnessMeter();
        const auto& st = meter.getStats();
        const auto m = st.momentary(), s = st.shortTerm(), i = st.integrated(), lra = st.loudnessRange(), tp = meter.truePeakMax();

        paintNote (g, st, meter);
        paintReadouts (g, m, s, i, lra, tp, st);
        paintGraph (g, i);
        paintBar (g, m, s, i);
    }

private:
    void timerCallback() override
    {
        const auto& st = engine.getLoudnessMeter().getStats();   // polled by the main window's timer
        const auto blocks = st.getSubBlockCount();

        if (blocks < lastBlocks)   // a reset
            clearHistory();

        if (blocks != lastBlocks)
        {
            // one point per sub-block that arrived, so a tick that came late keeps the time axis honest (100 ms a point);
            // a tick's jitter (a sub-block or two) repeats the value, a real stall leaves a gap where nothing was seen
            const auto s = st.shortTerm();
            const auto arrived = (int) juce::jmin ((juce::int64) historyLength, blocks - lastBlocks);
            const bool stalled = arrived > 3;

            for (int k = 0; k < arrived; ++k)
            {
                const bool last = k == arrived - 1;
                history[(size_t) historyPos] = (s.valid && (last || ! stalled)) ? (float) s.value : nothing;
                historyPos = (historyPos + 1) % historyLength;
            }

            lastBlocks = blocks;
        }

        repaint();
    }

    void clearHistory()
    {
        std::fill (history.begin(), history.end(), nothing);
        historyPos = 0;
        lastBlocks = 0;
    }

    void selectTarget (double lufs)
    {
        int id = 1;

        for (size_t k = 0; k < std::size (targets); ++k)
            if (std::abs (targets[k].lufs - lufs) < 0.01)
                id = (int) k + 1;

        target = targets[(size_t) id - 1].lufs;
        targetCombo.setSelectedId (id, juce::dontSendNotification);
    }

    /** Green within 1 LU of the target, red above it (too loud), yellow below (too quiet); dim without a value. */
    juce::Colour colourFor (const LoudnessValue& v) const
    {
        if (! v.valid)
            return Palette::dimText;

        const double d = v.value - target;
        return d > 1.0 ? Palette::danger : d < -1.0 ? Palette::meterYellow : Palette::lampOn;
    }

    static float xFor (juce::Rectangle<float> r, double lufs)
    {
        return r.getX() + r.getWidth() * (float) juce::jlimit (0.0, 1.0, (lufs - scaleMin) / (scaleMax - scaleMin));
    }

    static float yFor (juce::Rectangle<float> r, double lufs)
    {
        return r.getY() + r.getHeight() * (float) juce::jlimit (0.0, 1.0, (scaleMax - lufs) / (scaleMax - scaleMin));
    }

    void paintNote (juce::Graphics& g, const LoudnessStats& st, const LoudnessMeter& meter)
    {
        g.setColour (Palette::dimText);
        g.setFont (bodyFont (12.5f));
        juce::StringArray parts;   // what matters first: a narrow window cuts the end

        if (meter.getDroppedSubBlocks() > 0)
            parts.add (ko ("끊긴 구간 있음"));

        if (! engine.isDeviceRunning())
            parts.add (ko ("오디오 멈춤 - 장치가 돌면 잽니다"));

        parts.add (ko ("측정 시간 ") + formatTimeMs (st.elapsedSeconds(), false));
        g.drawText (parts.joinIntoString ("   "), noteArea, juce::Justification::centredLeft, true);
    }

    /** A number with its unit at the right, the caption above: the value in a big bold font, the unit small and dim.
        Returns the height used. */
    int paintValue (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& caption, const juce::String& value, const juce::String& unit,
                    juce::Colour valueColour, float valuePt)
    {
        g.setColour (Palette::dimText);
        g.setFont (captionFont());
        g.drawText (caption, r.removeFromTop (18), juce::Justification::centredLeft, true);

        const juce::Font valueFont (juce::FontOptions (pt (valuePt), juce::Font::bold));
        const int valueH = (int) std::ceil (valueFont.getHeight()) + 2;
        auto row = r.removeFromTop (valueH);
        g.setFont (valueFont);
        g.setColour (valueColour);
        const int valueW = juce::jmin (row.getWidth(), (int) std::ceil (juce::GlyphArrangement::getStringWidth (valueFont, value)) + 2);
        g.drawText (value, row.removeFromLeft (valueW), juce::Justification::centredLeft, false);

        if (unit.isNotEmpty() && row.getWidth() > 20)
        {
            row.removeFromLeft (6);
            g.setColour (Palette::dimText);
            g.setFont (bodyFont (13.0f));
            g.drawText (unit, row.withTrimmedTop (valueH / 2 - 2), juce::Justification::centredLeft, true);
        }

        return 18 + valueH;
    }

    static int valueRowHeight (float valuePt)
    {
        return 18 + (int) std::ceil (juce::Font (juce::FontOptions (pt (valuePt), juce::Font::bold)).getHeight()) + 2;
    }

    void paintReadouts (juce::Graphics& g, const LoudnessValue& m, const LoudnessValue& s, const LoudnessValue& i,
                        const LoudnessValue& lra, const LoudnessValue& tp, const LoudnessStats& st)
    {
        auto card = readoutArea;
        g.setColour (Palette::card);
        g.fillRoundedRectangle (card.toFloat(), Palette::controlRadius);
        g.setColour (Palette::line);
        g.drawRoundedRectangle (card.toFloat().reduced (0.5f), Palette::controlRadius, 1.0f);

        auto area = card.reduced (16, 12);
        const bool tall = area.getHeight() >= 330;
        auto separator = [&]
        {
            area.removeFromTop (tall ? 10 : 6);
            g.setColour (Palette::line);
            g.fillRect (area.removeFromTop (1));
            area.removeFromTop (tall ? 10 : 6);
        };

        // the integrated value: the number of the stream, largest
        area.removeFromTop (paintValue (g, area, ko ("통합 라우드니스  (초기화 후 전체)"), valueText (i), "LUFS", colourFor (i), tall ? 54.0f : 44.0f));
        g.setColour (i.valid ? colourFor (i) : Palette::dimText);
        g.setFont (bodyFont (13.0f));
        g.drawText (i.valid ? ko ("목표 ") + juce::String (target, 0) + ko (" 대비 ") + signedText (i.value - target) + " LU"
                            : ko ("소리가 들어오면 잽니다"), area.removeFromTop (18), juce::Justification::centredLeft, true);
        separator();

        // short-term and momentary side by side
        {
            auto row = area.removeFromTop (valueRowHeight (30.0f));
            auto left = row.removeFromLeft (row.getWidth() / 2);
            paintValue (g, left, ko ("단기 (S, 3초)"), valueText (s), "LUFS", colourFor (s), 30.0f);
            paintValue (g, row, ko ("순간 (M)"), valueText (m), "LUFS", colourFor (m), 30.0f);
        }
        separator();

        // range and true peak
        {
            auto row = area.removeFromTop (valueRowHeight (24.0f));
            auto left = row.removeFromLeft (row.getWidth() / 2);
            paintValue (g, left, ko ("범위 (LRA)"), valueText (lra), "LU", lra.valid ? Palette::text : Palette::dimText, 24.0f);
            paintValue (g, row, ko ("트루 피크 (최대)"), valueText (tp), "dBTP", ! tp.valid ? Palette::dimText : tp.value > truePeakCeiling ? Palette::danger : Palette::text, 24.0f);
        }
        separator();

        g.setColour (Palette::dimText);
        g.setFont (bodyFont (12.5f));
        g.drawText (ko ("최대 단기 ") + valueText (st.maxShortTerm()) + ko ("   최대 순간 ") + valueText (st.maxMomentary()), area.removeFromTop (18), juce::Justification::centredLeft, true);
    }

    void paintGraph (juce::Graphics& g, const LoudnessValue& i)
    {
        auto area = graphArea;

        if (area.getHeight() < 80 || area.getWidth() < 120)
            return;

        auto axis = area.removeFromBottom (16);
        auto labels = area.removeFromLeft (34);
        const auto plot = area.toFloat();
        g.setColour (Palette::meterBg);
        g.fillRoundedRectangle (plot, 6.0f);

        g.setFont (juce::Font (juce::FontOptions (pt (10.5f))));

        for (int db = (int) scaleMin; db <= (int) scaleMax; db += 10)
        {
            const float y = yFor (plot, db);
            g.setColour (Palette::line.withAlpha (0.7f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
            g.setColour (Palette::dimText);
            g.drawText (juce::String (db), juce::Rectangle<int> (labels.getX(), (int) y - 7, labels.getWidth() - 4, 14), juce::Justification::centredRight, false);
        }

        // the target: its ±1 LU band and its line
        g.setColour (Palette::lampOn.withAlpha (0.16f));
        g.fillRect (juce::Rectangle<float> (plot.getX(), yFor (plot, target + 1.0), plot.getWidth(), yFor (plot, target - 1.0) - yFor (plot, target + 1.0)));
        g.setColour (Palette::lampOn.withAlpha (0.9f));
        g.drawHorizontalLine ((int) yFor (plot, target), plot.getX(), plot.getRight());

        // the short-term curve: oldest at the left, now at the right, the area under it filled; a gap where there was no value
        juce::Path line, fill;
        bool drawing = false;
        float runStartX = 0.0f, lastX = plot.getX();

        auto closeFill = [&] (float x)
        {
            fill.lineTo (x, plot.getBottom());
            fill.lineTo (runStartX, plot.getBottom());
            fill.closeSubPath();
        };

        for (int k = 0; k < historyLength; ++k)
        {
            const float v = history[(size_t) ((historyPos + k) % historyLength)];
            const float x = plot.getX() + plot.getWidth() * (float) k / (float) (historyLength - 1);

            if (std::isnan (v))
            {
                if (drawing)
                    closeFill (lastX);

                drawing = false;
                continue;
            }

            const float y = yFor (plot, v);

            if (drawing)
            {
                line.lineTo (x, y);
                fill.lineTo (x, y);
            }
            else
            {
                line.startNewSubPath (x, y);
                fill.startNewSubPath (x, y);
                runStartX = x;
            }

            drawing = true;
            lastX = x;
        }

        if (drawing)
            closeFill (lastX);

        g.setColour (Palette::accent.withAlpha (0.22f));
        g.fillPath (fill);
        g.setColour (Palette::accent);
        g.strokePath (line, juce::PathStrokeType (2.0f));

        if (i.valid)
        {
            g.setColour (Palette::meterYellow.withAlpha (0.9f));
            const float dashes[] = { 6.0f, 4.0f };
            g.drawDashedLine (juce::Line<float> (plot.getX(), yFor (plot, i.value), plot.getRight(), yFor (plot, i.value)), dashes, 2, 1.5f);
        }

        g.setColour (Palette::line);
        g.drawRoundedRectangle (plot.reduced (0.5f), 6.0f, 1.0f);

        g.setColour (Palette::dimText);
        g.setFont (captionFont());
        g.drawText (ko ("단기 라우드니스 (S)  ·  최근 3분"), plot.toNearestInt().reduced (10, 6), juce::Justification::topLeft, true);
        g.setFont (juce::Font (juce::FontOptions (pt (10.5f))));
        g.drawText (ko ("3분 전"), axis.withX (area.getX()).withWidth (60), juce::Justification::centredLeft, false);
        g.drawText (ko ("지금"), axis.withLeft (area.getRight() - 60), juce::Justification::centredRight, false);
    }

    void paintBar (juce::Graphics& g, const LoudnessValue& m, const LoudnessValue& s, const LoudnessValue& i)
    {
        auto area = barArea;
        auto caption = area.removeFromTop (18);
        g.setColour (Palette::dimText);
        g.setFont (captionFont());
        g.drawText (ko ("라우드니스 막대"), caption.removeFromLeft (110), juce::Justification::centredLeft, true);
        g.setFont (bodyFont (12.0f));
        g.drawText (caption.getWidth() >= 470 ? ko ("파란 막대 순간(M)  ·  흰 줄 단기(S)  ·  노란 줄 통합(I)  ·  초록 점선 목표 ±1 LU")
                                              : ko ("막대 M · 흰 줄 S · 노란 줄 I · 점선 목표"), caption, juce::Justification::centredRight, true);
        auto scale = area.removeFromBottom (14);
        const auto track = area.reduced (0, 2).toFloat();
        g.setColour (Palette::meterBg);
        g.fillRoundedRectangle (track, 6.0f);

        if (m.valid)
        {
            g.setColour (Palette::accent.withAlpha (0.85f));
            g.fillRoundedRectangle (track.withRight (xFor (track, m.value)), 6.0f);
        }

        g.setColour (Palette::lampOn.withAlpha (0.18f));
        g.fillRect (juce::Rectangle<float> (xFor (track, target - 1.0), track.getY(), xFor (track, target + 1.0) - xFor (track, target - 1.0), track.getHeight()));
        g.setColour (Palette::lampOn);
        const float dashes[] = { 4.0f, 3.0f };
        g.drawDashedLine (juce::Line<float> (xFor (track, target), track.getY(), xFor (track, target), track.getBottom()), dashes, 2, 2.0f);

        if (s.valid)
        {
            g.setColour (Palette::text);
            g.fillRect (juce::Rectangle<float> (xFor (track, s.value) - 1.5f, track.getY(), 3.0f, track.getHeight()));
        }

        if (i.valid)
        {
            g.setColour (Palette::meterYellow);
            g.fillRect (juce::Rectangle<float> (xFor (track, i.value) - 1.5f, track.getY() - 3.0f, 3.0f, track.getHeight() + 6.0f));
        }

        g.setColour (Palette::dimText);
        g.setFont (juce::Font (juce::FontOptions (pt (10.5f))));

        for (int db = (int) scaleMin; db <= (int) scaleMax; db += 10)
        {
            // the end labels start / stop at their tick, inside the bar: centred on it they ran 18 px past the
            // window's edge and lost '-60''s minus and half the 0
            const int x = (int) xFor (track, db);
            const auto label = db == (int) scaleMin ? juce::Rectangle<int> (x, scale.getY(), 36, scale.getHeight())
                             : db == (int) scaleMax ? juce::Rectangle<int> (x - 36, scale.getY(), 36, scale.getHeight())
                                                    : juce::Rectangle<int> (x - 18, scale.getY(), 36, scale.getHeight());
            const auto justification = db == (int) scaleMin ? juce::Justification::centredLeft : db == (int) scaleMax ? juce::Justification::centredRight : juce::Justification::centred;
            g.drawText (juce::String (db), label, justification, false);
        }
    }

    MixEngine& engine;
    LiveMixSettings& settings;
    double target = -14.0;
    std::vector<float> history;
    int historyPos = 0;
    juce::int64 lastBlocks = 0;

    juce::Rectangle<int> noteArea, readoutArea, graphArea, barArea;
    juce::Label targetCaption;
    juce::ComboBox targetCombo;
    juce::TextButton resetButton;
    juce::ToggleButton onTopToggle;
};

//==============================================================================
LoudnessWindow::LoudnessWindow (MixEngine& engine, LiveMixSettings& settings)
    : DocumentWindow (ko ("LUFS 미터 - 마스터 출력"), Palette::background, DocumentWindow::allButtons)
{
    setUsingNativeTitleBar (true);
    auto* c = new Content (engine, settings);
    content = c;
    setContentOwned (c, true);
    setResizable (true, false);
    setResizeLimits (640, 460, 10000, 10000);
    setAlwaysOnTop (c->isAlwaysOnTopWanted());
    centreWithSize (getWidth(), getHeight());   // the owner then centres it on its own display (centreAroundComponent)
}

LoudnessWindow::~LoudnessWindow()
{
    clearContentComponent();
}

void LoudnessWindow::open()
{
    setVisible (true);
    toFront (true);
}

void LoudnessWindow::closeButtonPressed()
{
    setVisible (false);   // kept: the numbers go on, reopening is instant
}

} // namespace gocue::livemix
