#include "CollabWindow.h"
#include "qrcodegen.hpp"
#include <cmath>

namespace gocue::livemix
{
namespace
{
using State = collab::CollabSession::State;
bool inCall (State state) { return state == State::connected || state == State::disconnected; }
juce::String rateNumber (uint32_t rate) { return rate == 44100 ? "44.1" : "48"; }
void card (juce::Graphics& g, juce::Rectangle<int> bounds, juce::Colour line = Palette::line)
{
    if (bounds.isEmpty()) return;
    g.setColour (Palette::card);
    g.fillRoundedRectangle (bounds.toFloat(), Palette::cardRadius);
    g.setColour (line);
    g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), Palette::cardRadius, 1.0f);
}
juce::TextLayout messageLayout (const juce::String& text, int width, float size, juce::Colour colour)
{
    if (width <= 0) return {};
    // DirectWrite permits breaks inside Korean words; the reference uses word-break: keep-all.
    const auto font = bodyFont (size);
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
    attributed.append (wrapped + line, font, colour);
    juce::TextLayout layout;
    layout.createLayout (attributed, (float) width);
    return layout;
}
}

struct CollabPanel::RequestCard : juce::Component
{
    RequestCard (CollabPanel& p, const collab::CollabSession::WebRequestSnapshot& request) : owner (p), value (request)
    {
        setComponentID ("request-" + value.requestId);
        for (auto* l : { &caption, &name, &web })
        {
            l->setBorderSize (juce::BorderSize<int> (0)); l->setMinimumHorizontalScale (1.0f); addAndMakeVisible (*l);
        }
        caption.setText (ko ("들어오기 요청"), juce::dontSendNotification); caption.setFont (captionFont());
        caption.setColour (juce::Label::textColourId, Palette::dimText);
        name.setFont (titleFont()); name.setTooltip (value.nickname);
        web.setFont (titleFont (12)); web.setText (ko ("웹"), juce::dontSendNotification);
        web.setJustificationType (juce::Justification::centred); web.setColour (juce::Label::textColourId, Palette::accent);
        accept.setComponentID ("accept"); accept.setButtonText (ko ("수락"));
        reject.setComponentID ("reject"); reject.setButtonText (ko ("거절"));
        accept.setToggleState (true, juce::dontSendNotification);
        for (auto* button : { &accept, &reject }) { button->setWantsKeyboardFocus (false); addAndMakeVisible (*button); }
        accept.onClick = [this] { if (value.canAccept && owner.actions.acceptWebRequest) owner.actions.acceptWebRequest (value.requestId); };
        reject.onClick = [this] { if (value.canReject && owner.actions.rejectWebRequest) owner.actions.rejectWebRequest (value.requestId); };
        update (request);
    }
    void update (const collab::CollabSession::WebRequestSnapshot& request)
    {
        value = request;
        accept.setEnabled (value.canAccept); reject.setEnabled (value.canReject);
        accept.setAlpha (value.canAccept ? 1.0f : 0.45f); reject.setAlpha (value.canReject ? 1.0f : 0.45f);
        resized();
    }
    void resized() override
    {
        auto inner = getLocalBounds().reduced (14);
        caption.setBounds (inner.removeFromTop (15)); inner.removeFromTop (10);
        auto row = inner.removeFromTop (21);
        const int width = juce::jmin (row.getWidth() - 32, juce::GlyphArrangement::getStringWidthInt (name.getFont(), value.nickname));
        name.setBounds (row.removeFromLeft (juce::jmax (0, width)));
        name.setText (ellipsisedText (name.getFont(), value.nickname, name.getWidth()), juce::dontSendNotification);
        row.removeFromLeft (8); web.setBounds (row.removeFromLeft (24));
        inner.removeFromTop (10); row = inner.removeFromTop (36);
        accept.setBounds (row.removeFromLeft ((row.getWidth() - 8) / 2)); row.removeFromLeft (8); reject.setBounds (row);
    }
    void paint (juce::Graphics& g) override
    {
        card (g, getLocalBounds(), Palette::accent);
        g.setColour (Palette::accent.withAlpha (0.5f));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), Palette::cardRadius - 1, 1.0f);
        g.drawRoundedRectangle (web.getBounds().toFloat().reduced (0.5f, 2.5f), 6, 1);
    }
    CollabPanel& owner;
    collab::CollabSession::WebRequestSnapshot value;
    juce::Label caption, name, web;
    juce::TextButton accept, reject;
};

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
void CollabPanel::Message::resized() { layout = messageLayout (text, getWidth(), size, colour); }
void CollabPanel::Message::paint (juce::Graphics& g) { layout.draw (g, getLocalBounds().toFloat()); }
int CollabPanel::Message::heightFor (int width) const
{
    return (int) std::ceil (messageLayout (text, width, size, colour).getHeight());
}
void CollabPanel::Statistic::paint (juce::Graphics& g)
{
    g.setColour (Palette::card2);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), Palette::controlRadius);
    g.setColour (Palette::line);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), Palette::controlRadius, 1.0f);
    auto area = getLocalBounds().reduced (8, 8);
    g.setColour (Palette::dimText); g.setFont (captionFont());
    g.drawText (caption, area.removeFromTop (15), juce::Justification::centredLeft);
    area.removeFromTop (2);
    const int unitWidth = juce::GlyphArrangement::getStringWidthInt (bodyFont (14.0f), unit);
    auto valueFont = titleFont (20.0f);
    const int available = area.getWidth() - unitWidth - 3;
    const int naturalWidth = juce::GlyphArrangement::getStringWidthInt (valueFont, value);
    if (naturalWidth > available && available > 0)
        valueFont.setHeight (valueFont.getHeight() * available / naturalWidth);
    const int width = juce::GlyphArrangement::getStringWidthInt (valueFont, value);
    g.setColour (Palette::text); g.setFont (valueFont);
    g.drawText (value, area.removeFromLeft (width), juce::Justification::centredLeft);
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
    label (rateCaption, "rate-caption", ko ("샘플레이트"), captionFont(), Palette::dimText);
    label (roomRate, "room-rate", {}, titleFont (14.0f), Palette::dimText);
    roomRate.setJustificationType (juce::Justification::centredRight);
    label (waiting, "waiting", ko ("상대를 기다리는 중"), titleFont (14.0f), Palette::dimText);
    label (notice, "notice", {}, bodyFont (14.0f), Palette::danger);
    label (peerNick, "peer-nick", {}, titleFont(), Palette::text);
    label (connection, "connection", {}, titleFont (14.0f), Palette::lampOn);
    label (volumeCaption, "volume-caption", {}, captionFont(), Palette::dimText);
    label (volumeValue, "volume-value", {}, captionFont(), Palette::text);
    volumeValue.setJustificationType (juce::Justification::centredRight);
    label (roomCode, "room-code", {}, captionFont(), Palette::dimText);
    label (secondNick, "peer-nick-2", {}, titleFont (15), Palette::text);
    label (secondVolumeValue, "volume-value-2", {}, titleFont (14), Palette::text);
    secondVolumeValue.setJustificationType (juce::Justification::centredRight);
    peerNick.setFont (titleFont (15)); volumeValue.setFont (titleFont (14));
    for (size_t i = 0; i < 2; ++i)
    {
        label (seatLabels[i], "", ko ("자리 ") + juce::String ((int) i + 1), titleFont (12), Palette::dimText);
        seatLabels[i].setComponentID ("seat-" + juce::String ((int) i + 1));
        seatLabels[i].setJustificationType (juce::Justification::centred);
        label (webLabels[i], "", ko ("웹"), titleFont (12), Palette::accent);
        webLabels[i].setJustificationType (juce::Justification::centred);
        webLabels[i].setComponentID ("web-" + juce::String ((int) i + 1));
        label (delays[i], "", {}, titleFont (14), Palette::text);
        delays[i].setComponentID ("delay-" + juce::String ((int) i + 1));
    }
    for (int i = 0; i < 4; ++i)
    {
        auto& digit = roomDigits[(size_t) i];
        label (digit, "", {}, titleFont (44.333f), Palette::text); // 40 CSS px in variant A
        digit.setComponentID ("room-digit-" + juce::String (i));
        digit.setJustificationType (juce::Justification::centred);
    }
    auto button = [this] (juce::TextButton& b, const char* id, const juce::String& text, std::function<void()> action)
    {
        // no keyboard focus (the app's rule): hiding the focused nickname must not hand Enter to 방 닫기
        b.setComponentID (id); b.setButtonText (text); b.onClick = std::move (action); b.setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    };
    const auto create = [this]
    {
        if (snapshot.state != State::checkingHost && actions.create) actions.create (settings.getCollabNick());
    };
    button (createButton, "create", ko ("방 만들기"), create);
    createButton.setToggleState (true, juce::dontSendNotification);
    for (int i = 0; i < 2; ++i)
    {
        auto& chip = rateChips[(size_t) i];
        const int rate = i == 0 ? 48000 : 44100;
        chip.setComponentID ("rate-" + juce::String (rate));
        chip.setButtonText (rateNumber (rate) + " kHz");
        chip.setClickingTogglesState (false); chip.setWantsKeyboardFocus (false);
        chip.onClick = [this, rate]
        {
            settings.setCollabRate (rate); settings.saveIfNeeded(); updateEnabled();
        };
        addAndMakeVisible (chip);
    }
    button (joinButton, "join", ko ("참가"), [this] { join(); });
    button (retryButton, "retry", ko ("다시 확인"), create);
    button (dismissButton, "dismiss", ko ("닫기"), actions.leave);
    button (leaveButton, "leave", ko ("방 닫기"), actions.leave);
    button (rebufferButton, "rebuffer", ko ("다시 맞추기"), actions.rebuffer);
    for (size_t i = 0; i < 2; ++i)
    {
        button (kickButtons[i], "", ko ("내보내기"), [this, i]
        {
            if (kickButtons[i].isEnabled() && actions.kickWebGuest) actions.kickWebGuest (people[i].participantId);
        });
        kickButtons[i].setComponentID ("kick-" + juce::String ((int) i + 1));
    }
    label (inviteCaption, "invite-caption", ko ("웹 초대"), captionFont(), Palette::dimText);
    label (inviteUrl, "invite-url", {}, bodyFont (14), Palette::dimText);
    button (copyInvite, "copy-invite", ko ("주소 복사"), [this] { juce::SystemClipboard::copyTextToClipboard (snapshot.webInviteUrl); });
    button (renewInvite, "renew-invite", ko ("새 주소"), [this]
    {
        if (snapshot.canCreateWebInvite && actions.createWebInvite) actions.createWebInvite();
    });
    requestViewport.setComponentID ("web-requests");
    requestViewport.setViewedComponent (&requestHolder, false); requestViewport.setScrollBarsShown (true, false);
    requestViewport.setScrollBarThickness (8); addAndMakeVisible (requestViewport);
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
    reason.setComponentID ("reason"); reason.size = 14.0f; reason.colour = Palette::dimText; addAndMakeVisible (reason);
    headphones.setComponentID ("headphones");
    headphones.setTexts (ko ("상대방 목소리 ON"), ko ("상대방 목소리 OFF"), {});
    headphones.onClick = [this]
    {
        if (actions.monitor) actions.monitor (snapshot.monitorVolume, ! headphones.isOn());
    };
    addAndMakeVisible (headphones);
    volume.setComponentID ("volume"); volume.setSliderStyle (juce::Slider::LinearHorizontal);
    volume.setRange (0, 100, 1); volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volume.setScrollWheelEnabled (false); volume.setWantsKeyboardFocus (false);
    volume.onValueChange = [this]
    {
        volumeValue.setText (juce::String ((int) volume.getValue()) + "%", juce::dontSendNotification);
        if (people[0].participantId != 0 && actions.personMonitorVolume)
            actions.personMonitorVolume (people[0].participantId, (float) volume.getValue() / 100.0f);
        else if (actions.monitor) actions.monitor ((float) volume.getValue() / 100.0f, headphones.isOn());
    };
    addAndMakeVisible (volume);
    secondVolume.setComponentID ("volume-2"); secondVolume.setSliderStyle (juce::Slider::LinearHorizontal);
    secondVolume.setRange (0, 100, 1); secondVolume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    secondVolume.setScrollWheelEnabled (false); secondVolume.setWantsKeyboardFocus (false);
    secondVolume.onValueChange = [this]
    {
        secondVolumeValue.setText (juce::String ((int) secondVolume.getValue()) + "%", juce::dontSendNotification);
        if (actions.personMonitorVolume) actions.personMonitorVolume (people[1].participantId, (float) secondVolume.getValue() / 100.0f);
    };
    addAndMakeVisible (secondVolume);
    // the partner's output, as a channel card's 출력 row: 마스터, a direct pair, or OBS monitoring only
    label (routeCaption, "route-caption", ko ("출력"), captionFont(), Palette::dimText);
    const juce::String routeTexts[] { ko ("마스터"), ko ("직접 출력"), ko ("OBS에서 듣기") };
    const char* routeIds[] { "route-master", "route-direct", "route-obs" };
    for (int i = 0; i < 3; ++i)
    {
        auto& chip = routeChips[(size_t) i];
        chip.setComponentID (routeIds[i]); chip.setButtonText (routeTexts[i]); chip.setClickingTogglesState (false);
        chip.onClick = [this, i]
        {
            if (actions.route) actions.route ((collab::CollabAudio::Route) i, snapshot.routeFirst);
        };
        addAndMakeVisible (chip);
    }
    routePair.setComponentID ("route-pair"); routePair.setWantsKeyboardFocus (false);
    routePair.onChange = [this]
    {
        // like a channel's direct output: a pair picked on a running ASIO device is stored; Windows shows 1-2 only
        const int sel = routePair.getSelectedId();
        const auto outputs = actions.outputs ? actions.outputs() : OutputPairs {};
        if (sel > 0 && outputs.asio && outputs.running && sel + 1 <= outputs.numOutputs && actions.route)
            actions.route (collab::CollabAudio::Route::direct, sel - 1);
    };
    addAndMakeVisible (routePair);
    meter.setComponentID ("peer-meter"); addAndMakeVisible (meter);
    const juce::String captions[] { ko ("지연"), ko ("버퍼"), ko ("손실"), ko ("샘플레이트") };
    for (int i = 0; i < 4; ++i)
    {
        auto& stat = stats[(size_t) i]; stat.caption = captions[i]; stat.unit = i == 3 ? "kHz" : i == 2 ? "%" : "ms";
        stat.setComponentID ("stat-" + juce::String (i)); addAndMakeVisible (stat);
    }
    setSize (440, 430);
    setSnapshot ({});
}
CollabPanel::~CollabPanel() { requestViewport.setViewedComponent (nullptr, false); }

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
    rateCaption.setVisible (entry && ! failed);
    for (auto& chip : rateChips) chip.setVisible (entry && ! failed);
    roomRate.setVisible (host && snapshot.roomMode == collab::contracts::RoomMode::legacy2);
    roomRate.setText (rateNumber (snapshot.wireRate) + " kHz", juce::dontSendNotification);
    for (auto& digit : roomDigits) digit.setVisible (host);
    waiting.setVisible (host); leaveButton.setVisible (host || call);
    message.setVisible (failed); reason.setVisible (failed); retryButton.setVisible (failed); dismissButton.setVisible (failed);
    message.setText (snapshot.message.upToFirstOccurrenceOf ("\n", false, false));
    reason.setText (snapshot.reason);
    notice.setText (snapshot.message, juce::dontSendNotification); notice.setTooltip (snapshot.message);
    notice.setVisible (entry && ! failed && snapshot.message.isNotEmpty());
    notice.setColour (juce::Label::textColourId, state == State::error ? Palette::danger : Palette::dimText);
    createButton.setButtonText (state == State::checkingHost ? ko ("포트 확인 중") : ko ("방 만들기"));
    joinButton.setButtonText (state == State::joining ? ko ("연결 중") : ko ("참가"));
    for (int i = 0; i < 4; ++i) roomDigits[(size_t) i].setText (snapshot.code.substring (i, i + 1), juce::dontSendNotification);
    leaveButton.setButtonText (snapshot.isHost ? ko ("방 닫기") : ko ("나가기"));
    for (auto* c : std::initializer_list<juce::Component*> { &peerNick, &connection, &headphones, &volumeCaption,
                                                           &volumeValue, &volume, &meter, &rebufferButton, &routePair, &routeCaption })
        c->setVisible (call);
    for (auto& stat : stats) stat.setVisible (call && snapshot.roomMode == collab::contracts::RoomMode::legacy2);
    for (auto& chip : routeChips) chip.setVisible (call);
    for (int i = 0; i < 3; ++i) routeChips[(size_t) i].setToggleState ((int) snapshot.route == i, juce::dontSendNotification);
    const auto outputs = actions.outputs ? actions.outputs() : OutputPairs {};
    const bool direct = snapshot.route == collab::CollabAudio::Route::direct;
    const auto pairKey = outputs.names.joinIntoString ("\n") + "|" + juce::String ((int) outputs.asio) + juce::String ((int) outputs.hasOutput)
                         + "|" + juce::String (snapshot.routeFirst) + "|" + juce::String ((int) direct);
    if (pairKey != routePairKey && ! routePair.isPopupActive())
    {
        routePairKey = pairKey;
        fillDirectOutputCombo (routePair, routeChips[1], outputs.names, outputs.asio, outputs.hasOutput, snapshot.routeFirst, direct);
        routeChips[1].setAlpha (routeChips[1].isEnabled() ? 1.0f : 0.45f);   // no output device: it looks as off as it is
    }
    // heard in OBS: LiveMix plays no partner, so the headphone switch and volume have nothing to change (the two labels
    // only fade: a disabled Label already halves its text)
    const bool audible = snapshot.route != collab::CollabAudio::Route::obs;
    for (auto* c : std::initializer_list<juce::Component*> { &headphones, &volume, &volumeCaption, &volumeValue })
    {
        if (c != &volumeCaption && c != &volumeValue) c->setEnabled (audible);
        c->setAlpha (audible ? 1.0f : 0.4f);
    }
    connection.setText (state == State::disconnected ? ko ("연결 끊김") : ko ("연결됨"), juce::dontSendNotification);
    connection.setColour (juce::Label::textColourId, state == State::disconnected ? Palette::danger : Palette::lampOn);
    headphones.setOn (snapshot.monitorOn);
    if (! volume.isMouseButtonDown()) volume.setValue (std::lround (snapshot.monitorVolume * 100.0f), juce::dontSendNotification);
    volumeValue.setText (juce::String ((int) volume.getValue()) + "%", juce::dontSendNotification);
    stats[0].value = juce::String ((int) std::lround (snapshot.estimatedOneWayMs));
    stats[1].value = juce::String (juce::roundToInt (snapshot.bufferMs));   // whole ms, as the delay beside it
    stats[2].value = juce::String (snapshot.lossPercent, 1);
    stats[3].value = rateNumber (snapshot.wireRate);
    updatePeople(); updateWeb();
    updateEnabled(); resized(); repaint();
}

