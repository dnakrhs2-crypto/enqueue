#include "CollabWindow.h"
#include <cmath>

namespace gocue::livemix
{
namespace
{
using State = collab::CollabSession::State;
bool inCall (State state) { return state == State::connected || state == State::disconnected; }
void card (juce::Graphics& g, juce::Rectangle<int> bounds, juce::Colour line = Palette::line)
{
    if (bounds.isEmpty()) return;
    g.setColour (Palette::card);
    g.fillRoundedRectangle (bounds.toFloat(), Palette::cardRadius);
    g.setColour (line);
    g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), Palette::cardRadius, 1.0f);
}
juce::TextLayout messageLayout (const juce::String& text, int width)
{
    if (width <= 0) return {};
    // DirectWrite permits breaks inside Korean words; the reference uses word-break: keep-all.
    const auto font = bodyFont (15.0f);
    juce::String wrapped, line;
    for (const auto& word : juce::StringArray::fromTokens (text, " ", ""))
    {
        const auto next = line.isEmpty() ? word : line + " " + word;
        if (line.isNotEmpty() && juce::GlyphArrangement::getStringWidthInt (font, next) > width)
        {
            wrapped += line + "\n";
            line = word;
        }
        else line = next;
    }
    juce::AttributedString attributed;
    attributed.setWordWrap (juce::AttributedString::none);
    attributed.append (wrapped + line, font, Palette::text);
    juce::TextLayout layout;
    layout.createLayout (attributed, (float) width);
    return layout;
}
}

CollabField::CollabField()
{
    setFont (bodyFont (15.0f));
    setMultiLine (false);
    setScrollbarsShown (false);
    setBorder (juce::BorderSize<int> (0, 12, 0, 12));
    setIndents (0, 0);
    setJustification (juce::Justification::centredLeft);
    for (int colour : { backgroundColourId, outlineColourId, focusedOutlineColourId, shadowColourId })
        setColour (colour, juce::Colours::transparentBlack);
}
void CollabField::paint (juce::Graphics& g)
{
    g.setColour (Palette::card2);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), Palette::controlRadius);
    juce::TextEditor::paint (g);
}
void CollabField::paintOverChildren (juce::Graphics& g)
{
    const bool focused = isEnabled() && ! isReadOnly() && (firstEmpty || hasKeyboardFocus (false));
    g.setColour (focused ? Palette::accent : Palette::line);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), Palette::controlRadius, 1.0f);
    if (focused)
    {
        g.setColour (Palette::accent.withAlpha (0.5f));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), Palette::controlRadius - 1.0f, 1.0f);
    }
}

CollabCodeEntry::Digit::Digit (CollabCodeEntry& owner, int position) : entry (owner), index (position)
{
    setComponentID ("digit-" + juce::String (index));
    setInputRestrictions (1, "0123456789");
    setJustification (juce::Justification::centred);
    setFont (titleFont (33.25f)); // 30 CSS px in variant A, at the app's Malgun Gothic metrics
    setSelectAllWhenFocused (true);
    onEscapeKey = [this] { if (entry.onEscape) entry.onEscape(); };
    onTextChange = [this] { entry.changed(); };
}
void CollabCodeEntry::Digit::insertTextAtCaret (const juce::String& text) { entry.insert (index, text); }
bool CollabCodeEntry::Digit::keyPressed (const juce::KeyPress& key)
{
    if (isReadOnly() || ! isEnabled()) return false;
    if (key == juce::KeyPress::backspaceKey) { entry.backspace (index); return true; }
    if (key == juce::KeyPress::returnKey)
    {
        if (entry.complete() && entry.onSubmit) entry.onSubmit();
        return true;
    }
    if (key == juce::KeyPress::leftKey) { entry.focus (juce::jmax (0, index - 1)); return true; }
    if (key == juce::KeyPress::rightKey) { entry.focus (juce::jmin (3, index + 1)); return true; }
    return CollabField::keyPressed (key);
}
CollabCodeEntry::CollabCodeEntry()
{
    for (int i = 0; i < 4; ++i)
    {
        digits[(size_t) i] = std::make_unique<Digit> (*this, i);
        addAndMakeVisible (*digits[(size_t) i]);
    }
    changed();
}
juce::String CollabCodeEntry::getCode() const
{
    juce::String result;
    for (const auto& digit : digits) result += digit->getText();
    return result;
}
bool CollabCodeEntry::complete() const { return getCode().length() == 4; }
void CollabCodeEntry::focus (int index)
{
    auto& digit = *digits[(size_t) index];
    if (digit.isShowing()) digit.grabKeyboardFocus();
    digit.selectAll();
}
void CollabCodeEntry::insert (int index, const juce::String& text)
{
    if (! isEnabled()) return;
    const auto numbers = text.retainCharacters ("0123456789");
    if (numbers.isEmpty()) return;
    if (numbers.length() >= 4) index = 0;
    int next = index;
    for (int i = 0; i < numbers.length() && next < 4; ++i, ++next)
        digits[(size_t) next]->setText (numbers.substring (i, i + 1), false);
    focus (juce::jmin (3, next));
    changed();
}
void CollabCodeEntry::backspace (int index)
{
    if (digits[(size_t) index]->isEmpty() && index > 0) --index;
    digits[(size_t) index]->setText ({}, false);
    focus (index);
    changed();
}
void CollabCodeEntry::changed()
{
    bool foundEmpty = false;
    for (auto& digit : digits)
    {
        digit->firstEmpty = ! foundEmpty && digit->isEmpty();
        foundEmpty = foundEmpty || digit->isEmpty();
        digit->repaint();
    }
    if (onChange) onChange();
}
void CollabCodeEntry::resized()
{
    for (int i = 0; i < 4; ++i) digits[(size_t) i]->setBounds (i * 66, 0, 58, 58);
}

