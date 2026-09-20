#include "ui/KeyCapture.h"
#include "app/ShortcutDisplay.h"
#include "ui/UiUtils.h"
#include "ui/ShortcutRouter.h"
#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue
{
KeyCaptureSession::Result KeyCaptureSession::special (const juce::KeyPress& key)
{
    const juce::KeyPress keyboard (key.getKeyCode(), key.getModifiers().withoutMouseButtons(), key.getTextCharacter());
    if (const auto valid = ShortcutKeyCodec::validate (keyboard); valid.failed())
        return { Outcome::rejected, {}, ko ("지원하지 않는 키입니다. Win 조합·한/영·한자·IME·미디어 키는 사용할 수 없습니다.") };
    return { Outcome::candidate, ShortcutKeyCodec::normalise (keyboard), {} };
}
KeyCaptureSession::Result KeyCaptureSession::input (const juce::KeyPress& key, bool repeat, bool systemKey)
{
    if (repeat) return { Outcome::repeated, {}, {} };
    if (key.isKeyCode (juce::KeyPress::escapeKey)) return { Outcome::cancelled, {}, {} };
    if (systemKey) return { Outcome::rejected, {}, ko ("Win 조합·한/영·한자·IME 키는 지원하지 않습니다.") };
    if (key.getKeyCode() == 0) return { Outcome::waiting, {}, ko ("Ctrl / Alt / Shift와 함께 사용할 키를 누르세요.") };
    for (const auto code : { juce::KeyPress::playKey, juce::KeyPress::stopKey, juce::KeyPress::fastForwardKey, juce::KeyPress::rewindKey })
        if (key.isKeyCode (code))
            return { Outcome::rejected, {}, ko ("미디어 키는 지원하지 않습니다. F13~F24 같은 키로 장치를 설정하세요.") };
    return special (key);
}
void KeyCaptureSession::reset (bool projectCue)
{
    value = {}; cue = projectCue; confirmed = false; device.clear(); address.reset();
    observations.clear(); identifiers.clear(); values.clear(); lastChange = 0;
}
bool KeyCaptureSession::chooseKey (const juce::KeyPress& key)
{
    if (! std::holds_alternative<std::monostate> (value) || ! key.isValid()) return false;
    value = key;
    return true;
}
bool KeyCaptureSession::observe (const MidiInputEvent& event, const juce::String& identifier)
{
    if (event.channel < 1 || event.channel > 16 || event.number < 0 || event.number > 127 || event.value < 0 || event.value > 127
        || (event.kind != MidiTrigger::Kind::note && event.kind != MidiTrigger::Kind::cc)) return false;
    observations[event.token()] = event;
    identifiers[event.token()] = identifier;
    if (std::holds_alternative<std::monostate> (value))
    {
        if (event.kind == MidiTrigger::Kind::note && (! event.noteOn || event.value == 0)) return false;
        MidiTrigger trigger;
        trigger.kind = event.kind; trigger.channel = event.channel; trigger.number = event.number;
        trigger.source = cue ? juce::String ("any") : identifier;
        value = trigger; device = identifier; address = event.token();
    }
    if (! address || ! (*address == event.token()) || device != identifier) return false;
    if (event.kind == MidiTrigger::Kind::cc && (values.empty() || values.back() != event.value))
    {
        values.push_back (event.value);
        if (values.size() > 128) values.erase (values.begin() + 1);
        lastChange = event.observedTimeMs;
    }
    return true;
}
void KeyCaptureSession::edit (MidiTrigger trigger)
{
    if (cue) trigger.source = "any";
    if (std::holds_alternative<std::monostate> (value)) confirmed = true; // editing a stored rule
    else if (const auto* old = std::get_if<MidiTrigger> (&value); old != nullptr && old->kind != trigger.kind) confirmed = false;
    value = std::move (trigger);
}
KeyCaptureSession::Suggestion KeyCaptureSession::suggestion() const
{
    Suggestion result;
    if (const auto* trigger = std::get_if<MidiTrigger> (&value)) result.trigger = *trigger;
    result.text = ko ("값 변화 미확인 — 입력 형태를 선택하고 규칙을 확인하세요.");
    if (values.size() < 2) return result;
    result.changed = true;
    const auto [lo, hi] = std::minmax_element (values.begin(), values.end());
    result.trigger.highThreshold = (*lo + *hi + 1) / 2;
    result.trigger.lowThreshold = juce::jmax (*lo, result.trigger.highThreshold - 1);
    result.trigger.edge = values[1] < values[0] ? MidiTrigger::Edge::falling : MidiTrigger::Edge::rising;
    const bool continuous = std::any_of (values.begin(), values.end(), [lo, hi] (int v) { return v != *lo && v != *hi; });
    if (continuous)
    {
        result.trigger.behavior = MidiTrigger::Behavior::pulse;
        result.trigger.edge = MidiTrigger::Edge::rising;
        result.trigger.lowThreshold = juce::jmax (*lo, result.trigger.highThreshold - 4);
    }
    result.text = ko ("관측 ") + juce::String (*lo) + "–" + juce::String (*hi) + ko (" · 제안: ")
        + (continuous ? ko ("노브·페이더") : result.trigger.edge == MidiTrigger::Edge::falling ? ko ("하강 페달·버튼") : ko ("상승 페달·버튼"))
        + ko (" · high ") + juce::String (result.trigger.highThreshold) + " / low " + juce::String (result.trigger.lowThreshold)
        + ko (" (자동 확정 아님)");
    return result;
}
void KeyCaptureSession::choosePreset (Preset preset)
{
    if (auto* trigger = std::get_if<MidiTrigger> (&value); trigger != nullptr && trigger->kind == MidiTrigger::Kind::cc)
    {
        auto suggested = suggestion().trigger;
        suggested.behavior = preset == Preset::pedal ? MidiTrigger::Behavior::gate : MidiTrigger::Behavior::pulse;
        if (preset == Preset::toggle) suggested.edge = MidiTrigger::Edge::both;
        else if (preset == Preset::knob) suggested.edge = MidiTrigger::Edge::rising;
        *trigger = suggested;
        confirmed = true;
    }
}
bool KeyCaptureSession::needsPreset() const
{
    const auto* trigger = std::get_if<MidiTrigger> (&value);
    return trigger != nullptr && trigger->kind == MidiTrigger::Kind::cc && ! confirmed;
}
bool KeyCaptureSession::canRegister() const
{
    if (const auto* key = std::get_if<juce::KeyPress> (&value)) return key->isValid();
    const auto* trigger = std::get_if<MidiTrigger> (&value);
    return trigger != nullptr && ! needsPreset() && trigger->validate (cue).wasOk();
}
bool KeyCaptureSession::released() const
{
    for (const auto& [token, event] : observations)
    {
        juce::ignoreUnused (token);
        if (event.kind == MidiTrigger::Kind::note && event.noteOn && event.value > 0) return false;
    }
    if (const auto* trigger = std::get_if<MidiTrigger> (&value);
        trigger != nullptr && trigger->kind == MidiTrigger::Kind::cc && trigger->behavior == MidiTrigger::Behavior::gate)
    {
        // Require an actual inactive region, including when thresholds were edited.
        for (const auto& [token, event] : observations)
            if (event.kind == MidiTrigger::Kind::cc && (address ? token == *address
                : MidiTriggerRules::matchesAddress (*trigger, event, identifiers.at (token))))
            {
                if (trigger->edge == MidiTrigger::Edge::rising ? event.value > trigger->lowThreshold : event.value < trigger->highThreshold)
                    return false;
            }
    }
    return true;
}
bool KeyCaptureSession::moving (double nowMs) const
{
    const auto* trigger = std::get_if<MidiTrigger> (&value);
    return trigger != nullptr && trigger->kind == MidiTrigger::Kind::cc && trigger->behavior == MidiTrigger::Behavior::pulse
        && ! values.empty() && nowMs - lastChange < 200.0;
}

KeyCapture::KeyCapture (ShortcutService& s, std::function<bool()> held) : service (&s), keysHeld (std::move (held))
{
    if (! keysHeld)
        keysHeld = []
        {
           #if JUCE_WINDOWS
            for (int vk = VK_BACK; vk < 256; ++vk)
                if ((GetAsyncKeyState (vk) & 0x8000) != 0) return true;
           #else
            for (const auto& key : ShortcutKeyCodec::allowedKeys())
                if (juce::KeyPress::isKeyCurrentlyDown (key.code)) return true;
            if (juce::ModifierKeys::getCurrentModifiersRealtime().isAnyModifierKeyDown()) return true;
           #endif
            return false;
        };
    setWantsKeyboardFocus (true);
    setComponentID ("hotkeyCapture");
    status.setFont (Palette::font (Palette::bodySize));
    status.setJustificationType (juce::Justification::topLeft);
    status.setMinimumHorizontalScale (1.0f);
    status.setInterceptsMouseClicks (false, false);
    statusView.setViewedComponent (&status, false);
    statusView.setScrollBarsShown (true, false);
    statusView.setWantsKeyboardFocus (false);
    addAndMakeVisible (statusView);
    registerButton.setButtonText (ko ("등록"));
    cancelButton.setButtonText (ko ("취소"));
    specialButton.setButtonText (ko ("특수 키 선택"));
    relearnButton.setButtonText (ko ("다시 학습"));
    specialButton.setTooltip (ko ("Esc를 후보로 선택합니다. Ctrl / Alt / Shift를 누른 채 클릭하면 조합도 선택할 수 있습니다."));
    for (auto* button : { &registerButton, &cancelButton, &specialButton, &relearnButton })
    {
        button->setWantsKeyboardFocus (false);
        button->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (button);
    }
    addChildComponent (ruleFields);
    ruleFields.getProperties().set ("inputCaptureField", true);
    const auto field = [this] (juce::Label& label, const char* text, juce::Component& control)
    {
        label.setText (ko (text), juce::dontSendNotification);
        label.setFont (Palette::font());
        label.setMinimumHorizontalScale (1.0f);
        ruleFields.addAndMakeVisible (label);
        ruleFields.addAndMakeVisible (control);
    };
    field (sourceLabel, "장치 범위", source); field (kindLabel, "종류", kind); field (channelLabel, "채널", channel);
    field (presetLabel, "입력 형태 확인", preset); field (numberLabel, "번호 (0–127)", number);
    field (highLabel, "high (1–127)", high); field (lowLabel, "low (0–126)", low);
    field (edgeLabel, "엣지", edge); field (behaviorLabel, "동작", behavior);
    field (debounceLabel, "바운스 (0–500ms)", debounce); field (velocityLabel, "최소 velocity (1–127)", velocity);
    kind.addItem (ko ("노트"), 1); kind.addItem ("CC", 2);
    channel.addItem (ko ("전체 채널"), 1);
    for (int i = 1; i <= 16; ++i) channel.addItem ("ch" + juce::String (i), i + 1);
    preset.addItem (ko ("페달·버튼"), 1); preset.addItem (ko ("토글"), 2); preset.addItem (ko ("노브·페이더"), 3);
    preset.setComponentID ("midiPreset");
    number.setComponentID ("midiNumber"); high.setComponentID ("midiHigh"); low.setComponentID ("midiLow");
    debounce.setComponentID ("midiDebounce"); velocity.setComponentID ("midiVelocity");
    preset.setTextWhenNothingSelected (ko ("선택하여 확인"));
    edge.addItem (ko ("상승"), 1); edge.addItem (ko ("하강"), 2); edge.addItem (ko ("양쪽"), 3);
    behavior.addItem (ko ("gate · 누름/해제"), 1); behavior.addItem (ko ("pulse · 전환마다"), 2);
    for (auto* box : { &source, &kind, &channel, &edge, &behavior }) box->onChange = [this] { readFields(); };
    preset.onChange = [this]
    {
        if (syncing || preset.getSelectedId() == 0) return;
        session.choosePreset (static_cast<KeyCaptureSession::Preset> (preset.getSelectedId() - 1));
        updateCandidate (true);
    };
    for (auto* editor : { &number, &high, &low, &debounce, &velocity })
    {
        editor->setFont (Palette::font());
        editor->setInputRestrictions (3, "0123456789");
        editor->onTextChange = [this] { readFields(); };
        editor->onEscapeKey = [this] { cancel(); };
    }
    cancelButton.onClick = [this] { cancel(); };
    specialButton.onClick = [this] { chooseSpecial(); };
    relearnButton.onClick = [this] { relearn(); };
    registerButton.onClick = [this]
    {
        if (! active || ! session.canRegister() || service == nullptr || service->isEditingLocked()) return;
        const auto checked = decision();
        if (! checked.allowed) { showText (checked.message, true); return; }
        registrationPending = true;
        registerButton.setEnabled (false);
        specialButton.setEnabled (false);
        relearnButton.setEnabled (false);
        ruleFields.setEnabled (false);
        showText (ko ("누른 키·노트·CC gate를 모두 놓으면 등록합니다. Esc = 취소"), false);
        pollKeyRelease();
    };
}
KeyCapture::~KeyCapture() { cancel (false); }
void KeyCapture::start (const juce::String& name, bool cue, std::optional<MidiTrigger> initial)
{
    cancel (false);
    if (service == nullptr || service->isEditingLocked()) return;
    target = name; projectCue = cue; active = true; session.reset (cue);
    if (initial) session.edit (*initial);
    const auto token = ++generation;
    const juce::Component::SafePointer<KeyCapture> safe (this);
    service->beginCapture (this, [safe, token] (const juce::KeyPress& key)
        { if (safe != nullptr && safe->generation == token) safe->receive (key); },
        [safe, token] { if (safe != nullptr && safe->generation == token) safe->cancel(); });
    service->setMidiCaptureReceiver (this, [safe, token] (const MidiInputEvent& event, const juce::String& id)
        { if (safe != nullptr && safe->generation == token) safe->receiveMidi (event, id); });
    preset.setSelectedId (0, juce::dontSendNotification);
    updateCandidate (true);
    startTimerHz (40);
    if (isShowing()) grabKeyboardFocus();
}
void KeyCapture::relearn()
{
    if (! active || submitting) return;
    start (target, projectCue);
}
void KeyCapture::cancel (bool notify)
{
    stopTimer();
    registrationPending = submitting = observedAvailableDevice = false;
    ++generation;
    const bool wasActive = active;
    active = false;
    session.reset (projectCue);
    if (service != nullptr) service->endCapture (this);
    registerButton.setEnabled (false);
    specialButton.setEnabled (true); relearnButton.setEnabled (true); ruleFields.setEnabled (true);
    if (wasActive && notify && onFinished) { auto finished = onFinished; finished(); }
}
void KeyCapture::receive (const juce::KeyPress& key)
{
    if (! active) return;
    if (key.isKeyCode (juce::KeyPress::escapeKey)) { cancel(); return; }
    if (registrationPending || submitting) return;
    bool systemKey = false;
   #if JUCE_WINDOWS
    systemKey = (GetKeyState (VK_LWIN) & 0x8000) != 0 || (GetKeyState (VK_RWIN) & 0x8000) != 0;
   #endif
    accept (KeyCaptureSession::input (key, false, systemKey));
}
void KeyCapture::receiveMidi (const MidiInputEvent& event, const juce::String& id)
{
    if (! active) return;
    const bool first = std::holds_alternative<std::monostate> (session.candidate());
    if (first && keysHeld()) return;
    uiDirty |= session.observe (event, id);
    observedAvailableDevice |= service != nullptr && session.observedDevice() == id && service->getAvailableMidiDevices().count (id) != 0;
    if (first && ! registrationPending && ! submitting) { updateCandidate (true); uiDirty = false; }
}
KeyCapture::Decision KeyCapture::decision() const
{
    if (const auto* key = std::get_if<juce::KeyPress> (&session.candidate())) return validate ? validate (*key) : Decision();
    if (const auto* trigger = std::get_if<MidiTrigger> (&session.candidate()))
    {
        if (auto result = trigger->validate (projectCue); result.failed()) return { false, result.getErrorMessage() };
        if (session.needsPreset()) return { false, ko ("페달·버튼 / 토글 / 노브·페이더를 선택해 확인하세요.") };
        return validateMidi ? validateMidi (*trigger) : Decision();
    }
    return { false, {} };
}
void KeyCapture::pollKeyRelease()
{
    if (! active) return;
    if (service == nullptr || service->isEditingLocked()) { cancel(); return; }
    if (observedAvailableDevice && service->getAvailableMidiDevices().count (session.observedDevice()) == 0) { cancel(); return; }
    if (uiDirty && ! registrationPending && ! submitting) { updateCandidate(); uiDirty = false; }
    if (! registrationPending || submitting || keysHeld() || ! session.released()) return;
    const auto checked = decision();
    if (! checked.allowed) { complete (juce::Result::fail (checked.message)); return; }
    registrationPending = false; submitting = true;
    showText (ko ("충돌 확인·저장 중 — Esc = 취소"), false);
    const auto token = generation;
    const juce::Component::SafePointer<KeyCapture> safe (this);
    Completion done = [safe, token] (juce::Result result)
    { if (safe != nullptr && safe->generation == token && safe->active) safe->complete (result); };
    const auto candidate = session.candidate();
    if (onSubmit) { auto submit = onSubmit; submit (candidate, std::move (done)); }
    else
    {
        auto callback = onRegister;
        if (const auto* key = std::get_if<juce::KeyPress> (&candidate); key != nullptr && callback) callback (*key);
        done (juce::Result::ok());
    }
}
void KeyCapture::complete (juce::Result result)
{
    if (result.wasOk()) { cancel(); return; }
    submitting = registrationPending = false;
    ruleFields.setEnabled (true); specialButton.setEnabled (true); relearnButton.setEnabled (true);
    registerButton.setEnabled (session.canRegister());
    showText (ko ("등록하지 못했습니다: ") + result.getErrorMessage() + ko ("\n후보를 유지했습니다. 다시 등록하거나 Esc로 취소하세요."), true);
}
void KeyCapture::accept (KeyCaptureSession::Result result)
{
    if (result.outcome == KeyCaptureSession::Outcome::cancelled) { cancel(); return; }
    if (result.outcome == KeyCaptureSession::Outcome::candidate && session.chooseKey (result.key)) updateCandidate();
    else if (result.outcome == KeyCaptureSession::Outcome::rejected && std::holds_alternative<std::monostate> (session.candidate()))
        showText (result.message, true);
}
void KeyCapture::chooseSpecial()
{
    if (active && service != nullptr && ! service->isEditingLocked())
        accept (KeyCaptureSession::special (juce::KeyPress (juce::KeyPress::escapeKey,
            juce::ModifierKeys::getCurrentModifiersRealtime().withoutMouseButtons(), 0)));
}
void KeyCapture::readFields()
{
    if (syncing || ! active) return;
    const auto* current = std::get_if<MidiTrigger> (&session.candidate());
    if (current == nullptr) return;
    auto trigger = *current;
    trigger.source = sources[source.getSelectedId() - 1];
    trigger.kind = kind.getSelectedId() == 1 ? MidiTrigger::Kind::note : MidiTrigger::Kind::cc;
    trigger.channel = channel.getSelectedId() - 1;
    const auto numberOf = [] (const juce::TextEditor& e) { return e.getText().isEmpty() ? -1 : e.getText().getIntValue(); };
    trigger.number = numberOf (number); trigger.highThreshold = numberOf (high); trigger.lowThreshold = numberOf (low);
    trigger.debounceMs = numberOf (debounce); trigger.minVelocity = numberOf (velocity);
    trigger.edge = static_cast<MidiTrigger::Edge> (edge.getSelectedId() - 1);
    trigger.behavior = static_cast<MidiTrigger::Behavior> (behavior.getSelectedId() - 1);
    session.edit (trigger);
    updateCandidate();
}
void KeyCapture::updateCandidate (bool syncFields)
{
    const auto* trigger = std::get_if<MidiTrigger> (&session.candidate());
    ruleFields.setVisible (trigger != nullptr);
    juce::String text = target;
    if (const auto* key = std::get_if<juce::KeyPress> (&session.candidate()))
        text += ko (" — 후보: ") + ShortcutDisplay::key (*key) + ko ("\n등록을 눌러 확정하세요. Esc = 취소. 넘패드는 NumLock에 따라 달라집니다.");
    else if (trigger != nullptr)
    {
        text += "\n" + trigger->display (trigger->source == "any" ? ko ("허용 장치 모두") : ShortcutDisplay::deviceName (*service, trigger->source));
        if (session.observedDevice().isNotEmpty()) text += ko ("\n관측 장치: ") + ShortcutDisplay::deviceName (*service, session.observedDevice());
        if (trigger->kind == MidiTrigger::Kind::cc)
            text += "\n" + session.suggestion().text + ko ("\nCC는 연결 뒤 첫 값은 기준값이라 실행되지 않음. 페달·버튼은 노트 권장.")
                + (trigger->behavior == MidiTrigger::Behavior::pulse ? ko ("\n등록 후 움직임 종료 대기: 200ms 정지하면 현재 값으로 재기준화.") : juce::String())
                + (trigger->edge == MidiTrigger::Edge::both ? ko ("\n양쪽 엣지: 일반 페달은 누를 때와 놓을 때 각각 실행됩니다.") : juce::String());
        if (syncFields)
        {
            const juce::ScopedValueSetter<bool> guard (syncing, true);
            sources.clear(); source.clear (juce::dontSendNotification);
            const auto addSource = [&] (const juce::String& id)
            {
                if (sources.contains (id)) return;
                sources.add (id);
                source.addItem (id == "any" ? ko ("허용 장치 모두") : ShortcutDisplay::deviceName (*service, id), sources.size());
            };
            addSource ("any");
            if (! projectCue)
            {
                for (const auto& p : service->getAvailableMidiDevices()) addSource (p.first);
                for (const auto& p : service->getMidiInputSettings().selected) addSource (p.first);
                addSource (trigger->source);
                if (session.observedDevice().isNotEmpty()) addSource (session.observedDevice());
            }
            source.setSelectedId (sources.indexOf (trigger->source) + 1, juce::dontSendNotification);
            kind.setSelectedId (trigger->kind == MidiTrigger::Kind::note ? 1 : 2, juce::dontSendNotification);
            channel.setSelectedId (trigger->channel + 1, juce::dontSendNotification);
            number.setText (juce::String (trigger->number), false); high.setText (juce::String (trigger->highThreshold), false);
            low.setText (juce::String (trigger->lowThreshold), false); debounce.setText (juce::String (trigger->debounceMs), false);
            velocity.setText (juce::String (trigger->minVelocity), false);
            edge.setSelectedId (static_cast<int> (trigger->edge) + 1, juce::dontSendNotification);
            behavior.setSelectedId (static_cast<int> (trigger->behavior) + 1, juce::dontSendNotification);
        }
        source.setEnabled (! projectCue);
        const bool cc = trigger->kind == MidiTrigger::Kind::cc;
        for (auto* c : std::initializer_list<juce::Component*> { &preset, &high, &low, &edge, &behavior }) c->setEnabled (cc);
        velocity.setEnabled (! cc);
    }
    else text += ko (" — 이미 누른 입력을 먼저 놓고 키 또는 MIDI 입력을 보내세요. Esc = 취소\n넘패드는 NumLock에 따라 달라지며 Enter는 구분하지 않습니다. 미디어 장치는 F13~F24로 설정하세요.\nMIDI 입력은 ‘MIDI 입력 — 이 PC’에서 먼저 선택하세요.");
    const auto checked = decision();
    if (checked.message.isNotEmpty()) text += "\n" + checked.message;
    registerButton.setEnabled (active && session.canRegister() && checked.allowed);
    specialButton.setEnabled (std::holds_alternative<std::monostate> (session.candidate()));
    showText (text, checked.message.isNotEmpty());
}
void KeyCapture::showText (const juce::String& text, bool warning)
{
    status.setText (text, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, warning ? Palette::warn : Palette::text);
    resized();
}
void KeyCapture::resized()
{
    auto area = getLocalBounds().reduced (6);
    auto buttons = area.removeFromBottom (26);
    registerButton.setBounds (buttons.removeFromRight (64)); buttons.removeFromRight (6);
    cancelButton.setBounds (buttons.removeFromRight (64));
    specialButton.setBounds (buttons.removeFromLeft (110)); buttons.removeFromLeft (6);
    relearnButton.setBounds (buttons.removeFromLeft (92));
    area.removeFromBottom (4);
    if (ruleFields.isVisible())
    {
        ruleFields.setBounds (area.removeFromBottom (188));
        const int width = ruleFields.getWidth() / 3;
        int i = 0;
        const auto place = [&] (juce::Label& label, juce::Component& control)
        {
            const int x = (i % 3) * width, y = (i / 3) * 47;
            label.setBounds (x, y, width - 6, 19);
            control.setBounds (x, y + 19, width - 6, 26);
            ++i;
        };
        place (sourceLabel, source); place (kindLabel, kind); place (channelLabel, channel);
        place (presetLabel, preset); place (numberLabel, number); place (velocityLabel, velocity);
        place (highLabel, high); place (lowLabel, low); place (debounceLabel, debounce);
        place (edgeLabel, edge); place (behaviorLabel, behavior);
        area.removeFromBottom (4);
    }
    statusView.setBounds (area);
    const int width = juce::jmax (40, area.getWidth() - statusView.getScrollBarThickness());
    juce::AttributedString text;
    text.append (status.getText(), status.getFont());
    juce::TextLayout layout;
    layout.createLayout (text, static_cast<float> (width - 8));
    status.setSize (width, juce::jmax (area.getHeight(), juce::roundToInt (layout.getHeight()) + 10));
}
void KeyCapture::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel2); g.setColour (Palette::standby); g.drawRect (getLocalBounds());
}
bool KeyCapture::keyPressed (const juce::KeyPress& key)
{
    if (active && key.isKeyCode (juce::KeyPress::escapeKey)) { cancel(); return true; }
    return active && ! ruleFields.hasKeyboardFocus (true);
}
void KeyCapture::focusLost (FocusChangeType)
{
    if (! active || hasKeyboardFocus (true) || submitting) return;
    if (! isShowing()) { cancel(); return; }
    const juce::Component::SafePointer<KeyCapture> safe (this);
    const auto token = generation;
    juce::MessageManager::callAsync ([safe, token]
    {
        auto* modal = juce::Component::getCurrentlyModalComponent();
        const bool popup = modal != nullptr && safe != nullptr && modal != safe.getComponent() && ! modal->isParentOf (safe.getComponent());
        if (safe != nullptr && safe->generation == token && safe->active && ! safe->hasKeyboardFocus (true)
            && ! safe->submitting && ! popup) safe->cancel();
    });
}
void KeyCapture::visibilityChanged() { if (! isShowing()) cancel(); }

