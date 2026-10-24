#include "ui/MainComponent.h"
#include "ui/MidiModalScope.h"
#include "app/Commands.h"
#include "app/ShortcutCatalog.h"
#include "RemoteClient.h"
#include <windows.h>

namespace gocue::tests
{
struct RemoteTestAccess
{
    static ProjectDocument& document (MainComponent& m) { return m.document; }
    static CueController& controller (MainComponent& m) { return m.controller; }
    static remote::Service& service (MainComponent& m) { return *m.remoteService; }
    static juce::Result execute (MainComponent& m, uint64_t connection, const remote::Request& r, double time) { return m.performRemote (connection, r, time); }
    static void release (MainComponent& m, uint64_t connection) { m.releaseRemote (connection); }
    static void stopTimers (MainComponent& m) { m.stopTimer(); m.scheduler.stopTicking(); m.controller.getFadeRunner().stopTicking(); }
    static void move (MainComponent& m, int from, int before, int parent = -1)
    { if (parent >= 0) m.table.onMoveRowsInto ({ from }, parent); else m.table.onMoveRows ({ from }, before); }
    static void card (MainComponent& m, const juce::Uuid& id, const juce::String& op)
    { if (op == "stop") m.activeCues.onStopRequested (id); else m.activeCues.onPauseRequested (id, op == "resume"); }
    static void cart (MainComponent& m, const juce::Uuid& id, bool stop)
    { if (stop) m.cart.onStop (id); else if (const auto* cue = m.document.findCueAnywhere (id)) m.cart.onTrigger (*cue); }
    static void wait (MainComponent& m, const WaitProgress& w) { m.activeCues.onCancelWaitRequested (w.cueId, w.kind, w.startId); }
    static bool menuFits (MainComponent& m)
    {
        int width = 0;
        const auto names = m.getMenuBarNames();
        for (int i = 0; i < names.size(); ++i) width += m.menuBar.getLookAndFeel().getMenuBarItemWidth (m.menuBar, i, names[i]);
        return width <= m.menuBar.getWidth() && ! m.menuBar.getBounds().intersects (m.modeToggle.getBounds());
    }
};
namespace
{
using remote::object;
using remote::encode;
struct LoopbackScope
{
    LoopbackScope() : previous (juce::SystemStats::getEnvironmentVariable ("ENQUEUE_REMOTE_LOOPBACK", {})) { SetEnvironmentVariableW (L"ENQUEUE_REMOTE_LOOPBACK", L"1"); }
    ~LoopbackScope() { SetEnvironmentVariableW (L"ENQUEUE_REMOTE_LOOPBACK", previous.isEmpty() ? nullptr : previous.toWideCharPointer()); }
    juce::String previous;
};
struct Fixture
{
    Fixture() : storage (directory.getChildFile ("test.settings"), options()), settings (storage)
    {
        directory.createDirectory();
        engine.prepare (44100, 256);
        main = std::make_unique<MainComponent> (engine, settings, commands);
        main->setLookAndFeel (&theme);
        RemoteTestAccess::stopTimers (*main);
        controller().clock = [this] { return time * 0.001; };
        document().settings.requireKeyUp = false;
        document().settings.doubleGoSeconds = 0;
    }
    ~Fixture() { main.reset(); storage.saveIfNeeded(); directory.deleteRecursively(); }
    static juce::PropertiesFile::Options options() { juce::PropertiesFile::Options o; o.millisecondsBeforeSaving = -1; return o; }
    ProjectDocument& document() { return RemoteTestAccess::document (*main); }
    CueController& controller() { return RemoteTestAccess::controller (*main); }
    remote::Service& service() { return RemoteTestAccess::service (*main); }
    juce::var send (juce::var request, uint64_t connection = 1)
    {
        if (! request.hasProperty ("id")) request.getDynamicObject()->setProperty ("id", ++ids[connection]);
        const auto parsed = remote::Request::parse (encode (request));
        return sessions[connection].dispatch (parsed, [&] (const auto& r) { return RemoteTestAccess::execute (*main, connection, r, time); });
    }
    juce::var go (bool down, int touch = 1, uint64_t connection = 1) { return send (object ({ { "t", "go" }, { "down", down }, { "touch", touch } }), connection); }
    void release (uint64_t connection) { RemoteTestAccess::release (*main, connection); sessions.erase (connection); }
    juce::Uuid addWait (const juce::String& name = "wait")
    {
        Cue cue; cue.type = CueType::control; cue.control.kind = ControlKind::wait; cue.control.seconds = 30; cue.name = name;
        document().cues.add (cue);
        return cue.id;
    }
    void waits (int count = 6) { for (int i = 0; i < count; ++i) addWait ("wait " + juce::String (i)); document().cues.setPlayheadIndex (0); }
    int waiting() { return static_cast<int> (controller().getRunningWaits().size()); }
    juce::String tab() { return document().getContainerInfo (document().getActiveContainer()).id.toString(); }
    juce::Uuid sound()
    {
        const auto file = directory.getChildFile ("tone.wav");
        if (! file.existsAsFile())
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (44100).withNumChannels (2).withBitsPerSample (16));
            juce::AudioBuffer<float> samples (2, 44100 * 3);
            for (int c = 0; c < 2; ++c) for (int s = 0; s < samples.getNumSamples(); ++s) samples.setSample (c, s, 0.25f);
            if (writer != nullptr) writer->writeFromAudioSampleBuffer (samples, 0, samples.getNumSamples());
        }
        Cue cue; cue.name = "tone"; cue.file = file; cue.durationSeconds = 3; cue.fadeOutMs = 700;
        document().cues.add (cue);
        return cue.id;
    }
    void render (int blocks = 4)
    { juce::AudioBuffer<float> buffer (2, 256); for (int i = 0; i < blocks; ++i) engine.renderBlock (buffer, 256); engine.reapIfNeeded(); }
    void publish() { service().dispatchPending(); service().publish (remote::PlaybackSnapshot::capture (engine, controller())); }
    double time = 10000;
    LoopbackScope loopback;
    juce::File directory = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("EnqueueRemoteTests", "");
    juce::PropertiesFile storage;
    AppSettings settings;
    AudioEngine engine { 0 };
    juce::ApplicationCommandManager commands;
    GoCueLookAndFeel theme;
    std::unique_ptr<MainComponent> main;
    std::map<uint64_t, remote::Session> sessions;
    std::map<uint64_t, juce::int64> ids;
};
bool ok (const juce::var& value) { return static_cast<bool> (value["ok"]); }
juce::var command (const char* cmd) { return object ({ { "t", "cmd" }, { "cmd", cmd } }); }
juce::var cueRequest (const juce::Uuid& id, const char* op)
{ return object ({ { "t", "cue" }, { "cue", id.toString() }, { "op", op }, { "order", 1 } }); }
juce::var waitRequest (const WaitProgress& wait)
{
    return object ({ { "t", "wait" }, { "cue", wait.cueId.toString() }, { "kind", wait.kind == WaitProgress::Kind::preWait ? "pre" : wait.kind == WaitProgress::Kind::postWait ? "post" : "wait" }, { "startId", wait.startId } });
}
juce::var moveRequest (Fixture& f, const juce::Uuid& id, const juce::Uuid& before = juce::Uuid::null(), const juce::Uuid& parent = juce::Uuid::null())
{
    return object ({ { "t", "move" }, { "tab", f.tab() }, { "cue", id.toString() },
        { "before", before.isNull() ? juce::var() : juce::var (before.toString()) }, { "parent", parent.isNull() ? juce::var() : juce::var (parent.toString()) } });
}
}