void CollabPanel::Message::setText (const juce::String& value) { text = value; resized(); repaint(); }
void CollabPanel::Message::resized() { layout = messageLayout (text, getWidth()); }
void CollabPanel::Message::paint (juce::Graphics& g) { layout.draw (g, getLocalBounds().toFloat()); }
int CollabPanel::Message::heightFor (int width) const { return (int) std::ceil (messageLayout (text, width).getHeight()); }
void CollabPanel::Statistic::paint (juce::Graphics& g)
{
    g.setColour (Palette::card2);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), Palette::controlRadius);
    g.setColour (Palette::line);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), Palette::controlRadius, 1.0f);
    auto area = getLocalBounds().reduced (12, 8);
    g.setColour (Palette::dimText); g.setFont (captionFont());
    g.drawText (caption, area.removeFromTop (15), juce::Justification::centredLeft);
    area.removeFromTop (2);
    const int unitWidth = juce::GlyphArrangement::getStringWidthInt (bodyFont (14.0f), unit);
    const auto shown = ellipsisedText (titleFont (20.0f), value, area.getWidth() - unitWidth - 3);
    const int width = juce::GlyphArrangement::getStringWidthInt (titleFont (20.0f), shown);
    g.setColour (Palette::text); g.setFont (titleFont (20.0f));
    g.drawText (shown, area.removeFromLeft (width), juce::Justification::centredLeft);
    area.removeFromLeft (3);
    g.setColour (Palette::dimText); g.setFont (bodyFont (14.0f));
    g.drawText (unit, area.withTrimmedTop (5), juce::Justification::centredLeft);
}

