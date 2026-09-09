#pragma once

#include "MixDocument.h"
#include "PluginPreset.h"
#include "Widgets.h"
#include "ui/PluginWindows.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace gocue::livemix
{

/** The VST3 chain of one channel / FX / the master as a vertical list: drag a row's number to reorder, a switch for
    on / bypass, 열기 for the plugin's window, ✕ to remove, "+ 플러그인 추가" grouped by manufacturer, or
    found by typing in 플러그인 검색. */
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
    std::function<void (const juce::String& text, bool error)> onStatus;   // a word for the status line (a preset saved)
    std::function<void()> onPresetSaved;   // a preset file was written: the plugin manager's list refreshes

    void showAddMenu (juce::Component* anchor);
    /** The same menu, next to an area of the screen (a button's bounds captured before a layout moved it). */
    void showAddMenu (juce::Rectangle<int> screenArea);
    /** 플러그인 검색: a window that filters the enabled plugins as the operator types, for the chains whose
        makers are too many to walk through by submenu. What is picked goes to the end of this chain. */
    void showPluginSearch();
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
    /** A preset into this chain: asked whether to replace or append when the chain is not empty. */
    void loadPreset (const PluginPreset& preset);
    /** The search window closes with the drawer, and after a pick. 'only' closes it only when it is still that
        window, so a deferred close cannot take away one opened since; null closes whichever is open. */
    void closePluginSearch (juce::Component* only = nullptr);
    /** A plugin picked in the search window, looked up in the enabled list again before it goes into the chain. */
    void addPickedPlugin (const juce::PluginDescription& description);
    void applyPreset (const PluginPreset& preset, bool replace);
    /** The chain as it is, states included, under a name asked for. */
    void saveChainAsPreset();

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
    juce::Component::SafePointer<juce::DialogWindow> pickerWindow;   // 플러그인 검색, while it is open

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChainDrawer)
};

} // namespace gocue::livemix