class RemoteProtocolTests : public juce::UnitTest
{
public:
    RemoteProtocolTests() : UnitTest ("Remote protocol", "Enqueue") {}
    void runTest() override
    {
        beginTest ("invalid JSON, unknown type and wrong payload return ack false");
        for (const auto* json : { "{", "[]", "null", "{\"t\":\"unknown\",\"id\":1}", "{\"t\":\"ping\"}",
            "{\"t\":\"go\",\"id\":1,\"down\":1,\"touch\":2}", "{\"t\":\"move\",\"id\":1,\"cue\":\"a\",\"tab\":\"b\"}",
            "{\"t\":\"cue\",\"id\":1,\"cue\":\"a\",\"op\":\"toggle\",\"order\":1}" })
        {
            remote::Session session;
            bool executed = false;
            const auto reply = session.dispatch (remote::Request::parse (json), [&] (const auto&) { executed = true; return juce::Result::ok(); });
            expect (! ok (reply)); expect (! executed); expectEquals (reply["t"].toString(), juce::String ("ack"));
            expect (reply["msg"].toString().isNotEmpty());
        }
        beginTest ("all client message types parse and unicode server messages round trip");
        for (const auto& message : std::vector<juce::var> {
            object ({ { "t", "go" }, { "down", true }, { "touch", 1 } }), command ("pause"), command ("fade"), command ("panic"),
            cueRequest (juce::Uuid(), "pause"), cueRequest (juce::Uuid(), "resume"), cueRequest (juce::Uuid(), "stop"),
            object ({ { "t", "wait" }, { "cue", "a" }, { "kind", "pre" }, { "startId", 1 } }),
            object ({ { "t", "fire" }, { "cue", "a" } }), object ({ { "t", "stopcue" }, { "cue", "a" } }),
            object ({ { "t", "tab" }, { "tab", "a" } }), object ({ { "t", "playhead" }, { "tab", "a" }, { "cue", "b" } }),
            object ({ { "t", "move" }, { "tab", "a" }, { "cue", "b" }, { "before", {} }, { "parent", {} } }),
            object ({ { "t", "mode" }, { "show", true } }), object ({ { "t", "ping" } }), object ({ { "t", "hi" }, { "device", ko ("iPad · 사파리") } }) })
        {
            message.getDynamicObject()->setProperty ("id", 1);
            const auto request = remote::Request::parse (encode (message));
            expect (request.error.wasOk(), encode (message));
            expectEquals (request.id, juce::int64 (1));
        }
        expectEquals (juce::JSON::parse (encode (remote::bye ("newkey")))["reason"].toString(), juce::String ("newkey"));
        beginTest ("same id, including rejected requests, is never executed twice");
        remote::Session session;
        int executions = 0;
        auto run = [&] (const remote::Request&) { ++executions; return juce::Result::fail (ko ("쇼 모드")); };
        const auto request = remote::Request::parse ("{\"t\":\"ping\",\"id\":1}");
        const auto first = session.dispatch (request, run);
        expectEquals (encode (first), encode (session.dispatch (request, run)));
        expectEquals (executions, 1);
    }
};