void CollabPanel::updatePeople()
{
    people = {};
    personVisible = {};
    for (size_t n = 0; n < juce::jmin (snapshot.personCount, snapshot.people.size()); ++n)
    {
        const auto& p = snapshot.people[n];
        if (! p.isSelf && p.slot >= 1 && p.slot <= 2)
        {
            const auto i = (size_t) p.slot - 1;
            people[i] = p; personVisible[i] = inCall (snapshot.state);
        }
    }
    // Preserve the original slot-one projection for legacy snapshots without a roster.
    if (snapshot.personCount == 0 && inCall (snapshot.state))
    {
        personVisible[0] = true;
        people[0].slot = 1; people[0].nickname = snapshot.peerNick;
        people[0].monitorVolume = snapshot.monitorVolume;
        people[0].estimatedOneWayMs = snapshot.estimatedOneWayMs;
    }
    const bool audible = snapshot.route != collab::CollabAudio::Route::obs;
    volumeCaption.setVisible (false);
    roomCode.setVisible (inCall (snapshot.state));
    roomCode.setText (ko ("방 코드 ") + snapshot.code, juce::dontSendNotification);
    for (size_t i = 0; i < 2; ++i)
    {
        auto& name = i == 0 ? peerNick : secondNick;
        auto& slider = i == 0 ? volume : secondVolume;
        auto& percent = i == 0 ? volumeValue : secondVolumeValue;
        for (auto* component : std::initializer_list<juce::Component*> { &name, &slider, &percent, &seatLabels[i], &delays[i] })
            component->setVisible (personVisible[i]);
        webLabels[i].setVisible (personVisible[i] && people[i].kind == collab::contracts::ParticipantKind::web);
        kickButtons[i].setVisible (webLabels[i].isVisible() && snapshot.isHost && people[i].participantId != 0
                                  && people[i].participantId == snapshot.kickableWebId
                                  && people[i].state == collab::contracts::ConnectionState::connected);
        kickButtons[i].setEnabled (snapshot.signalingConnected);
        kickButtons[i].setAlpha (snapshot.signalingConnected ? 1.0f : 0.45f);
        slider.setEnabled (audible); slider.setAlpha (audible ? 1.0f : 0.4f); percent.setAlpha (slider.getAlpha());
        if (! slider.isMouseButtonDown()) slider.setValue (std::lround (people[i].monitorVolume * 100.0f), juce::dontSendNotification);
        percent.setText (juce::String ((int) slider.getValue()) + "%", juce::dontSendNotification);
        name.setTooltip (people[i].nickname);
        delays[i].setText (juce::String ((int) std::lround (people[i].estimatedOneWayMs)) + " ms", juce::dontSendNotification);
    }
}