CollabPanel::CollabPanel (LiveMixSettings& prefs, Actions callbacks) : settings (prefs), actions (std::move (callbacks))
{
    auto label = [this] (juce::Label& l, const char* id, const juce::String& text, juce::Font font, juce::Colour colour)
    {
        l.setComponentID (id); l.setText (text, juce::dontSendNotification); l.setFont (font);
        l.setBorderSize (juce::BorderSize<int> (0)); l.setMinimumHorizontalScale (1.0f);
        l.setColour (juce::Label::textColourId, colour); addAndMakeVisible (l);
    };
    label (nickCaption, "nick-caption", ko ("닉네임"), captionFont(), Palette::dimText);
    label (codeCaption, "code-caption", ko ("방 코드"), captionFont(), Palette::dimText);
    label (waiting, "waiting", ko ("상대를 기다리는 중"), titleFont (14.0f), Palette::dimText);
    label (reason, "reason", {}, bodyFont (14.0f), Palette::dimText);
    label (notice, "notice", {}, bodyFont (14.0f), Palette::danger);
    label (peerNick, "peer-nick", {}, titleFont(), Palette::text);
    label (connection, "connection", {}, titleFont (14.0f), Palette::lampOn);
    label (volumeCaption, "volume-caption", ko ("헤드폰 음량"), captionFont(), Palette::dimText);
    label (volumeValue, "volume-value", {}, captionFont(), Palette::text);
    volumeValue.setJustificationType (juce::Justification::centredRight);
    for (int i = 0; i < 4; ++i)
    {
        auto& digit = roomDigits[(size_t) i];
        label (digit, "", {}, titleFont (44.333f), Palette::text); // 40 CSS px in variant A
        digit.setComponentID ("room-digit-" + juce::String (i));
        digit.setJustificationType (juce::Justification::centred);
    }
    auto button = [this] (juce::TextButton& b, const char* id, const juce::String& text, std::function<void()> action)
    {
        b.setComponentID (id); b.setButtonText (text); b.onClick = std::move (action); addAndMakeVisible (b);
    };
    const auto create = [this] { if (actions.create) actions.create (settings.getCollabNick()); };
    button (createButton, "create", ko ("방 만들기"), create);
    createButton.setToggleState (true, juce::dontSendNotification);
    button (joinButton, "join", ko ("참가"), [this] { join(); });
    button (retryButton, "retry", ko ("다시 확인"), create);
    button (dismissButton, "dismiss", ko ("닫기"), actions.leave);
    button (leaveButton, "leave", ko ("방 닫기"), actions.leave);
    button (rebufferButton, "rebuffer", ko ("다시 맞추기"), actions.rebuffer);
    nickname.setComponentID ("nickname"); nickname.setInputRestrictions (40);
    nickname.setText (settings.getCollabNick(), false);
    nickname.onTextChange = [this]
    {
        settings.setCollabNick (nickname.getText()); settings.saveIfNeeded(); updateEnabled();
    };
    nickname.onFocusLost = [this] { nickname.setText (settings.getCollabNick(), false); };
    nickname.onEscapeKey = actions.hide;
    addAndMakeVisible (nickname);
    code.setComponentID ("code"); code.onChange = [this] { updateEnabled(); };
    code.onSubmit = [this] { join(); }; code.onEscape = actions.hide; addAndMakeVisible (code);
    message.setComponentID ("host-message"); addAndMakeVisible (message);
    headphones.setComponentID ("headphones");
    headphones.setTexts (ko ("헤드폰 ON"), ko ("헤드폰 OFF"), {});
    headphones.onClick = [this]
    {
        if (actions.monitor) actions.monitor ((float) volume.getValue() / 100.0f, ! headphones.isOn());
    };
    addAndMakeVisible (headphones);
    volume.setComponentID ("volume"); volume.setSliderStyle (juce::Slider::LinearHorizontal);
    volume.setRange (0, 100, 1); volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volume.setScrollWheelEnabled (false); volume.setWantsKeyboardFocus (false);
    volume.onValueChange = [this]
    {
        volumeValue.setText (juce::String ((int) volume.getValue()) + "%", juce::dontSendNotification);
        if (actions.monitor) actions.monitor ((float) volume.getValue() / 100.0f, headphones.isOn());
    };
    addAndMakeVisible (volume);
    meter.setComponentID ("peer-meter"); addAndMakeVisible (meter);
    const juce::String captions[] { ko ("지연"), ko ("버퍼"), ko ("손실") };
    for (int i = 0; i < 3; ++i)
    {
        auto& stat = stats[(size_t) i]; stat.caption = captions[i]; stat.unit = i == 2 ? "%" : "ms";
        stat.setComponentID ("stat-" + juce::String (i)); addAndMakeVisible (stat);
    }
    setSize (440, 430);
    setSnapshot ({});
}

