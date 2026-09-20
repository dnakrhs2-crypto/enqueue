#pragma once

#include "app/MidiInputService.h"
#include "app/ProjectDocument.h"

namespace gocue
{
/** All mutable MIDI rule, ownership and execution state lives on the message thread. */
class MidiTriggerRouter : private ShortcutService::Listener, private ProjectDocument::Listener
{
public:
    struct Callbacks
    {
        std::function<MidiRoutingContext()> context;
        std::function<void (const juce::Uuid&, const InputInvocation&)> cue;
        std::function<void (double timeMs, bool hardStop)> panic;
        std::function<bool()> requireGoKeyUp;
        std::function<double()> clockMs;
    };
    MidiTriggerRouter (ShortcutService&, juce::ApplicationCommandManager&, ProjectDocument&, Callbacks);
    ~MidiTriggerRouter() override;
    MidiInputService::Callbacks inputCallbacks();
    bool route (const MidiInputEvent&, const juce::String& identifier, bool execute = true);
    void connectionChanged (uint64_t input, uint64_t connection, bool connected);
    void inputFault (uint64_t input, bool panic = false);
    void refreshBindings();
    const std::vector<MidiBinding>& cueBindings() const noexcept { return cues; }
private:
    struct Runtime { MidiBinding binding; MidiTriggerRules rules; bool live = true; };
    void shortcutsChanged() override;
    void captureStateChanged() override;
    void documentStateChanged() override {}
    void midiTriggersChanged() override { refreshBindings(); }
    void containersChanged() override { refreshBindings(); }
    void projectReplaced() override;
    void releaseGo (const InputToken&, const MidiInputEvent* = nullptr);
    ShortcutService& shortcuts;
    juce::ApplicationCommandManager& manager;
    ProjectDocument& document;
    Callbacks callbacks;
    std::vector<Runtime> bindings;
    std::vector<MidiBinding> cues;
    std::map<uint64_t, uint64_t> connections;
    std::map<InputToken, std::pair<MidiInputEvent, juce::String>> lastObserved;
    bool rebuilding = false;
};
}