void CollabPanel::updateWeb()
{
    const size_t count = snapshot.isHost ? juce::jmin (snapshot.webRequestCount, snapshot.webRequests.size()) : 0;
    bool changed = count != requests.size();
    for (size_t i = 0; ! changed && i < count; ++i)
        changed = requests[i]->value.requestId != snapshot.webRequests[i].requestId;
    if (changed)
    {
        requests.clear();
        for (size_t i = 0; i < count; ++i)
        {
            auto request = std::make_unique<RequestCard> (*this, snapshot.webRequests[i]);
            requestHolder.addAndMakeVisible (*request); requests.push_back (std::move (request));
        }
    }
    else for (size_t i = 0; i < count; ++i) requests[i]->update (snapshot.webRequests[i]);
    requestViewport.setVisible (count != 0);
    const bool invite = snapshot.isHost && snapshot.canCreateWebInvite && requests.empty();
    for (auto* component : std::initializer_list<juce::Component*> { &inviteCaption, &inviteUrl, &copyInvite, &renewInvite })
        component->setVisible (invite);
    copyInvite.setEnabled (invite && snapshot.webInviteUrl.isNotEmpty());
    copyInvite.setAlpha (copyInvite.isEnabled() ? 1.0f : 0.45f);
    renewInvite.setEnabled (invite);
    if (qrUrl != snapshot.webInviteQrUrl)
    {
        qrUrl = snapshot.webInviteQrUrl; qrImage = {};
        if (qrUrl.isNotEmpty())
        {
            const auto qr = qrcodegen::QrCode::encodeText (qrUrl.toRawUTF8(), qrcodegen::QrCode::Ecc::MEDIUM);
            const int size = qr.getSize() + 8; // four-module quiet zone
            qrImage = juce::Image (juce::Image::RGB, size, size, false);
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                    qrImage.setPixelAt (x, y, qr.getModule (x - 4, y - 4) ? juce::Colours::black : juce::Colours::white);
        }
    }
    inviteUrl.setTooltip (snapshot.webInviteUrl);
}