class RemoteInputTests : public juce::UnitTest
{
public:
    RemoteInputTests() : UnitTest ("Remote input and PC actions", "Enqueue") {}
    void runTest() override
    {
        beginTest ("real MainComponent GO down executes once and up never executes GO");
        {
            Fixture f; f.waits();
            expect (ok (f.go (true))); expectEquals (f.waiting(), 1);
            expect (ok (f.go (false))); expectEquals (f.waiting(), 1);
            expectEquals (f.document().cues.getPlayheadIndex(), 1);
            expect (! f.main->getShortcutService().activations().anyHeld());
        }
        beginTest ("duplicate id crosses the production dispatch once");
        {
            Fixture f; f.waits();
            auto request = object ({ { "t", "go" }, { "id", 10 }, { "down", true }, { "touch", 1 } });
            expect (ok (f.send (request))); expect (ok (f.send (request))); expectEquals (f.waiting(), 1);
            f.release (1);
        }
        beginTest ("keyboard and remote share requireKeyUp until the final release");
        {
            Fixture f; f.waits(); f.document().settings.requireKeyUp = true;
            auto& shortcuts = f.main->getShortcutService();
            const InputToken key { InputKind::keyboard, 1, 0, 32 };
            expect (shortcuts.activations().press (key, true, true));
            shortcuts.invokeInput ({ InputKind::keyboard, "transport.go", CommandIDs::go, true, key, f.time, InputInvocation::nextEventID() });
            expectEquals (f.waiting(), 1);
            f.go (true); expectEquals (f.waiting(), 1);
            shortcuts.activations().release (key);
            expect (! shortcuts.activations().consumeRelease());
            f.go (false); f.go (true, 2); expectEquals (f.waiting(), 2);
            f.go (true, 3); expectEquals (f.waiting(), 2);
            f.go (false, 2); f.go (true, 4); expectEquals (f.waiting(), 2);
            f.go (false, 3); f.go (false, 4); f.go (true, 5); expectEquals (f.waiting(), 3);
            f.release (1);
        }
        beginTest ("double GO follows the project setting, including when requireKeyUp is off");
        {
            Fixture f; f.waits(); f.document().settings.doubleGoSeconds = 0.5;
            f.go (true); f.go (false); f.time += 100; f.go (true, 2); f.go (false, 2); expectEquals (f.waiting(), 1);
            f.time += 500; f.go (true, 3); expectEquals (f.waiting(), 2);
            f.document().settings.doubleGoSeconds = 0; f.time += 1; f.go (true, 4); expectEquals (f.waiting(), 3);
            f.release (1);
        }
        beginTest ("disconnect releases only its own source and an old connection cannot release a new one");
        {
            Fixture f; f.waits(); f.document().settings.requireKeyUp = true;
            f.go (true, 1, 101); f.go (true, 1, 102); expectEquals (f.waiting(), 1);
            f.release (101); expect (f.main->getShortcutService().activations().anyHeld());
            f.go (true, 2, 102); expectEquals (f.waiting(), 1);
            f.release (102); expect (! f.main->getShortcutService().activations().anyHeld());
            f.go (true, 1, 103); expectEquals (f.waiting(), 2);
            f.release (102); expect (f.main->getShortcutService().activations().anyHeld()); f.release (103);
        }
        beginTest ("modal blocks ordinary requests, allows panic and releases; capture suppresses panic too");
        {
            Fixture f; f.waits(); f.go (true);
            {
                MidiModalScope modal;
                expect (! ok (f.go (true, 2))); expect (! ok (f.send (command ("pause"))));
                expect (ok (f.go (false))); expect (ok (f.go (false, 2)));
                expect (! f.main->getShortcutService().activations().anyHeld());
                expect (ok (f.send (command ("panic")))); expectEquals (f.waiting(), 0);
            }
            f.time += 5000;
            f.go (true, 3); expectEquals (f.waiting(), 1);
            f.main->getShortcutService().beginCapture (&f);
            expect (! ok (f.go (true, 4))); expect (! ok (f.send (command ("panic")))); expectEquals (f.waiting(), 1);
            expect (ok (f.go (false, 3))); expect (ok (f.go (false, 4)));
            f.main->getShortcutService().endCapture (&f);
            f.go (true, 5); expectEquals (f.waiting(), 2); f.release (1);
        }
        testModes();
        testAudio();
        testWaits();
    }
private:
    void testModes();
    void testAudio();
    void testWaits();
};

