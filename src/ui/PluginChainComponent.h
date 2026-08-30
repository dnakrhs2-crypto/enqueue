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
class PluginChainComponent : public juce::Component
{
public:
    PluginChainComponent (AudioEngine& engine, PluginWindowManager& windows);
    ~PluginChainComponent() override;

    /** Binds the strip to a chain (null disables it). ownerName prefixes editor window titles. */
    void setChain (PluginChain* chain, const juce::String& ownerName);
    PluginChain* getChain() const noexcept { return chain; }

    /** Rebuilds the slot views from the chain. */
    void refresh();

    std::function<void()> onOpenPluginManager;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    class SlotView;

    void showAddMenu();
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
