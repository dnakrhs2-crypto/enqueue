#pragma once

#include "app/ShortcutService.h"

#include <set>

namespace gocue::shortcut_test
{
/** Records every actual command-target call, including the input event and down/up flag.
    Session B can attach its router to the same manager/target without changing the recorder. */
class CatalogTarget : public juce::ApplicationCommandTarget
{
public:
    explicit CatalogTarget (const ShortcutCatalog& c) : catalog (c) {}
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>& ids) override
    {
        for (const auto& entry : catalog.getCommands())
            ids.add (entry.commandID);
    }
    void getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info) override
    {
        catalog.getCommandInfo (id, info);
        info.setActive (disabledCommands.count (id) == 0);
    }
    bool perform (const InvocationInfo& info) override
    {
        invocations.push_back (info);
        return true;
    }
    int invocationCount (juce::CommandID id) const
    {
        return static_cast<int> (std::count_if (invocations.begin(), invocations.end(),
                                               [id] (const auto& info) { return info.commandID == id; }));
    }
    std::vector<InvocationInfo> invocations;
    std::set<juce::CommandID> disabledCommands;
private:
    const ShortcutCatalog& catalog;
};

struct Harness : ShortcutService::Listener
{
    explicit Harness (const ShortcutCatalog& catalog = ShortcutCatalog::get()) : target (catalog)
    {
        manager.registerAllCommandsForTarget (&target);
        manager.setFirstCommandTarget (&target);
        service = std::make_unique<ShortcutService> (manager, [this] (const juce::String& current, const juce::String& previous)
        {
            ++saves;
            if (onSave)
                onSave();
            if (failSave)
                return juce::Result::fail ("injected disk failure");
            currentXml = current;
            lastGoodXml = previous;
            return juce::Result::ok();
        }, catalog);
        service->addListener (this);
    }
    void shortcutsChanged() override
    {
        ++notifications;
        if (onNotify)
            onNotify();
    }
    /** Exercise the existing component -> JUCE mapping input path, without looking up or
        directly invoking a command ID. This is not the session-B ownership router. */
    bool pressKey (const juce::KeyPress& key, juce::Component& origin)
    {
        return origin.keyPressed (key) || manager.getKeyMappings()->keyPressed (key, &origin);
    }
    CatalogTarget target;
    juce::ApplicationCommandManager manager;
    std::unique_ptr<ShortcutService> service;
    bool failSave = false;
    int saves = 0, notifications = 0;
    juce::String currentXml, lastGoodXml;
    std::function<void()> onSave, onNotify;
};
}