void RemoteInputTests::testModes()
{
    beginTest ("mode assigns, show mode blocks move but permits playback, tab and the actual playhead");
    Fixture f; f.waits();
    const auto a = f.document().cues.get (0).id, b = f.document().cues.get (1).id;
    const auto show = object ({ { "t", "mode" }, { "show", true } });
    expect (ok (f.send (show))); expect (ok (f.send (show)));
    expect (f.main->getShortcutService().isEditingLocked());
    auto move = f.send (moveRequest (f, a)); expect (! ok (move)); expectEquals (move["msg"].toString(), ko ("쇼 모드"));
    expect (ok (f.go (true))); expectEquals (f.waiting(), 1); f.go (false);
    f.document().cues.setLockPlayheadToSelection (false);
    f.document().cues.setSelectedIndex (0);
    expect (ok (f.send (object ({ { "t", "playhead" }, { "tab", f.tab() }, { "cue", b.toString() } }))));
    expectEquals (f.document().cues.getPlayheadIndex(), 1); expectEquals (f.document().cues.getSelectedIndex(), 0); expectEquals (f.waiting(), 1);
    const int other = f.document().addContainer ("cart", true);
    const auto otherId = f.document().getContainerInfo (other).id.toString();
    expect (ok (f.send (object ({ { "t", "tab" }, { "tab", otherId } })))); expectEquals (f.document().getActiveContainer(), other);
    expect (ok (f.send (object ({ { "t", "mode" }, { "show", false } }))));
    expect (! f.main->getShortcutService().isEditingLocked());
}

void RemoteInputTests::testAudio()
{
    beginTest ("PC cart fire and remote fire share solo playback, armed and double-fire rules");
    {
        Fixture f;
        const auto id = f.sound();
        f.document().cues.update (0, [] (Cue& cue) { cue.armed = false; });
        expect (ok (f.send (object ({ { "t", "fire" }, { "cue", id.toString() } })))); expect (! f.engine.isPlaying (id));
        f.document().cues.update (0, [] (Cue& cue) { cue.armed = true; cue.preWaitSeconds = 10; cue.secondTrigger = SecondTriggerAction::stop; });
        f.document().settings.doubleGoSeconds = 0.5;
        RemoteTestAccess::cart (*f.main, id, false); f.render(); expect (f.engine.isPlaying (id)); expectEquals (f.waiting(), 0);
        expect (ok (f.send (object ({ { "t", "fire" }, { "cue", id.toString() } }))));
        f.render(); expect (! f.engine.getPlayingCues().front().fadingOut);
        f.time += 600;
        expect (ok (f.send (object ({ { "t", "fire" }, { "cue", id.toString() } }))));
        f.render(); expect (f.engine.getPlayingCues().front().fadingOut);
    }
    for (const auto* op : { "pause", "resume", "stop" })
    {
        beginTest (juce::String ("cue ") + op + " matches the PC active-card callback");
        Fixture pc, web;
        const auto a = pc.sound(), b = web.sound();
        RemoteTestAccess::cart (*pc.main, a, false); RemoteTestAccess::cart (*web.main, b, false);
        pc.render(); web.render();
        if (juce::String (op) == "resume") { pc.controller().pauseCue (a); web.controller().pauseCue (b); pc.render(); web.render(); }
        RemoteTestAccess::card (*pc.main, a, op);
        expect (ok (web.send (cueRequest (b, op)))); pc.render(); web.render();
        const auto p = pc.engine.getPlayingCues(), w = web.engine.getPlayingCues();
        expectEquals (static_cast<int> (p.size()), static_cast<int> (w.size()));
        expect (! p.empty() && ! w.empty());
        if (! p.empty() && ! w.empty()) { expect (p.front().paused == w.front().paused); expect (p.front().fadingOut == w.front().fadingOut); }
        if (juce::String (op) == "stop") { expect (web.engine.getPlayingCues().front().fadingOut); expect (! web.controller().hasPendingFor (b)); }
    }
    beginTest ("card stop preserves the running auto-continue follow; cart stop cancels the whole run");
    for (bool card : { true, false })
    {
        Fixture f;
        const auto id = f.sound(); f.addWait();
        f.document().cues.update (0, [] (Cue& cue) { cue.continueMode = ContinueMode::autoContinue; cue.postWaitSeconds = 5; });
        f.controller().fireSequence (0);
        f.render();
        expect (! f.controller().getRunningWaits().empty());
        expect (ok (f.send (card ? cueRequest (id, "stop") : object ({ { "t", "stopcue" }, { "cue", id.toString() } }))));
        f.render();
        for (const auto& wait : f.controller().getRunningWaits()) expect (wait.kind != WaitProgress::Kind::preWait || wait.cueId != id);
        if (card) expect (! f.controller().getRunningWaits().empty());
        else expectEquals (f.controller().getNumPending(), 0);
    }
    beginTest ("card stop cancels a scheduled restart just like the PC card");
    {
        Fixture pc, web;
        const auto a = pc.sound(), b = web.sound();
        for (auto* f : { &pc, &web })
        {
            f->addWait();
            f->document().cues.update (0, [] (Cue& cue) { cue.continueMode = ContinueMode::autoContinue; cue.postWaitSeconds = 5; cue.secondTrigger = SecondTriggerAction::hardStopRestart; });
            f->controller().fireSequence (0);
            f->document().cues.update (0, [] (Cue& cue) { cue.preWaitSeconds = 10; });
            f->controller().fireSequence (0);
            f->render();
            expect (f->controller().hasPendingFor (f->document().cues.get (0).id));
        }
        RemoteTestAccess::card (*pc.main, a, "stop");
        expect (ok (web.send (cueRequest (b, "stop")))); pc.render(); web.render();
        expectEquals (web.controller().getNumPending(), pc.controller().getNumPending());
        expectEquals (web.waiting(), pc.waiting()); expect (! web.controller().hasPendingFor (b));
    }
    beginTest ("transport pause/fade use PC target selection; panic is fade once then immediate inside 500 ms");
    {
        Fixture f; const auto id = f.sound(); RemoteTestAccess::cart (*f.main, id, false); f.render();
        expect (ok (f.send (command ("pause")))); f.render(); expect (f.engine.isPaused (id));
        expect (ok (f.send (command ("pause")))); f.render(); expect (! f.engine.isPaused (id));
        expect (ok (f.send (command ("fade")))); f.render(); expect (f.engine.getPlayingCues().front().fadingOut);
    }
    {
        Fixture f; const auto id = f.sound(); RemoteTestAccess::cart (*f.main, id, false); f.render();
        f.document().settings.panicSeconds = 1;
        expect (ok (f.send (command ("panic")))); f.render();
        expect (f.engine.isPlaying (id)); expect (f.controller().isPanicLatched()); expect (f.engine.getPlayingCues().front().fadingOut);
        f.time += 300;
        expect (ok (f.send (command ("panic")))); f.render(); expect (! f.engine.isPlaying (id));
        expect (f.controller().isPanicLatched());
    }
}

