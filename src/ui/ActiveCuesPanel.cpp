#include "ui/ActiveCuesPanel.h"

#include "model/CueColors.h"
#include "ui/UiUtils.h"

#include <algorithm>

namespace gocue
{

namespace
{
    constexpr int rowHeight = 58;
}

//==============================================================================
class ActiveCuesPanel::Row : public juce::Component
{
public:
    Row (AudioEngine& e, const juce::Uuid& cueId) : engine (e), id (cueId)
    {
        pauseButton.setWantsKeyboardFocus (false);
        pauseButton.onClick = [this]
        {
            if (paused)
                engine.resume (id);
            else
                engine.pause (id);
        };
        addAndMakeVisible (pauseButton);

        panicButton.setButtonText ("x");
        panicButton.setTooltip (ko ("이 큐 페이드 정지"));
        panicButton.setColour (juce::TextButton::buttonColourId, Palette::stopButton);
        panicButton.setWantsKeyboardFocus (false);
        panicButton.onClick = [this] { engine.fadeOutAndStop (id); };
        addAndMakeVisible (panicButton);

        nameLabel.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        nameLabel.setColour (juce::Label::textColourId, Palette::text);
        nameLabel.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (nameLabel);

        timeLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        timeLabel.setColour (juce::Label::textColourId, Palette::dimText);
        timeLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (timeLabel);
    }

    const juce::Uuid& getId() const noexcept { return id; }

    void update (const AudioEngine::PlayingCue& p, const Cue* cue)
    {
        paused = p.paused;
        fadingOut = p.fadingOut;
        fraction = p.lengthSeconds > 0.0 ? juce::jlimit (0.0, 1.0, p.positionSeconds / p.lengthSeconds) : 0.0;
        infinite = p.lengthSeconds < 0.0;
        colour = cue != nullptr && cue->color > 0 ? CueColors::get (cue->color) : juce::Colour();

        pauseButton.setButtonText (paused ? ko ("재개") : ko ("일시정지"));

        juce::String name;

        if (cue != nullptr)
            name = (cue->number.isNotEmpty() ? cue->number + "  " : juce::String()) + cue->name;
        else
            name = ko ("(삭제된 큐)");

        nameLabel.setText (name, juce::dontSendNotification);

        juce::String time = formatSeconds (p.positionSeconds);

        if (infinite)
            time << "  " << juce::String::fromUTF8 ("\xE2\x88\x9E");
        else
            time << "  -" << formatSeconds (juce::jmax (0.0, p.remainingSeconds));

        timeLabel.setText (time, juce::dontSendNotification);
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (6, 4);
        auto top = area.removeFromTop (22);
        pauseButton.setBounds (top.removeFromLeft (64));
        top.removeFromLeft (6);
        panicButton.setBounds (top.removeFromRight (26));
        top.removeFromRight (6);
        nameLabel.setBounds (top);
        area.removeFromTop (2);
        timeLabel.setBounds (area.removeFromTop (16));
        barArea = area.removeFromTop (10);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (Palette::rowEven);
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f);

        if (colour.getAlpha() > 0)
        {
            g.setColour (colour);
            g.fillRect (2, 2, 4, getHeight() - 4);
        }

        g.setColour (Palette::outline);
        g.fillRect (barArea);
        g.setColour (paused ? Palette::paused.brighter (0.4f) : (fadingOut ? Palette::fadingOut : Palette::playing.brighter (0.3f)));

        if (infinite)
            g.fillRect (barArea.withWidth (barArea.getWidth()));
        else
            g.fillRect (barArea.withWidth (juce::roundToInt (barArea.getWidth() * fraction)));
    }

    void mouseDown (const juce::MouseEvent& e) override { scrub (e); }
    void mouseDrag (const juce::MouseEvent& e) override { scrub (e); }

private:
    void scrub (const juce::MouseEvent& e)
    {
        if (infinite || ! barArea.expanded (0, 6).contains (e.getPosition()))
            return;

        const double f = juce::jlimit (0.0, 1.0, (double) (e.x - barArea.getX()) / (double) juce::jmax (1, barArea.getWidth()));
        engine.seekToFraction (id, f);
    }

    AudioEngine& engine;
    const juce::Uuid id;
    juce::TextButton pauseButton, panicButton;
    juce::Label nameLabel, timeLabel;
    juce::Rectangle<int> barArea;
    juce::Colour colour;
    double fraction = 0.0;
    bool paused = false, fadingOut = false, infinite = false;
};

//==============================================================================
ActiveCuesPanel::ActiveCuesPanel (AudioEngine& e, CueList& c) : engine (e), cues (c)
{
    title.setText (ko ("활성 큐"), juce::dontSendNotification);
    title.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    title.setColour (juce::Label::textColourId, Palette::dimText);
    addAndMakeVisible (title);

    emptyLabel.setText (ko ("재생 중인 큐 없음"), juce::dontSendNotification);
    emptyLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    emptyLabel.setColour (juce::Label::textColourId, Palette::dimText);
    emptyLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (emptyLabel);

    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false, true, false);
    viewport.setScrollBarThickness (8);
    addAndMakeVisible (viewport);
}

ActiveCuesPanel::~ActiveCuesPanel()
{
    viewport.setViewedComponent (nullptr, false);
}

void ActiveCuesPanel::setNewestFirst (bool shouldBeNewestFirst)
{
    newestFirst = shouldBeNewestFirst;
}

void ActiveCuesPanel::setPlayingCues (const std::vector<AudioEngine::PlayingCue>& playing)
{
    std::vector<const AudioEngine::PlayingCue*> active;

    for (const auto& p : playing)
        if (! p.loaded)
            active.push_back (&p);

    std::sort (active.begin(), active.end(), [this] (const AudioEngine::PlayingCue* a, const AudioEngine::PlayingCue* b)
    {
        return newestFirst ? a->startOrder > b->startOrder : a->startOrder < b->startOrder;
    });

    // reuse rows by cue id, drop the rest
    std::vector<std::unique_ptr<Row>> next;

    for (const auto* p : active)
    {
        std::unique_ptr<Row> row;

        for (auto& existing : rows)
            if (existing != nullptr && existing->getId() == p->id)
                row = std::move (existing);

        if (row == nullptr)
        {
            row = std::make_unique<Row> (engine, p->id);
            content.addAndMakeVisible (*row);
        }

        row->update (*p, cues.findById (p->id));
        next.push_back (std::move (row));
    }

    rows = std::move (next);
    emptyLabel.setVisible (rows.empty());

    const int width = juce::jmax (1, viewport.getMaximumVisibleWidth());
    int y = 0;

    for (auto& row : rows)
    {
        row->setBounds (0, y, width, rowHeight);
        y += rowHeight + 4;
    }

    content.setSize (width, juce::jmax (1, y));
}

void ActiveCuesPanel::resized()
{
    auto area = getLocalBounds();
    title.setBounds (area.removeFromTop (22).reduced (8, 2));
    viewport.setBounds (area.reduced (4, 2));
    emptyLabel.setBounds (area);

    const int width = juce::jmax (1, viewport.getMaximumVisibleWidth());
    int y = 0;

    for (auto& row : rows)
    {
        row->setBounds (0, y, width, rowHeight);
        y += rowHeight + 4;
    }

    content.setSize (width, juce::jmax (1, y));
}

void ActiveCuesPanel::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);
    g.setColour (Palette::outline);
    g.drawLine (0.5f, 0.0f, 0.5f, (float) getHeight());
}

} // namespace gocue
