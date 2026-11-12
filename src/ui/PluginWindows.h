#pragma once

#include "audio/PluginChain.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace gocue
{

/** A native-titled window hosting a plugin's own editor (or JUCE's generic one). */
class PluginEditorWindow : public juce::DocumentWindow
{
public:
    PluginEditorWindow (juce::AudioPluginInstance& plugin, const juce::String& title,
                        std::function<void (PluginEditorWindow&)> onClose,
                        bool activateFromHotkey = false,
                        std::function<void (PluginEditorWindow&)> placeWindow = {});
    ~PluginEditorWindow() override;

    juce::AudioPluginInstance& getPlugin() noexcept { return plugin; }
    const juce::AudioPluginInstance& getPlugin() const noexcept { return plugin; }
    void closeButtonPressed() override;

private:
    juce::AudioPluginInstance& plugin;
    std::function<void (PluginEditorWindow&)> onClose;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditorWindow)
};

/** Keeps at most one editor window per plugin instance and closes them before the
    instance goes away (it listens to every PluginChain the engine owns). */
class PluginWindowManager : public PluginChain::Listener
{
public:
    PluginWindowManager() = default;
    ~PluginWindowManager() override;

    void open (juce::AudioPluginInstance& plugin, const juce::String& title);
    /** Called synchronously from a global hotkey: claim the foreground before creating a slow editor. */
    void openInForeground (juce::AudioPluginInstance& plugin, const juce::String& title);
    bool isOpen (const juce::AudioPluginInstance& plugin) const;
    bool isInUse (const juce::AudioPluginInstance& plugin) const;
    /** External control: also counts visible, restored editors above the OS foreground window. */
    bool isInFront (const juce::AudioPluginInstance& plugin) const;
    /** External control fallback when activation is refused; does not leave the editor topmost. */
    void raiseWithoutActivating (juce::AudioPluginInstance& plugin);
    /** Same notification and lifetime as the window's close button; false if already closed. */
    bool closeByUser (juce::AudioPluginInstance& plugin);
    /** In-memory positions only. Off by default (Enqueue); disabling forgets any stored positions. */
    void setRememberWindowPositions (bool remember);
    void closeFor (juce::AudioPluginInstance* plugin);
    void closeAll();
    int getNumOpenWindows() const noexcept { return (int) windows.size(); }

    /** Forwarded from every chain, e.g. to mark the project dirty. */
    std::function<void (PluginChain&)> onChainChanged;
    /** User closed an editor, before it is destroyed. Optional; Enqueue leaves this unset. */
    std::function<void (juce::AudioPluginInstance&)> onWindowClosed;
    /** Optional foreground predicate for desktops without an OS foreground window (unit tests). */
    std::function<bool (const PluginEditorWindow&)> isWindowInUse;

    void pluginAboutToBeRemoved (PluginChain&, juce::AudioPluginInstance& plugin) override;
    void chainChanged (PluginChain& chain) override;

private:
    PluginEditorWindow* findWindow (const juce::AudioPluginInstance& plugin) const;
    void openWindow (juce::AudioPluginInstance& plugin, const juce::String& title, bool activateFromHotkey);
    void placeWindow (PluginEditorWindow& window);
    void closeWindow (PluginEditorWindow& window);

    std::vector<std::unique_ptr<PluginEditorWindow>> windows;
    bool rememberWindowPositions = false;
    std::map<const juce::AudioPluginInstance*, juce::Point<int>> positions;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindowManager)
};

} // namespace gocue