void RemoteInputTests::testWaits()
{
    for (const auto kind : { WaitProgress::Kind::preWait, WaitProgress::Kind::postWait, WaitProgress::Kind::waitCue })
    {
        beginTest ("remote wait x matches the PC wait-card callback: " + juce::String (static_cast<int> (kind)));
        Fixture pc, web;
        for (auto* f : { &pc, &web })
        {
            f->waits (2);
            f->document().cues.update (0, [kind] (Cue& cue)
            {
                if (kind == WaitProgress::Kind::preWait) cue.preWaitSeconds = 5;
                if (kind == WaitProgress::Kind::postWait) { cue.control.kind = ControlKind::memo; cue.continueMode = ContinueMode::autoContinue; cue.postWaitSeconds = 5; }
            });
            f->go (true); f->go (false);
        }
        const auto p = pc.controller().getRunningWaits(), w = web.controller().getRunningWaits();
        expect (! p.empty() && ! w.empty());
        if (p.empty() || w.empty()) continue;
        expect (w.front().kind == kind);
        RemoteTestAccess::wait (*pc.main, p.front()); expect (ok (web.send (waitRequest (w.front()))));
        expectEquals (web.waiting(), pc.waiting()); expectEquals (web.waiting(), 0);
        expectEquals (web.controller().getNumPending(), pc.controller().getNumPending());
    }
}

