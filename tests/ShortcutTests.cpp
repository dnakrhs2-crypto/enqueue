#include "app/AppSettings.h"
#include "app/ShortcutService.h"
#include "ShortcutLegacyDefaults.h"

#include <set>

namespace gocue::tests
{
namespace
{
using juce::KeyPress;
using juce::ModifierKeys;
using Owner = ShortcutKeyOwner;
constexpr int ctrl = ModifierKeys::ctrlModifier, alt = ModifierKeys::altModifier, shift = ModifierKeys::shiftModifier;

KeyPress key (int code, int modifiers = 0) { return { code, modifiers, 0 }; }
juce::String xml (const juce::String& actions)
{
    return "<ENQUEUE_SHORTCUTS schemaVersion=\"1\" platform=\"windows\">" + actions + "</ENQUEUE_SHORTCUTS>";
}

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
    void getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info) override { catalog.getCommandInfo (id, info); }
    bool perform (const InvocationInfo& info) override { invoked = info.commandID; return true; }
    juce::CommandID invoked = 0;
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
    CatalogTarget target;
    juce::ApplicationCommandManager manager;
    std::unique_ptr<ShortcutService> service;
    bool failSave = false;
    int saves = 0, notifications = 0;
    juce::String currentXml, lastGoodXml;
    std::function<void()> onSave, onNotify;
};

bool hasConflict (const Owner& owner, Owner::Kind kind, const juce::String& id)
{
    return std::any_of (owner.conflicts.begin(), owner.conflicts.end(), [&] (const auto& conflict) { return conflict.kind == kind && conflict.id == id; });
}
} // namespace

class ShortcutCatalogTests : public juce::UnitTest
{
public:
    ShortcutCatalogTests() : juce::UnitTest ("Shortcut catalog", "Enqueue") {}
    void runTest() override
    {
        const auto& catalog = ShortcutCatalog::get();
        beginTest ("all 74 MainComponent registration IDs and JUCE quit have unique stable catalog entries");
        auto registered = CommandIDs::getAllMainCommands(); // the exact path called by MainComponent::getAllCommands
        expectEquals (registered.size(), 74);
        registered.add (juce::StandardApplicationCommandIDs::quit);
        expectEquals (static_cast<int> (catalog.getCommands().size()), registered.size());
        std::set<juce::String> ids;
        std::set<juce::CommandID> commandIDs;
        for (const auto commandID : registered)
        {
            const auto* entry = catalog.find (commandID);
            expect (entry != nullptr, "Missing CommandID " + juce::String (commandID));
            if (entry == nullptr)
                continue;
            expect (ids.insert (entry->id).second);
            expect (commandIDs.insert (entry->commandID).second);
            expect (catalog.find (entry->id) == entry);
            expect (entry->isCommand() && entry->id.containsChar ('.'));
            expect (entry->name.isNotEmpty() && entry->description.isNotEmpty());
            expect (ShortcutCatalog::categoryLabel (entry->category).isNotEmpty());
            expect (ShortcutCatalog::scopeLabel (entry->scope).isNotEmpty());
        }

        beginTest ("original 0.10.6 getCommandInfo defaults are unchanged, including hook-owned Windows Esc");
        for (const auto commandID : registered)
        {
            const auto* entry = catalog.find (commandID);
            if (entry == nullptr)
                continue;
            const auto legacy = shortcut_test::legacyDefaults (commandID);
            expect (entry->defaultKeys == legacy, entry->id);
            juce::ApplicationCommandInfo info (commandID);
            expect (catalog.getCommandInfo (commandID, info));
            expect (info.defaultKeypresses == legacy, "command metadata: " + entry->id);
        }
        expect (catalog.find ("transport.panicAll")->defaultKeys == ShortcutKeys { key (KeyPress::escapeKey) });
        expect (catalog.find ("edit.redo")->defaultKeys == ShortcutKeys { key ('Y', ctrl), key ('Z', ctrl | shift) });
        expect (catalog.find ("cue.addMic")->defaultKeys == ShortcutKeys { key ('6', ctrl) });
        expect (! catalog.find ("transport.go")->allowsRepeat);
        expect (catalog.find ("cue.moveUp")->allowsRepeat);

        beginTest ("fixed component bindings are read-only, scoped, unique and include legacy modifier predicates");
        for (const auto& entry : catalog.getFixedComponents())
        {
            expect (! entry.isCommand());
            expect (ids.insert (entry.id).second);
            expect (catalog.find (entry.id) == &entry);
            expect (! entry.defaultKeys.isEmpty());
            expect (entry.scope != ShortcutScope::mainWindow && entry.scope != ShortcutScope::application && entry.scope != ShortcutScope::playback);
            expect (ShortcutCatalog::scopeLabel (entry.scope).isNotEmpty());
            for (const auto& binding : entry.defaultKeys)
                expect (ShortcutKeyCodec::validate (binding).wasOk());
        }
        expect (catalog.find ("cueTable.editNumber")->defaultKeys.contains (key ('N')));
        expect (catalog.find ("cueTable.delete")->defaultKeys.contains (key (KeyPress::backspaceKey)));
        expect (catalog.find ("waveform.trimStart")->defaultKeys.contains (key ('I', shift | alt)));
        expect (catalog.find ("waveform.movePointFine")->defaultKeys.contains (key (KeyPress::leftKey, ctrl | alt | shift)));
        expect (catalog.find ("levelMatrix.edit")->defaultKeys.contains (key (KeyPress::F2Key)));
        expect (catalog.find ("curveEditor.deletePoint")->defaultKeys.contains (key (KeyPress::deleteKey)));
        expect (catalog.find ("groupTimeline.selectChild")->defaultKeys.contains (key (KeyPress::upKey, ctrl)));
        expect (ShortcutService::calculateMapping (catalog, {}).wasOk());
    }
};