void CollabPanel::layoutPerson (size_t i, juce::Rectangle<int> bounds)
{
    personBounds[i] = bounds;
    auto inner = bounds.reduced (10, 0);
    auto row = inner.removeFromTop (44);
    seatLabels[i].setBounds (row.removeFromLeft (52).withSizeKeepingCentre (52, 22)); row.removeFromLeft (10);
    if (kickButtons[i].isVisible())
    {
        kickButtons[i].setBounds (row.removeFromRight (70).withSizeKeepingCentre (70, 28)); row.removeFromRight (10);
    }
    const int delayWidth = labelWidthForText (delays[i], delays[i].getText());
    delays[i].setBounds (row.removeFromRight (delayWidth)); row.removeFromRight (10);
    auto& name = i == 0 ? peerNick : secondNick;
    if (webLabels[i].isVisible())
    {
        const int nameWidth = juce::jmin (juce::GlyphArrangement::getStringWidthInt (name.getFont(), people[i].nickname), row.getWidth() - 32);
        name.setBounds (row.removeFromLeft (juce::jmax (0, nameWidth)));
        row.removeFromLeft (8); webLabels[i].setBounds (row.removeFromLeft (24).withSizeKeepingCentre (24, 18));
    }
    else name.setBounds (row);
    name.setText (ellipsisedText (name.getFont(), people[i].nickname, name.getWidth()), juce::dontSendNotification);
    row = inner.removeFromTop (28);
    (i == 0 ? volumeValue : secondVolumeValue).setBounds (row.removeFromRight (44)); row.removeFromRight (6);
    (i == 0 ? volume : secondVolume).setBounds (row);
}