class RemoteMoveTests : public juce::UnitTest
{
public:
    RemoteMoveTests() : UnitTest ("Remote move and model snapshots", "Enqueue") {}
    void runTest() override
    {
        for (int destination = 0; destination < 5; ++destination)
        {
            beginTest ("remote move equals PC row drop, undo and redo: " + juce::String (destination));
            Fixture f;
            Cue group; group.type = CueType::group; group.name = "group";
            f.document().cues.add (group);
            const auto child = f.addWait ("child"); f.document().cues.update (1, [&] (Cue& c) { c.parentId = group.id; });
            const auto sibling = f.addWait ("sibling"); f.document().cues.update (2, [&] (Cue& c) { c.parentId = group.id; });
            const auto outside = f.addWait ("outside"); const auto end = f.addWait ("end");
            auto& list = f.document().cues;
            const auto original = layout (list);
            juce::var request;
            if (destination == 0) { request = moveRequest (f, outside, child, group.id); RemoteTestAccess::move (*f.main, 3, 1); }
            if (destination == 1) { request = moveRequest (f, child, end); RemoteTestAccess::move (*f.main, 1, 4); }
            if (destination == 2) { request = moveRequest (f, outside, juce::Uuid::null(), group.id); RemoteTestAccess::move (*f.main, 3, -1, 0); }
            if (destination == 3) { request = moveRequest (f, child); RemoteTestAccess::move (*f.main, 1, list.size()); }
            if (destination == 4) { request = moveRequest (f, group.id); RemoteTestAccess::move (*f.main, 0, list.size()); }
            const auto expected = layout (list), undoName = f.document().getUndoName();
            expect (original != expected); expect (f.document().undo()); expectEquals (layout (list), original);
            expect (ok (f.send (request))); expectEquals (layout (list), expected); expectEquals (f.document().getUndoName(), undoName);
            expect (f.document().undo()); expectEquals (layout (list), original);
            expect (f.document().redo()); expectEquals (layout (list), expected);
        }
        beginTest ("list generation changes repeatedly while already dirty, including inactive lists, heads and container replacement");
        Fixture f; f.waits (2);
        remote::SnapshotModel model (f.document());
        auto previous = model.lists (false);
        const auto changed = [&] { auto next = model.lists (false); expect (static_cast<juce::int64> (next["gen"]) > static_cast<juce::int64> (previous["gen"])); previous = next; };
        f.document().cues.update (0, [] (Cue& c) { c.name = ko ("첫 수정"); }); changed();
        f.document().cues.update (0, [] (Cue& c) { c.name = ko ("둘째 수정"); c.number = "1.2"; c.armed = false; c.color = 2; }); changed();
        const auto id = f.document().cues.get (0).id;
        const auto firstTab = f.tab();
        const int other = f.document().addContainer ("other", false); changed();
        f.document().setActiveContainer (other); changed();
        auto* inactive = f.document().listContaining (id);
        expect (inactive != nullptr);
        if (inactive != nullptr)
        {
            inactive->update (0, [] (Cue& c) { c.name = "inactive edit"; }); changed();
            inactive->setPlayheadIndex (1); changed();
            expectEquals (previous["playhead"][juce::Identifier (firstTab)].toString(), inactive->get (1).id.toString());
        }
        expect (f.document().removeContainer (0)); changed();
        f.document().newProject(); changed();
        expectEquals (previous["tabs"].size(), 1);
        beginTest ("state is built from the shared engine/fade/wait snapshot and model next cue");
        const auto sound = f.sound(); f.addWait();
        RemoteTestAccess::cart (*f.main, sound, false); f.render();
        f.document().cues.setPlayheadIndex (1); f.go (true); f.go (false);
        auto snapshot = remote::PlaybackSnapshot::capture (f.engine, f.controller());
        const auto state = model.state (snapshot, false);
        expectEquals (state["playing"].size(), 1); expectEquals (state["waits"].size(), 1);
        expectEquals (static_cast<int> (state["counts"]["playing"]), 1);
        expectEquals (state["playing"][0]["id"].toString(), sound.toString());
        expectEquals (state["playing"][0]["st"].toString(), juce::String ("play"));
        expect (static_cast<double> (state["waits"][0]["remain"]) > 0);
    }
private:
    static juce::String layout (const CueList& list)
    {
        juce::String result;
        for (const auto& cue : list.getAll()) result += cue.id.toString() + ":" + cue.parentId.toString() + ";";
        return result;
    }
};

