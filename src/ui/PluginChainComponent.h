#pragma once

#include "audio/AudioEngine.h"
#include "audio/PluginChain.h"
#include "ui/PluginWindows.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace gocue
{

/** A horizontal strip of insert slots for one chain:
    [ name / bypass / edit / remove ] x N  ...  [+ add] [manage]. */
class PluginChainComponent : public juce::Component,
                             private juce::AsyncUpdater
{
public:
    PluginChainComponent (AudioEngine& engine, PluginWindowManager& windows);
    ~PluginChainComponent() override;

    /** Binds the strip to a chain (null disables it). ownerName prefixes editor window titles. */
    void setChain (PluginChain* chain, const juce::String& ownerName);
    PluginChain* getChain() const noexcept { return chain; }

    /** Rebuilds the slot views from the chain. */
    void refresh();

    /** Call when any chain changed (project switch, restore, edits elsewhere): refreshes if it is ours. */
    void chainChanged (PluginChain* changed);

    std::function<void()> onOpenPluginManager;
    /** Routes chain edits (add / remove / bypass) through the document so they are undoable.
        When unset, edits apply directly. */
    std::function<void (const juce::String& name, const std::function<void()>& edit)> performEdit;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    class SlotView;

    void handleAsyncUpdate() override { refresh(); }
    void showAddMenu();
    void runEdit (const juce::String& name, const std::function<void()>& edit);
    void addPlugin (const juce::PluginDescription& description);
    void openEditor (int index);
    void removeSlot (int index);
    void toggleBypass (int index);

    AudioEngine& engine;
    PluginWindowManager& windows;
    PluginChain* chain = nullptr;
    juce::String ownerName;

    juce::Viewport viewport;
    juce::Component strip;
    std::vector<std::unique_ptr<SlotView>> slotViews;
    juce::TextButton addButton, manageButton;
    juce::Label emptyLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChainComponent)
};

} // namespace gocue
