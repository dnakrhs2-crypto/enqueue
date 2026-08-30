#pragma once

#include "audio/PluginChain.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace gocue
{

/** A native-titled window hosting a plugin's own editor (or JUCE's generic one). */
class PluginEditorWindow : public juce::DocumentWindow
{
public:
    PluginEditorWindow (juce::AudioPluginInstance& plugin, const juce::String& title,
                        std::function<void (PluginEditorWindow&)> onClose);
    ~PluginEditorWindow() override;

    juce::AudioPluginInstance& getPlugin() noexcept { return plugin; }
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
    void closeFor (juce::AudioPluginInstance* plugin);
    void closeAll();
    int getNumOpenWindows() const noexcept { return (int) windows.size(); }

    /** Forwarded from every chain, e.g. to mark the project dirty. */
    std::function<void (PluginChain&)> onChainChanged;

    void pluginAboutToBeRemoved (PluginChain&, juce::AudioPluginInstance& plugin) override;
    void chainChanged (PluginChain& chain) override;

private:
    void closeWindow (PluginEditorWindow& window);

    std::vector<std::unique_ptr<PluginEditorWindow>> windows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindowManager)
};

} // namespace gocue
