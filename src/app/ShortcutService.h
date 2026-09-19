#pragma once

#include "app/ShortcutProfile.h"

#include <functional>
#include <optional>

namespace gocue
{

struct ShortcutDiagnostic
{
    enum class Code { invalidProfile, unknownAction, readOnlyAction, commandConflict, defaultSuppressed };
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
    bool standardUiConsumesKey = false;
    bool isRepeat = false;
    std::vector<CueHotkey> cueHotkeys; // may include every list/cart for conflict display; never modified
    std::function<bool (juce::CommandID)> commandEnabled;
    /** Optional refinement for selection/editability in the focused component. Omit for conflict previews. */
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
};

/** Message-thread data service. Does not dispatch input or alter project/cue-hotkey data.
    The router in session B will use resolveKeyOwner; session A only installs JUCE mappings. */
class ShortcutService
{
public:
    /** Must persist BOTH values atomically on success and leave storage unchanged on failure.
        AppSettings::saveKeyboardShortcuts supplies this contract, including automatic-save rollback. */
    using SaveFunction = std::function<juce::Result (const juce::String& currentXml, const juce::String& lastGoodXml)>;
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
    ShortcutKeyOwner resolveKeyOwner (const juce::KeyPress& key, const ShortcutKeyContext& context) const;

    ShortcutOperationResult addKey (const juce::String& actionID, const juce::KeyPress& key, ConflictPolicy policy = ConflictPolicy::reject);
    ShortcutOperationResult replaceKey (const juce::String& actionID, int index, const juce::KeyPress& key, ConflictPolicy policy = ConflictPolicy::reject);
    ShortcutOperationResult removeKey (const juce::String& actionID, int index);
    ShortcutOperationResult setKeys (const juce::String& actionID, const ShortcutKeys& keys, ConflictPolicy policy = ConflictPolicy::reject);
    ShortcutOperationResult restoreCommandDefaults (const juce::String& actionID);
    ShortcutOperationResult restoreAllDefaults();
    ShortcutOperationResult importProfile (const juce::String& xml); // replacement, never a partial merge
    juce::String exportProfile() const; // all resolved commands, including empty lists, plus unknown overrides

    void setEditingLocked (bool locked) noexcept { editingLocked = locked; }
    void addListener (Listener* listener) { listeners.add (listener); }
    void removeListener (Listener* listener) { listeners.remove (listener); }

private:
    ShortcutOperationResult commit (ShortcutProfile candidate);
    ShortcutOperationResult checkEditableCommand (const juce::String& actionID) const;
    void synchroniseMappings();

    juce::ApplicationCommandManager& manager;
    SaveFunction save;
    const ShortcutCatalog& catalog;
    ShortcutProfile profile;
    ShortcutMappingResult mapping;
    juce::ListenerList<Listener> listeners;
    bool editingLocked = false, inTransaction = false;
};

} // namespace gocue