void CollabPanel::layoutInvite (juce::Rectangle<int> bounds)
{
    inviteCard = bounds;
    auto inner = bounds.reduced (14);
    inviteCaption.setBounds (inner.removeFromTop (15)); inner.removeFromTop (10);
    qrBounds = inner.removeFromLeft (116).withHeight (116); inner.removeFromLeft (12);
    inviteField = inner.removeFromTop (36);
    inviteUrl.setBounds (inviteField.reduced (10, 0));
    // Shown like the approved mock (no scheme) so more of the address fits; [주소 복사] still copies the full URL.
    const auto shownUrl = snapshot.webInviteUrl.contains ("://") ? snapshot.webInviteUrl.fromFirstOccurrenceOf ("://", false, false)
                                                                 : snapshot.webInviteUrl;
    inviteUrl.setText (ellipsisedText (inviteUrl.getFont(), shownUrl, inviteUrl.getWidth()), juce::dontSendNotification);
    inner.removeFromTop (8); copyInvite.setBounds (inner.removeFromTop (36));
    inner.removeFromTop (8); renewInvite.setBounds (inner.removeFromTop (36));
}
void CollabPanel::updateEnabled()
{
    const bool available = snapshot.state == State::off || snapshot.state == State::error || snapshot.state == State::hostUnavailable;
    const bool named = settings.getCollabNick().isNotEmpty();
    // while busy, the pressed button reads 포트 확인 중 / 연결 중 at full strength (its click does nothing then)
    const bool checking = snapshot.state == State::checkingHost, joining = snapshot.state == State::joining;
    createButton.setEnabled ((available && named) || checking); retryButton.setEnabled (available && named);
    joinButton.setEnabled ((available && named && code.complete()) || joining); code.setEnabled (available);
    createButton.setInterceptsMouseClicks (! checking, false); joinButton.setInterceptsMouseClicks (! joining, false);
    for (int i = 0; i < 2; ++i)
    {
        auto& chip = rateChips[(size_t) i];
        chip.setEnabled (available); chip.setAlpha (available ? 1.0f : 0.45f);
        chip.setToggleState (settings.getCollabRate() == (i == 0 ? 48000 : 44100), juce::dontSendNotification);
    }
    for (auto* button : { &createButton, &joinButton, &retryButton }) button->setAlpha (button->isEnabled() ? 1.0f : 0.45f);
}
void CollabPanel::join()
{
    if (joinButton.isEnabled() && snapshot.state != State::joining && actions.join) actions.join (code.getCode(), settings.getCollabNick());
}
bool CollabPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey) { if (actions.hide) actions.hide(); return true; }
    return false;
}

