#include "ui/ActiveCuesPanel.h"

#include "model/CueColors.h"
#include "ui/UiUtils.h"

#include <algorithm>

namespace gocue
{

class ActiveCuesPanel::Row : public juce::Component
{
public:
    Row (ActiveCuesPanel& o, AudioEngine& e, const juce::Uuid& cueId) : owner (o), engine (e), id (cueId)
    {
        pauseButton.setWantsKeyboardFocus (false);
        pauseButton.getProperties().set ("slateSmall", true);
        pauseButton.setColour (juce::TextButton::buttonColourId, Palette::panel);
        pauseButton.onClick = [this]
        {
            if (owner.onPauseRequested)
                owner.onPauseRequested (id, paused);
            else if (paused)
                engine.resume (id);
            else
                engine.pause (id);
        };
        addAndMakeVisible (pauseButton);

        panicButton.setButtonText (ko ("×"));
        panicButton.setTooltip (ko ("이 큐 페이드 정지"));
        panicButton.setColour (juce::TextButton::buttonColourId, Palette::panel);
        panicButton.setColour (juce::TextButton::textColourOffId, Palette::stopButton);
        panicButton.getProperties().set ("slateSmall", true);
        panicButton.getProperties().set ("slateColourOutline", true);
        panicButton.setWantsKeyboardFocus (false);
        panicButton.onClick = [this]
        {
            if (owner.onStopRequested)
                owner.onStopRequested (id);
            else
                engine.fadeOutAndStop (id);
        };
        addAndMakeVisible (panicButton);

        numberLabel.setFont (Palette::monoFont (Palette::bodySize).boldened());
        numberLabel.setColour (juce::Label::textColourId, Palette::muted);
        nameLabel.setFont (Palette::font (Palette::bodySize, true));
        nameLabel.setColour (juce::Label::textColourId, Palette::text);
        timeLabel.setFont (Palette::monoFont (Palette::headerSize));
        timeLabel.setColour (juce::Label::textColourId, Palette::muted);
        timeLabel.setJustificationType (juce::Justification::centredRight);
        remainingLabel.setFont (Palette::monoFont (Palette::remainingSize).boldened());
        remainingLabel.setJustificationType (juce::Justification::centredLeft);
        for (auto* label : { &numberLabel, &nameLabel, &timeLabel, &remainingLabel })
        {
            label->setMinimumHorizontalScale (1.0f);
            label->setBorderSize (juce::BorderSize<int> (0));
            addAndMakeVisible (label);
        }
    }

    const juce::Uuid& getId() const noexcept { return id; }

    void update (const AudioEngine::PlayingCue& p, const Cue* cue)
    {
        paused = p.paused;
        fadingOut = p.fadingOut;
        colourIndex = cue != nullptr ? cue->color : 0;
        fraction = p.progress >= 0.0 ? juce::jlimit (0.0, 1.0, p.progress) : 0.0;
        infinite = p.progress < 0.0;
        pauseButton.setButtonText (paused ? ko ("재개") : ko ("일시정지"));
        numberLabel.setText (cue != nullptr ? cue->number : juce::String(), juce::dontSendNotification);
        nameLabel.setText (cue != nullptr ? cue->name : ko ("(삭제된 큐)"), juce::dontSendNotification);
        nameLabel.setTooltip (nameLabel.getText());
        stateText = paused ? ko ("일시정지") : fadingOut ? ko ("페이드 아웃") : ko ("재생 중");

        const auto infinity = ko ("∞");
        auto clock = [] (double seconds) { return seconds > 0.0 ? formatSeconds (seconds) : juce::String ("0:00.0"); };
        timeLabel.setText (clock (p.positionSeconds) + " / " + (infinite ? infinity : clock (juce::jmax (0.0, p.lengthSeconds))),
                           juce::dontSendNotification);
        remainingLabel.setText (infinite ? infinity : "-" + clock (juce::jmax (0.0, p.remainingSeconds)), juce::dontSendNotification);
        remainingLabel.setColour (juce::Label::textColourId, stateColour());
        timeLabel.setTooltip (timeLabel.getText());
        remainingLabel.setTooltip (remainingLabel.getText());
        resized();
        repaint();
    }

