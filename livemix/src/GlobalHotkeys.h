#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace gocue::livemix
{

/** System-wide hotkeys (Windows RegisterHotKey): they fire whatever has the focus, the window minimised or in the
    tray included. A registered key no longer reaches other programs while LiveMix runs, so the settings only take
    keys that make sense for that (F keys, the number pad, or a letter / digit with Ctrl, Alt or Shift). */
class GlobalHotkeys
{
public:
    GlobalHotkeys();
    ~GlobalHotkeys();

    /** Registers 'key' under 'id' (replacing what that id had). False, with the reason, when the key cannot be a
        system-wide hotkey or Windows refuses it (another program holds it). An invalid key clears the id. */
    bool set (int id, const juce::KeyPress& key, juce::String& error);
    void clear (int id);

    /** Message thread. */
    std::function<void (int id)> onHotkey;

    //==============================================================================
    /** The Windows modifier flags and virtual key for a JUCE key; false when it cannot be a system-wide hotkey. */
    static bool toWindowsHotkey (const juce::KeyPress& key, unsigned int& modifiers, unsigned int& virtualKey);

    /** Why 'key' is not accepted as a global hotkey (a bare letter or digit would hijack typing everywhere; an
        unmappable key), or an empty string when it is fine. */
    static juce::String reasonToRefuse (const juce::KeyPress& key);

private:
    class Window;
    std::unique_ptr<Window> window;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlobalHotkeys)
};

} // namespace gocue::livemix