struct CollabPanel::Layout
{
    int padding = 14, gap = 10, outerGap = 12;
    int height = 0;
    juce::Rectangle<int> nickCaption, nickname, requests, first, join, stats, invite, buttons;
};

CollabPanel::Layout CollabPanel::layoutForSize (int width, int height) const
{
    const auto state = snapshot.state;
    const bool failed = state == State::hostUnavailable;
    const auto measure = [&] (bool compact)
    {
        Layout result;
        result.padding = compact ? 10 : 14;
        result.gap = compact ? 6 : 10;
        result.outerGap = compact ? 6 : 12;
        const int padding = result.padding, gap = result.gap, outerGap = result.outerGap;
        // Lay out the outer rows without clipping to the available height. The same rectangles
        // drive resized() and the viewport's content height, so no separate minimum can drift.
        int y = 16;
        const auto take = [&] (int rowHeight)
        {
            const juce::Rectangle<int> row (16, y, width - 32, rowHeight);
            y += rowHeight;
            return row;
        };
        if (! inCall (state))
        {
            result.nickCaption = take (15); y += outerGap;
            result.nickname = take (36); y += outerGap;
        }
        const int count = (int) personVisible[0] + (int) personVisible[1];
        const int callHeight = 28 + 15 + 76 * count + juce::jmax (0, count - 1) * 8 + 32 + 40 + 46 + 4 * gap;
        if (requestViewport.isVisible())
        {
            // Preserve the request list's own scrolling and let it use spare window height.
            const int remaining = inCall (state) ? callHeight + (stats[0].isVisible() ? 58 + outerGap : 0) : 154;
            const int available = juce::jmax (120, height - 16 - y - remaining - 36 - 2 * outerGap);
            result.requests = take (juce::jmin (available, (int) requests.size() * 132 - 12));
            y += outerGap;
        }
        if (inCall (state))
        {
            result.first = take (callHeight);
            if (stats[0].isVisible()) { y += outerGap; result.stats = take (58); }
            if (inviteCaption.isVisible()) { y += outerGap; result.invite = take (177); }
            y += outerGap; result.buttons = take (36);
        }
        else if (state == State::hostWaiting)
        {
            const bool webRoom = snapshot.roomMode == collab::contracts::RoomMode::v2;
            result.first = take (webRoom ? 154 : 202);
            if (webRoom)
            {
                if (inviteCaption.isVisible()) { y += outerGap; result.invite = take (177); }
                y += outerGap; result.buttons = take (36);
            }
        }
        else
        {
            const int textWidth = width - 32 - 2 * padding;
            result.first = take (failed ? 2 * padding + message.heightFor (textWidth) + reason.heightFor (textWidth) + 36 + 2 * gap
                                        : 2 * padding + 15 + 34 + 44 + 2 * gap);
            y += outerGap;
            result.join = take (2 * padding + 15 + 58 + 44 + 2 * gap + (notice.isVisible() ? 18 + gap : 0));
        }
        result.height = y + 16;
        return result;
    };
    const auto roomy = measure (false);
    // Preserve the existing compact layout, using the visible height even when the panel scrolls.
    return height < 430 || (notice.isVisible() && height < 458) || (failed && height < roomy.height) ? measure (true) : roomy;
}

