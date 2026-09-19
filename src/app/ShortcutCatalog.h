#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace gocue
{

using ShortcutKeys = juce::Array<juce::KeyPress>;

enum class ShortcutCategory { playback, cue, edit, view, fileSettings };
enum class ShortcutScope { mainWindow, playback, application, cueTable, waveform, levelMatrix, curveEditor, groupTimeline };

/** Stable IDs are the persistence contract; CommandIDs and translated labels are not. */
struct ShortcutDefinition
{
    juce::String id, name, description;
    ShortcutCategory category = ShortcutCategory::edit;
    ShortcutScope scope = ShortcutScope::mainWindow;
    bool allowsRepeat = false;
    juce::CommandID commandID = 0; // zero: read-only component binding, never a configurable command
    ShortcutKeys defaultKeys;
    juce::String menuCategory;
    int commandFlags = 0;

    bool isCommand() const noexcept { return commandID != 0; }
};

class ShortcutCatalog
{
public:
    ShortcutCatalog();
    /** Also permits testing an app update with added commands/default keys. */
    ShortcutCatalog (std::vector<ShortcutDefinition> commands, std::vector<ShortcutDefinition> fixedComponents);

    static const ShortcutCatalog& get();
    const std::vector<ShortcutDefinition>& getCommands() const noexcept { return commands; }
    const std::vector<ShortcutDefinition>& getFixedComponents() const noexcept { return fixedComponents; }
    const ShortcutDefinition* find (const juce::String& id) const;
    const ShortcutDefinition* find (juce::CommandID commandID) const;

    /** Static text/defaults only. The target still supplies live names, ticks and enabled state. */
    bool getCommandInfo (juce::CommandID commandID, juce::ApplicationCommandInfo& result) const;
    static juce::String categoryLabel (ShortcutCategory category);
    static juce::String scopeLabel (ShortcutScope scope);

private:
    std::vector<ShortcutDefinition> commands, fixedComponents;
};

} // namespace gocue