class ShortcutProfileTests : public juce::UnitTest
{
public:
    ShortcutProfileTests() : juce::UnitTest ("Shortcut profile and strict key codec", "Enqueue") {}
    void runTest() override
    {
        beginTest ("every allowlisted key and all eight Ctrl/Alt/Shift combinations round trip exactly");
        std::set<juce::String> names;
        for (const auto& named : ShortcutKeyCodec::allowedKeys())
        {
            expect (names.insert (named.name).second);
            for (const int modifiers : { 0, ctrl, alt, shift, ctrl | alt, ctrl | shift, alt | shift, ctrl | alt | shift })
            {
                const auto original = key (named.code, modifiers);
                const auto element = ShortcutKeyCodec::toXml (original);
                expect (element != nullptr);
                if (element == nullptr)
                    continue;
                KeyPress parsed;
                expect (ShortcutKeyCodec::parse (*element, parsed).wasOk(), named.name);
                expect (original == parsed, named.name);
                expectEquals (ShortcutKeyCodec::keyName (parsed), named.name);
            }
        }
        expectEquals (ShortcutKeyCodec::keyName (key ('s', ctrl)), juce::String ("S"));
        expect (ShortcutKeyCodec::validate (key (KeyPress::playKey)).failed());
        expect (ShortcutKeyCodec::validate (key (KeyPress::F25Key)).failed());
        expect (ShortcutKeyCodec::validate (key ('A', ModifierKeys::leftButtonModifier)).failed());
        expect (ShortcutKeyCodec::validate (KeyPress()).failed());

        beginTest ("unknown names, modifier typos, empty values and duplicate keys reject the whole profile");
        for (const auto* invalid : {
            "<KEY key=\"Spce\"/>", "<KEY key=\"F25\"/>", "<KEY key=\"F01\"/>", "<KEY key=\"\"/>", "<KEY/>",
            "<KEY key=\"space\"/>", "<KEY key=\"Ctrl+S\"/>", "<KEY key=\"Ctrl\"/>", "<KEY key=\"MediaPlay\"/>",
            "<KEY key=\"S\" ctrl=\"true\"/>", "<KEY key=\"S\" ctrl=\"2\"/>", "<KEY key=\"S\" alt=\"\"/>",
            "<KEY key=\"S\" shift=\" 1\"/>", "<KEY key=\"S\" win=\"1\"/>", "<KEY key=\"S\" command=\"1\"/>",
            "<KEY key=\"S\" Ctrl=\"1\"/>", "<KEY key=\"S\" ctrl=\"1\" ctrl=\"0\"/>",
            "<KEY key=\"S\"><KEY key=\"A\"/></KEY>", "<KEY key=\"S\"/><KEY key=\"S\"/>" })
        {
            const auto source = xml ("<ACTION id=\"file.save\">" + juce::String (invalid) + "</ACTION>");
            const auto parsed = ShortcutProfile::parse (source);
            expect (! parsed.wasOk(), invalid);
            expect (parsed.profile.overrides.empty());
            expectEquals (parsed.originalXml, source);
        }

        beginTest ("malformed XML, wrong structure/platform, missing/old/future schemas preserve the original input");
        for (const auto& source : juce::StringArray {
            "", " ", "<ENQUEUE_SHORTCUTS", "<ENQUEUE_SHORTCUTS/>", "<FOREIGN schemaVersion=\"1\" platform=\"windows\"/>",
            "<ENQUEUE_SHORTCUTS schemaVersion=\"0\" platform=\"windows\"/>",
            "<ENQUEUE_SHORTCUTS schemaVersion=\"2\" platform=\"windows\"/>",
            "<ENQUEUE_SHORTCUTS schemaVersion=\"99999999999999999999999\" platform=\"windows\"/>",
            "<ENQUEUE_SHORTCUTS schemaVersion=\"01\" platform=\"windows\"/>",
            "<ENQUEUE_SHORTCUTS schemaVersion=\"one\" platform=\"windows\"/>",
            "<ENQUEUE_SHORTCUTS schemaVersion=\"1\" platform=\"macos\"/>",
            "<ENQUEUE_SHORTCUTS schemaVersion=\"1\" platform=\"windows\" extra=\"yes\"/>",
            xml ("<ACTION/>"), xml ("<ACTION id=\"bad id\"/>"), xml ("<ACTION id=\"file.save\"/><ACTION id=\"file.save\"/>"),
            xml ("<ACTION id=\"file.save\" unknown=\"1\"/>"), xml ("<ACTION id=\"file.save\"><KEY key=\"S\"/></WRONG>"),
            xml ("") + "garbage", xml ("") + xml (""), xml ("text"),
            "<!DOCTYPE ENQUEUE_SHORTCUTS [<!ENTITY x SYSTEM 'file:///missing'>]>" + xml (""),
            xml ("<ACTION id=\"file.save\"><![CDATA[ignored]]></ACTION>") })
        {
            const auto parsed = ShortcutProfile::parse (source);
            expect (! parsed.wasOk(), source);
            expectEquals (parsed.originalXml, source);
        }
        expect (ShortcutProfile::parse ("<ENQUEUE_SHORTCUTS schemaVersion=\"0\" platform=\"windows\"/>").error == ShortcutProfileParseResult::Error::oldSchema);
        expect (ShortcutProfile::parse ("<ENQUEUE_SHORTCUTS schemaVersion=\"2\" platform=\"windows\"/>").error == ShortcutProfileParseResult::Error::futureSchema);

        beginTest ("multiple keys, explicit empty lists, inheritance, unknown IDs and key order survive persistence");
        const auto parsed = ShortcutProfile::parse (xml (
            "<ACTION id=\"transport.go\"><KEY key=\"F24\" alt=\"1\"/><KEY key=\"Space\"/><KEY key=\"F13\" ctrl=\"0\"/></ACTION>"
            "<ACTION id=\"transport.preview\"/><ACTION id=\"future.action\"><KEY key=\"NumPad1\" ctrl=\"1\" shift=\"1\"/></ACTION>"));
        expect (parsed.wasOk(), parsed.message);
        juce::String saved;
        expect (parsed.profile.serialise (saved).wasOk());
        const auto again = ShortcutProfile::parse (saved);
        expect (again.wasOk(), again.message);
        expect (again.profile == parsed.profile);
        expect (again.profile.overrides.at ("transport.go") == ShortcutKeys { key (KeyPress::F24Key, alt), key (KeyPress::spaceKey), key (KeyPress::F13Key) });
        const auto mapping = ShortcutService::calculateMapping (ShortcutCatalog::get(), again.profile);
        expect (mapping.wasOk());
        expect (mapping.keys.at ("transport.preview").isEmpty());
        expect (mapping.keys.at ("file.save") == ShortcutKeys { key ('S', ctrl) });
        expect (mapping.keys.count ("future.action") == 0);
        expect (mapping.diagnostics.size() == 1 && mapping.diagnostics.front().code == ShortcutDiagnostic::Code::unknownAction);
        expect (ShortcutProfile::parse ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- portable -->" + xml ("")).wasOk());

        beginTest ("programmatic profiles cannot serialize invalid keys or duplicate aliases");
        ShortcutProfile invalid;
        invalid.overrides["file.save"] = { key ('S', ctrl), key ('s', ctrl) };
        juce::String untouched = "unchanged";
        expect (invalid.serialise (untouched).failed());
        expectEquals (untouched, juce::String ("unchanged"));
    }
};

class ShortcutServiceTests : public juce::UnitTest
{
public:
    ShortcutServiceTests() : juce::UnitTest ("Shortcut service transactions and ownership", "Enqueue") {}
    void runTest() override
    {
        beginTest ("add, replace and remove preserve order; per-command/global defaults restore inheritance");
        {
            Harness h;
            auto& service = *h.service;
            expect (service.addKey ("transport.go", key (KeyPress::F13Key)).wasOk());
            expect (service.getKeys ("transport.go") == ShortcutKeys { key (KeyPress::spaceKey), key (KeyPress::F13Key) });
            const int saves = h.saves;
            expect (service.addKey ("transport.go", key (KeyPress::F13Key)).wasOk());
            expectEquals (h.saves, saves);
            expect (service.replaceKey ("transport.go", 0, key (KeyPress::F14Key)).wasOk());
            expect (service.getKeys ("transport.go") == ShortcutKeys { key (KeyPress::F14Key), key (KeyPress::F13Key) });
            expect (service.removeKey ("transport.go", 1).wasOk());
            expect (service.setKeys ("transport.preview", {}).wasOk());
            expect (service.getProfile().overrides.count ("transport.preview") == 1);
            expect (service.restoreCommandDefaults ("transport.go").wasOk());
            expect (service.getProfile().overrides.count ("transport.go") == 0);
            expect (service.getProfile().overrides.count ("transport.preview") == 1);
            expect (service.getKeys ("transport.go") == ShortcutKeys { key (KeyPress::spaceKey) });
            expect (service.restoreAllDefaults().wasOk());
            expect (service.getProfile().overrides.empty());
            expect (service.getKeys ("transport.preview") == ShortcutKeys { key ('V') });
            expect (service.addKey ("cueTable.editNumber", key (KeyPress::F13Key)).failed());
            expect (service.setKeys ("missing.action", {}).failed());
            expect (service.replaceKey ("transport.go", -1, key (KeyPress::F13Key)).failed());
            expect (service.removeKey ("transport.go", 99).failed());
        }

        beginTest ("command duplicates are rejected; explicit move clears the old owner in the same transaction");
        {
            Harness h;
            const auto rejected = h.service->addKey ("file.save", key ('V'));
            expect (rejected.failed());
            expectEquals (h.saves, 0);
            expect (rejected.diagnostics.size() == 1 && rejected.diagnostics.front().otherActionID == "transport.preview");
            expect (h.service->addKey ("file.save", key ('V'), ShortcutService::ConflictPolicy::move).wasOk());
            expectEquals (h.saves, 1);
            expect (h.service->getKeys ("transport.preview").isEmpty());
            expect (h.service->getProfile().overrides.at ("transport.preview").isEmpty());
            expectEquals (h.manager.getKeyMappings()->findCommandForKeyPress (key ('V')), static_cast<juce::CommandID> (CommandIDs::saveProject));
            const auto before = h.service->exportProfile();
            expect (h.service->importProfile (xml ("<ACTION id=\"file.save\"><KEY key=\"F13\"/></ACTION>"
                                                   "<ACTION id=\"transport.go\"><KEY key=\"F13\"/></ACTION>")).failed());
            expectEquals (h.service->exportProfile(), before);
        }

        beginTest ("app updates add defaults but cannot take a user's key or extend an overridden list");
        {
            auto commands = ShortcutCatalog::get().getCommands();
            auto added = commands.front();
            added.id = "transport.newAction";
            added.commandID = 0x7001;
            added.defaultKeys = { key (KeyPress::F13Key), key (KeyPress::F14Key) };
            commands.push_back (added);
            commands.front().defaultKeys.add (key (KeyPress::F15Key)); // a new default on an existing overridden command
            ShortcutCatalog updated (commands, {});
            ShortcutProfile profile;
            profile.overrides["transport.go"] = { key (KeyPress::F13Key) };
            const auto mapping = ShortcutService::calculateMapping (updated, profile);
            expect (mapping.wasOk());
            expect (mapping.keys.at ("transport.go") == ShortcutKeys { key (KeyPress::F13Key) });
            expect (mapping.keys.at ("transport.newAction") == ShortcutKeys { key (KeyPress::F14Key) });
            expect (mapping.diagnostics.size() == 1);
            expect (mapping.diagnostics.front().code == ShortcutDiagnostic::Code::defaultSuppressed);
            expectEquals (mapping.diagnostics.front().actionID, juce::String ("transport.newAction"));
            expectEquals (mapping.diagnostics.front().otherActionID, juce::String ("transport.go"));
            profile.overrides["transport.go"] = {};
            expect (ShortcutService::calculateMapping (updated, profile).keys.at ("transport.go").isEmpty());
            expect (ShortcutService::calculateMapping (updated, profile).keys.at ("transport.newAction") == added.defaultKeys);
            commands.back().defaultKeys = { key ('S', ctrl) };
            expect (ShortcutService::calculateMapping (ShortcutCatalog (commands, {}), {}).failed());
        }

        beginTest ("export freezes every resolved command; import replaces overrides and preserves unknown IDs");
        {
            Harness source;
            expect (source.service->importProfile (xml ("<ACTION id=\"transport.preview\"/>"
                                                        "<ACTION id=\"future.action\"><KEY key=\"F24\"/></ACTION>")).wasOk());
            expect (source.service->addKey ("transport.go", key (KeyPress::F13Key)).wasOk());
            const auto exported = source.service->exportProfile();
            const auto parsed = ShortcutProfile::parse (exported);
            expect (parsed.wasOk());
            expectEquals (static_cast<int> (parsed.profile.overrides.size()), 76);
            Harness destination;
            expect (destination.service->setKeys ("file.save", { key (KeyPress::F14Key) }).wasOk());
            expect (destination.service->importProfile (exported).wasOk());
            expectEquals (destination.service->exportProfile(), exported);
            expect (destination.service->getProfile().overrides.count ("future.action") == 1);
            expect (destination.service->importProfile (xml ("<ACTION id=\"transport.preview\"/>")).wasOk());
            expectEquals (static_cast<int> (destination.service->getProfile().overrides.size()), 1);
            expect (destination.service->getKeys ("file.save") == ShortcutKeys { key ('S', ctrl) });
            const auto before = destination.service->exportProfile();
            expect (destination.service->importProfile (xml ("<ACTION id=\"file.save\"><KEY key=\"F13\"/></ACTION>"
                                                             "<ACTION id=\"future.other\"><KEY key=\"nonsense\"/></ACTION>")).failed());
            expect (destination.service->importProfile (xml ("<ACTION id=\"cueTable.editNumber\"/>")).failed());
            expectEquals (destination.service->exportProfile(), before);
        }

        beginTest ("startup recovers from last-good/defaults without saving or replacing damaged/future XML");
        {
            Harness h;
            const auto good = xml ("<ACTION id=\"file.save\"><KEY key=\"F13\"/></ACTION>");
            const auto future = juce::String ("<ENQUEUE_SHORTCUTS schemaVersion=\"2\" platform=\"windows\"/>");
            auto report = h.service->restore (future, good);
            expect (report.source == ShortcutService::RestoreReport::Source::lastGood);
            expectEquals (report.rejectedXml, future);
            expect (report.message.isNotEmpty());
            expect (h.service->getKeys ("file.save") == ShortcutKeys { key (KeyPress::F13Key) });
            expectEquals (h.saves, 0);
            report = h.service->restore (juce::String ("broken"), juce::String ("also broken"));
            expect (report.source == ShortcutService::RestoreReport::Source::defaults);
            expectEquals (report.rejectedLastGoodXml, juce::String ("also broken"));
            expect (h.service->getKeys ("file.save") == ShortcutKeys { key ('S', ctrl) });
            report = h.service->restore (std::nullopt, good);
            expect (report.source == ShortcutService::RestoreReport::Source::defaults); // no primary value means a fresh/default profile
            report = h.service->restore (juce::String(), good);
            expect (report.source == ShortcutService::RestoreReport::Source::lastGood); // present but empty is damaged
            report = h.service->restore (good, future);
            expect (report.source == ShortcutService::RestoreReport::Source::current);
            expectEquals (h.saves, 0);
            expect (h.service->addKey ("transport.go", key (KeyPress::F14Key)).wasOk());
            expect (ShortcutProfile::parse (h.lastGoodXml).profile == ShortcutProfile::parse (good).profile);
        }

        beginTest ("every edit/import/reset is all-or-nothing on disk failure, including a cross-command move");
        {
            Harness h;
            expect (h.service->addKey ("transport.go", key (KeyPress::F13Key)).wasOk());
            const auto beforeProfile = h.service->getProfile();
            const auto beforeExport = h.service->exportProfile();
            const auto beforeXml = h.currentXml, beforeGood = h.lastGoodXml;
            const auto beforeJuce = h.manager.getKeyMappings()->createXml (false)->toString();
            const int beforeNotifications = h.notifications;
            h.failSave = true;
            const std::vector<std::function<ShortcutOperationResult()>> edits {
                [&] { return h.service->addKey ("transport.go", key (KeyPress::F14Key)); },
                [&] { return h.service->replaceKey ("transport.go", 0, key (KeyPress::F14Key)); },
                [&] { return h.service->removeKey ("transport.go", 0); },
                [&] { return h.service->setKeys ("transport.preview", {}); },
                [&] { return h.service->restoreCommandDefaults ("transport.go"); },
                [&] { return h.service->restoreAllDefaults(); },
                [&] { return h.service->importProfile (xml ("")); },
                [&] { return h.service->addKey ("file.save", key ('V'), ShortcutService::ConflictPolicy::move); }
            };
            for (const auto& edit : edits)
            {
                expect (edit().failed());
                expect (h.service->getProfile() == beforeProfile);
                expectEquals (h.service->exportProfile(), beforeExport);
                expectEquals (h.currentXml, beforeXml);
                expectEquals (h.lastGoodXml, beforeGood);
                expectEquals (h.manager.getKeyMappings()->createXml (false)->toString(), beforeJuce);
                expectEquals (h.notifications, beforeNotifications);
            }
            h.failSave = false;
            h.onSave = [&]
            {
                expect (h.service->getProfile() == beforeProfile);
                expectEquals (h.manager.getKeyMappings()->findCommandForKeyPress (key ('V')), static_cast<juce::CommandID> (CommandIDs::preview));
                expect (h.service->restoreAllDefaults().failed()); // a reentrant mutation cannot interleave with the transaction
            };
            h.onNotify = [&]
            {
                expect (ShortcutProfile::parse (h.currentXml).profile == h.service->getProfile());
                expectEquals (h.manager.getKeyMappings()->findCommandForKeyPress (key ('V')), static_cast<juce::CommandID> (CommandIDs::saveProject));
                expect (h.service->getKeys ("transport.preview").isEmpty());
            };
            expect (h.service->addKey ("file.save", key ('V'), ShortcutService::ConflictPolicy::move).wasOk());
            expectEquals (h.notifications, beforeNotifications + 1);
        }

        beginTest ("JUCE mappings have no stale duplicates and retain order and quit/panic keys after replacement");
        {
            Harness h;
            auto* mappings = h.manager.getKeyMappings();
            mappings->addKeyPress (CommandIDs::saveProject, key (KeyPress::F13Key));
            mappings->addKeyPress (CommandIDs::preview, key (KeyPress::F13Key)); // exercise JUCE's failure to clear other owners
            expect (h.service->setKeys ("transport.go", { key (KeyPress::F13Key), key (KeyPress::F24Key) }).wasOk());
            expectEquals (mappings->findCommandForKeyPress (key (KeyPress::F13Key)), static_cast<juce::CommandID> (CommandIDs::go));
            expectEquals (mappings->findCommandForKeyPress (key (KeyPress::spaceKey)), static_cast<juce::CommandID> (0));
            for (const auto& entry : ShortcutCatalog::get().getCommands())
                expect (mappings->getKeyPressesAssignedToCommand (entry.commandID) == h.service->getKeys (entry.id), entry.id);
            expectEquals (mappings->findCommandForKeyPress (key (KeyPress::escapeKey)), static_cast<juce::CommandID> (CommandIDs::panicAll));
            expectEquals (mappings->findCommandForKeyPress (key ('Q', ctrl)), static_cast<juce::CommandID> (juce::StandardApplicationCommandIDs::quit));
            juce::PopupMenu menu;
            menu.addCommandItem (&h.manager, CommandIDs::go);
            juce::PopupMenu::MenuItemIterator item (menu);
            expect (item.next());
            // JUCE fills the shortcut text when the popup window opens, using this manager and item ID.
            expect (item.getItem().shortcutKeyDescription.isEmpty());
            expect (item.getItem().commandManager == &h.manager);
            expect (item.getItem().commandManager->getKeyMappings()->getKeyPressesAssignedToCommand (item.getItem().itemID)
                    == ShortcutKeys { key (KeyPress::F13Key), key (KeyPress::F24Key) });
            expectEquals (item.getItem().text, juce::String ("GO"));
            expect (h.service->setKeys ("app.quit", { key (KeyPress::F15Key) }).wasOk());
            expectEquals (mappings->findCommandForKeyPress (key ('Q', ctrl)), static_cast<juce::CommandID> (0));
            juce::ApplicationCommandTarget::InvocationInfo invocation (mappings->findCommandForKeyPress (key (KeyPress::F15Key)));
            invocation.invocationMethod = juce::ApplicationCommandTarget::InvocationInfo::fromKeyPress;
            invocation.keyPress = key (KeyPress::F15Key);
            expect (h.manager.invoke (invocation, false));
            expectEquals (h.target.invoked, static_cast<juce::CommandID> (juce::StandardApplicationCommandIDs::quit));
        }

        beginTest ("show mode blocks all mutation paths at the service boundary");
        {
            Harness h;
            h.service->setEditingLocked (true);
            expect (h.service->addKey ("transport.go", key (KeyPress::F13Key)).failed());
            expect (h.service->replaceKey ("transport.go", 0, key (KeyPress::F13Key)).failed());
            expect (h.service->removeKey ("transport.go", 0).failed());
            expect (h.service->restoreCommandDefaults ("transport.go").failed());
            expect (h.service->restoreAllDefaults().failed());
            expect (h.service->importProfile (xml ("")).failed());
            expectEquals (h.saves, 0);
        }

        testOwnership();
    }

private:
    void testOwnership()
    {
        beginTest ("commands own conflicting cue keys even when disabled/outside their scope; cues retain their data");
        Harness h;
        ShortcutKeyContext context;
        context.cueHotkeys = { { "cue.one", key ('S', ctrl), true, true }, { "cue.otherList", key ('S', ctrl), false, true } };
        auto owner = h.service->resolveKeyOwner (key ('S', ctrl), context);
        expect (owner.kind == Owner::Kind::command && owner.reason == Owner::Reason::commandOverCue);
        expectEquals (owner.id, juce::String ("file.save"));
        expect (hasConflict (owner, Owner::Kind::cueHotkey, "cue.one"));
        expect (hasConflict (owner, Owner::Kind::cueHotkey, "cue.otherList"));
        context.commandEnabled = [] (juce::CommandID) { return false; };
        owner = h.service->resolveKeyOwner (key ('S', ctrl), context);
        expect (owner.kind == Owner::Kind::blocked && owner.reason == Owner::Reason::disabled);
        context.commandEnabled = {};
        context.window = ShortcutKeyContext::Window::auxiliary;
        owner = h.service->resolveKeyOwner (key ('S', ctrl), context);
        expect (owner.kind == Owner::Kind::blocked && owner.reason == Owner::Reason::outsideScope);
        expect (h.service->resolveKeyOwner (key (KeyPress::spaceKey), context).kind == Owner::Kind::command);
        context.window = ShortcutKeyContext::Window::modal;
        expect (h.service->resolveKeyOwner (key (KeyPress::spaceKey), context).kind == Owner::Kind::blocked);
        expect (h.service->resolveKeyOwner (key (KeyPress::escapeKey), context).kind == Owner::Kind::command);
        context.window = ShortcutKeyContext::Window::main;
        expect (h.service->setKeys ("file.save", {}).wasOk());
        owner = h.service->resolveKeyOwner (key ('S', ctrl), context);
        expect (owner.kind == Owner::Kind::cueHotkey && owner.id == "cue.one");
        expect (context.cueHotkeys.front().key == key ('S', ctrl));
        expect (h.service->restoreCommandDefaults ("file.save").wasOk());
        expect (h.service->resolveKeyOwner (key ('S', ctrl), context).kind == Owner::Kind::command);

        beginTest ("fixed keys win only in their focused component and report the conflicting command/cue");
        expect (h.service->setKeys ("file.save", { key ('N'), key (KeyPress::deleteKey), key (KeyPress::F2Key) }, ShortcutService::ConflictPolicy::move).wasOk());
        context.cueHotkeys = { { "cue.n", key ('N'), true, true } };
        context.focus = ShortcutScope::cueTable;
        owner = h.service->resolveKeyOwner (key ('N'), context);
        expect (owner.kind == Owner::Kind::fixedComponent && owner.id == "cueTable.editNumber");
        expect (hasConflict (owner, Owner::Kind::command, "file.save"));
        expect (hasConflict (owner, Owner::Kind::cueHotkey, "cue.n"));
        context.focus = ShortcutScope::waveform;
        expect (h.service->resolveKeyOwner (key ('N'), context).kind == Owner::Kind::command);
        expectEquals (h.service->resolveKeyOwner (key (KeyPress::deleteKey), context).id, juce::String ("waveform.deleteSelection"));
        context.focus = ShortcutScope::levelMatrix;
        expectEquals (h.service->resolveKeyOwner (key (KeyPress::F2Key), context).id, juce::String ("levelMatrix.edit"));
        context.focus = ShortcutScope::curveEditor;
        expectEquals (h.service->resolveKeyOwner (key (KeyPress::deleteKey), context).id, juce::String ("curveEditor.deletePoint"));
        context.focus = ShortcutScope::groupTimeline;
        expectEquals (h.service->resolveKeyOwner (key (KeyPress::leftKey, alt | shift), context).id, juce::String ("groupTimeline.movePreWaitFine"));
        context.focus = ShortcutScope::cueTable;
        context.componentCanHandle = [] (const juce::String&) { return false; };
        expect (h.service->resolveKeyOwner (key ('N'), context).kind == Owner::Kind::command);
        context.componentCanHandle = {};

        beginTest ("capture, panic, text editing, scope and repeat policies produce one explicit owner");
        context.textEditing = true;
        expect (h.service->resolveKeyOwner (key ('N'), context).kind == Owner::Kind::standardUi);
        expect (h.service->resolveKeyOwner (key ('C', ctrl), context).kind == Owner::Kind::standardUi);
        expect (h.service->resolveKeyOwner (key (KeyPress::F2Key), context).kind == Owner::Kind::command);
        expect (h.service->resolveKeyOwner (key (KeyPress::escapeKey), context).kind == Owner::Kind::command);
        expect (h.service->setKeys ("transport.panicAll", { key ('N') }, ShortcutService::ConflictPolicy::move).wasOk());
        expectEquals (h.service->resolveKeyOwner (key ('N'), context).id, juce::String ("transport.panicAll"));
        context.captureActive = true;
        expect (h.service->resolveKeyOwner (key ('N'), context).kind == Owner::Kind::capture);
        context.applicationActive = false;
        expect (h.service->resolveKeyOwner (key ('N'), context).reason == Owner::Reason::inactiveApp);
        context = {};
        context.isRepeat = true;
        expect (h.service->resolveKeyOwner (key (KeyPress::spaceKey), context).reason == Owner::Reason::repeatSuppressed);
        expect (h.service->resolveKeyOwner (key (KeyPress::upKey, ctrl), context).kind == Owner::Kind::command);
        context.isRepeat = false;
        context.standardUiConsumesKey = true;
        expect (h.service->resolveKeyOwner (key (KeyPress::spaceKey), context).kind == Owner::Kind::standardUi);
        context.standardUiConsumesKey = false;
        context.cueHotkeys = { { "cue.one", key (KeyPress::F24Key), true, true }, { "cue.two", key (KeyPress::F24Key), true, true } };
        expect (h.service->resolveKeyOwner (key (KeyPress::F24Key), context).kind == Owner::Kind::conflict);
        expect (h.service->resolveKeyOwner (key (KeyPress::F23Key), context).kind == Owner::Kind::none);
    }
};

class ShortcutSettingsTests : public juce::UnitTest
{
public:
    ShortcutSettingsTests() : juce::UnitTest ("Shortcut AppSettings persistence", "Enqueue") {}
    void runTest() override
    {
        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("enqueue_shortcuts_" + juce::Uuid().toString());
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        options.millisecondsBeforeSaving = 1000;
        beginTest ("successful immediate save persists both XML values and can restore the JUCE mapping on restart");
        expect (root.createDirectory().wasOk());
        const auto file = root.getChildFile ("Enqueue.settings");
        const auto custom = xml ("<ACTION id=\"file.save\"><KEY key=\"F13\"/></ACTION>");
        {
            juce::PropertiesFile properties (file, options);
            AppSettings settings (properties);
            expect (! settings.getKeyboardShortcutsXml().has_value());
            expect (settings.saveKeyboardShortcuts (custom, xml ("")));
            expect (! properties.needsToBeSaved());
            expect (file.existsAsFile());
        }
        {
            juce::PropertiesFile properties (file, options);
            AppSettings settings (properties);
            expect (settings.getKeyboardShortcutsXml().has_value());
            expect (settings.getKeyboardShortcutsLastGoodXml().has_value());
            Harness h;
            expect (h.service->restore (settings.getKeyboardShortcutsXml(), settings.getKeyboardShortcutsLastGoodXml()).source
                    == ShortcutService::RestoreReport::Source::current);
            expectEquals (h.manager.getKeyMappings()->findCommandForKeyPress (key (KeyPress::F13Key)), static_cast<juce::CommandID> (CommandIDs::saveProject));
            expectEquals (h.manager.getKeyMappings()->findCommandForKeyPress (key ('S', ctrl)), static_cast<juce::CommandID> (0));
        }

        beginTest ("save failure restores raw primary/last-good and unrelated pending settings before a later flush");
        const auto blocker = root.getChildFile ("blocked");
        expect (blocker.replaceWithText ("not a directory"));
        const auto blockedFile = blocker.getChildFile ("Enqueue.settings");
        {
            juce::PropertiesFile properties (blockedFile, options);
            AppSettings settings (properties);
            properties.setValue ("keyboardShortcuts", "broken original XML");
            properties.setValue ("keyboardShortcutsLastGood", xml (""));
            settings.setWindowState ("unrelated pending state");
            expect (! settings.saveKeyboardShortcuts (custom, custom));
            expectEquals (*settings.getKeyboardShortcutsXml(), juce::String ("broken original XML"));
            expectEquals (*settings.getKeyboardShortcutsLastGoodXml(), xml (""));
            expect (properties.needsToBeSaved());
            expectEquals (settings.getWindowState(), juce::String ("unrelated pending state"));
            expect (blocker.deleteFile());
            expect (settings.saveNow()); // the same path used by delayed saving/exit, after the failure is removed
        }
        {
            juce::PropertiesFile properties (blockedFile, options);
            AppSettings settings (properties);
            expectEquals (*settings.getKeyboardShortcutsXml(), juce::String ("broken original XML"));
            expect (ShortcutProfile::parse (*settings.getKeyboardShortcutsLastGoodXml()).profile.overrides.empty());
            expectEquals (settings.getWindowState(), juce::String ("unrelated pending state"));
        }

        beginTest ("failure restores missing properties and a previously clean dirty flag");
        {
            auto disabled = options;
            disabled.doNotSave = true;
            juce::PropertiesFile properties (root.getChildFile ("never.settings"), disabled);
            AppSettings settings (properties);
            expect (! properties.needsToBeSaved());
            expect (! settings.saveKeyboardShortcuts (custom, custom));
            expect (! properties.needsToBeSaved());
            expect (! settings.getKeyboardShortcutsXml().has_value());
            expect (! settings.getKeyboardShortcutsLastGoodXml().has_value());
            settings.flush();
            expect (! properties.getFile().exists());
        }
        expect (root.deleteRecursively());
    }
};

static ShortcutCatalogTests shortcutCatalogTests;
static ShortcutProfileTests shortcutProfileTests;
static ShortcutServiceTests shortcutServiceTests;
static ShortcutSettingsTests shortcutSettingsTests;

} // namespace gocue::tests