void CollabPanel::resized()
{
    const auto layout = layoutForSize (getWidth(), viewportHeight > 0 ? viewportHeight : getHeight());
    const int padding = layout.padding, gap = layout.gap;
    firstCard = layout.first; joinCard = layout.join;
    inviteCard = qrBounds = {};
    personBounds = {};
    const auto state = snapshot.state;
    const bool failed = state == State::hostUnavailable;
    const auto layoutRequests = [&]
    {
        if (! requestViewport.isVisible()) return;
        // One request remains fully visible; a long pending list scrolls within the same card area.
        const int height = layout.requests.getHeight();
        requestViewport.setBounds (layout.requests);
        const int width = requestViewport.getWidth() - ((int) requests.size() * 132 - 12 > height ? 8 : 0);
        requestHolder.setSize (width, (int) requests.size() * 132 - 12);
        for (size_t i = 0; i < requests.size(); ++i) requests[i]->setBounds (0, (int) i * 132, width, 120);
    };
    if (inCall (state))
    {
        layoutRequests();
        // the 출력 row keeps every control at its size: the 400 px window only tightens the gaps
        const int callGap = gap;
        auto inner = firstCard.reduced (14);
        auto row = inner.removeFromTop (15);
        connection.setBounds (row.removeFromRight (labelWidthForText (connection, connection.getText())));
        row.removeFromRight (10); roomCode.setBounds (row);
        inner.removeFromTop (callGap);
        bool first = true;
        for (size_t i = 0; i < 2; ++i)
            if (personVisible[i])
            {
                if (! first) inner.removeFromTop (8);
                layoutPerson (i, inner.removeFromTop (76)); first = false;
            }
        inner.removeFromTop (callGap); row = inner.removeFromTop (32);
        // 마스터 58, 직접 출력 74, OBS에서 듣기 100; the pair keeps the 88 px of a channel card's direct output, where
        // '31-32 …' reads in full; the caption shows only when it fits too
        const bool captioned = row.getWidth() >= 40 + 58 + 74 + 100 + 3 * 6 + 88;
        routeCaption.setVisible (captioned);
        if (captioned) { routeCaption.setBounds (row.removeFromLeft (34)); row.removeFromLeft (6); }
        routeChips[0].setBounds (row.removeFromLeft (58)); row.removeFromLeft (6);
        routeChips[1].setBounds (row.removeFromLeft (74)); row.removeFromLeft (6);
        routeChips[2].setBounds (row.removeFromRight (100)); row.removeFromRight (6);
        routePair.setBounds (row);
        inner.removeFromTop (callGap); headphones.setBounds (inner.removeFromTop (40));
        inner.removeFromTop (callGap); meter.setBounds (inner.removeFromTop (46));
        if (stats[0].isVisible())
        {
            row = layout.stats;
            const int statWidth = (row.getWidth() - 24) / 4;
            for (int i = 0; i < 4; ++i)
            {
                stats[(size_t) i].setBounds (i == 3 ? row : row.removeFromLeft (statWidth));
                if (i < 3) row.removeFromLeft (8);
            }
        }
        if (inviteCaption.isVisible()) layoutInvite (layout.invite);
        row = layout.buttons;
        rebufferButton.setBounds (row.removeFromLeft ((row.getWidth() - 8) / 2));
        row.removeFromLeft (8); leaveButton.setBounds (row);
        return;
    }
    nickCaption.setBounds (layout.nickCaption);
    nickname.setBounds (layout.nickname);
    layoutRequests();
    if (state == State::hostWaiting)
    {
        const bool webRoom = snapshot.roomMode == collab::contracts::RoomMode::v2;
        auto inner = firstCard.reduced (14);
        auto caption = inner.removeFromTop (17);
        if (roomRate.isVisible()) roomRate.setBounds (caption.removeFromRight (labelWidthForText (roomRate, roomRate.getText())));
        codeCaption.setBounds (caption); inner.removeFromTop (10);
        const auto row = inner.removeFromTop (74);
        for (int i = 0; i < 4; ++i) roomDigits[(size_t) i].setBounds (row.getX() + i * 76, row.getY(), 66, 74);
        inner.removeFromTop (10); waiting.setBounds (inner.removeFromTop (17));
        if (webRoom)
        {
            if (inviteCaption.isVisible()) layoutInvite (layout.invite);
            leaveButton.setBounds (layout.buttons);
        }
        else { inner.removeFromTop (10); leaveButton.setBounds (inner.removeFromTop (36)); }
        return;
    }
    if (failed)
    {
        const int messageHeight = message.heightFor (firstCard.getWidth() - 2 * padding);
        const int reasonHeight = reason.heightFor (firstCard.getWidth() - 2 * padding);
        auto inner = firstCard.reduced (padding);
        message.setBounds (inner.removeFromTop (messageHeight)); inner.removeFromTop (gap);
        reason.setBounds (inner.removeFromTop (reasonHeight)); inner.removeFromTop (gap);
        auto row = inner.removeFromTop (36);
        retryButton.setBounds (row.removeFromLeft ((row.getWidth() - 8) / 2));
        row.removeFromLeft (8); dismissButton.setBounds (row);
    }
    else
    {
        auto inner = firstCard.reduced (padding);
        rateCaption.setBounds (inner.removeFromTop (15)); inner.removeFromTop (gap);
        auto row = inner.removeFromTop (34);
        rateChips[0].setBounds (row.removeFromLeft ((row.getWidth() - 8) / 2));
        row.removeFromLeft (8); rateChips[1].setBounds (row);
        inner.removeFromTop (gap); createButton.setBounds (inner.removeFromTop (44));
    }
    auto inner = joinCard.reduced (padding);
    codeCaption.setBounds (inner.removeFromTop (15)); inner.removeFromTop (gap);
    code.setBounds (inner.removeFromTop (58).withWidth (256)); inner.removeFromTop (gap);
    joinButton.setBounds (inner.removeFromTop (44)); inner.removeFromTop (gap);
    notice.setBounds (inner.removeFromTop (18));
}
void CollabPanel::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);
    card (g, firstCard, snapshot.state == State::hostUnavailable ? Palette::danger : Palette::line);
    card (g, joinCard);
    card (g, inviteCard);
    for (size_t i = 0; i < personBounds.size(); ++i)
    {
        if (personBounds[i].isEmpty()) continue;
        g.setColour (Palette::card2); g.fillRoundedRectangle (personBounds[i].toFloat(), Palette::controlRadius);
        g.setColour (Palette::line); g.drawRoundedRectangle (personBounds[i].toFloat().reduced (0.5f), Palette::controlRadius, 1);
        g.setColour (Palette::background); g.fillRoundedRectangle (seatLabels[i].getBounds().toFloat(), 6);
        if (webLabels[i].isVisible())
        {
            g.setColour (Palette::accent.withAlpha (0.5f));
            g.drawRoundedRectangle (webLabels[i].getBounds().toFloat().reduced (0.5f), 6, 1);
        }
    }
    if (! inviteCard.isEmpty())
    {
        g.setColour (Palette::card2); g.fillRoundedRectangle (inviteField.toFloat(), Palette::controlRadius);
        g.setColour (Palette::line); g.drawRoundedRectangle (inviteField.toFloat().reduced (0.5f), Palette::controlRadius, 1);
        if (qrImage.isValid())
        {
            g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
            g.drawImageWithin (qrImage, qrBounds.getX(), qrBounds.getY(), qrBounds.getWidth(), qrBounds.getHeight(), juce::RectanglePlacement::stretchToFit);
        }
    }
    if (snapshot.state == State::hostWaiting)
        for (const auto& digit : roomDigits)
        {
            g.setColour (Palette::card2); g.fillRoundedRectangle (digit.getBounds().toFloat(), Palette::controlRadius);
            g.setColour (Palette::line); g.drawRoundedRectangle (digit.getBounds().toFloat().reduced (0.5f), Palette::controlRadius, 1.0f);
        }
}