KeyCaptureButton::KeyCaptureButton()
{
    setWantsKeyboardFocus (false); getProperties().set ("slateKeycap", true);
}
KeyCaptureButton::~KeyCaptureButton() { cancelCapture(); }
void KeyCaptureButton::setHotkey (const juce::String& hotkey)
{
    setButtonText (hotkey.isEmpty() ? ko ("핫키: 없음") : ko ("핫키: ") + hotkey);
    setTooltip (getButtonText());
}
void KeyCaptureButton::cancelCapture()
{
    ++generation;
    if (capture != nullptr) capture->detachService();
    if (callout != nullptr) callout->dismiss();
    capture = nullptr; callout = nullptr;
}
void KeyCaptureButton::clicked()
{
    if (service == nullptr || service->isEditingLocked()) return;
    cancelCapture();
    const auto token = generation;
    const juce::Component::SafePointer<KeyCaptureButton> safe (this);
    auto widget = std::make_unique<KeyCapture> (*service);
    capture = widget.get();
    widget->setSize (600, 370);
    widget->validate = [safe, token] (const juce::KeyPress& key)
    {
        if (safe == nullptr || safe->generation != token || ! safe->isEnabled()) return KeyCapture::Decision { false, ko ("캡처가 취소되었습니다.") };
        const auto reason = safe->validate ? safe->validate (key) : juce::String();
        return KeyCapture::Decision { reason.isEmpty(), reason };
    };
    widget->validateMidi = [safe, token] (const MidiTrigger& trigger)
    {
        if (safe == nullptr || safe->generation != token || ! safe->isEnabled()) return KeyCapture::Decision { false, ko ("캡처가 취소되었습니다.") };
        return safe->validateMidi ? safe->validateMidi (trigger) : KeyCapture::Decision { safe->onMidiChanged != nullptr, {} };
    };
    widget->onSubmit = [safe, token] (const KeyCaptureSession::Candidate& candidate, KeyCapture::Completion done)
    {
        if (safe == nullptr || safe->generation != token || ! safe->isEnabled()) return;
        if (const auto* trigger = std::get_if<MidiTrigger> (&candidate))
            done (safe->onMidiChanged ? safe->onMidiChanged (*trigger) : juce::Result::fail ("MIDI target unavailable"));
        else if (const auto* key = std::get_if<juce::KeyPress> (&candidate))
        {
            if (safe->onHotkeyChanged) { auto callback = safe->onHotkeyChanged; callback (key->getTextDescription()); }
            done (juce::Result::ok());
        }
    };
    widget->onFinished = [safe, token] { if (safe != nullptr && safe->generation == token) safe->cancelCapture(); };
    callout = &juce::CallOutBox::launchAsynchronously (std::move (widget), getScreenBounds(), nullptr);
    ShortcutRouter::watchWindow (callout.getComponent());
    juce::MessageManager::callAsync ([safe, token]
    {
        if (safe != nullptr && safe->generation == token && safe->capture != nullptr && safe->isShowing())
            safe->capture->start (ko ("큐 입력"), true, safe->editMidi);
    });
}
}
