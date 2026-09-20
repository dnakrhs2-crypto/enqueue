#pragma once
#include "app/ShortcutService.h"

namespace gocue
{
/** Native file choosers may have no focused JUCE modal component. Their explicit
    lifetime joins JUCE's modal-component count when classifying MIDI input. */
class MidiModalScope
{
public:
    MidiModalScope() { ++count; }
    ~MidiModalScope() { --count; }
    static bool active() { return count != 0 || juce::Component::getNumCurrentlyModalComponents() != 0; }
private:
    inline static int count = 0; // message-thread only
    JUCE_DECLARE_NON_COPYABLE (MidiModalScope)
};
inline void launchMidiFileChooser (juce::FileChooser& chooser, int flags, std::function<void (const juce::FileChooser&)> callback)
{
    chooser.launchAsync (flags, [scope = std::make_shared<MidiModalScope>(), callback = std::move (callback)] (const juce::FileChooser& fc) mutable
    {
        // Release at completion even if the chooser object/its callback is retained.
        auto lifetime = std::move (scope);
        callback (fc);
    });
}
inline MidiRoutingContext currentMidiContext (juce::Component& main)
{
    MidiRoutingContext context;
    context.applicationActive = juce::Process::isForegroundProcess();
    context.modal = MidiModalScope::active();
    context.window = context.applicationActive ? ShortcutKeyContext::Window::auxiliary : ShortcutKeyContext::Window::outsideApp;
    auto* focus = juce::Component::getCurrentlyFocusedComponent();
    if (focus == &main || main.isParentOf (focus)) context.window = ShortcutKeyContext::Window::main;
    for (auto* c = focus; c != nullptr; c = c->getParentComponent())
    {
        if (auto* editor = dynamic_cast<juce::TextEditor*> (c)) context.textEditing |= ! editor->isReadOnly();
        if (c->getProperties().contains ("shortcutWindow"))
        {
            context.window = static_cast<ShortcutKeyContext::Window> (static_cast<int> (c->getProperties()["shortcutWindow"]));
            break;
        }
    }
    return context;
}
}
