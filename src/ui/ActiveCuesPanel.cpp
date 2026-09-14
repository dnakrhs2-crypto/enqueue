#include "ui/ActiveCuesPanel.h"

#include "model/CueColors.h"
#include "ui/UiUtils.h"

#include <algorithm>

namespace gocue
{

namespace
{
    /** Which of a cue's waits is its card's main state: its own wait first, then its pre-wait, then a post-wait. */
    int waitPriority (WaitProgress::Kind kind) noexcept
    {
        return kind == WaitProgress::Kind::waitCue ? 0 : kind == WaitProgress::Kind::preWait ? 1 : 2;
    }

    juce::String waitPillText (const WaitProgress& w, double now)
    {
        const auto name = w.kind == WaitProgress::Kind::waitCue ? ko ("대기 ")
                        : w.kind == WaitProgress::Kind::preWait ? ko ("프리웨이트 ") : ko ("포스트웨이트 ");
        return name + formatCountdown (w.remaining (now));
    }
}

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
            // a waiting card cancels its wait (a post-wait card: the next cue's start); a running card stops its sound
            const auto target = stopTarget.isNull() ? id : stopTarget;

            if (waiting && owner.onCancelWaitRequested)
                owner.onCancelWaitRequested (target);
            else if (owner.onStopRequested)
                owner.onStopRequested (target);
            else
                engine.fadeOutAndStop (target);
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

    /** A running instance. 'extraWait' = a wait of this very cue running alongside (its post-wait, or the pre-wait of a
        restart put on while it plays): a countdown pill next to the state. */
    void update (const AudioEngine::PlayingCue& p, const Cue* cue, const WaitProgress* extraWait, double now)
    {
        waiting = false;
        stopTarget = juce::Uuid::null();
        paused = p.paused;
        fadingOut = p.fadingOut;
        colourIndex = cue != nullptr ? cue->color : 0;
        fraction = p.progress >= 0.0 ? juce::jlimit (0.0, 1.0, p.progress) : 0.0;
        infinite = p.progress < 0.0;
        pauseButton.setVisible (true);
        pauseButton.setButtonText (paused ? ko ("재개") : ko ("일시정지"));
        panicButton.setTooltip (ko ("이 큐 페이드 정지"));
        setNames (cue);
        stateText = paused ? ko ("일시정지") : fadingOut ? ko ("페이드 아웃") : ko ("재생 중");
        extraText = extraWait != nullptr ? waitPillText (*extraWait, now) : juce::String();

        const auto infinity = ko ("∞");
        timeLabel.setText (clockText (p.positionSeconds) + " / " + (infinite ? infinity : clockText (juce::jmax (0.0, p.lengthSeconds))),
                           juce::dontSendNotification);
        remainingLabel.setText (infinite ? infinity : formatCountdown (juce::jmax (0.0, p.remainingSeconds)), juce::dontSendNotification);
        finishUpdate();
    }

    /** A wait counting down for a cue that is not running: its own wait, its pre-wait, or the post-wait left after its
        sound. 'extraWait' = a second wait of the same cue (a wait cue's post-wait), as a pill. Elapsed / total of the
        wait, what is left of it, a bar; the panic button cancels it. */
    void updateWait (const WaitProgress& w, const WaitProgress* extraWait, const Cue* cue, double now)
    {
        waiting = true;
        stopTarget = w.kind == WaitProgress::Kind::postWait ? w.startsCueId : juce::Uuid::null();   // the next cue's start
        paused = false;
        fadingOut = false;
        infinite = false;
        colourIndex = cue != nullptr ? cue->color : 0;
        fraction = w.fraction (now);
        pauseButton.setVisible (false);
        panicButton.setTooltip (w.kind == WaitProgress::Kind::postWait ? ko ("다음 큐가 이어지지 않게 취소")
                                                                       : ko ("이 대기를 취소 (뒤에 예약된 자동 계속도 함께)"));
        setNames (cue);
        stateText = w.kind == WaitProgress::Kind::waitCue ? ko ("대기")
                  : w.kind == WaitProgress::Kind::preWait ? ko ("프리웨이트") : ko ("포스트웨이트");
        extraText = extraWait != nullptr ? waitPillText (*extraWait, now) : juce::String();
        timeLabel.setText (clockText (juce::jmax (0.0, now - w.startedAt)) + " / " + clockText (w.total()), juce::dontSendNotification);
        remainingLabel.setText (formatCountdown (w.remaining (now)), juce::dontSendNotification);
        finishUpdate();
    }