void CollabPanel::setSnapshot (const collab::CollabSession::Snapshot& value)
{
    snapshot = value;
    const auto state = snapshot.state;
    const bool call = inCall (state), host = state == State::hostWaiting, failed = state == State::hostUnavailable;
    const bool entry = ! call && ! host;
    nickCaption.setVisible (! call); nickname.setVisible (! call);
    nickname.setReadOnly (! entry || state == State::checkingHost || state == State::joining);
    codeCaption.setVisible (! call); code.setVisible (entry);
    createButton.setVisible (entry && ! failed); joinButton.setVisible (entry);
    for (auto& digit : roomDigits) digit.setVisible (host);
    waiting.setVisible (host); leaveButton.setVisible (host || call);
    message.setVisible (failed); reason.setVisible (failed); retryButton.setVisible (failed); dismissButton.setVisible (failed);
    message.setText (snapshot.message.upToFirstOccurrenceOf ("\n", false, false));
    reason.setText (snapshot.reason, juce::dontSendNotification); reason.setTooltip (snapshot.reason);
    notice.setText (snapshot.message, juce::dontSendNotification); notice.setTooltip (snapshot.message);
    notice.setVisible (entry && ! failed && snapshot.message.isNotEmpty());
    notice.setColour (juce::Label::textColourId, state == State::error ? Palette::danger : Palette::dimText);
    createButton.setButtonText (state == State::checkingHost ? ko ("포트 확인 중") : ko ("방 만들기"));
    joinButton.setButtonText (state == State::joining ? ko ("연결 중") : ko ("참가"));
    for (int i = 0; i < 4; ++i) roomDigits[(size_t) i].setText (snapshot.code.substring (i, i + 1), juce::dontSendNotification);
    leaveButton.setButtonText (snapshot.isHost ? ko ("방 닫기") : ko ("나가기"));
    for (auto* c : std::initializer_list<juce::Component*> { &peerNick, &connection, &headphones, &volumeCaption,
                                                           &volumeValue, &volume, &meter, &rebufferButton }) c->setVisible (call);
    for (auto& stat : stats) stat.setVisible (call);
    connection.setText (state == State::disconnected ? ko ("연결 끊김") : ko ("연결됨"), juce::dontSendNotification);
    connection.setColour (juce::Label::textColourId, state == State::disconnected ? Palette::danger : Palette::lampOn);
    headphones.setOn (snapshot.monitorOn);
    if (! volume.isMouseButtonDown()) volume.setValue (std::lround (snapshot.monitorVolume * 100.0f), juce::dontSendNotification);
    volumeValue.setText (juce::String ((int) volume.getValue()) + "%", juce::dontSendNotification);
    stats[0].value = juce::String ((int) std::lround (snapshot.estimatedOneWayMs));
    stats[1].value = juce::String (snapshot.bufferMs, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    stats[2].value = juce::String (snapshot.lossPercent, 1);
    updateEnabled(); resized(); repaint();
}
void CollabPanel::updateEnabled()
{
    const bool available = snapshot.state == State::off || snapshot.state == State::error || snapshot.state == State::hostUnavailable;
    const bool named = settings.getCollabNick().isNotEmpty();
    createButton.setEnabled (available && named); retryButton.setEnabled (available && named);
    joinButton.setEnabled (available && named && code.complete()); code.setEnabled (available);
    for (auto* button : { &createButton, &joinButton, &retryButton }) button->setAlpha (button->isEnabled() ? 1.0f : 0.45f);
}
void CollabPanel::join()
{
    if (joinButton.isEnabled() && actions.join) actions.join (code.getCode(), settings.getCollabNick());
}
bool CollabPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey) { if (actions.hide) actions.hide(); return true; }
    return false;
}