namespace
{
struct SocketClient
{
    RemoteClient client;
    std::vector<juce::var> inbox;
    juce::var take (Fixture& f, const char* type, int id = 0, int timeout = 3000)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeout;
        do
        {
            f.publish();
            for (const auto& text : client.take()) inbox.push_back (juce::JSON::parse (juce::String::fromUTF8 (text.c_str())));
            for (auto i = inbox.begin(); i != inbox.end(); ++i)
                if ((*i)["t"].toString() == type && (id == 0 || static_cast<int> ((*i)["id"]) == id))
                { auto result = *i; inbox.erase (i); return result; }
            juce::Thread::sleep (2);
        } while (juce::Time::getMillisecondCounterHiRes() < deadline);
        return {};
    }
    void send (const juce::var& value) { client.send (encode (value).toStdString()); }
};
}
class RemoteServerTests : public juce::UnitTest
{
public:
    RemoteServerTests() : UnitTest ("Remote real loopback server", "Enqueue") {}
    void runTest() override
    {
        beginTest ("loopback service serves embedded assets with MIME/no-store and returns 404 elsewhere");
        Fixture f; f.waits(); f.publish();
        f.service().setEnabled (true);
        expect (f.service().getPort() >= 49820 && f.service().getPort() <= 49829);
        expect (f.service().address().startsWith ("http://127.0.0.1:"));
        if (f.service().getPort() == 0) return;
        for (const auto& asset : std::vector<std::pair<const char*, const char*>> {
            { "/", "text/html" }, { "/app.js", "text/javascript" }, { "/app.css", "text/css" }, { "/icons/x-bold.svg", "image/svg+xml" } })
        {
            int status = 0;
            const juce::String result (RemoteClient::http (f.service().getPort(), asset.first, status));
            expectEquals (status, 200, asset.first); expect (result.contains ("Cache-Control: no-store")); expect (result.contains (asset.second));
            expect (result.length() > 100);
        }
        for (const auto* path : { "/missing", "/index.html", "/CMakeLists.txt", "/icons/missing.svg" })
        { int status = 0; RemoteClient::http (f.service().getPort(), path, status); expectEquals (status, 404, path); }
        beginTest ("real CivetWeb websocket receives hello/lists/state, then executes GO once and updates device name");
        SocketClient first;
        expect (first.client.connect (f.service().getPort(), f.service().key().toStdString()), first.client.error);
        const auto hello = first.take (f, "hello");
        expectEquals (static_cast<int> (hello["proto"]), 1);
        expectEquals (hello["server"].toString(), f.service().installId());
        expect (hello["pc"].toString().isNotEmpty() && hello["app"].toString().isNotEmpty());
        const auto lists = first.take (f, "lists"); expectEquals (lists["tabs"].size(), 1); expectEquals (lists["cues"][juce::Identifier (f.tab())].size(), 6);
        const auto state = first.take (f, "state"); expect (state["seq"].isInt() || state["seq"].isInt64());
        first.send (object ({ { "t", "hi" }, { "id", 1 }, { "device", ko ("iPad · 사파리") } }));
        expect (ok (first.take (f, "ack", 1))); expectEquals (static_cast<int> (f.service().devices().size()), 1);
        if (! f.service().devices().empty()) expectEquals (f.service().devices().begin()->second.name, ko ("iPad · 사파리"));
        auto down = object ({ { "t", "go" }, { "id", 2 }, { "touch", 1 }, { "down", true } });
        first.send (down); expect (ok (first.take (f, "ack", 2))); expectEquals (f.waiting(), 1);
        first.send (down); expect (ok (first.take (f, "ack", 2))); expectEquals (f.waiting(), 1);
        first.send (object ({ { "t", "go" }, { "id", 3 }, { "touch", 1 }, { "down", false } }));
        expect (ok (first.take (f, "ack", 3))); expectEquals (f.waiting(), 1);
        first.client.send ("not json"); expect (! ok (first.take (f, "ack")));
        first.send (object ({ { "t", "unknown" }, { "id", 4 } })); expect (! ok (first.take (f, "ack", 4)));
        first.send (object ({ { "t", "ping" }, { "id", 5 } })); expect (ok (first.take (f, "ack", 5)));

        beginTest ("wrong key gets bye key; rotation gets bye newkey, retains port/install id and refuses the old key");
        SocketClient wrong;
        expect (wrong.client.connect (f.service().getPort(), "wrong"), wrong.client.error);
        expectEquals (wrong.take (f, "bye")["reason"].toString(), juce::String ("key")); wrong.client.close();
        const auto previousKey = f.service().key(), server = f.service().installId(); const auto port = f.service().getPort();
        f.service().renewKey();
        expectEquals (first.take (f, "bye")["reason"].toString(), juce::String ("newkey")); first.client.close();
        expect (f.service().key() != previousKey); expectEquals (f.service().key().length(), 43);
        expectEquals (f.service().getPort(), port); expectEquals (f.service().installId(), server);
        SocketClient old;
        expect (old.client.connect (f.service().getPort(), previousKey.toStdString()), old.client.error);
        expectEquals (old.take (f, "bye")["reason"].toString(), juce::String ("key")); old.client.close();

        beginTest ("socket disconnect releases GO; a different live connection keeps its own hold");
        SocketClient a, b;
        f.document().settings.requireKeyUp = true;
        expect (a.client.connect (f.service().getPort(), f.service().key().toStdString()), a.client.error);
        expect (b.client.connect (f.service().getPort(), f.service().key().toStdString()), b.client.error);
        expect (a.take (f, "hello").isObject()); expect (b.take (f, "hello").isObject());
        for (auto* c : { &a, &b })
        { c->send (object ({ { "t", "go" }, { "id", 1 }, { "touch", 1 }, { "down", true } })); expect (ok (c->take (f, "ack", 1))); }
        a.client.close();
        const auto disconnectDeadline = juce::Time::getMillisecondCounterHiRes() + 3000;
        while (f.service().devices().size() > 1 && juce::Time::getMillisecondCounterHiRes() < disconnectDeadline) { f.publish(); juce::Thread::sleep (2); }
        expectEquals (static_cast<int> (f.service().devices().size()), 1); expect (f.main->getShortcutService().activations().anyHeld());
        f.service().setEnabled (false);
        expectEquals (b.take (f, "bye")["reason"].toString(), juce::String ("off")); b.client.close();
        expect (! f.main->getShortcutService().activations().anyHeld()); expect (f.service().devices().empty()); expect (f.service().address().isEmpty());

        beginTest ("saved enabled/key/install id survive, safe mode stays off without clearing the preference");
        f.settings.setRemoteEnabled (true);
        f.service().restoreEnabled (true); expect (! f.service().isEnabled()); expect (f.settings.getRemoteEnabled());
        f.service().restoreEnabled (false); expect (f.service().isEnabled()); expect (f.service().getPort() > 0);
        expectEquals (f.service().key(), f.settings.getRemoteKey()); expectEquals (f.service().installId(), f.settings.getRemoteInstallId());
        f.service().setEnabled (false);
        {
            remote::Service reopened (f.settings, f.document(), { {}, {}, [] { return false; } });
            expectEquals (reopened.key(), f.service().key()); expectEquals (reopened.installId(), server);
        }
        beginTest ("busy ports advance through the ten-port range; unavailable ports are reflected in the panel");
        std::vector<std::unique_ptr<juce::StreamingSocket>> occupied;
        for (int p = 49820; p <= 49829; ++p)
        {
            auto listener = std::make_unique<juce::StreamingSocket>();
            expect (listener->createListener (p, "127.0.0.1"), juce::String (p));
            occupied.push_back (std::move (listener));
        }
        f.service().setEnabled (true); expect (f.service().failedToOpenPort()); expectEquals (f.service().getPort(), 0);
        occupied[1].reset(); f.service().setEnabled (true); expectEquals (f.service().getPort(), 49821);
        f.service().setEnabled (false);
    }
};