    juce::Colour stateColour() const noexcept
    {
        return waiting ? Palette::waiting : paused ? Palette::paused : (fadingOut ? Palette::fadingOut : Palette::playing);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, Palette::cardInset);
        auto top = area.removeFromTop (20);
        const auto pillFont = Palette::font (Palette::pillSize, true);
        const int stateWidth = juce::GlyphArrangement::getStringWidthInt (pillFont, stateText) + 14;
        stateBounds = top.removeFromRight (juce::jmin (stateWidth, top.getWidth() / 2));
        top.removeFromRight (8);
        extraBounds = {};
        if (extraText.isNotEmpty())
        {
            // the countdown pill keeps its whole text where it can: the name gives way first (it ellipsises)
            const int extraWidth = juce::GlyphArrangement::getStringWidthInt (pillFont, extraText) + 14;
            extraBounds = top.removeFromRight (juce::jmin (extraWidth, juce::jmax (0, top.getWidth() * 3 / 5)));
            top.removeFromRight (8);
        }
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
        const auto pillFont = Palette::font (Palette::pillSize, true);
        const auto statePill = stateBounds.toFloat();
        g.setColour (stateColour().withAlpha (Palette::statePillAlpha));
        g.fillRoundedRectangle (statePill, Palette::pillRadius (statePill));
        g.setColour (stateColour());
        g.setFont (pillFont);
        g.drawText (stateText, stateBounds.reduced (7, 0), juce::Justification::centred, true);
        if (! extraBounds.isEmpty())
        {
            const auto extraPill = extraBounds.toFloat();
            g.setColour (Palette::waiting.withAlpha (Palette::statePillAlpha));
            g.fillRoundedRectangle (extraPill, Palette::pillRadius (extraPill));
            g.setColour (Palette::waiting);
            g.drawText (extraText, extraBounds.reduced (7, 0), juce::Justification::centred, true);
        }
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
    static juce::String clockText (double seconds)
    {
        return seconds > 0.0 ? formatSeconds (seconds) : juce::String ("0:00.0");
    }

    void setNames (const Cue* cue)
    {
        numberLabel.setText (cue != nullptr ? cue->number : juce::String(), juce::dontSendNotification);
        nameLabel.setText (cue != nullptr ? cue->name : ko ("(삭제된 큐)"), juce::dontSendNotification);
        nameLabel.setTooltip (nameLabel.getText());
    }

    void finishUpdate()
    {
        remainingLabel.setColour (juce::Label::textColourId, stateColour());
        timeLabel.setTooltip (timeLabel.getText());
        remainingLabel.setTooltip (remainingLabel.getText());
        resized();
        repaint();
    }

    void scrub (const juce::MouseEvent& e)
    {
        // a wait has no position to move to
        if (infinite || waiting || ! owner.isScrubEnabled() || ! barArea.expanded (0, 6).contains (e.getPosition()))
            return;
        const double f = juce::jlimit (0.0, 1.0, (double) (e.x - barArea.getX()) / (double) juce::jmax (1, barArea.getWidth()));
        engine.seekToFraction (id, f);
    }

