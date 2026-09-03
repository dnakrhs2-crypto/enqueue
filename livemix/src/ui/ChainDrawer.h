#pragma once

#include "MixDocument.h"
#include "Widgets.h"
#include "ui/PluginWindows.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace gocue::livemix
{

/** The VST3 chain of one channel / FX / the master as a vertical list: drag the ≡ to reorder, a switch for
    on / bypass, 열기 for the plugin's window, ✕ to remove, "+ 플러그인 추가" grouped by manufacturer. */
class ChainDrawer : public juce::Component
{
public:
    ChainDrawer (MixDocument& document, PluginWindowManager& windows);
    ~ChainDrawer() override;

    /** Shows 'chain' (null hides the rows). 'title' names the owner. */
    void setChain (PluginChain* chain, const juce::String& title);
    PluginChain* getChain() const noexcept { return chain; }
    void refresh();

    std::function<void()> onClose;
    std::function<void()> onOpenPluginManager;
    std::function<void()> onChainEdited;   // after every edit (the document marks itself dirty)

    void showAddMenu (juce::Component* anchor);
    void addPlugin (const juce::PluginDescription& description);
    void openEditor (int index);

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    struct Row;

    void removeSlot (int index);
    void moveSlot (int from, int to);
    void toggleBypass (int index);
    void layoutRows();

    MixDocument& document;
    PluginWindowManager& windows;
    PluginChain* chain = nullptr;
    juce::String ownerTitle;

    juce::Label title, note, legend;
    juce::TextButton closeButton { juce::String::fromUTF8 ("\xE2\x9C\x95") };   // ✕
    juce::TextButton addButton;
    juce::Viewport viewport;
    juce::Component rowsHolder;
    std::vector<std::unique_ptr<Row>> rows;
    int dragFrom = -1, dragTarget = -1;
    int revision = 0;   // bumped by setChain(): deferred work posted for another chain is dropped

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChainDrawer)
};

} // namespace gocue::livemix
