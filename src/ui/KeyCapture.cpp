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
    if (repeat)
        return { Outcome::repeated, {}, {} };
    if (key.isKeyCode (juce::KeyPress::escapeKey))
        return { Outcome::cancelled, {}, {} };
    if (systemKey)
        return { Outcome::rejected, {}, ko ("Win 조합·한/영·한자·IME 키는 지원하지 않습니다.") };
    if (key.getKeyCode() == 0)
        return { Outcome::waiting, {}, ko ("Ctrl / Alt / Shift와 함께 사용할 키를 누르세요.") };
    for (const auto code : { juce::KeyPress::playKey, juce::KeyPress::stopKey, juce::KeyPress::fastForwardKey, juce::KeyPress::rewindKey })
        if (key.isKeyCode (code))
            return { Outcome::rejected, {}, ko ("미디어 키는 지원하지 않습니다. F13~F24 같은 키로 장치를 설정하세요.") };
    return special (key);
}

KeyCapture::KeyCapture (ShortcutService& s, std::function<bool()> held) : service (&s), keysHeld (std::move (held))
{
    if (! keysHeld)
        keysHeld = []
        {
           #if JUCE_WINDOWS
            // Includes exact keypad VKs and left/right modifiers, but excludes mouse buttons.
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
    specialButton.setTooltip (ko ("Esc를 후보로 선택합니다. Ctrl / Alt / Shift를 누른 채 클릭하면 조합도 선택할 수 있습니다."));
    for (auto* button : { &registerButton, &cancelButton, &specialButton })
    {
        button->setWantsKeyboardFocus (false);
        button->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (button);
    }
    cancelButton.onClick = [this] { cancel(); };
    specialButton.onClick = [this] { chooseSpecial(); };
    registerButton.onClick = [this]
    {
        if (! active || ! candidate.isValid() || service == nullptr || service->isEditingLocked())
            return;
        const auto decision = validate ? validate (candidate) : Decision();
        if (! decision.allowed)
        {
            registerButton.setEnabled (false);
            showText (decision.message, true);
            return;
        }
        registrationPending = true;
        registerButton.setEnabled (false);
        specialButton.setEnabled (false);
        showText (ko ("누른 키를 모두 떼면 등록합니다. Esc = 취소"), false);
        startTimerHz (40);
        pollKeyRelease();
    };
}

KeyCapture::~KeyCapture() { cancel (false); }

void KeyCapture::start (const juce::String& name)
{
    cancel (false);
    if (service == nullptr || service->isEditingLocked())
        return;
    target = name;
    active = true;
    candidate = {};
    registerButton.setEnabled (false);
    const auto token = ++generation;
    const juce::Component::SafePointer<KeyCapture> safe (this);
    service->beginCapture (this,
        [safe, token] (const juce::KeyPress& key)
        { if (safe != nullptr && safe->generation == token) safe->receive (key); },
        [safe, token]
        { if (safe != nullptr && safe->generation == token) safe->cancel(); });
    showText (target + ko (" — 키를 누르세요. Esc = 취소\n넘패드는 NumLock에 따라 달라지며 Enter는 구분하지 않습니다. 미디어 장치는 F13~F24로 설정하세요."), false);
    grabKeyboardFocus();
}

void KeyCapture::cancel (bool notify)
{
    stopTimer();
    registrationPending = false;
    specialButton.setEnabled (true);
    ++generation;
    const bool wasActive = active;
    active = false;
    candidate = {};
    if (service != nullptr) service->endCapture (this);
    registerButton.setEnabled (false);
    if (wasActive && notify && onFinished)
        onFinished();
}

void KeyCapture::receive (const juce::KeyPress& key)
{
    if (! active)
        return;
    if (registrationPending)
    {
        if (key.isKeyCode (juce::KeyPress::escapeKey)) cancel();
        return;
    }
    bool systemKey = false;
   #if JUCE_WINDOWS
    systemKey = (GetKeyState (VK_LWIN) & 0x8000) != 0 || (GetKeyState (VK_RWIN) & 0x8000) != 0;
   #endif
    accept (KeyCaptureSession::input (key, false, systemKey));
}

void KeyCapture::pollKeyRelease()
{
    if (! registrationPending || ! active || keysHeld()) return;
    if (service == nullptr || service->isEditingLocked()) { cancel(); return; }
    const auto decision = validate ? validate (candidate) : Decision();
    if (! decision.allowed)
    {
        stopTimer();
        registrationPending = false;
        specialButton.setEnabled (true);
        showText (decision.message, true);
        return;
    }
    const auto key = candidate;
    auto callback = onRegister;
    auto finished = onFinished;
    cancel (false); // the router also quarantines any late queued key-downs
    if (callback) callback (key);
    if (finished) finished();
}

void KeyCapture::accept (KeyCaptureSession::Result result)
{
    using Outcome = KeyCaptureSession::Outcome;
    if (result.outcome == Outcome::cancelled)
    {
        cancel();
        return;
    }
    if (result.outcome == Outcome::repeated || result.outcome == Outcome::waiting)
        return;
    candidate = result.key;
    const auto decision = candidate.isValid() && validate ? validate (candidate) : Decision();
    registerButton.setEnabled (candidate.isValid() && decision.allowed);
    showText (target + (candidate.isValid() ? ko (" — 후보: ") + ShortcutDisplay::key (candidate) : juce::String())
              + "\n" + (result.message.isNotEmpty() ? result.message : decision.message.isNotEmpty() ? decision.message
                 : ko ("등록을 눌러 확정하세요. Esc = 취소. 넘패드는 NumLock에 따라 달라집니다.")),
              result.outcome == Outcome::rejected || decision.message.isNotEmpty());
}

void KeyCapture::chooseSpecial()
{
    // Esc is the only key that cannot be typed. Mouse selection keeps capture
    // active even in the modal settings dialog; held Ctrl/Alt/Shift are optional.
    if (active && service != nullptr && ! service->isEditingLocked())
        accept (KeyCaptureSession::special (juce::KeyPress (juce::KeyPress::escapeKey,
            juce::ModifierKeys::getCurrentModifiersRealtime().withoutMouseButtons(), 0)));
}

void KeyCapture::showText (const juce::String& text, bool warning)
{
    status.setText (text, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, warning ? Palette::warn : Palette::text);
    resized();
    statusView.setViewPosition (0, 0);
}

void KeyCapture::resized()
{
    auto area = getLocalBounds().reduced (4);
    auto buttons = area.removeFromBottom (26);
    registerButton.setBounds (buttons.removeFromRight (68));
    buttons.removeFromRight (6);
    cancelButton.setBounds (buttons.removeFromRight (68));
    specialButton.setBounds (buttons.removeFromLeft (114));
    area.removeFromBottom (4);
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
    g.fillAll (Palette::panel2);
    g.setColour (Palette::standby);
    g.drawRect (getLocalBounds());
}
void KeyCapture::focusLost (FocusChangeType) { if (active && ! hasKeyboardFocus (true)) cancel(); }
void KeyCapture::visibilityChanged() { if (! isShowing()) cancel(); }

KeyCaptureButton::KeyCaptureButton()
{
    setWantsKeyboardFocus (false);
    getProperties().set ("slateKeycap", true);
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
    capture = nullptr;
    callout = nullptr;
}
void KeyCaptureButton::clicked()
{
    if (service == nullptr || service->isEditingLocked()) return;
    cancelCapture();
    const auto token = generation;
    const juce::Component::SafePointer<KeyCaptureButton> safe (this);
    auto widget = std::make_unique<KeyCapture> (*service);
    capture = widget.get();
    widget->setSize (480, 150);
    widget->validate = [safe, token] (const juce::KeyPress& key)
    {
        if (safe == nullptr || safe->generation != token || ! safe->isEnabled())
            return KeyCapture::Decision { false, ko ("캡처가 취소되었습니다.") };
        const auto reason = safe->validate ? safe->validate (key) : juce::String();
        return KeyCapture::Decision { reason.isEmpty(), reason };
    };
    widget->onRegister = [safe, token] (const juce::KeyPress& key)
    {
        if (safe != nullptr && safe->generation == token && safe->isEnabled() && safe->onHotkeyChanged)
            safe->onHotkeyChanged (key.getTextDescription());
    };
    widget->onFinished = [safe, token]
    {
        if (safe != nullptr && safe->generation == token) safe->cancelCapture();
    };
    callout = &juce::CallOutBox::launchAsynchronously (std::move (widget), getScreenBounds(), nullptr);
    ShortcutRouter::watchWindow (callout.getComponent());
    juce::MessageManager::callAsync ([safe, token]
    {
        if (safe != nullptr && safe->generation == token && safe->capture != nullptr && safe->isShowing())
            safe->capture->start (ko ("큐 핫키"));
    });
}
}