void CollabPanel::resized()
{
    auto area = getLocalBounds().reduced (16);
    firstCard = joinCard = {};
    const auto state = snapshot.state;
    const bool failed = state == State::hostUnavailable;
    // The 400 px minimum preserves type and control sizes by tightening only card padding and inner gaps.
    const int padding = failed && getHeight() < 430 ? 10 : 14;
    const int gap = failed && getHeight() < 430 ? 6 : 10;
    if (inCall (state))
    {
        firstCard = area.removeFromTop (222);
        auto inner = firstCard.reduced (14);
        auto row = inner.removeFromTop (21);
        connection.setBounds (row.removeFromRight (labelWidthForText (connection, connection.getText())));
        row.removeFromRight (10); peerNick.setBounds (row);
        peerNick.setText (ellipsisedText (peerNick.getFont(), snapshot.peerNick, row.getWidth()), juce::dontSendNotification);
        peerNick.setTooltip (snapshot.peerNick);
        inner.removeFromTop (10); headphones.setBounds (inner.removeFromTop (40));
        inner.removeFromTop (10); row = inner.removeFromTop (15);
        volumeValue.setBounds (row.removeFromRight (60)); volumeCaption.setBounds (row);
        inner.removeFromTop (10); volume.setBounds (inner.removeFromTop (32));
        inner.removeFromTop (10); meter.setBounds (inner.removeFromTop (46));
        area.removeFromTop (12);
        row = area.removeFromTop (58);
        const int statWidth = (row.getWidth() - 16) / 3;
        for (int i = 0; i < 3; ++i)
        {
            stats[(size_t) i].setBounds (i == 2 ? row : row.removeFromLeft (statWidth));
            if (i < 2) row.removeFromLeft (8);
        }
        area.removeFromTop (12); row = area.removeFromTop (36);
        rebufferButton.setBounds (row.removeFromLeft ((row.getWidth() - 8) / 2));
        row.removeFromLeft (8); leaveButton.setBounds (row);
        return;
    }
    nickCaption.setBounds (area.removeFromTop (15)); area.removeFromTop (12);
    nickname.setBounds (area.removeFromTop (36)); area.removeFromTop (12);
    if (state == State::hostWaiting)
    {
        firstCard = area.removeFromTop (200);
        auto inner = firstCard.reduced (14);
        codeCaption.setBounds (inner.removeFromTop (15)); inner.removeFromTop (10);
        const auto row = inner.removeFromTop (74);
        for (int i = 0; i < 4; ++i) roomDigits[(size_t) i].setBounds (row.getX() + i * 76, row.getY(), 66, 74);
        inner.removeFromTop (10); waiting.setBounds (inner.removeFromTop (17));
        inner.removeFromTop (10); leaveButton.setBounds (inner.removeFromTop (36));
        return;
    }
    if (failed)
    {
        const int messageHeight = message.heightFor (area.getWidth() - 2 * padding);
        firstCard = area.removeFromTop (2 * padding + messageHeight + 17 + 36 + 2 * gap);
        auto inner = firstCard.reduced (padding);
        message.setBounds (inner.removeFromTop (messageHeight)); inner.removeFromTop (gap);
        reason.setBounds (inner.removeFromTop (17)); inner.removeFromTop (gap);
        auto row = inner.removeFromTop (36);
        retryButton.setBounds (row.removeFromLeft ((row.getWidth() - 8) / 2));
        row.removeFromLeft (8); dismissButton.setBounds (row);
    }
    else
    {
        firstCard = area.removeFromTop (72);
        createButton.setBounds (firstCard.reduced (14));
    }
    area.removeFromTop (12);
    joinCard = area.removeFromTop (2 * padding + 15 + 58 + 44 + 2 * gap + (notice.isVisible() ? 28 : 0));
    auto inner = joinCard.reduced (padding);
    codeCaption.setBounds (inner.removeFromTop (15)); inner.removeFromTop (gap);
    code.setBounds (inner.removeFromTop (58).withWidth (256)); inner.removeFromTop (gap);
    joinButton.setBounds (inner.removeFromTop (44)); inner.removeFromTop (10);
    notice.setBounds (inner.removeFromTop (18));
}
void CollabPanel::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);
    card (g, firstCard, snapshot.state == State::hostUnavailable ? Palette::danger : Palette::line);
    card (g, joinCard);
    if (snapshot.state == State::hostWaiting)
        for (const auto& digit : roomDigits)
        {
            g.setColour (Palette::card2); g.fillRoundedRectangle (digit.getBounds().toFloat(), Palette::controlRadius);
            g.setColour (Palette::line); g.drawRoundedRectangle (digit.getBounds().toFloat().reduced (0.5f), Palette::controlRadius, 1.0f);
        }
}

CollabWindow::CollabWindow (collab::CollabSession& collab, LiveMixSettings& settings)
    : DocumentWindow (ko ("합방"), Palette::background, DocumentWindow::allButtons), session (collab)
{
    setUsingNativeTitleBar (true);
    CollabPanel::Actions actions;
    actions.create = [this] (const auto& nick) { session.createRoom (nick); timerCallback(); };
    actions.join = [this] (const auto& code, const auto& nick) { session.joinRoom (code, nick); timerCallback(); };
    actions.leave = [this] { session.leave(); timerCallback(); };
    actions.rebuffer = [this] { session.rebuffer(); timerCallback(); };
    actions.monitor = [this] (float value, bool on) { session.setMonitor (value, on); timerCallback(); };
    actions.hide = [this] { closeButtonPressed(); };
    content = new CollabPanel (settings, std::move (actions));
    setContentOwned (content, true);
    setResizable (true, false);
    const auto border = getContentComponentBorder();
    setResizeLimits (400 + border.getLeftAndRight(), 400 + border.getTopAndBottom(), 10000, 10000);
    timerCallback(); startTimer (200);
}
CollabWindow::~CollabWindow() { stopTimer(); clearContentComponent(); }
void CollabWindow::open() { timerCallback(); setVisible (true); toFront (true); }
void CollabWindow::closeButtonPressed() { setVisible (false); }
bool CollabWindow::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey) { closeButtonPressed(); return true; }
    return false;
}
void CollabWindow::timerCallback() { content->setSnapshot (session.getSnapshot()); }
}
