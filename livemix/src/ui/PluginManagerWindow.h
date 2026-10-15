#pragma once

#include "LiveMixSettings.h"
#include "PluginPreset.h"
#include "audio/PluginHost.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue::livemix
{

/** 플러그인 관리: a window of its own (hidden on close). The scanned plugins with a "사용" switch each (a plugin
    switched off stays out of every '+ 추가' menu), the VST2 switch (off unless the operator wants it), the scans,
    and the plugin presets of this PC: made here from the enabled plugins (1. 2. 3. ..., in a window of its own that
    goes with this one), renamed, deleted, saved to / loaded from a file, and loaded into any chain from its '+ 추가' menu. */
class PluginManagerWindow : public juce::DocumentWindow
{
public:
    PluginManagerWindow (PluginHost& host, LiveMixSettings& settings, const juce::File& presetsFolder);
    ~PluginManagerWindow() override;

    void open();
    void closeButtonPressed() override;

    /** The preset files changed (made, renamed, deleted, imported): menus that list them refresh. */
    std::function<void()> onPresetsChanged;
    /** The VST2 switch changed (already stored and applied to the host): the owner may say so in its status line. */
    std::function<void (bool enabled)> onVst2Changed;

    /** The list of presets shown again (after a backup restore put a file into the folder). */
    void refreshPresets();

private:
    class Content;
    Content* content = nullptr;
    juce::TooltipWindow tooltips { this, 700 };   // LiveMix has no app-wide one: this window's tips

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManagerWindow)
};

} // namespace gocue::livemix