    ActiveCuesPanel& owner;
    AudioEngine& engine;
    const juce::Uuid id;
    juce::Uuid stopTarget = juce::Uuid::null();   // what the panic button acts on when it is not this cue (a post-wait card: the next cue)
    juce::TextButton pauseButton, panicButton;
    juce::Label numberLabel, nameLabel, timeLabel, remainingLabel;
    juce::Rectangle<int> barArea, stateBounds, extraBounds, colourBounds;
    juce::String stateText, extraText;
    int colourIndex = 0;
    double fraction = 0.0;
    bool paused = false, fadingOut = false, infinite = false, waiting = false;
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

void ActiveCuesPanel::setPlayingCount (int numPlaying, int numPaused, int numWaiting)
{
    auto text = ko ("재생 중 ") + juce::String (numPlaying);
    if (numPaused > 0)
        text << ko (" (일시정지 ") << numPaused << ")";
    if (numWaiting > 0)
        text << ko (" · 대기 ") << numWaiting;
    playingLabel.setText (text, juce::dontSendNotification);
    playingLabel.setTooltip (text);
}

void ActiveCuesPanel::setPlayingCues (const std::vector<AudioEngine::PlayingCue>& playing, const std::vector<WaitProgress>& waits, double now)
{
    std::vector<const AudioEngine::PlayingCue*> active;

    for (const auto& p : playing)
        if (! p.loaded)
            active.push_back (&p);

    std::sort (active.begin(), active.end(), [this] (const AudioEngine::PlayingCue* a, const AudioEngine::PlayingCue* b)
    {
        return newestFirst ? a->startOrder > b->startOrder : a->startOrder < b->startOrder;
    });

    // a wait of a cue that runs already rides on its card as a pill; every other cue with waits gets one card, its
    // main state being its own wait before its pre-wait before a post-wait, a second wait (a wait cue's post-wait) a pill
    auto isActive = [&active] (const juce::Uuid& id)
    {
        return std::any_of (active.begin(), active.end(), [&id] (const AudioEngine::PlayingCue* p) { return p->id == id; });
    };
    struct Waiting { juce::Uuid cueId; const WaitProgress* main = nullptr; const WaitProgress* extra = nullptr; };
    std::vector<Waiting> waitingCards;   // in the order the waits were reported

    for (const auto& w : waits)
    {
        if (isActive (w.cueId))
            continue;

        auto it = std::find_if (waitingCards.begin(), waitingCards.end(), [&w] (const Waiting& c) { return c.cueId == w.cueId; });

        if (it == waitingCards.end())
        {
            waitingCards.push_back ({ w.cueId, &w, nullptr });
        }
        else if (waitPriority (w.kind) < waitPriority (it->main->kind))
        {
            it->extra = it->main;
            it->main = &w;
        }
        else if (it->extra == nullptr)
        {
            it->extra = &w;
        }
    }

    setPlayingCount ((int) active.size(), (int) std::count_if (active.begin(), active.end(), [] (const auto* p) { return p->paused; }),
                     (int) waitingCards.size());

    // reuse rows by cue id, drop the rest
    std::vector<std::unique_ptr<Row>> next;

    auto takeRow = [this] (const juce::Uuid& id)
    {
        std::unique_ptr<Row> row;

        for (auto& existing : rows)
            if (existing != nullptr && existing->getId() == id)
                row = std::move (existing);

        if (row == nullptr)
        {
            row = std::make_unique<Row> (*this, engine, id);
            content.addAndMakeVisible (*row);
        }

        return row;
    };

    auto lookup = [this] (const juce::Uuid& id) { return findCue ? findCue (id) : cues.findById (id); };

    for (const auto* p : active)
    {
        auto row = takeRow (p->id);
        const WaitProgress* extra = nullptr;   // the pre-wait of a pending restart before the post-wait of the run

        for (const auto& w : waits)
            if (w.cueId == p->id && (extra == nullptr || waitPriority (w.kind) < waitPriority (extra->kind)))
                extra = &w;

        row->update (*p, lookup (p->id), extra, now);
        next.push_back (std::move (row));
    }

    for (const auto& c : waitingCards)
    {
        auto row = takeRow (c.cueId);
        row->updateWait (*c.main, c.extra, lookup (c.cueId), now);
        next.push_back (std::move (row));
    }

    rows = std::move (next);
    emptyLabel.setVisible (rows.empty());
    layoutRows();
}

void ActiveCuesPanel::layoutRows()
{
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
    layoutRows();
}

void ActiveCuesPanel::paint (juce::Graphics& g)
{
    Palette::drawCard (g, getLocalBounds());
    g.setColour (Palette::outline);
    g.fillRect (1, Palette::cardHeaderHeight - 1, getWidth() - 2, 1);
}

} // namespace gocue
