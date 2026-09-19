#pragma once
#include "app/ShortcutService.h"

namespace gocue
{
/** Input rules shared by cue hotkeys and commands. Physical repeat/release tracking
    belongs to ShortcutRouter; this model also accepts a repeat flag for deterministic tests. */
class KeyCaptureSession
{
public:
    enum class Outcome { waiting, candidate, rejected, cancelled, repeated };
    struct Result { Outcome outcome; juce::KeyPress key; juce::String message; };
    static Result input (const juce::KeyPress&, bool repeat = false, bool systemKey = false);
    static Result special (const juce::KeyPress&); // explicit Esc selection, never a typed Esc
};

/** Dedicated focus component, never a TextEditor. Mouse buttons keep capture focus.
    SafePointer + generation protect all deferred deliveries and special-key menus. */
class KeyCapture : public juce::Component, private juce::Timer
{
public:
    struct Decision { bool allowed = true; juce::String message; };
    explicit KeyCapture (ShortcutService&, std::function<bool()> keysHeld = {});
    ~KeyCapture() override;
    std::function<Decision (const juce::KeyPress&)> validate;
    std::function<void (const juce::KeyPress&)> onRegister;
    std::function<void()> onFinished;
    void start (const juce::String& target);
    void cancel (bool notify = true);
    void detachService() { cancel (false); service = nullptr; }
    bool isCapturing() const noexcept { return active; }
    void receive (const juce::KeyPress&);
    /** Also used by deterministic release tests; the UI timer calls this at 40 Hz. */
    void pollKeyRelease();
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override { return active; }
    bool keyStateChanged (bool) override { return active; }
    void focusLost (FocusChangeType) override;
    void visibilityChanged() override;

private:
    void accept (KeyCaptureSession::Result);
    void chooseSpecial();
    void timerCallback() override { pollKeyRelease(); }
    void showText (const juce::String&, bool warning);
    ShortcutService* service;
    juce::TextButton registerButton, cancelButton, specialButton;
    juce::Label status;
    juce::Viewport statusView;
    juce::String target;
    juce::KeyPress candidate;
    uint64_t generation = 0;
    std::function<bool()> keysHeld;
    bool active = false, registrationPending = false;
};

/** Compact cue-inspector entry point; the callout contains the same KeyCapture. */
class KeyCaptureButton : public juce::TextButton
{
public:
    KeyCaptureButton();
    ~KeyCaptureButton() override;
    void setService (ShortcutService& s) { service = &s; }
    void setHotkey (const juce::String&);
    void cancelCapture();
    std::function<void (const juce::String&)> onHotkeyChanged;
    std::function<juce::String (const juce::KeyPress&)> validate;
    void clicked() override;
    void visibilityChanged() override { if (! isShowing()) cancelCapture(); }
    void enablementChanged() override { if (! isEnabled()) cancelCapture(); }
private:
    ShortcutService* service = nullptr;
    juce::Component::SafePointer<KeyCapture> capture;
    juce::Component::SafePointer<juce::CallOutBox> callout;
    uint64_t generation = 0;
};
}
