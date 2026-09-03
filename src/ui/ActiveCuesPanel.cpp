#include "ui/ActiveCuesPanel.h"

#include "model/CueColors.h"
#include "ui/UiUtils.h"

#include <algorithm>

namespace gocue
{

namespace
{
    constexpr int rowHeight = 124;   // the big card (design pick 01, 2026-09-03)
}

//==============================================================================
class ActiveCuesPanel::Row : public juce::Component
{
public:
    Row (ActiveCuesPanel& o, AudioEngine& e, const juce::Uuid& cueId) : owner (o), engine (e), id (cueId)
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
        panicButton.setColour (juce::TextButton::buttonColourId, Palette::stopButton.darker (0.12f));
        panicButton.setWantsKeyboardFocus (false);
        panicButton.onClick = [this]
        {
            if (owner.onStopRequested)
                owner.onStopRequested (id);
            else
                engine.fadeOutAndStop (id);
        };
        addAndMakeVisible (panicButton);

        nameLabel.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
        nameLabel.setColour (juce::Label::textColourId, Palette::text);
        nameLabel.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (nameLabel);

        timeLabel.setFont (juce::Font (juce::FontOptions (15.0f)));   // "position / length"
        timeLabel.setColour (juce::Label::textColourId, Palette::dimText);
        timeLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (timeLabel);

        remainingLabel.setFont (juce::Font (juce::FontOptions (30.0f, juce::Font::bold)));   // the number the operator watches
        remainingLabel.setJustificationType (juce::Justification::centredRight);
        remainingLabel.setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (remainingLabel);
    }

    const juce::Uuid& getId() const noexcept { return id; }

    void update (const AudioEngine::PlayingCue& p, const Cue* cue)
    {
        paused = p.paused;
        fadingOut = p.fadingOut;
        fraction = p.progress >= 0.0 ? juce::jlimit (0.0, 1.0, p.progress) : 0.0;
        infinite = p.progress < 0.0;
        colour = cue != nullptr && cue->color > 0 ? CueColors::get (cue->color) : juce::Colour();

        pauseButton.setButtonText (paused ? ko ("재개") : ko ("일시정지"));

        juce::String name;

        if (cue != nullptr)
            name = (cue->number.isNotEmpty() ? cue->number + "  " : juce::String()) + cue->name;
        else
            name = ko ("(삭제된 큐)");

        nameLabel.setText (name, juce::dontSendNotification);

        const auto infinity = juce::String::fromUTF8 ("\xE2\x88\x9E");
        timeLabel.setText (formatSeconds (p.positionSeconds) + " / " + (infinite ? infinity : formatSeconds (juce::jmax (0.0, p.lengthSeconds))),
                           juce::dontSendNotification);
        remainingLabel.setText (infinite ? infinity : "-" + formatSeconds (juce::jmax (0.0, p.remainingSeconds)), juce::dontSendNotification);
        remainingLabel.setColour (juce::Label::textColourId, stateColour());
        repaint();
    }

    juce::Colour stateColour() const noexcept
    {
        return paused ? Palette::paused : (fadingOut ? Palette::fadingOut : Palette::playing);
    }

    void resized() override
    {
        // [ 상태 띠 8 ] [ pill | name ............ x ]
        //                [ pos / length          -remaining ]
        //                [ ==== progress ==== ]
        auto area = getLocalBounds().reduced (12, 10);
        area.removeFromLeft (10);   // the state stripe
        auto top = area.removeFromTop (32);
        pauseButton.setBounds (top.removeFromLeft (96));
        top.removeFromLeft (10);
        panicButton.setBounds (top.removeFromRight (30));
        top.removeFromRight (8);
        nameLabel.setBounds (top);
        area.removeFromTop (6);
        auto middle = area.removeFromTop (36);
        remainingLabel.setBounds (middle.removeFromRight (150));
        timeLabel.setBounds (middle);
        area.removeFromTop (6);
        barArea = area.removeFromTop (14);
    }

    void paint (juce::Graphics& g) override
    {
        const auto card = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (Palette::panel);
        g.fillRoundedRectangle (card, Palette::cornerRadius);
        g.setColour (Palette::outline);
        g.drawRoundedRectangle (card, Palette::cornerRadius, 1.0f);

        // the state stripe down the left edge (green playing / yellow paused / orange fading)
        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (card.withWidth (9.0f).getSmallestIntegerContainer());
            g.setColour (stateColour());
            g.fillRoundedRectangle (card, Palette::cornerRadius);
        }

        if (colour.getAlpha() > 0)   // the cue's own colour as a thin stripe next to it
        {
            g.setColour (colour);
            g.fillRect (11, 8, 4, getHeight() - 16);
        }

        const auto track = barArea.toFloat();
        g.setColour (Palette::outline);
        g.fillRoundedRectangle (track, track.getHeight() * 0.5f);
        g.setColour (stateColour());
        g.fillRoundedRectangle (track.withWidth (infinite ? track.getWidth() : (float) track.getWidth() * (float) fraction), track.getHeight() * 0.5f);
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

    ActiveCuesPanel& owner;
    AudioEngine& engine;
    const juce::Uuid id;
    juce::TextButton pauseButton, panicButton;
    juce::Label nameLabel, timeLabel, remainingLabel;
    juce::Rectangle<int> barArea;
    juce::Colour colour;
    double fraction = 0.0;
    bool paused = false, fadingOut = false, infinite = false;
};

//==============================================================================
ActiveCuesPanel::ActiveCuesPanel (AudioEngine& e, CueList& c) : engine (e), cues (c)
{
    title.setText (ko ("활성 큐"), juce::dontSendNotification);
    title.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    title.setColour (juce::Label::textColourId, Palette::dimText);
    addAndMakeVisible (title);

    emptyLabel.setText (ko ("재생 중인 큐 없음"), juce::dontSendNotification);
    emptyLabel.setFont (juce::Font (juce::FontOptions (14.0f)));
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
            row = std::make_unique<Row> (*this, engine, p->id);
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
