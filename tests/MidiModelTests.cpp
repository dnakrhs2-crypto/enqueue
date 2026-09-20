#include "MidiTestHarness.h"
#include "app/AppSettings.h"
#include "model/CuePropertyPaste.h"

namespace gocue::tests
{
using namespace midi_test;
class MidiModelTests : public juce::UnitTest
{
public:
    MidiModelTests() : UnitTest ("MIDI model, rules and persistence", "Enqueue") {}
    void runTest() override
    {
        beginTest ("strict Note/CC values, display, XML and JSON round trips");
        for (int n = 0; n < 128; ++n)
            for (int ch = 0; ch <= 16; ++ch)
            {
                const auto t = note (n, ch, "port identifier & name");
                MidiTrigger copy;
                expect (t.validate().wasOk());
                expect (MidiTrigger::fromXml (*t.toXml(), copy).wasOk() && copy == t);
                expect (MidiTrigger::fromVar (t.toVar(), copy).wasOk() && copy == t);
            }
        expectEquals (MidiTrigger::noteName (48), juce::String ("C3"));
        expectEquals (MidiTrigger::noteName (60), juce::String ("C4"));
        expectEquals (note (48).display (juce::String::fromUTF8 ("장치명")), juce::String::fromUTF8 ("노트 C3 (48) · ch1 · 장치명"));
        auto ccAny = cc(); ccAny.channel = 0;
        expectEquals (ccAny.display(), juce::String::fromUTF8 ("CC 64 ≥64 · 상승 · ch전체"));
        for (auto bad : { -1, 128 }) { auto t = note (bad); expect (t.validate().failed()); }
        for (auto bad : { -1, 17 }) { auto t = note(); t.channel = bad; expect (t.validate().failed()); }
        for (auto bad : { 0, 128 }) { auto t = note(); t.minVelocity = bad; expect (t.validate().failed()); }
        for (auto bad : { -1, 501 }) { auto t = note(); t.debounceMs = bad; expect (t.validate().failed()); }
        for (auto low : { -1, 64, 127 }) { auto t = cc(); t.lowThreshold = low; expect (t.validate().failed()); }
        expect (cc (MidiTrigger::Edge::both).validate().failed());
        expect (note (60, 1, "device").validate (true).failed());
        for (const auto& field : { "channel", "number", "minVelocity", "debounceMs" })
        {
            auto x = note().toXml(); x->setAttribute (field, "1.0"); MidiTrigger untouched = note (80);
            expect (MidiTrigger::fromXml (*x, untouched).failed()); expectEquals (untouched.number, 80);
        }
        for (const auto& value : { juce::var (1.5), juce::var (true), juce::var ("60"), juce::var() })
        {
            auto v = note().toVar(); v.getDynamicObject()->setProperty ("number", value); MidiTrigger t;
            expect (MidiTrigger::fromVar (v, t).failed());
        }

        beginTest ("first Note on fires without preparation; repeats, zero/minimum velocity and physical state stay independent");
        MidiTriggerRules rules;
        auto t = note(); t.minVelocity = 50;
        expect (rules.observe (t, message (on())).activated);
        expect (rules.observe (t, message (off(), 1010)).ready);
        expect (! rules.observe (t, message (on (60, 49), 1020)).activated);
        expect (! rules.observe (t, message (on (60, 80), 1030)).activated);
        expect (rules.observe (t, message (on (60, 0), 1040)).released);
        expect (rules.observe (t, message (on (60, 50), 1050)).activated);
        expect (! rules.observe (t, message (on(), 2000)).activated);
        rules.observe (t, message (off(), 2001));
        expect (rules.observe (t, message (on(), 2002)).activated);
        rules.observe (t, message (off(), 2003));
        expect (! rules.observe (t, message (on(), 2010)).activated);
        rules.observe (t, message (off(), 2011));
        expect (rules.observe (t, message (on(), 2022)).activated); // debounce inclusive boundary
        for (const auto& e : { message (on(), 2100, 2), message (on (60, 100, 2), 2100), message (on(), 2100, 1, 2) })
            expect (rules.observe (t, e).activated);
        rules.observe (t, message (off(), 2200, 2));
        expect (rules.observe (t, message (on(), 2220, 2)).activated);
        rules.observe (t, message (off (60, 2), 2200));
        expect (rules.observe (t, message (on (60, 100, 2), 2220)).activated);
        rules.observe (t, message (off(), 2200, 1, 2));
        expect (rules.observe (t, message (on(), 2220, 1, 2)).activated);

        beginTest ("CC hysteresis, identical values, gate release during bounce, inverted gate and pulse both");
        for (auto edge : { MidiTrigger::Edge::rising, MidiTrigger::Edge::falling })
        {
            auto trigger = cc (edge);
            MidiTriggerRules state;
            const bool rising = edge == MidiTrigger::Edge::rising;
            const auto send = [&] (int value, double time) { return state.observe (trigger, message (control (value), time)); };
            expect (! send (rising ? 0 : 127, 1000).activated);
            expect (send (rising ? 64 : 60, 1010).activated);
            expect (! send (rising ? 64 : 60, 1011).activated);
            expect (! send (62, 1012).activated);
            expect (send (rising ? 60 : 64, 1013).released);
            expect (! send (rising ? 64 : 60, 1014).activated);
            expect (send (rising ? 0 : 127, 1015).released);
            expect (send (rising ? 127 : 0, 1030).activated);
        }
        {
            const auto trigger = cc (MidiTrigger::Edge::both, MidiTrigger::Behavior::pulse);
            MidiTriggerRules state;
            expect (! state.observe (trigger, message (control (127), 1000)).activated);
            expect (state.observe (trigger, message (control (0), 1020)).activated);
            expect (state.observe (trigger, message (control (127), 1040)).activated);
            expect (! state.observe (trigger, message (control (126), 1060)).activated);
            state.quarantine (1070);
            expect (! state.observe (trigger, message (control (0), 1080)).activated);
            expect (! state.observe (trigger, message (control (127), 1090)).activated);
            expect (state.observe (trigger, message (control (0), 1290)).activated); // 200ms value quiet, then next real transition
        }

        beginTest ("intersection table: source/channel wildcards, minimum velocity and CC directions/thresholds");
        const auto intersects = MidiTriggerRules::intersects;
        expect (intersects (note(), note (60, 0, "A")));
        expect (! intersects (note (60, 1, "A"), note (60, 1, "B")));
        expect (! intersects (note (60, 1), note (60, 2)));
        expect (! intersects (note (60), cc (MidiTrigger::Edge::rising, MidiTrigger::Behavior::gate, 60)));
        auto velocity = note(); velocity.minVelocity = 127; expect (intersects (note(), velocity));
        auto threshold = cc(); threshold.highThreshold = 100; expect (intersects (cc(), threshold));
        expect (! intersects (cc(), cc (MidiTrigger::Edge::falling)));
        expect (intersects (cc (MidiTrigger::Edge::both, MidiTrigger::Behavior::pulse), cc()));
        expect (intersects (cc (MidiTrigger::Edge::both, MidiTrigger::Behavior::pulse), cc (MidiTrigger::Edge::falling)));
        testProfiles();
        testProject();
        testStorage();
    }
private:
    void testProfiles()
    {
        beginTest ("MIDI v1 strict parsing, explicit empty, unknown IDs and device hints round trip");
        MidiShortcutProfile p;
        p.overrides["transport.go"] = { note(), cc() };
        p.overrides["future.action"] = { note (20, 0, "device<&>") };
        p.overrides["transport.preview"] = {};
        p.deviceNames["device<&>"] = juce::String::fromUTF8 ("같은 이름");
        juce::String xml; expect (p.serialise (xml).wasOk());
        const auto parsed = MidiShortcutProfile::parse (xml);
        expect (parsed.wasOk() && parsed.profile == p); expectEquals (parsed.originalXml, xml);
        for (const auto& bad : { xml.replace ("schemaVersion=\"1\"", "schemaVersion=\"2\""), xml + "<ACTION/>",
                                xml.replace ("</ACTION>", "</DEVICE>"), xml.replace ("kind=\"note\"", "kind=\"note\" kind=\"cc\""),
                                xml.replace ("number=\"60\"", "number=\"128\""), xml.replace ("source=\"any\"", "source=\"&unknown;\""),
                                juce::String ("<!DOCTYPE ENQUEUE_MIDI_SHORTCUTS><ENQUEUE_MIDI_SHORTCUTS schemaVersion='1'/>"),
                                juce::String ("<ENQUEUE_MIDI_SHORTCUTS schemaVersion='1'><ACTION id='a'/><ACTION id='a'/></ENQUEUE_MIDI_SHORTCUTS>") })
        { const auto r = MidiShortcutProfile::parse (bad); expect (! r.wasOk(), bad); expectEquals (r.originalXml, bad); }

        beginTest ("combined v2 atomic import; v1 keyboard-only import preserves MIDI; complete 75-command export");
        Harness h;
        expect (h.service->replaceMidiProfile (p).wasOk());
        const auto combined = h.service->exportCombinedProfile();
        const auto exchange = ShortcutProfile::parseExchange (combined);
        expect (exchange.wasOk() && exchange.replacesMidi);
        expectEquals (static_cast<int> (exchange.keyboard.overrides.size()), 75);
        expectEquals (static_cast<int> (exchange.midi.overrides.size()), 76);
        expect (h.service->importProfile (combined).wasOk());
        expect (h.lastTransaction.keyboard && h.lastTransaction.midi && ! h.lastTransaction.devices);
        const auto midiBefore = h.service->getMidiProfile();
        expect (h.service->importProfile (h.service->exportProfile()).wasOk());
        expect (h.service->getMidiProfile() == midiBefore);
        const auto keyboardBefore = h.service->getProfile();
        h.failSave = true;
        expect (h.service->importProfile (combined).failed());
        expect (h.service->getProfile() == keyboardBefore && h.service->getMidiProfile() == midiBefore);

        beginTest ("MIDI restore preserves rejected originals; last-good is previous applied good profile");
        Harness restore;
        auto report = restore.service->restoreMidi (juce::String ("broken original"), xml);
        expect (report.source == ShortcutService::RestoreReport::Source::lastGood);
        expectEquals (report.rejectedXml, juce::String ("broken original")); expectEquals (restore.inputSaves, 0);
        expect (restore.service->setMidiTriggers ("transport.go", { note (61) }).wasOk());
        expect (MidiShortcutProfile::parse (restore.lastTransaction.midi->lastGood).profile == p);
        expect (! restore.lastTransaction.keyboard);
        report = restore.service->restoreMidi (juce::String ("bad"), juce::String ("bad good"));
        expect (report.source == ShortcutService::RestoreReport::Source::defaults);
        expectEquals (report.rejectedLastGoodXml, juce::String ("bad good"));
        expect (restore.service->getMidiProfile().overrides.empty());
        MidiInputSettings inputs; inputs.selected["A"] = "Saved name";
        expect (inputs.serialise (xml).wasOk());
        report = restore.service->restoreMidiInputs (juce::String ("bad input"), xml);
        expect (report.source == ShortcutService::RestoreReport::Source::lastGood && restore.service->getMidiInputSettings() == inputs);
        report = restore.service->restoreMidiInputs (juce::String ("bad input"), juce::String ("bad good"));
        expect (report.source == ShortcutService::RestoreReport::Source::defaults);
        expect (restore.service->getMidiInputSettings().selected.empty());
        expect (! restore.service->getMidiInputSettings().autoUseAll && restore.service->getMidiInputSettings().allowBackgroundPlayback);
    }
    void testProject()
    {
        beginTest ("project v7 strict all-list round trip; malformed MIDI leaves current project unchanged; v1-v6 empty");
        Project project;
        Cue c; c.name = "original"; c.midiTriggers = { note(), cc() };
        project.ensureMainList().cues.push_back (c);
        CueContainer cart; cart.isCart = true; Cue d; d.midiTriggers = { note (61) }; cart.cues.push_back (d); project.lists.push_back (cart);
        const auto json = ProjectSerializer::toJson (project);
        Project restored;
        expect (ProjectSerializer::fromJson (json, restored).wasOk());
        expect (restored.cues()[0].midiTriggers == c.midiTriggers && restored.lists[1].cues[0].midiTriggers == d.midiTriggers);
        auto var = ProjectSerializer::toVar (project);
        auto& trigger = var["lists"][0]["cues"][0]["midiTriggers"][0];
        trigger.getDynamicObject()->setProperty ("number", 128);
        expect (ProjectSerializer::fromJson (juce::JSON::toString (var), restored).failed());
        expectEquals (restored.cues()[0].name, juce::String ("original"));
        for (const auto& bad : { juce::var ("bad"), juce::var (true), juce::var() })
        {
            auto v = ProjectSerializer::toVar (project);
            v["lists"][0]["cues"][0].getDynamicObject()->setProperty ("midiTriggers", bad);
            expect (ProjectSerializer::fromJson (juce::JSON::toString (v), restored).failed());
        }
        for (int version = 1; version <= 6; ++version)
        {
            auto v = ProjectSerializer::toVar (project); v.getDynamicObject()->setProperty ("version", version);
            expect (ProjectSerializer::fromJson (juce::JSON::toString (v), restored).wasOk());
            expect (restored.cues()[0].midiTriggers.empty() && restored.lists[1].cues[0].midiTriggers.empty());
        }
        expect (ProjectSerializer::fromJson (json.replace ("\"version\": 7", "\"version\": 8"), restored).failed());

        beginTest ("duplicate, template and property paste do not copy MIDI; undo/redo preserves originals and notifications");
        expect (c.duplicated().midiTriggers.empty());
        WorkspaceSettings settings; settings.hasCueTemplate = true; settings.cueTemplate = c;
        Cue applied; settings.applyTemplate (applied); expect (applied.midiTriggers.empty());
        Cue target; target.midiTriggers = { note (99) };
        CuePropertyPaste::apply (c, target, {}); expect (target.midiTriggers == MidiTriggers { note (99) });
        ProjectDocument document; document.adopt (project, {});
        expectEquals (static_cast<int> (document.getMidiTriggers().size()), 3);
        expect (document.setMidiTriggers (c.id, { note (70) }).wasOk());
        expect (document.undo()); expect (document.findCueAnywhere (c.id)->midiTriggers == c.midiTriggers);
        expect (document.redo()); expect (document.findCueAnywhere (c.id)->midiTriggers == MidiTriggers { note (70) });
        expect (document.setMidiTriggers (c.id, { note (61) }).failed());
    }
    void testStorage()
    {
        beginTest ("one disk transaction for all six values, base64 envelope, 0.11.0-compatible keyboard and opaque values");
        const auto directory = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("enqueue-midi-" + juce::Uuid().toString());
        expect (directory.createDirectory().wasOk());
        juce::PropertiesFile::Options options; options.storageFormat = juce::PropertiesFile::storeAsXML; options.millisecondsBeforeSaving = -1;
        const auto file = directory.getChildFile ("settings.xml");
        {
            juce::PropertiesFile properties (file, options);
            AppSettings settings (properties);
            InputSettingsTransaction tx;
            tx.keyboard = InputSettingsTransaction::Pair { "keyboard original", "keyboard previous" };
            tx.midi = InputSettingsTransaction::Pair { "MIDI original", "MIDI previous" };
            tx.devices = InputSettingsTransaction::Pair { "device original", "device previous" };
            expect (settings.saveInputSettings (tx));
            expectEquals (*settings.getMidiShortcutsXml(), juce::String ("MIDI original"));
            expectEquals (*settings.getMidiInputSettingsLastGoodXml(), juce::String ("device previous"));
            expect (properties.getValue ("midiShortcuts").startsWith ("enqueue-shortcuts-base64-v1:"));
        }
        {
            // The old app reads/writes its keyboard pair, leaving opaque unknown properties untouched.
            juce::PropertiesFile oldApp (file, options); AppSettings settings (oldApp);
            ShortcutProfile keyboard; juce::String xml; expect (keyboard.serialise (xml).wasOk());
            expect (settings.saveKeyboardShortcuts (xml, xml));
        }
        {
            juce::PropertiesFile properties (file, options); AppSettings settings (properties);
            expectEquals (*settings.getMidiShortcutsXml(), juce::String ("MIDI original"));
            expectEquals (*settings.getMidiShortcutsLastGoodXml(), juce::String ("MIDI previous"));
            expectEquals (*settings.getMidiInputSettingsXml(), juce::String ("device original"));
            expect (ShortcutProfile::parse (*settings.getKeyboardShortcutsXml()).wasOk());
        }
        beginTest ("save failure rolls back every value and preexisting dirty state, including delayed-save candidate");
        const auto blocker = directory.getChildFile ("blocker"); expect (blocker.replaceWithText ("file instead of directory"));
        {
            juce::PropertiesFile properties (blocker.getChildFile ("settings.xml"), options); AppSettings settings (properties);
            properties.setValue ("midiShortcuts", "old raw"); properties.setNeedsToBeSaved (false);
            InputSettingsTransaction tx;
            tx.midi = InputSettingsTransaction::Pair { "new", "old" }; tx.keyboard = tx.midi; tx.devices = tx.midi;
            expect (! settings.saveInputSettings (tx));
            expectEquals (properties.getValue ("midiShortcuts"), juce::String ("old raw"));
            expect (! properties.containsKey ("keyboardShortcuts") && ! properties.containsKey ("midiInputSettings"));
            expect (! properties.needsToBeSaved());
            properties.setNeedsToBeSaved (true);
            expect (! settings.saveInputSettings (tx)); expect (properties.needsToBeSaved());
            expectEquals (properties.getValue ("midiShortcuts"), juce::String ("old raw"));
            properties.setNeedsToBeSaved (false);
        }
        expect (directory.deleteRecursively());
    }
};
static MidiModelTests midiModelTests;
}