CollabViewport::CollabViewport (LiveMixSettings& settings, CollabPanel::Actions actions)
    : panel (settings, std::move (actions))
{
    setOpaque (true);
    setViewedComponent (&panel, false);
    setScrollBarsShown (true, false); // inherits the same LiveMix scrollbars as the main channel list
    setSize (panel.getWidth(), panel.getHeight());
}
CollabViewport::~CollabViewport() { setViewedComponent (nullptr, false); }
void CollabViewport::setSnapshot (const collab::CollabSession::Snapshot& snapshot)
{
    panel.setSnapshot (snapshot);
    resized();
}
void CollabViewport::resized()
{
    juce::Viewport::resized();
    panel.viewportHeight = getHeight();
    const bool scrolls = panel.layoutForSize (getWidth(), getHeight()).height > getHeight();
    const int width = getWidth() - (scrolls ? getScrollBarThickness() : 0);
    panel.setSize (width, juce::jmax (getHeight(), panel.layoutForSize (width, getHeight()).height));
    panel.resized();
}

CollabWindow::CollabWindow (collab::CollabSession& collab, LiveMixSettings& settings, std::function<CollabPanel::OutputPairs()> outputs)
    : DocumentWindow (ko ("합방"), Palette::background, DocumentWindow::allButtons), session (collab)
{
    setUsingNativeTitleBar (true);
    CollabPanel::Actions actions;
    actions.route = [this] (collab::CollabAudio::Route route, int first) { session.setRoute (route, first); timerCallback(); };
    actions.outputs = std::move (outputs);
    actions.create = [this] (const auto& nick) { session.createRoom (nick); timerCallback(); };
    actions.join = [this] (const auto& code, const auto& nick) { session.joinRoom (code, nick); timerCallback(); };
    actions.leave = [this] { session.leave(); timerCallback(); };
    actions.rebuffer = [this] { session.rebuffer(); timerCallback(); };
    actions.monitor = [this] (float value, bool on) { session.setMonitor (value, on); timerCallback(); };
    actions.personMonitorVolume = [this] (auto id, float value) { session.setPersonMonitorVolume (id, value); timerCallback(); };
    actions.createWebInvite = [this] { session.createWebInvite(); };
    actions.acceptWebRequest = [this] (const auto& id) { session.acceptWebRequest (id); };
    actions.rejectWebRequest = [this] (const auto& id) { session.rejectWebRequest (id); };
    actions.kickWebGuest = [this] (auto id) { session.kickWebGuest (id); };
    actions.hide = [this] { closeButtonPressed(); };
    content = new CollabViewport (settings, std::move (actions));
    setContentOwned (content, true);
    setResizable (true, false);
    const auto border = getContentComponentBorder();
    setResizeLimits (400 + border.getLeftAndRight(), 400 + border.getTopAndBottom(), 10000, 10000);
    timerCallback(); startTimer (200);
}
CollabWindow::~CollabWindow() { stopTimer(); clearContentComponent(); }
juce::Rectangle<int> CollabWindow::grownBounds (juce::Rectangle<int> current, juce::Rectangle<int> workArea, juce::BorderSize<int> frame)
{
    const int room = workArea.getHeight() - frame.getTopAndBottom();
    const int height = juce::jlimit (juce::jmin (400, room), 800, room);
    const int top = workArea.getY() + frame.getTop();
    const int lowest = juce::jmax (top, workArea.getBottom() - frame.getBottom() - height);
    return { current.getX(), juce::jlimit (top, lowest, current.getY()), current.getWidth(), height };
}
void CollabWindow::open() { timerCallback(); setVisible (true); toFront (true); }
void CollabWindow::closeButtonPressed() { setVisible (false); }
bool CollabWindow::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey) { closeButtonPressed(); return true; }
    return false;
}
void CollabWindow::timerCallback()
{
    const auto snapshot = session.getSnapshot();
    const bool webRoom = snapshot.roomMode == collab::contracts::RoomMode::v2 && snapshot.state != State::off;
    if (webRoom && ! expandedForRoom)
    {
        // The native title bar and borders sit outside these bounds: fit the whole window in the work area and move it up
        // rather than leaving [다시 맞추기]·[방 닫기] below a 1080p-at-150 % or 768p screen.
        const auto frame = getPeer() != nullptr ? getPeer()->getFrameSize() : juce::BorderSize<int>();
        const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getBounds());
        const auto grown = display != nullptr ? grownBounds (getBounds(), display->userArea, frame) : getBounds().withHeight (800);
        const auto border = getContentComponentBorder();
        setResizeLimits (400 + border.getLeftAndRight(), juce::jmin (640, grown.getHeight()), 10000, 10000);
        setBounds (grown);
        expandedForRoom = true;
    }
    if (roomCode != snapshot.code || ! snapshot.canCreateWebInvite) inviteRequested = false;
    roomCode = snapshot.code;
    if (snapshot.isHost && snapshot.canCreateWebInvite && snapshot.webInviteUrl.isEmpty() && ! inviteRequested)
    {
        inviteRequested = true; session.createWebInvite();
    }
    content->setSnapshot (snapshot);
}
}