class RemotePanelTests : public juce::UnitTest
{
public:
    RemotePanelTests() : UnitTest ("Remote PC panel layout", "Enqueue") {}
    void runTest() override
    {
        Fixture f;
        remote::Panel panel (f.service(), [] {}, [] {});
        panel.setLookAndFeel (&f.theme);
        beginTest ("remote menu after help and stable command ids");
        const auto names = f.main->getMenuBarNames();
        expectEquals (names[names.size() - 2], ko ("도움말")); expectEquals (names[names.size() - 1], ko ("외부기기 연동"));
        for (const auto* id : { "remote.toggle", "remote.show", "remote.newKey" }) expect (ShortcutCatalog::get().find (id) != nullptr);
        f.main->setSize (860, 608);
        expect (RemoteTestAccess::menuFits (*f.main), "menu fits beside the mode toggle at the main window minimum width");
        for (const auto& size : { juce::Point<int> (720, 448), juce::Point<int> (820, 470), juce::Point<int> (1050, 600) })
        {
            beginTest ("horizontal panel controls stay inside and do not overlap: " + size.toString());
            panel.setSize (size.x, size.y);
            for (auto* child : panel.getChildren())
            {
                expect (panel.getLocalBounds().contains (child->getBounds()));
                expect (child->getHeight() >= 20 && child->getWidth() >= 60);
                for (auto* other : panel.getChildren())
                { if (other == child) break; expect (! child->getBounds().intersects (other->getBounds())); }
            }
        }
        beginTest ("off address empty, on address readonly mono and QR can be rendered at 100/125/150 percent without a desktop window");
        panel.setSize (820, 470);
        juce::TextEditor* address = nullptr;
        juce::ListBox* deviceList = nullptr;
        for (auto* child : panel.getChildren()) if (auto* text = dynamic_cast<juce::TextEditor*> (child)) address = text;
        for (auto* child : panel.getChildren()) if (auto* list = dynamic_cast<juce::ListBox*> (child)) deviceList = list;
        expect (address != nullptr);
        expect (deviceList != nullptr);
        if (address != nullptr) { expect (address->isReadOnly()); expect (address->getText().isEmpty()); }
        std::vector<std::unique_ptr<SocketClient>> devices;
        for (bool enabled : { false, true })
        {
            f.service().setEnabled (enabled);
            if (address != nullptr) expectEquals (address->getText(), f.service().address());
            if (enabled && f.service().getPort() > 0)
            {
                beginTest ("an open panel follows hi/disconnect, scrolls long lists and keeps full long names in tooltips");
                for (int i = 0; i < 6; ++i)
                {
                    auto client = std::make_unique<SocketClient>();
                    expect (client->client.connect (f.service().getPort(), f.service().key().toStdString()), client->client.error);
                    expect (client->take (f, "hello").isObject());
                    const auto deviceName = i == 0 ? ko ("공연장 음향 조정실의 긴 기기 이름 iPad Pro · 사파리") : ko ("iPhone · 사파리 ") + juce::String (i);
                    client->send (object ({ { "t", "hi" }, { "id", 1 }, { "device", deviceName } }));
                    expect (ok (client->take (f, "ack", 1)));
                    devices.push_back (std::move (client));
                }
                if (deviceList != nullptr)
                {
                    expectEquals (deviceList->getListBoxModel()->getNumRows(), 6);
                    expect (deviceList->getListBoxModel()->getTooltipForRow (0).startsWith (ko ("공연장 음향 조정실의 긴 기기 이름")));
                    deviceList->scrollToEnsureRowIsOnscreen (5);
                    expect (deviceList->getViewport()->getViewPositionY() > 0);
                    deviceList->scrollToEnsureRowIsOnscreen (0);
                }
            }
            for (float scale : { 1.0f, 1.25f, 1.5f })
            {
                auto image = panel.createComponentSnapshot (panel.getLocalBounds(), true, scale);
                expect (image.isValid()); expectEquals (image.getWidth(), juce::roundToInt (820 * scale));
                const auto output = juce::SystemStats::getEnvironmentVariable ("ENQUEUE_REMOTE_REVIEW_DIR", {});
                if (output.isNotEmpty())
                {
                    auto folder = juce::File (output); folder.createDirectory();
                    auto stream = folder.getChildFile (juce::String (enabled ? "on-" : "off-") + juce::String (juce::roundToInt (scale * 100)) + ".png").createOutputStream();
                    if (stream != nullptr) { stream->setPosition (0); stream->truncate(); juce::PNGImageFormat().writeImageToStream (image, *stream); }
                }
            }
        }
        f.service().setEnabled (false);
        if (deviceList != nullptr) expectEquals (deviceList->getListBoxModel()->getNumRows(), 0);
        if (address != nullptr) expect (address->getText().isEmpty());
        for (auto& client : devices) client->client.close();
        panel.setLookAndFeel (nullptr);
    }
};

static RemoteProtocolTests remoteProtocolTests;
static RemoteInputTests remoteInputTests;
static RemoteMoveTests remoteMoveTests;
static RemoteServerTests remoteServerTests;
static RemotePanelTests remotePanelTests;
}