    juce::Colour stateColour() const noexcept
    {
        return paused ? Palette::paused : (fadingOut ? Palette::fadingOut : Palette::playing);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, Palette::cardInset);
        auto top = area.removeFromTop (20);
        const int stateWidth = juce::GlyphArrangement::getStringWidthInt (Palette::font (Palette::pillSize, true), stateText) + 14;
        stateBounds = top.removeFromRight (juce::jmin (stateWidth, top.getWidth() / 2));
        top.removeFromRight (8);
        const int numberWidth = juce::GlyphArrangement::getStringWidthInt (numberLabel.getFont(), numberLabel.getText());
        numberLabel.setBounds (top.removeFromLeft (juce::jmin (numberWidth, top.getWidth() / 3)));
        if (numberWidth > 0)
            top.removeFromLeft (8);
        colourBounds = {};
        if (colourIndex > 0)
        {
            colourBounds = top.removeFromLeft (Palette::colourBarWidth)
                              .withSizeKeepingCentre (Palette::colourBarWidth, Palette::colourBarHeight);
            top.removeFromLeft (8);
        }
        nameLabel.setBounds (top);
        area.removeFromTop (6);
        auto middle = area.removeFromTop (30);
        const int timeWidth = juce::GlyphArrangement::getStringWidthInt (timeLabel.getFont(), timeLabel.getText());
        timeLabel.setBounds (middle.removeFromRight (juce::jmin (timeWidth, middle.getWidth() / 2)).withTrimmedTop (12));
        middle.removeFromRight (6);
        remainingLabel.setBounds (middle);
        area.removeFromTop (6);
        barArea = area.removeFromTop (Palette::progressHeight);
        area.removeFromTop (6);
        auto buttons = area.removeFromTop (Palette::miniButtonHeight);
        panicButton.setBounds (buttons.removeFromRight (Palette::miniButtonHeight));
        buttons.removeFromRight (6);
        const int pauseWidth = juce::GlyphArrangement::getStringWidthInt (Palette::font (Palette::headerSize, true), pauseButton.getButtonText()) + 20;
        pauseButton.setBounds (buttons.removeFromRight (pauseWidth));
    }

    void paint (juce::Graphics& g) override
    {
        const auto card = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (Palette::panel2);
        g.fillRoundedRectangle (card, Palette::cornerRadius);
        g.setColour (Palette::outline);
        g.drawRoundedRectangle (card, Palette::cornerRadius, Palette::borderWidth);
        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (getLocalBounds().withWidth (Palette::playheadWidth));
            g.setColour (stateColour());
            g.fillRoundedRectangle (card, Palette::cornerRadius);
        }
        if (colourIndex > 0)
        {
            g.setColour (CueColors::get (colourIndex));
            g.fillRoundedRectangle (colourBounds.toFloat(), Palette::colourBarRadius);
        }
        const auto statePill = stateBounds.toFloat();
        g.setColour (stateColour().withAlpha (Palette::statePillAlpha));
        g.fillRoundedRectangle (statePill, Palette::pillRadius (statePill));
        g.setColour (stateColour());
        g.setFont (Palette::font (Palette::pillSize, true));
        g.drawText (stateText, stateBounds.reduced (7, 0), juce::Justification::centred, true);
        const auto track = barArea.toFloat();
        g.setColour (Palette::outline.withAlpha (Palette::trackAlpha));
        g.fillRoundedRectangle (track, Palette::pillRadius (track));
        const auto progress = track.withWidth (infinite ? track.getWidth() : track.getWidth() * (float) fraction);
        g.setColour (stateColour());
        g.fillRoundedRectangle (progress, Palette::pillRadius (progress));
    }

    void mouseDown (const juce::MouseEvent& e) override { scrub (e); }
    void mouseDrag (const juce::MouseEvent& e) override { scrub (e); }

