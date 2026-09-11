#pragma once

#include "app/AppSettings.h"
#include "audio/PluginHost.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue
{

/** 플러그인 관리 (Ctrl+P): a window of its own, hidden on close like the manual. The scanned VST3 plugins in a table
    with a search box and a "사용" switch each - a plugin switched off stays out of every '+ 추가' menu (what is
    already in a chain is not touched) - plus the scan, "전부 사용" / "전부 해제" for the rows shown, and "목록에서
    빼기". The switches are kept in the user settings (AppSettings) and applied to the PluginHost at launch. */
class PluginManagerWindow : public juce::DocumentWindow
{
public:
    PluginManagerWindow (PluginHost& host, AppSettings& settings);
    ~PluginManagerWindow() override;

    /** Brings the window up; typing starts the search at once. */
    void open();
    void closeButtonPressed() override;

private:
    class Content;
    Content* content = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManagerWindow)
};

} // namespace gocue
