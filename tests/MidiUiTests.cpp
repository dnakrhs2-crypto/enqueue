#include "MidiTestHarness.h"
#include "ui/KeyCapture.h"
#include "ui/ShortcutSettingsTab.h"
#include "ui/CueMidiPanel.h"
#include "app/ShortcutDisplay.h"

namespace gocue::tests
{
using namespace midi_test;
namespace
{
juce::Component* findControl (juce::Component& root, const juce::String& id)
{
    if (root.getComponentID() == id) return &root;
    for (auto* child : root.getChildren()) if (auto* found = findControl (*child, id)) return found;
    return nullptr;
}
bool click (juce::Component& root, const juce::String& name)
{
    for (auto* child : root.getChildren())
    {
        if (auto* button = dynamic_cast<juce::TextButton*> (child); button != nullptr && button->getButtonText() == name)
        { if (! button->isEnabled()) return false; auto callback = button->onClick; if (callback) callback(); return true; }
        if (click (*child, name)) return true;
    }
    return false;
}
juce::String ko (const char* text) { return juce::String::fromUTF8 (text); }
}
class MidiUiTests : public juce::UnitTest
{
public:
    MidiUiTests() : juce::UnitTest ("MIDI capture, settings, inspector and exchange UI", "Enqueue") {}
    void runTest() override
    {
        testCaptureModel(); testCaptureWidget(); testCaptureBoundaries(); testSettings(); testExchange(); testInspector();
    }
    void testCaptureModel()
    {
        beginTest ("first valid key or note freezes candidate; releases and foreign addresses cannot replace it");
        KeyCaptureSession session;
        session.reset();
        expect (! session.observe (message (off()), "A"));
        expect (! session.observe (message (on (60, 0)), "A"));
        expect (session.observe (message (on()), "A"));
        expect (! session.observe (message (on (61)), "A"));
        expect (! session.observe (message (on(), 1001, 2), "B"));
        expect (! session.chooseKey (juce::KeyPress ('A')));
        const auto first = std::get<MidiTrigger> (session.candidate());
        expect (first == note (60, 1, "A"));
        expect (! session.released());
        session.observe (message (off()), "A");
        session.observe (message (off (61)), "A");
        session.observe (message (off(), 1002, 2), "B");
        expect (session.released());
        expect (std::get<MidiTrigger> (session.candidate()) == first);
        session.reset (true);
        session.observe (message (on (48, 50, 3)), "A");
        expect (std::get<MidiTrigger> (session.candidate()) == note (48, 3));
        session.reset();
        expect (session.chooseKey (juce::KeyPress ('A')));
        session.observe (message (control (127)), "A");
        expect (std::holds_alternative<juce::KeyPress> (session.candidate()));
        MidiInputEvent unsupported;
        expect (! MidiInputEvent::copyMessage (juce::MidiMessage::programChange (1, 2), unsupported));

        beginTest ("CC address lock, single-value uncertainty, rising/falling/continuous/narrow suggestions require explicit preset");
        for (const bool falling : { false, true })
        {
            session.reset();
            session.observe (message (control (falling ? 127 : 0)), "A");
            expect (session.needsPreset() && ! session.canRegister());
            expect (! session.suggestion().changed);
            expect (! session.observe (message (control (50, 65)), "A"));
            expect (! session.observe (message (control (80, 64, 2)), "A"));
            expect (! session.observe (message (control (100), 1001, 2), "B"));
            session.observe (message (control (falling ? 0 : 127), 1010), "A");
            expectEquals (session.suggestion().trigger.highThreshold, 64);
            expect (session.suggestion().trigger.edge == (falling ? MidiTrigger::Edge::falling : MidiTrigger::Edge::rising));
            expect (! session.canRegister());
            session.choosePreset (KeyCaptureSession::Preset::pedal);
            expect (session.canRegister() && ! session.released());
            session.observe (message (control (falling ? 127 : 0), 1020), "A");
            expect (session.released());
        }
        session.reset();
        for (int v : { 40, 41, 46, 50 }) session.observe (message (control (v), 1000 + v), "A");
        expectEquals (session.suggestion().trigger.highThreshold, 45);
        expect (session.suggestion().trigger.behavior == MidiTrigger::Behavior::pulse);
        session.choosePreset (KeyCaptureSession::Preset::knob);
        expect (session.canRegister() && session.released());
        expect (session.moving (1249)); expect (! session.moving (1250));
        session.observe (message (control (50), 1240), "A");
        expect (! session.moving (1250)); // identical values do not prolong movement
        session.choosePreset (KeyCaptureSession::Preset::toggle);
        expect (std::get<MidiTrigger> (session.candidate()).edge == MidiTrigger::Edge::both);
        auto bad = std::get<MidiTrigger> (session.candidate());
        bad.lowThreshold = bad.highThreshold; session.edit (bad); expect (! session.canRegister());
        session.reset(); expect (std::holds_alternative<std::monostate> (session.candidate()));
    }
    void testCaptureWidget()
    {
        beginTest ("held Note or CC gate that starts capture must release before a candidate can be learned");
        for (bool gate : { false, true })
        {
            Harness start;
            start.service->setMidiTriggers ("transport.go", { gate ? cc() : note() });
            if (gate) start.send (control (0));
            start.send (gate ? control (127) : on());
            KeyCapture widget (*start.service, [] { return false; });
            widget.start ("start");
            start.send (on (72));
            expect (std::holds_alternative<std::monostate> (widget.model().candidate()));
            start.send (gate ? control (0) : off());
            expect (std::holds_alternative<std::monostate> (widget.model().candidate()));
            start.send (off (72)); start.send (on (73));
            expect (std::get<MidiTrigger> (widget.model().candidate()).number == 73);
            widget.cancel();
        }
        beginTest ("capture owns keyboard and MIDI through release, async conflict confirmation, failed save and retry");
        Harness h;
        expect (h.service->setMidiTriggers ("transport.panicAll", { note (61) }).wasOk());
        bool held = false;
        KeyCapture capture (*h.service, [&] { return held; });
        capture.setSize (600, 370);
        KeyCapture::Completion completion;
        KeyCaptureSession::Candidate proposed;
        capture.onSubmit = [&] (const auto& candidate, auto done) { proposed = candidate; completion = std::move (done); };
        capture.start ("GO");
        h.send (on()); h.send (on (61));
        expect (h.panics.empty());
        expect (click (capture, ko ("등록")));
        capture.pollKeyRelease(); expect (! completion);
        h.send (off()); h.send (off (61)); capture.pollKeyRelease();
        expect (completion != nullptr && h.service->isCapturing());
        h.send (on (61)); expect (h.panics.empty());
        completion (juce::Result::fail ("disk failure"));
        expect (capture.isCapturing() && h.service->isCapturing());
        expect (std::get<MidiTrigger> (capture.model().candidate()).number == 60);
        h.send (off (61));
        capture.onSubmit = [&] (const auto& candidate, auto done)
        {
            h.onInputSave = [&] { expect (h.service->isCapturing()); };
            done (h.service->setMidiTriggers ("transport.go", { std::get<MidiTrigger> (candidate) }).status);
        };
        expect (click (capture, ko ("등록")));
        expect (! capture.isCapturing() && ! h.service->isCapturing());
        expectEquals (h.downs (CommandIDs::go), 0);
        h.now += 30; h.send (on()); expectEquals (h.downs (CommandIDs::go), 1);
        h.send (off());
        capture.start ("cancel"); capture.receive (juce::KeyPress ('A'));
        capture.receive (juce::KeyPress (juce::KeyPress::escapeKey));
        expect (! h.service->isCapturing());
        completion (juce::Result::ok()); // stale completion cannot affect a new owner
        capture.start ("new");
        completion (juce::Result::ok()); expect (h.service->isCapturing());
        h.service->setEditingLocked (true); expect (! capture.isCapturing());
        h.service->setEditingLocked (false);

        beginTest ("CC controls require preset; pulse registration quarantines movement until 200ms and next edge fires once");
        KeyCapture pulse (*h.service, [] { return false; });
        pulse.setSize (600, 370);
        pulse.onSubmit = [&] (const auto& candidate, auto done)
        { done (h.service->setMidiTriggers ("transport.preview", { std::get<MidiTrigger> (candidate) }).status); };
        pulse.start ("preview");
        h.send (control (0)); h.now += 10; h.send (control (127));
        expect (! click (pulse, ko ("등록")));
        auto* preset = dynamic_cast<juce::ComboBox*> (findControl (pulse, "midiPreset"));
        expect (preset != nullptr);
        if (preset != nullptr) { preset->setSelectedId (2, juce::dontSendNotification); preset->onChange(); }
        expect (click (pulse, ko ("등록")));
        expect (! h.service->isCapturing());
        expect (h.router->bindingStatus ({ "transport.preview", h.service->getMidiTriggers ("transport.preview")[0], CommandIDs::preview, true }).contains (ko ("움직임")));
        h.now += 10; h.send (control (0)); expectEquals (h.downs (CommandIDs::preview), 0);
        h.now += 199; h.send (control (0)); expectEquals (h.downs (CommandIDs::preview), 0);
        h.now += 1; h.send (control (127)); expectEquals (h.downs (CommandIDs::preview), 1);

        beginTest ("rule text fields retain capture and receive editing keys; Esc never leaks to panic");
        juce::Component fields; juce::TextEditor editor; fields.addAndMakeVisible (editor);
        fields.getProperties().set ("inputCaptureField", true);
        ShortcutRouter::Callbacks callbacks;
        callbacks.keyDown = [] (int) { return false; }; callbacks.nativeKeyDown = [] (int) { return false; };
        ShortcutRouter keys (*h.service, h.manager, callbacks);
        pulse.start ("fields");
        const auto count = h.target.invocations.size();
        expect (! keys.route (juce::KeyPress ('1'), &editor, {}, 10000));
        expect (h.service->isCapturing());
        expect (keys.route (juce::KeyPress (juce::KeyPress::escapeKey), &editor, {}, 10001));
        expect (! h.service->isCapturing()); expect (h.target.invocations.size() == count);
    }
    void testCaptureBoundaries()
    {
        for (bool falling : { false, true })
        {
            beginTest (juce::String ("capture releases the initially held rule on opposite CC gates: ") + (falling ? "falling" : "rising"));
            Harness h;
            expect (h.service->setMidiTriggers ("transport.go", { cc() }).wasOk());
            expect (h.service->setMidiTriggers ("transport.preview", { cc (MidiTrigger::Edge::falling) }).wasOk());
            expect (h.service->setMidiTriggers ("transport.panicAll", { note (73) }).wasOk());
            h.send (control (falling ? 0 : 127)); // baseline is physically held, without execution
            KeyCapture widget (*h.service, [] { return false; });
            widget.start ("opposite gates");
            h.send (control (62)); // hysteresis is still held
            h.send (on (72));
            expect (std::holds_alternative<std::monostate> (widget.model().candidate()));
            h.send (control (falling ? 127 : 0)); // first release of the rule held at capture start
            h.send (off (72)); h.send (on (73));
            const auto* learned = std::get_if<MidiTrigger> (&widget.model().candidate());
            expect (learned != nullptr && learned->number == 73, "the opposite gate must not inherit the release wait");
            expectEquals (h.downs (CommandIDs::go), 0);
            expectEquals (h.downs (CommandIDs::preview), 0);
            expect (h.panics.empty() && h.service->isCapturing());
        }
        for (bool fault : { false, true })
        {
            beginTest (fault ? "capture cancels pending registration on noncandidate port input loss"
                             : "capture cancels pending registration on noncandidate port disconnect and reconnect");
            Harness h; FakeDevices backend; backend.list = { { "A", "A" }, { "B", "B" } };
            MidiInputSettings selected; selected.autoUseAll = true;
            expect (h.service->setMidiInputSettings (selected).wasOk());
            expect (h.service->setMidiTriggers ("transport.go", { note (61) }).wasOk());
            MidiInputService input (*h.service, h.router->inputCallbacks(), backend.backend(), false);
            KeyCapture widget (*h.service, [] { return false; });
            int submitted = 0;
            widget.onSubmit = [&] (const auto&, auto done) { ++submitted; done (juce::Result::ok()); };
            widget.start ("port boundary");
            backend.send ("A", on()); drain (input, backend.now);
            backend.send ("B", on (61)); drain (input, backend.now);
            backend.send ("A", off()); drain (input, backend.now);
            expect (click (widget, ko ("등록")));
            expect (widget.isCapturing() && h.service->activations().anyHeld());
            expectEquals (submitted, 0);
            if (fault)
            {
                for (int i = 0; i < 8193; ++i) backend.send ("B", on (61));
                drain (input, backend.now);
            }
            else
            {
                backend.list.pop_back(); input.refresh();
                backend.list.push_back ({ "B", "B" }); input.refresh();
                backend.send ("B", off (61)); drain (input, backend.now);
            }
            widget.pollKeyRelease();
            expect (! widget.isCapturing() && ! h.service->isCapturing(), "loss must retire every observation and capture token");
            expect (! h.service->activations().anyHeld());
            expectEquals (submitted, 0, "a lost release must cancel rather than silently register");
            expectEquals (h.downs (CommandIDs::go), 0);
            widget.start ("fresh capture");
            backend.send ("A", on (74)); backend.send ("A", off (74)); drain (input, backend.now);
            expect (click (widget, ko ("등록")));
            expectEquals (submitted, 1);
            expect (! h.service->isCapturing());
        }
    }
    void testSettings()
    {
        namespace Model = ShortcutSettingsModel;
        beginTest ("MIDI headers, CC ranges and rule details preserve UTF-8 punctuation");
        {
            Harness textHarness; FakeDevices backend; backend.list = { { "A", "A" } };
            MidiInputService input (*textHarness.service, textHarness.router->inputCallbacks(), backend.backend(), false);
            MidiInputSettingsPanel panel (*textHarness.service, &input);
            for (auto* child : panel.getChildren())
                if (auto* fold = dynamic_cast<juce::TextButton*> (child); fold != nullptr && fold->getButtonText().contains (ko ("MIDI 입력")))
                {
                    expectEquals (fold->getButtonText(), ko ("+ MIDI 입력 — 이 PC  ·  MIDI 0"));
                    fold->onClick();
                    expectEquals (fold->getButtonText(), ko ("− MIDI 입력 — 이 PC  ·  MIDI 0"));
                }
            KeyCaptureSession model; model.reset();
            model.observe (message (control (0)), "A"); model.observe (message (control (127)), "A");
            expect (model.suggestion().text.contains (ko ("0–127")));
            expect (ShortcutDisplay::midiDetails (*textHarness.service, cc()).contains (ko (" · high 64 / low 60 · gate")));
        }
        beginTest ("MIDI waiting tooltip preserves the next-message explanation alongside overload history");
        {
            Harness statusHarness; FakeDevices backend; backend.list = { { "A", "A" } };
            MidiInputSettings selected; selected.autoUseAll = true;
            expect (statusHarness.service->setMidiInputSettings (selected).wasOk());
            MidiInputService input (*statusHarness.service, statusHarness.router->inputCallbacks(), backend.backend(), false);
            MidiInputSettingsPanel panel (*statusHarness.service, &input);
            juce::ListBox* devices = nullptr;
            for (auto* child : panel.getChildren())
                if (auto* list = dynamic_cast<juce::ListBox*> (child)) devices = list;
            expect (devices != nullptr);
            const auto checkState = [&] (const juce::String& state, const juce::String& tooltip)
            {
                if (devices == nullptr) return;
                panel.refresh();
                std::unique_ptr<juce::Component> row (devices->getListBoxModel()->refreshComponentForRow (0, false, nullptr));
                juce::Label* label = nullptr;
                for (auto* child : row->getChildren())
                    if (auto* text = dynamic_cast<juce::Label*> (child)) label = text;
                expect (label != nullptr);
                if (label != nullptr) { expectEquals (label->getText(), state); expectEquals (label->getTooltip(), tooltip); }
            };
            checkState (ko ("연결"), ko ("연결"));
            backend.send ("A", on()); drain (input, backend.now + 101);
            expect (input.devices()[0].status == MidiInputService::Status::waiting && input.devices()[0].overloaded);
            const auto overload = ko (" · 이 연결의 과부하 기록 있음. 푸터에서 누계를 확인하세요.");
            checkState (ko ("준비 대기"), ko ("입력 손실 뒤 다음 MIDI 메시지를 기다리는 중입니다.") + overload);
            backend.send ("A", off()); drain (input, backend.now);
            checkState (ko ("연결"), ko ("연결") + overload);
        }
        beginTest ("MIDI display and status filters share device names, channel labels and compact counts");
        Harness h;
        h.service->setAvailableMidiDevices ({ { "A", ko ("무대 페달") } });
        expect (h.service->setMidiTriggers ("transport.go", { note (48, 1, "A"), note (49) }).wasOk());
        auto rows = Model::rows (*h.service, {}, { { "A", "Stage", MidiInputService::Status::disconnected, 1, 1 } });
        auto found = Model::filter (rows, ko ("무대 페달"), Model::Category::all, Model::Status::disconnected);
        expectEquals (static_cast<int> (found.size()), 1);
        expect (found[0].midiText.contains (ko ("노트 C3 (48) · ch1")));
        rows = Model::rows (*h.service, {}, { { "A", "Stage", MidiInputService::Status::waiting, 1, 1 } });
        expectEquals (static_cast<int> (Model::filter (rows, "GO", Model::Category::all, Model::Status::waiting).size()), 1);
        rows = Model::rows (*h.service, {}, { { "A", "Stage", MidiInputService::Status::connected, 1, 1, true } });
        expectEquals (static_cast<int> (Model::filter (rows, "GO", Model::Category::all, Model::Status::overloaded).size()), 1);
        expectEquals (Model::compactMidi (*h.service, h.service->getMidiTriggers ("transport.go"), 1, [] (const auto& text) { return text.length(); }), juce::String ("+2"));
        expect (ShortcutDisplay::currentInputs (h.service.get(), CommandIDs::go).contains ("MIDI:"));
        expectEquals (ShortcutDisplay::currentKeys (h.service.get(), CommandIDs::go), juce::String ("Space"));
        expect (ShortcutDisplay::midi (*h.service, { cc() }).contains (ko ("상승")));

        beginTest ("fixed settings layout keeps expanded devices, learning fields and actions inside 640x571 at 100-150 percent");
        {
            ShortcutSettingsTab tab (*h.service, h.document);
            tab.setSize (624, 514);
            MidiInputSettingsPanel* devices = nullptr;
            KeyCapture* capture = nullptr;
            juce::ListBox* commands = nullptr;
            for (auto* child : tab.getChildren())
            {
                if (auto* panel = dynamic_cast<MidiInputSettingsPanel*> (child)) devices = panel;
                if (auto* widget = dynamic_cast<KeyCapture*> (child)) capture = widget;
                if (auto* list = dynamic_cast<juce::ListBox*> (child)) commands = list;
            }
            expect (devices != nullptr && capture != nullptr && commands != nullptr);
            if (devices != nullptr && capture != nullptr && commands != nullptr)
                for (float scale : { 1.0f, 1.1f, 1.25f, 1.5f })
                {
                    tab.setTransform (juce::AffineTransform::scale (scale));
                    for (bool expanded : { false, true })
                    {
                        if ((devices->preferredHeight() > 28) != expanded)
                            for (auto* child : devices->getChildren())
                                if (auto* button = dynamic_cast<juce::TextButton*> (child); button != nullptr && button->getButtonText().contains (ko ("MIDI 입력")))
                                { auto callback = button->onClick; callback(); }
                        tab.resized();
                        expectEquals (devices->getHeight(), expanded ? 188 : 28);
                        expect (commands->getHeight() >= 50);
                        for (auto* child : tab.getChildren()) expect (tab.getLocalBounds().contains (child->getBounds()));
                    }
                    expect (click (tab, ko ("학습")));
                    h.service->deliverCaptureMidi (message (control (0)), "A");
                    expectEquals (capture->getX(), 10); expectEquals (capture->getY(), 28);
                    expectEquals (capture->getWidth(), 604); expectEquals (capture->getHeight(), 442);
                    for (auto* child : capture->getChildren()) expect (capture->getLocalBounds().contains (child->getBounds()));
                    auto* high = findControl (*capture, "midiHigh");
                    expect (high != nullptr && high->getParentComponent()->getLocalBounds().contains (high->getBounds()));
                    tab.cancelCapture();
                }
        }

        beginTest ("device reconnect is atomic, retains input policy and reports collisions or storage failure");
        MidiInputSettings settings; settings.selected["old"] = "Old"; settings.allowBackgroundPlayback = false;
        expect (h.service->setMidiInputSettings (settings).wasOk());
        expect (h.service->setMidiTriggers ("transport.go", { note (60, 1, "old") }).wasOk());
        h.service->setAvailableMidiDevices ({ { "new", "New" } });
        h.failSave = true;
        expect (h.service->reconnectMidiInput ("old", "new", "New").failed());
        expect (h.service->getMidiInputSettings() == settings);
        expectEquals (h.service->getMidiTriggers ("transport.go")[0].source, juce::String ("old"));
        h.failSave = false;
        expect (h.service->reconnectMidiInput ("old", "new", "New").wasOk());
        expect (h.lastTransaction.midi && h.lastTransaction.devices && ! h.lastTransaction.keyboard);
        expect (h.service->getMidiInputSettings().selected.count ("new") == 1);
        expect (! h.service->getMidiInputSettings().allowBackgroundPlayback);
        expect (h.service->setMidiTriggers ("transport.preview", { note (60, 1, "old") }).wasOk());
        expect (h.service->reconnectMidiInput ("old", "new", "New").failed()); // GO already owns the destination
        expectEquals (h.service->getMidiTriggers ("transport.preview")[0].source, juce::String ("old"));
        h.service->setEditingLocked (true);
        expect (h.service->reconnectMidiInput ("new", "new", "New").failed());
    }
    void testExchange()
    {
        namespace Model = ShortcutSettingsModel;
        beginTest ("v1/v2 import previews match actual transaction, preserve PC options, unknown IDs and disconnected device references");
        Harness h;
        h.service->setMidiTriggers ("transport.go", { note (48, 1, "absent") });
        auto midi = h.service->getMidiProfile(); midi.overrides["future.action"] = { note (90, 0, "absent") };
        expect (h.service->replaceMidiProfile (midi).wasOk());
        const auto combined = h.service->exportCombinedProfile();
        expect (combined.contains ("deviceRef=") && combined.contains ("identifier=\"absent\""));
        const auto parsed = ShortcutProfile::parseExchange (combined);
        expect (parsed.wasOk()); expectEquals (parsed.midi.overrides.at ("transport.go")[0].source, juce::String ("absent"));
        expect (parsed.midi.overrides.count ("future.action") == 1);
        expectEquals (static_cast<int> (parsed.keyboard.overrides.size()), 75);
        auto preview = Model::previewImport (*h.service, h.service->exportProfile(), {});
        expect (preview.applicable && ! preview.replacesMidi && preview.text().contains (ko ("키보드만 교체, MIDI 유지")));
        preview = Model::previewImport (*h.service, combined, {});
        expect (preview.applicable && preview.replacesMidi && preview.changes.isEmpty());
        const auto inputs = h.service->getMidiInputSettings();
        expect (h.service->importProfile (combined).wasOk()); expect (h.service->getMidiInputSettings() == inputs);
        expect (! h.lastTransaction.devices);
        auto broken = combined.replace ("deviceRef=\"device1\"", "deviceRef=\"missing\"");
        expect (! Model::previewImport (*h.service, broken, {}).applicable);
        expect (! Model::previewImport (*h.service, combined.replace ("schemaVersion=\"2\"", "schemaVersion=\"99\""), {}).applicable);
        const auto triggerXml = note().toXml()->toString (juce::XmlElement::TextFormat().withoutHeader());
        const auto collision = "<ENQUEUE_SHORTCUTS schemaVersion='2' platform='windows'><ACTION id='transport.go'><MIDI>"
            + triggerXml + "</MIDI></ACTION><ACTION id='transport.preview'><MIDI>" + triggerXml + "</MIDI></ACTION></ENQUEUE_SHORTCUTS>";
        preview = Model::previewImport (*h.service, collision, {});
        expect (! preview.applicable && preview.error.contains ("MIDI conflict"), preview.error);
        expect (h.service->getMidiTriggers ("transport.go")[0].number == 48);
        expect (h.service->setMidiTriggers ("transport.panicAll", { note (62) }).wasOk());
        ShortcutProfile withoutPanic;
        withoutPanic.overrides["transport.panicAll"] = {};
        juce::String noPanicXml; withoutPanic.serialise (noPanicXml);
        expect (Model::previewImport (*h.service, noPanicXml, {}).removesLastPanic); // MIDI panic does not remove the keyboard warning
        expect (h.service->setKeys ("transport.panicAll", {}).wasOk());
        expect (ShortcutDisplay::currentKeys (h.service.get(), CommandIDs::panicAll) == ko ("미지정"));
    }
    void testInspector()
    {
        beginTest ("inspector MIDI append/change/remove preserve keyboard hotkey, project undo and source any; conflicts recover automatically");
        Harness h; Cue cue; cue.name = "Target"; cue.hotkey = "A";
        h.document.cues.add (cue); h.document.cues.setSelectedIndex (0);
        CueMidiPanel panel (h.document, *h.service);
        panel.setSize (836, 48); panel.setCue (cue.id);
        expect (panel.apply (-1, note (48, 1, "specific")).wasOk());
        expect (panel.triggers() == MidiTriggers { note (48) });
        expectEquals (h.document.cues.get (0).hotkey, juce::String ("A"));
        expect (panel.apply (-1, note (49)).wasOk());
        expect (panel.apply (0, note (50)).wasOk());
        expect (panel.remove (1).wasOk()); expectEquals (static_cast<int> (panel.triggers().size()), 1);
        expect (h.document.undo()); expectEquals (static_cast<int> (panel.triggers().size()), 2);
        expect (h.document.undo()); expect (panel.triggers()[0] == note (48));
        expect (h.service->setMidiTriggers ("transport.go", { note (48) }).wasOk());
        expect (panel.statusFor (note (48)).contains (ko ("충돌하여 비활성")));
        expect (h.service->setMidiTriggers ("transport.go", {}).wasOk());
        expect (! panel.statusFor (note (48)).contains (ko ("충돌")));
        Cue other; other.midiTriggers = { note (70) }; h.document.cues.add (other); h.document.cues.setSelectedIndex (0);
        expect (panel.apply (-1, note (70)).failed());
        h.document.perform ("loaded conflict", [&] { h.document.cues.update (0, [] (Cue& c) { c.midiTriggers = { note (70), note (71) }; }); });
        expect (panel.remove (1).wasOk()); // a retained file/Undo conflict cannot prevent deleting an unrelated item
        expect (panel.triggers() == MidiTriggers { note (70) });
        expect (panel.statusFor (note (70)).contains (ko ("충돌")));
        expect (h.document.undo()); expectEquals (static_cast<int> (panel.triggers().size()), 2);
        expect (panel.remove (0).wasOk()); expect (panel.triggers() == MidiTriggers { note (71) });
        h.service->setEditingLocked (true); expect (panel.remove (0).failed());
        panel.setCue (other.id); expect (panel.triggers().empty());
    }
};
static MidiUiTests midiUiTests;
}
