#pragma once
#include "app/ShortcutService.h"
#include <variant>

namespace gocue
{
/** Input rules shared by cue hotkeys and commands. Physical repeat/release tracking
    belongs to ShortcutRouter; this model also accepts a repeat flag for deterministic tests. */
class KeyCaptureSession
{
public:
    using Candidate = std::variant<std::monostate, juce::KeyPress, MidiTrigger>;
    enum class Preset { pedal, toggle, knob };
    struct Suggestion { MidiTrigger trigger; juce::String text; bool changed = false; };
    enum class Outcome { waiting, candidate, rejected, cancelled, repeated };
    struct Result { Outcome outcome; juce::KeyPress key; juce::String message; };
    static Result input (const juce::KeyPress&, bool repeat = false, bool systemKey = false);
    static Result special (const juce::KeyPress&); // explicit Esc selection, never a typed Esc
    void reset (bool projectCue = false);
    bool chooseKey (const juce::KeyPress&);
    bool observe (const MidiInputEvent&, const juce::String& identifier);
    void edit (MidiTrigger);
    void choosePreset (Preset);
    Suggestion suggestion() const;
    bool canRegister() const;
    bool released() const;
    bool moving (double nowMs) const;
    const Candidate& candidate() const { return value; }
    const juce::String& observedDevice() const { return device; }
    bool needsPreset() const;
private:
    Candidate value;
    bool cue = false, confirmed = false;
    juce::String device;
    std::optional<InputToken> address;
    std::map<InputToken, MidiInputEvent> observations;
    std::map<InputToken, juce::String> identifiers;
    std::vector<int> values;
    double lastChange = 0;
};

/** Dedicated capture focus with child MIDI rule editors. Mouse actions retain capture.
    SafePointer + generation protect all deferred deliveries and special-key menus. */
class KeyCapture : public juce::Component, private juce::Timer
{
public:
    struct Decision { bool allowed = true; juce::String message; };
    /** Message-thread test seam; production uses the native foreground process check. */
    static std::function<bool()> foregroundProcessCheck;
    explicit KeyCapture (ShortcutService&, std::function<bool()> keysHeld = {});
    ~KeyCapture() override;
    std::function<Decision (const juce::KeyPress&)> validate;
    std::function<void (const juce::KeyPress&)> onRegister;
    using Completion = std::function<void (juce::Result)>;
    std::function<Decision (const MidiTrigger&)> validateMidi;
    std::function<void (const KeyCaptureSession::Candidate&, Completion)> onSubmit;
    std::function<void()> onFinished;
    void start (const juce::String& target, bool projectCue = false, std::optional<MidiTrigger> edit = {});
    void relearn();
    void cancel (bool notify = true);
    void detachService() { cancel (false); service = nullptr; }
    bool isCapturing() const noexcept { return active; }
    void receive (const juce::KeyPress&);
    void receiveMidi (const MidiInputEvent&, const juce::String&);
    const KeyCaptureSession& model() const { return session; }
    /** Also used by deterministic release tests; the UI timer calls this at 40 Hz. */
    void pollKeyRelease();
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool) override { return active; }
    void focusLost (FocusChangeType) override;
    void focusOfChildComponentChanged (FocusChangeType) override;
    void visibilityChanged() override;

private:
    void cancelIfFocusOutside();
    void accept (KeyCaptureSession::Result);
    void chooseSpecial();
    void timerCallback() override;
    void showText (const juce::String&, bool warning);
    void updateCandidate (bool syncFields = false);
    void readFields();
    void complete (juce::Result);
    Decision decision() const;
    ShortcutService* service;
    juce::TextButton registerButton, cancelButton, specialButton, relearnButton;
    juce::Label status;
    juce::Viewport statusView;
    juce::String target;
    KeyCaptureSession session;
    juce::Component ruleFields;
    juce::ComboBox source, kind, channel, preset, edge, behavior;
    juce::StringArray sources;
    juce::TextEditor number, high, low, debounce, velocity;
    juce::Label sourceLabel, kindLabel, channelLabel, presetLabel, numberLabel, highLabel, lowLabel, edgeLabel, behaviorLabel, debounceLabel, velocityLabel;
    uint64_t generation = 0;
    std::function<bool()> keysHeld;
    bool active = false, registrationPending = false, submitting = false, projectCue = false, syncing = false, uiDirty = false;
    bool observedAvailableDevice = false;
    bool focusRecheckPending = false;
};

/** Compact cue-inspector entry point; the callout contains the same KeyCapture. */
class KeyCaptureButton : public juce::TextButton
{
public:
    KeyCaptureButton();
    ~KeyCaptureButton() override;
    void setService (ShortcutService& s) { service = &s; }
    void detachService() { cancelCapture(); service = nullptr; }
    void setHotkey (const juce::String&);
    void cancelCapture();
    std::function<void (const juce::String&)> onHotkeyChanged;
    std::function<juce::String (const juce::KeyPress&)> validate;
    std::function<KeyCapture::Decision (const MidiTrigger&)> validateMidi;
    std::function<juce::Result (const MidiTrigger&)> onMidiChanged;
    std::optional<MidiTrigger> editMidi;
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