private:
    void scrub (const juce::MouseEvent& e)
    {
        if (infinite || ! owner.isScrubEnabled() || ! barArea.expanded (0, 6).contains (e.getPosition()))
            return;
        const double f = juce::jlimit (0.0, 1.0, (double) (e.x - barArea.getX()) / (double) juce::jmax (1, barArea.getWidth()));
        engine.seekToFraction (id, f);
    }

    ActiveCuesPanel& owner;
    AudioEngine& engine;
    const juce::Uuid id;
    juce::TextButton pauseButton, panicButton;
    juce::Label numberLabel, nameLabel, timeLabel, remainingLabel;
    juce::Rectangle<int> barArea, stateBounds, colourBounds;
    juce::String stateText;
    int colourIndex = 0;
    double fraction = 0.0;
    bool paused = false, fadingOut = false, infinite = false;
};

//==============================================================================
ActiveCuesPanel::ActiveCuesPanel (AudioEngine& e, CueList& c) : engine (e), cues (c)
{
    title.setText (ko ("활성 큐"), juce::dontSendNotification);
    title.setFont (Palette::font (Palette::bodySize, true));
    title.setColour (juce::Label::textColourId, Palette::text);
    title.setBorderSize (juce::BorderSize<int> (0));
    addAndMakeVisible (title);

    playingLabel.setFont (Palette::font (Palette::fileSize));
    playingLabel.setColour (juce::Label::textColourId, Palette::muted);
    playingLabel.setJustificationType (juce::Justification::centredRight);
    playingLabel.setMinimumHorizontalScale (1.0f);
    playingLabel.setBorderSize (juce::BorderSize<int> (0));
    addAndMakeVisible (playingLabel);
    setPlayingCount (0, 0);

    emptyLabel.setText (ko ("재생 중인 큐 없음"), juce::dontSendNotification);
    emptyLabel.setFont (Palette::font());
    emptyLabel.setColour (juce::Label::textColourId, Palette::dimText);
    emptyLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (emptyLabel);

    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false, true, false);
    viewport.setScrollBarThickness (Palette::scrollBarWidth);
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

void ActiveCuesPanel::setPlayingCount (int numPlaying, int numPaused)
{
    auto text = ko ("재생 중 ") + juce::String (numPlaying);
    if (numPaused > 0)
        text << ko (" (일시정지 ") << numPaused << ")";
    playingLabel.setText (text, juce::dontSendNotification);
    playingLabel.setTooltip (text);
}

void ActiveCuesPanel::setPlayingCues (const std::vector<AudioEngine::PlayingCue>& playing)
{
    std::vector<const AudioEngine::PlayingCue*> active;

    for (const auto& p : playing)
        if (! p.loaded)
            active.push_back (&p);

    setPlayingCount ((int) active.size(), (int) std::count_if (active.begin(), active.end(), [] (const auto* p) { return p->paused; }));

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
        row->setBounds (0, y, width, Palette::activeCardHeight);
        y += Palette::activeCardHeight + Palette::cardInset;
    }

    content.setSize (width, juce::jmax (1, y - Palette::cardInset));
}

void ActiveCuesPanel::resized()
{
    auto area = getLocalBounds().reduced (1);
    auto heading = area.removeFromTop (Palette::cardHeaderHeight - 1).reduced (14, 0);
    title.setBounds (heading.removeFromLeft (64));
    playingLabel.setBounds (heading);
    viewport.setBounds (area.reduced (Palette::cardInset));
    emptyLabel.setBounds (area);

    const int width = juce::jmax (1, viewport.getMaximumVisibleWidth());
    int y = 0;

    for (auto& row : rows)
    {
        row->setBounds (0, y, width, Palette::activeCardHeight);
        y += Palette::activeCardHeight + Palette::cardInset;
    }

    content.setSize (width, juce::jmax (1, y - Palette::cardInset));
}

void ActiveCuesPanel::paint (juce::Graphics& g)
{
    Palette::drawCard (g, getLocalBounds());
    g.setColour (Palette::outline);
    g.fillRect (1, Palette::cardHeaderHeight - 1, getWidth() - 2, 1);
}

} // namespace gocue
