#pragma once

#include "app/ShortcutProfile.h"
#include "app/PanicKeyHook.h"
#include "app/MidiTriggerRules.h"
#include "app/PanicGestureGate.h"

#include <functional>
#include <optional>

namespace gocue
{

struct ShortcutDiagnostic
{
    enum class Code { invalidProfile, unknownAction, readOnlyAction, commandConflict, defaultSuppressed, panicKeyUnsupported };
    Code code;
    juce::String actionID, otherActionID;
    juce::KeyPress key;
    juce::String message;
};

struct ShortcutOperationResult
{
    juce::Result status = juce::Result::ok();
    std::vector<ShortcutDiagnostic> diagnostics;
    bool wasOk() const noexcept { return status.wasOk(); }
    bool failed() const noexcept { return status.failed(); }
    juce::String getErrorMessage() const { return status.getErrorMessage(); }
};

struct ShortcutMappingResult : ShortcutOperationResult
{
    ShortcutProfile::Overrides keys;
    std::vector<PanicKeyBinding> panicBindings;
};

struct ShortcutKeyContext
{
    enum class Window { main, auxiliary, modal, nativePlugin, outsideApp };
    struct CueHotkey
    {
        juce::String id;
        juce::KeyPress key;
        bool inActiveContainer = true;
        bool enabled = true;
    };

    Window window = Window::main;
    ShortcutScope focus = ShortcutScope::mainWindow;
    bool applicationActive = true;
    bool captureActive = false;
    bool textEditing = false;
    bool standardUiConsumesKey = false; // other standard controls/dialogs; text input is classified centrally
    bool isRepeat = false;
    /** Native observation disambiguates JUCE character aliases (e.g. keypad '+').
        It is used only for the panic owner; fixed component behavior is unchanged. */
    std::optional<PanicKeyBinding> nativeKey;
    bool nativePanicOwned = false; // ownership observed before JUCE's asynchronous modifier query
    std::vector<CueHotkey> cueHotkeys; // may include every list/cart for conflict display; never modified
    std::function<bool (juce::CommandID)> commandEnabled;
    /** Execution availability only: false blocks/consumes the owned key, never releases it.
        Omit for conflict previews. */
    std::function<bool (const juce::String&)> componentCanHandle;
};

struct ShortcutKeyOwner
{
    enum class Kind { none, command, fixedComponent, cueHotkey, blocked, capture, standardUi, conflict };
    enum class Reason { unassigned, commandBinding, componentFocus, commandOverCue, cueBinding, captureActive,
                        standardUi, inactiveApp, outsideScope, disabled, repeatSuppressed, ambiguousCueHotkey };
    struct Conflict
    {
        Kind kind;
        juce::String id;
        juce::CommandID commandID = 0;
    };

    Kind kind = Kind::none;
    Reason reason = Reason::unassigned;
    juce::String id;
    juce::CommandID commandID = 0;
    std::vector<Conflict> conflicts;

    /** Deliver exclusively to the owner; blocked owners consume without execution.
        Unassigned keys and input outside the active app pass through. */
    bool shouldConsume() const noexcept { return kind != Kind::none && reason != Reason::inactiveApp; }
};

struct MidiBinding
{
    juce::String id;
    MidiTrigger trigger;
    juce::CommandID commandID = 0; // zero = cue UUID
    bool enabled = true;
};
struct MidiRoutingContext
{
    ShortcutKeyContext::Window window = ShortcutKeyContext::Window::main;
    bool applicationActive = true, modal = false, textEditing = false, allowBackgroundPlayback = true;
    std::vector<MidiBinding> cues;
    std::function<bool (juce::CommandID)> commandEnabled;
};
struct MidiOwner
{
    enum class Kind { none, capture, panic, command, cue, blocked };
    Kind kind = Kind::none;
    juce::String id, reason;
    juce::CommandID commandID = 0;
    std::vector<juce::String> conflicts;
};

/** Message-thread mappings, ownership and shared capture state. JUCE mappings are
    for display; ShortcutRouter is the only command/cue keyboard dispatcher. */
class ShortcutService
{
public:
    /** Must persist BOTH values atomically on success and leave storage unchanged on failure.
        AppSettings::saveKeyboardShortcuts supplies this contract, including automatic-save rollback. */
    using SaveFunction = std::function<juce::Result (const juce::String& currentXml, const juce::String& lastGoodXml)>;
    using SaveInputsFunction = std::function<juce::Result (const InputSettingsTransaction&)>;
    enum class ConflictPolicy { reject, move }; // move explicitly removes keys from their previous command
    struct RestoreReport
    {
        enum class Source { defaults, current, lastGood };
        Source source = Source::defaults;
        juce::String rejectedXml, rejectedLastGoodXml, message;
        std::vector<ShortcutDiagnostic> diagnostics;
    };
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void shortcutsChanged() = 0;
        virtual void captureStateChanged() {}
        virtual void shortcutEditingLockChanged() {}
        virtual void midiInputSettingsChanged() {}
    };

    ShortcutService (juce::ApplicationCommandManager& manager, SaveFunction save,
                     const ShortcutCatalog& catalog = ShortcutCatalog::get());

    /** Startup only: recover from last-good/defaults without writing either stored value. */
    RestoreReport restore (const std::optional<juce::String>& currentXml, const std::optional<juce::String>& lastGoodXml);
    static ShortcutMappingResult calculateMapping (const ShortcutCatalog& catalog, const ShortcutProfile& profile);
    const ShortcutProfile& getProfile() const noexcept { return profile; }
    const ShortcutKeys& getKeys (const juce::String& actionID) const;
    const ShortcutKeys& getKeys (juce::CommandID commandID) const;
    const std::vector<ShortcutDiagnostic>& getDiagnostics() const noexcept { return mapping.diagnostics; }
    const std::vector<PanicKeyBinding>& getPanicBindings() const noexcept { return mapping.panicBindings; }
    uint64_t getInputGeneration() const noexcept { return inputGeneration; }
    InputActivationTracker& activations() noexcept { return activationTracker; }
    PanicGestureGate& panicGestures() noexcept { return panicGate; }
    const InputInvocation* currentInvocation() const noexcept { return invocation; }
    bool invokeInput (const InputInvocation&, const juce::ApplicationCommandTarget::InvocationInfo* keyboardInfo = nullptr);
    /** Also used at project/input connection boundaries. Does not change stored mappings. */
    void invalidateInputRouting();
    void setInputStorage (SaveInputsFunction function) { saveInputs = std::move (function); }
    RestoreReport restoreMidi (const std::optional<juce::String>&, const std::optional<juce::String>&);
    RestoreReport restoreMidiInputs (const std::optional<juce::String>&, const std::optional<juce::String>&);
    const MidiShortcutProfile& getMidiProfile() const noexcept { return midiProfile; }
    const MidiInputSettings& getMidiInputSettings() const noexcept { return midiInputs; }
    const MidiTriggers& getMidiTriggers (const juce::String&) const;
    std::vector<MidiBinding> midiCommandBindings() const;
    MidiOwner resolveMidiOwner (const MidiBinding&, const MidiRoutingContext&, bool preview = false) const;
    ShortcutOperationResult setMidiTriggers (const juce::String&, MidiTriggers, ConflictPolicy = ConflictPolicy::reject);
    ShortcutOperationResult replaceMidiProfile (MidiShortcutProfile);
    ShortcutOperationResult setMidiInputSettings (MidiInputSettings);
    juce::String exportCombinedProfile() const; // v2; exportProfile() remains the explicit keyboard-only v1 API

    /** Message-thread capture ownership. A stale widget cannot end another widget's
        capture. The router waits for the activation keys to be released first. */
    using CaptureToken = const void*;
    void beginCapture (CaptureToken, std::function<void (const juce::KeyPress&)> receive = {},
                       std::function<void()> cancel = {});
    void endCapture (CaptureToken);
    void cancelCapture();
    bool isCapturing() const noexcept { return captureToken != nullptr; }
    void deliverCaptureKey (const juce::KeyPress&);
    void setMidiCaptureReceiver (CaptureToken, std::function<void (const MidiInputEvent&, const juce::String&)>);
    void deliverCaptureMidi (const MidiInputEvent&, const juce::String& identifier);
    /** Runtime callers preserve the original text character (numeric entry/text editor predicates).
        Character-less bindings can also be queried for conflict previews. */
    // Preview ignores the live capture owner, but uses the same binding/scope rules.
    ShortcutKeyOwner resolveKeyOwner (const juce::KeyPress& key, const ShortcutKeyContext& context, bool preview = false) const;

    ShortcutOperationResult addKey (const juce::String& actionID, const juce::KeyPress& key, ConflictPolicy policy = ConflictPolicy::reject);
    ShortcutOperationResult replaceKey (const juce::String& actionID, int index, const juce::KeyPress& key, ConflictPolicy policy = ConflictPolicy::reject);
    ShortcutOperationResult removeKey (const juce::String& actionID, int index);
    ShortcutOperationResult setKeys (const juce::String& actionID, const ShortcutKeys& keys, ConflictPolicy policy = ConflictPolicy::reject);
    ShortcutOperationResult restoreCommandDefaults (const juce::String& actionID, ConflictPolicy policy = ConflictPolicy::reject);
    ShortcutOperationResult restoreAllDefaults();
    ShortcutOperationResult importProfile (const juce::String& xml); // replacement, never a partial merge
    juce::String exportProfile() const; // all resolved commands, including empty lists, plus unknown overrides

    void setEditingLocked (bool locked);
    bool isEditingLocked() const noexcept { return editingLocked; }
    void addListener (Listener* listener) { listeners.add (listener); }
    void removeListener (Listener* listener) { listeners.remove (listener); }

private:
    ShortcutOperationResult commit (ShortcutProfile candidate);
    ShortcutOperationResult checkEditableCommand (const juce::String& actionID) const;
    void synchroniseMappings();
    juce::Result validateMidiMapping (const MidiShortcutProfile&) const;
    ShortcutOperationResult commitInputs (std::optional<ShortcutProfile>, std::optional<MidiShortcutProfile>, std::optional<MidiInputSettings>);

    juce::ApplicationCommandManager& manager;
    SaveFunction save;
    SaveInputsFunction saveInputs;
    const ShortcutCatalog& catalog;
    ShortcutProfile profile;
    ShortcutMappingResult mapping;
    MidiShortcutProfile midiProfile;
    MidiInputSettings midiInputs;
    InputActivationTracker activationTracker;
    PanicGestureGate panicGate;
    const InputInvocation* invocation = nullptr;
    juce::ListenerList<Listener> listeners;
    bool editingLocked = false, inTransaction = false;
    uint64_t inputGeneration = 0;
    CaptureToken captureToken = nullptr;
    std::function<void (const juce::KeyPress&)> captureReceiver;
    std::function<void()> captureCancellation;
    std::function<void (const MidiInputEvent&, const juce::String&)> midiCaptureReceiver;
};

} // namespace gocue
