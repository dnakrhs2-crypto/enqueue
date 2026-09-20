#include "MidiTestHarness.h"
#include "app/ShortcutDisplay.h"
#include <thread>
#include <chrono>
#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::tests
{
using namespace midi_test;
class MidiDeviceTests : public juce::UnitTest
{
public:
    MidiDeviceTests() : UnitTest ("MIDI device lifetime and overload", "Enqueue") {}
    void runTest() override
    {
        beginTest ("no devices/default selections; same names/duplicate notifications; failed open and explicit retry");
        Harness h; FakeDevices backend;
        MidiInputService input (*h.service, h.router->inputCallbacks(), backend.backend(), false);
        expect (input.devices().empty());
        backend.list = { { "Same name", "A" }, { "Same name", "B" }, { "Same name", "A" } };
        for (int i = 0; i < 100; ++i) input.deviceListChanged();
        input.drain(); expectEquals (backend.enumerations, 2); expectEquals (static_cast<int> (input.devices().size()), 2);
        expectEquals (backend.ports["A"].opens, 0);
        expect (input.devices()[0].status == MidiInputService::Status::notSelected);
        MidiInputSettings selected; selected.selected["A"] = "Saved name"; backend.ports["A"].fail = true;
        expect (h.service->setMidiInputSettings (selected).wasOk());
        expect (input.devices()[0].status == MidiInputService::Status::unavailable);
        backend.ports["A"].fail = false; input.refresh();
        expect (input.devices()[0].status == MidiInputService::Status::connected); expectEquals (backend.ports["A"].opens, 2);
        for (int i = 0; i < 10; ++i) input.refresh(); expectEquals (backend.ports["A"].opens, 2);
        expectEquals (backend.ports["B"].opens, 0);
        h.service->setMidiTriggers ("transport.preview", { note (60, 1, "A") });
        backend.send ("A", on()); drain (input, backend.now); expectEquals (h.downs (CommandIDs::preview), 1);
        backend.send ("A", off()); backend.now = 1025; backend.send ("A", on()); drain (input, backend.now);
        expectEquals (h.downs (CommandIDs::preview), 2); expect (input.devices()[0].status == MidiInputService::Status::connected);

        beginTest ("disconnect retains selection/mapping; changed identifier never replaces by name; reconnect starts a new generation");
        const auto oldConnection = input.devices()[0].connection;
        backend.send ("A", off()); backend.send ("A", on()); // queued before disappearing
        backend.list = { { "Same name", "B" }, { "Same name", "renamed-id" } }; input.refresh();
        drain (input, backend.now); expectEquals (h.downs (CommandIDs::preview), 2);
        expect (h.service->getMidiInputSettings().selected.count ("A") == 1); expect (! h.service->getMidiTriggers ("transport.preview").empty());
        expectEquals (backend.ports["renamed-id"].opens, 0); expect (input.devices()[0].status == MidiInputService::Status::disconnected);
        backend.list.push_back ({ "New display name", "A" }); input.refresh();
        expect (input.devices()[0].connection != oldConnection); expectEquals (backend.ports["A"].opens, 3);
        backend.send ("A", on()); drain (input, backend.now); expectEquals (h.downs (CommandIDs::preview), 3);
        backend.send ("A", off()); backend.now = 1050; backend.send ("A", on()); drain (input, backend.now);
        expectEquals (h.downs (CommandIDs::preview), 4);
        selected.autoUseAll = true; h.service->setMidiInputSettings (selected);
        expectEquals (backend.ports["B"].opens, 1); expectEquals (backend.ports["renamed-id"].opens, 1);
        backend.list.push_back ({ "New", "C" }); input.deviceListChanged(); input.drain(); expectEquals (backend.ports["C"].opens, 1);
        h.service->setEditingLocked (true); backend.list.pop_back(); input.refresh(); backend.list.push_back ({ "New", "C" }); input.refresh();
        expectEquals (backend.ports["C"].opens, 2); // reconnect is permitted in show mode
        h.service->setEditingLocked (false);

        beginTest ("disabled input invalidates queued packets; shutdown closes every input once and late notifications cannot reopen");
        backend.send ("A", off()); backend.now = 1100; backend.send ("A", on());
        selected.autoUseAll = false; selected.selected.clear(); h.service->setMidiInputSettings (selected);
        drain (input, backend.now); expectEquals (h.downs (CommandIDs::preview), 4);
        input.shutdown(); input.deviceListChanged(); input.drain(); input.refresh(); expect (input.devices().empty());
        expectEquals (backend.ports["A"].stops, 2); expectEquals (backend.ports["B"].stops, 1);
        testOverload();
        testPanicLoss();
        testPanicProjectLoss();
        testPortCapacity();
        testPortIsolation();
        testQueueReconnect();
        testQueue();
        testTransport();
        testCaptureRace();
        testShutdown();
        testConnectionStatus();
    }
private:
    void testConnectionStatus()
    {
        beginTest ("unmapped port connects on open; loss waits only until the next current message, independent of CC readiness");
        Harness h; FakeDevices backend; backend.list = { { "A", "A" } };
        MidiInputSettings selected; selected.autoUseAll = true;
        expect (h.service->setMidiInputSettings (selected).wasOk());
        MidiInputService input (*h.service, h.router->inputCallbacks(), backend.backend(), false);
        expect (input.devices()[0].status == MidiInputService::Status::connected);
        expect (! ShortcutDisplay::midiSummary (input, h.service.get()).contains (juce::String::fromUTF8 ("준비 대기")));
        backend.send ("A", on (64)); drain (input, backend.now);
        expect (input.devices()[0].status == MidiInputService::Status::connected);
        expect (h.service->setMidiTriggers ("transport.preview", { cc() }).wasOk());
        expect (h.router->bindingStatus ({ "transport.preview", cc(), CommandIDs::preview, true }).contains (juce::String::fromUTF8 ("CC 기준값 미수신")));
        for (int i = 0; i < 8193; ++i) backend.send ("A", off (64));
        drain (input, backend.now);
        expect (input.devices()[0].status == MidiInputService::Status::waiting);
        expect (ShortcutDisplay::midiSummary (input, h.service.get()).contains (juce::String::fromUTF8 ("준비 대기 1")));
        backend.send ("A", off (64)); drain (input, backend.now);
        expect (input.devices()[0].status == MidiInputService::Status::connected);
        expect (! ShortcutDisplay::midiSummary (input, h.service.get()).contains (juce::String::fromUTF8 ("준비 대기")));
        expect (h.router->bindingStatus ({ "transport.preview", cc(), CommandIDs::preview, true }).contains (juce::String::fromUTF8 ("CC 기준값 미수신")));
        expectEquals (h.downs (CommandIDs::preview), 0);
    }
    void testPortCapacity()
    {
        beginTest ("each port admits 8192 ordinary plus 512 panic packets; only capacity exhaustion records loss");
        Harness h; FakeDevices backend; backend.list = { { "A", "A" }, { "B", "B" } };
        MidiInputSettings settings; settings.autoUseAll = true;
        expect (h.service->setMidiInputSettings (settings).wasOk());
        expect (h.service->setMidiTriggers ("transport.panicAll", { note (61) }).wasOk());
        MidiInputService input (*h.service, h.router->inputCallbacks(), backend.backend(), false);
        for (int i = 0; i < 8192; ++i) { backend.send ("A", on (90)); backend.send ("B", on (90)); }
        expectEquals (static_cast<int> (input.counters().received), 16384);
        expectEquals (static_cast<int> (input.counters().capacityDropped), 0);
        expect (! input.hasInputFault());
        backend.send ("A", on (90)); backend.send ("B", on (90));
        expectEquals (static_cast<int> (input.counters().dropped), 2);
        for (int i = 0; i < 512; ++i) { backend.send ("A", off (61)); backend.send ("B", off (61)); }
        expectEquals (static_cast<int> (input.counters().panicDropped), 0, "ordinary loss cannot consume either port's reserve");
        expectEquals (static_cast<int> (input.counters().capacityDropped), 2);
        backend.send ("A", on (61)); backend.send ("B", on (61));
        expectEquals (static_cast<int> (input.counters().panicDropped), 2);
        expectEquals (static_cast<int> (input.counters().capacityDropped), 4);
        expectEquals (static_cast<int> (input.counters().contentionDropped), 0);
        expect (input.hasInputFault());
        drain (input, backend.now);
        expectEquals (static_cast<int> (input.counters().delivered), 17408);
        expectEquals (static_cast<int> (input.counters().stale), 0);
        expect (h.panics.empty(), "pre-loss reserved packets cannot rearm the lost Note");
        backend.send ("A", on (90)); backend.send ("B", on (90)); drain (input, backend.now);
        expectEquals (static_cast<int> (input.counters().delivered), 17410);
        expectEquals (static_cast<int> (input.counters().capacityDropped), 4, "reused queue slots admit packets again");
    }
    void testPortIsolation()
    {
        beginTest ("a full B queue cannot drop or starve A's GO release and reserved panic");
        Harness h; h.requireKeyUp = true;
        FakeDevices backend; backend.list = { { "A", "A" }, { "B", "B" } };
        MidiInputSettings settings; settings.autoUseAll = true;
        expect (h.service->setMidiInputSettings (settings).wasOk());
        expect (h.service->setMidiTriggers ("transport.go", { note() }).wasOk());
        expect (h.service->setMidiTriggers ("transport.panicAll", { note (61) }).wasOk());
        MidiInputService input (*h.service, h.router->inputCallbacks(), backend.backend(), false);
        backend.send ("A", on()); drain (input, backend.now);
        expect (h.service->activations().anyHeld());
        for (int i = 0; i < 8192; ++i) backend.send ("B", on (90));
        for (int i = 0; i < 512; ++i) backend.send ("B", off (61));
        backend.send ("B", on (61)); // only B overflows
        backend.send ("A", off()); backend.send ("A", on (61));
        expectEquals (static_cast<int> (input.counters().dropped), 0);
        expectEquals (static_cast<int> (input.counters().panicDropped), 1);
        // Round-robin admission to the consumer reaches A without draining B's
        // entire backlog. The time budget may limit each call to one packet.
        for (int i = 0; i < 4; ++i) input.drain (backend.now);
        expect (! h.service->activations().anyHeld());
        expectEquals (static_cast<int> (h.panics.size()), 1);
        expect (input.hasPending(), "B's full backlog remains after A was serviced");
        drain (input, backend.now);
        expectEquals (static_cast<int> (input.counters().capacityDropped), 1);
        expectEquals (static_cast<int> (input.counters().contentionDropped), 0);
    }
    void testQueueReconnect()
    {
        beginTest ("closing a full port removes only its backlog; reconnect has fresh capacity and notifier accounting");
        Harness h; FakeDevices backend; backend.list = { { "A", "A" }, { "B", "B" } };
        MidiInputSettings settings; settings.autoUseAll = true;
        expect (h.service->setMidiInputSettings (settings).wasOk());
        expect (h.service->setMidiTriggers ("transport.preview", { note() }).wasOk());
        MidiInputService input (*h.service, h.router->inputCallbacks(), backend.backend(), false);
        for (int i = 0; i < 8192; ++i) backend.send ("A", on (90));
        backend.send ("B", on());
        backend.list.erase (backend.list.begin()); input.refresh();
        expect (input.hasPending(), "B's packet survives closing A");
        drain (input, backend.now);
        expectEquals (static_cast<int> (input.counters().delivered), 1);
        expectEquals (h.downs (CommandIDs::preview), 0, "connection changes still invalidate queued routing");
        backend.list.push_back ({ "A", "A" }); input.refresh();
        backend.send ("A", on()); drain (input, backend.now);
        expectEquals (h.downs (CommandIDs::preview), 1, "the new connection's first Note fires");
        expectEquals (static_cast<int> (input.counters().capacityDropped), 0);
        expect (! input.hasPending());
        backend.send ("A", off()); input.shutdown();
        expect (! input.hasPending());
    }
    void testPanicProjectLoss()
    {
        for (bool released : { false, true })
        {
            beginTest (juce::String ("ordinary loss then project replacement preserves panic Note ")
                + (released ? "release from the reserved backlog" : "quarantine without another packet"));
            Harness h; FakeDevices backend; backend.list = { { "A", "A" } };
            MidiInputSettings settings; settings.selected["A"] = "A";
            expect (h.service->setMidiInputSettings (settings).wasOk());
            expect (h.service->setMidiTriggers ("transport.panicAll", { note (61) }).wasOk());
            MidiInputService input (*h.service, h.router->inputCallbacks(), backend.backend(), false);
            backend.send ("A", off (61)); backend.now = 1025; backend.send ("A", on (61)); drain (input, backend.now);
            expectEquals (static_cast<int> (h.panics.size()), 1);
            if (released) backend.send ("A", off (61));
            for (int i = released ? 1 : 0; i < 8192; ++i) backend.send ("A", on (90));
            backend.send ("A", on (90)); // ordinary loss must preserve the reserved Note state
            expectEquals (static_cast<int> (input.counters().dropped), 1);
            expectEquals (static_cast<int> (input.counters().panicDropped), 0);
            drain (input, backend.now);
            h.document.newProject();
            backend.now = 1050; backend.send ("A", on (61)); drain (input, backend.now);
            expectEquals (static_cast<int> (h.panics.size()), released ? 2 : 1,
                "only a physically released panic may fire immediately in the new project");
            backend.send ("A", off (61)); backend.now = 1075; backend.send ("A", on (61)); drain (input, backend.now);
            expectEquals (static_cast<int> (h.panics.size()), released ? 3 : 2);
        }
    }
    void testPanicLoss()
    {
        for (bool pulse : { false, true })
        {
            beginTest (juce::String ("panic reserved loss rebaselines opposite GO ") + (pulse ? "pulse" : "gate") + " and invalidates ordinary holds/backlog");
            Harness h; FakeDevices backend; backend.list = { { "A", "A" } };
            MidiInputSettings settings; settings.selected["A"] = "A";
            expect (h.service->setMidiInputSettings (settings).wasOk());
            expect (h.service->setMidiTriggers ("transport.panicAll", { cc() }).wasOk());
            expect (h.service->setMidiTriggers ("transport.go", { note(), cc (MidiTrigger::Edge::falling,
                pulse ? MidiTrigger::Behavior::pulse : MidiTrigger::Behavior::gate) }).wasOk());
            expect (h.service->setMidiTriggers ("transport.preview", { note (62) }).wasOk());
            MidiInputService input (*h.service, h.router->inputCallbacks(), backend.backend(), false);
            backend.send ("A", control (127)); backend.send ("A", off()); backend.send ("A", off (62)); drain (input, backend.now);
            backend.now = 1025; backend.send ("A", on()); drain (input, backend.now);
            expectEquals (h.downs (CommandIDs::go), 1);
            expect (h.service->activations().anyHeld());
            // Fill the ordinary quota, then all reserved slots. The lost packet
            // is the falling value, which must never be inferred on repetition.
            backend.send ("A", off (62)); backend.send ("A", on (62));
            for (int i = 2; i < 8192; ++i) backend.send ("A", on (90));
            for (int i = 0; i < 512; ++i) backend.send ("A", control (127));
            backend.send ("A", control (0));
            expectEquals (static_cast<int> (input.counters().panicDropped), 1);
            expectEquals (static_cast<int> (input.counters().dropped), 0);
            drain (input, backend.now);
            expect (! h.service->activations().anyHeld(), "lost reserved packets also invalidate GO tokens");
            expectEquals (h.downs (CommandIDs::preview), 0, "ordinary packets preceding the loss cannot rearm inputs");
            for (int i = 0; i < 3; ++i)
            {
                backend.now = 1050.0 + 25.0 * i; backend.send ("A", control (0)); drain (input, backend.now);
                expectEquals (h.downs (CommandIDs::go), 1, "first post-loss value is only a baseline");
            }
            backend.now = 1125; backend.send ("A", control (127)); drain (input, backend.now);
            backend.now = 1150; backend.send ("A", control (0)); drain (input, backend.now);
            expectEquals (h.downs (CommandIDs::go), 2, "a fresh falling edge works after preparation");
            expectEquals (static_cast<int> (h.panics.size()), 1);
        }
    }
    void testOverload()
    {
        beginTest ("normal overflow suppresses queued GO; reserved panic keeps state and ignores 100ms age limit");
        Harness h; FakeDevices backend; backend.list = { { "A", "A" } };
        h.service->setMidiTriggers ("transport.go", { note() }); h.service->setMidiTriggers ("transport.panicAll", { note (61) });
        MidiInputSettings settings; settings.selected["A"] = "A"; h.service->setMidiInputSettings (settings);
        MidiInputService input (*h.service, h.router->inputCallbacks(), backend.backend(), false);
        backend.send ("A", off()); backend.send ("A", off (61)); drain (input, 1000);
        backend.now = 1025;
        for (int i = 0; i < 9000; ++i) backend.send ("A", (i % 2 == 0) ? on() : off());
        backend.send ("A", on (61)); drain (input, 2000);
        expectEquals (h.downs (CommandIDs::go), 0); expectEquals (static_cast<int> (h.panics.size()), 1);
        expect (input.counters().dropped > 0 && input.counters().stale > 0 && input.hasInputFault());
        expectEquals (static_cast<int> (input.counters().panicDropped), 0);
        backend.now = 1300; backend.send ("A", on()); drain (input, backend.now);
        expectEquals (h.downs (CommandIDs::go), 1); // a fresh post-loss Note on needs no preparatory off
        backend.send ("A", off (61)); backend.now = 1525; backend.send ("A", on (61)); drain (input, 6000);
        expectEquals (static_cast<int> (h.panics.size()), 2); expect (h.panics.back()); // inclusive 500ms observation gap despite delayed execution
        backend.now = 7000; backend.send ("A", off()); backend.now = 7025; backend.send ("A", on()); drain (input, backend.now);
        expectEquals (h.downs (CommandIDs::go), 2);

        beginTest ("reserved capacity exhaustion is an explicit fault; unsupported messages never enter command routing");
        for (int i = 0; i < 9000; ++i) backend.send ("A", (i % 2 == 0) ? on (61) : off (61));
        expect (input.counters().panicDropped > 0); drain (input, backend.now);
        const auto before = input.counters().unsupported;
        backend.send ("A", juce::MidiMessage::programChange (1, 2)); backend.send ("A", juce::MidiMessage::midiClock());
        const juce::uint8 bytes[] { 0x7d, 0x01 }; backend.send ("A", juce::MidiMessage::createSysExMessage (bytes, 2));
        expectEquals (static_cast<int> (input.counters().unsupported - before), 3);
        beginTest ("ordinary loss preserves a held panic Note and its 500ms gesture history");
        {
            Harness held; FakeDevices device; device.list = { { "A", "A" } };
            expect (held.service->setMidiInputSettings (settings).wasOk());
            expect (held.service->setMidiTriggers ("transport.panicAll", { note (61) }).wasOk());
            MidiInputService service (*held.service, held.router->inputCallbacks(), device.backend(), false);
            device.send ("A", off (61)); device.now = 1025; device.send ("A", on (61)); drain (service, device.now);
            expectEquals (static_cast<int> (held.panics.size()), 1);
            for (int i = 0; i <= 8192; ++i) device.send ("A", on (90));
            device.now = 1100; device.send ("A", on (61)); drain (service, device.now);
            expectEquals (static_cast<int> (service.counters().dropped), 1);
            expectEquals (static_cast<int> (service.counters().panicDropped), 0);
            expectEquals (static_cast<int> (held.panics.size()), 1, "a repeated held panic must not become a fresh Note after ordinary loss");
            device.send ("A", off (61)); device.now = 1525; device.send ("A", on (61)); drain (service, device.now);
            expectEquals (static_cast<int> (held.panics.size()), 2);
            expect (! held.panics.empty() && held.panics.back(), "ordinary loss must preserve the inclusive 500ms gesture");
        }
        beginTest ("a CC address reserved for rising panic still expires its falling ordinary command after 100ms");
        {
            Harness shared; FakeDevices device; device.list = { { "A", "A" } };
            shared.service->setMidiInputSettings (settings);
            shared.service->setMidiTriggers ("transport.panicAll", { cc() });
            shared.service->setMidiTriggers ("transport.preview", { cc (MidiTrigger::Edge::falling) });
            MidiInputService service (*shared.service, shared.router->inputCallbacks(), device.backend(), false);
            device.send ("A", control (0)); drain (service, 1000);
            device.now = 1025; device.send ("A", control (127)); drain (service, 1025);
            expectEquals (static_cast<int> (shared.panics.size()), 1);
            device.now = 1050; device.send ("A", control (0)); drain (service, 1500);
            expectEquals (shared.downs (CommandIDs::preview), 0);
            device.now = 1550; device.send ("A", control (127)); drain (service, 1550);
            device.now = 1575; device.send ("A", control (0)); drain (service, 1575);
            expectEquals (shared.downs (CommandIDs::preview), 1);
        }
    }
    void testQueue()
    {
        for (int round = 0; round < 32; ++round) testQueueRound (round);
    }
    void testQueueRound (int round)
    {
        beginTest ("simultaneous producers preserve port order without loss, round " + juce::String (round + 1) + "/32");
        constexpr int producers = 32, perProducer = 2000;
        Harness h; FakeDevices backend;
        for (int p = 0; p < producers; ++p) backend.list.push_back ({ "Port " + juce::String (p), "port" + juce::String (p) });
        MidiInputSettings settings; settings.autoUseAll = true;
        expect (h.service->setMidiInputSettings (settings).wasOk());
        std::atomic<int> finished { 0 }, rejected { 0 };
        std::atomic<bool> start { false }, abort { false };
        std::vector<std::thread> threads;
        // At most 65 outstanding packets per port, 2080 total: less than even
        // the old shared queue's ordinary capacity. Any loss is a defect.
        std::array<std::atomic<int>, producers> consumed {};
        std::array<uint64_t, producers> serials {};
        int total = 0, orderErrors = 0;
        MidiInputService::Callbacks callbacks;
        callbacks.receive = [&] (const MidiInputEvent& e, const juce::String& id, bool execute)
        {
            const auto index = static_cast<size_t> (id.substring (4).getIntValue());
            auto& expected = consumed[index];
            if (! execute || e.value != expected.load() % 128 || e.eventID <= serials[index]) ++orderErrors;
            serials[index] = e.eventID;
            ++expected; ++total;
            return true;
        };
        MidiInputService input (*h.service, callbacks, backend.backend(), false);
        for (int port = 0; port < producers; ++port)
        {
            auto* callback = input.callbackFor ("port" + juce::String (port));
            threads.emplace_back ([&, port, callback]
            {
                while (! start.load()) std::this_thread::yield();
                for (int i = 0; i < perProducer && ! abort.load(); ++i)
                {
                    while (i - consumed[static_cast<size_t> (port)].load() > 64 && ! abort.load()) std::this_thread::yield();
                    if (abort.load()) break;
                    callback->handleIncomingMidiMessage (nullptr, control (i % 128));
                }
                ++finished;
            });
        }
        start = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (10);
        bool timedOut = false;
        while (finished.load() < producers || input.hasPending())
        {
            input.drain (backend.now);
            const auto counters = input.counters();
            rejected = static_cast<int> (counters.dropped + counters.panicDropped);
            timedOut |= std::chrono::steady_clock::now() > deadline;
            if (rejected.load() != 0 || timedOut) abort = true;
            if (! input.hasPending()) std::this_thread::yield();
        }
        for (auto& t : threads) t.join();
        if (rejected.load() != 0) logMessage ("capacity was bounded to 2080/8192; callback rejects=" + juce::String (rejected.load()));
        expect (! timedOut, "concurrent callbacks and drain must finish without a stalled port");
        expectEquals (rejected.load(), 0); expectEquals (orderErrors, 0); expectEquals (total, producers * perProducer);
        const auto counters = input.counters();
        expectEquals (static_cast<int> (counters.received), producers * perProducer);
        expectEquals (static_cast<int> (counters.delivered), producers * perProducer);
        expectEquals (static_cast<int> (counters.stale), 0);
        expectEquals (static_cast<int> (counters.capacityDropped), 0);
        expectEquals (static_cast<int> (counters.contentionDropped), 0);
        logMessage ("Concurrent MIDI round " + juce::String (round + 1) + ": received=" + juce::String (counters.received)
            + " delivered=" + juce::String (counters.delivered) + " capacityDropped=" + juce::String (counters.capacityDropped)
            + " contentionDropped=" + juce::String (counters.contentionDropped) + " orderErrors=" + juce::String (orderErrors));
    }
    void testShutdown()
    {
        beginTest ("shutdown during concurrent callbacks joins producers before port destruction and cancels pending updater");
        Harness h; FakeDevices backend; backend.list = { { "A", "A" } };
        MidiInputSettings settings; settings.selected["A"] = "A"; h.service->setMidiInputSettings (settings);
        auto input = std::make_unique<MidiInputService> (*h.service, h.router->inputCallbacks(), backend.backend(), true);
        auto* callback = input->callbackFor ("A");
        std::atomic<bool> stop { false }; std::atomic<int> sent { 0 };
        std::thread producer ([&]
        {
            while (! stop.load()) { callback->handleIncomingMidiMessage (nullptr, on()); ++sent; std::this_thread::yield(); }
        });
        backend.ports["A"].onStop = [&] { stop = true; producer.join(); };
        while (sent.load() < 100) std::this_thread::yield();
        input->shutdown(); expect (! input->hasPending());
        input.reset(); expectEquals (backend.ports["A"].stops, 1); expect (stop.load());
    }
    void testTransport()
    {
        beginTest ("short deterministic eight-port callback/router transport preserves order and executes once per edge");
        Harness h; FakeDevices backend;
        constexpr int portCount = 8, perPort = 128;
        for (int p = 0; p < portCount; ++p) backend.list.push_back ({ "Port " + juce::String (p), "port" + juce::String (p) });
        auto trigger = cc (MidiTrigger::Edge::both, MidiTrigger::Behavior::pulse); trigger.debounceMs = 0;
        expect (h.service->setMidiTriggers ("transport.preview", { trigger }).wasOk());
        MidiInputSettings settings; settings.autoUseAll = true;
        expect (h.service->setMidiInputSettings (settings).wasOk());
        auto callbacks = h.router->inputCallbacks();
        const auto receive = callbacks.receive;
        std::array<int, portCount> received {};
        std::array<uint64_t, portCount> serials {};
        int orderErrors = 0;
        callbacks.receive = [&] (const MidiInputEvent& e, const juce::String& id, bool execute)
        {
            const auto index = static_cast<size_t> (id.substring (4).getIntValue());
            if (e.value != (received[index] % 2 == 0 ? 0 : 127) || e.eventID <= serials[index]) ++orderErrors;
            serials[index] = e.eventID; ++received[index];
            return receive (e, id, execute);
        };
        MidiInputService input (*h.service, callbacks, backend.backend(), false);
        for (int i = 0; i < perPort; ++i)
        {
            backend.now = 1000.0 + 25.0 * i;
            for (int p = 0; p < portCount; ++p) backend.send ("port" + juce::String (p), control (i % 2 == 0 ? 0 : 127));
            drain (input, backend.now);
        }
        const auto counters = input.counters();
        expectEquals (static_cast<int> (counters.received), portCount * perPort);
        expectEquals (static_cast<int> (counters.delivered), portCount * perPort);
        expectEquals (static_cast<int> (counters.dropped + counters.panicDropped + counters.stale), 0);
        expectEquals (orderErrors, 0);
        expectEquals (h.downs (CommandIDs::preview), portCount * (perPort - 1));
        for (int n : received) expectEquals (n, perPort);
    }
    void testCaptureRace()
    {
        beginTest ("capture boundaries concurrent with producer recording never replay into the following routing generation");
        Harness h; FakeDevices backend; backend.list = { { "A", "A" } };
        h.service->setMidiTriggers ("transport.preview", { note() });
        MidiInputSettings settings; settings.selected["A"] = "A"; h.service->setMidiInputSettings (settings);
        MidiInputService input (*h.service, h.router->inputCallbacks(), backend.backend(), false);
        auto* callback = input.callbackFor ("A");
        int token = 0, nextToken = 0;
        for (int round = 0; round < 32; ++round)
        {
            h.service->beginCapture (&token);
            std::atomic<int> sent { 0 };
            std::thread producer ([&]
            {
                for (int i = 0; i < 200; ++i)
                {
                    callback->handleIncomingMidiMessage (nullptr, i % 2 == 0 ? off() : on());
                    ++sent;
                }
            });
            while (sent.load() == 0) std::this_thread::yield();
            h.service->endCapture (&token); h.service->beginCapture (&nextToken);
            producer.join(); h.service->endCapture (&nextToken);
            drain (input, backend.now);
            expectEquals (h.downs (CommandIDs::preview), round);
            backend.send ("A", off()); backend.now = 1100.0 + round * 100.0;
            backend.send ("A", on()); backend.send ("A", off()); drain (input, backend.now);
            expectEquals (h.downs (CommandIDs::preview), round + 1);
        }
        expectEquals (static_cast<int> (input.counters().dropped), 0);
    }
};
static MidiDeviceTests midiDeviceTests;

class MidiLoadTests : public juce::UnitTest
{
public:
    MidiLoadTests() : UnitTest ("MIDI 60-second transport load", "EnqueueMidiLoad") {}
    void runTest() override
    {
        beginTest ("8 producers, 10,000 messages/second for 60 seconds through real notifier/router/command invocation");
        struct Target : shortcut_test::CatalogTarget
        {
            Target() : CatalogTarget (ShortcutCatalog::get()) {}
            bool perform (const InvocationInfo& info) override
            {
                if (info.commandID == CommandIDs::preview && info.isKeyDown) { ++count; if (onInvoke) onInvoke(); }
                else ++unexpected;
                return true;
            }
            int count = 0, unexpected = 0;
            std::function<void()> onInvoke;
        } target;
        juce::ApplicationCommandManager manager; manager.registerAllCommandsForTarget (&target); manager.setFirstCommandTarget (&target);
        ShortcutService shortcuts (manager, {}); shortcuts.setInputStorage ([] (const InputSettingsTransaction&) { return juce::Result::ok(); });
        auto trigger = cc (MidiTrigger::Edge::both, MidiTrigger::Behavior::pulse); trigger.debounceMs = 0;
        expect (shortcuts.setMidiTriggers ("transport.preview", { trigger }).wasOk());
        MidiInputSettings settings; settings.autoUseAll = true; expect (shortcuts.setMidiInputSettings (settings).wasOk());
        ProjectDocument document; MidiTriggerRouter router (shortcuts, manager, document, {});
        FakeDevices backend;
        constexpr int producerCount = 8, perProducer = 75000;
        for (int p = 0; p < producerCount; ++p) backend.list.push_back ({ "Port " + juce::String (p), "port" + juce::String (p) });
        std::array<int, producerCount> received {};
        std::array<uint64_t, producerCount> serials {};
        int orderErrors = 0;
        std::vector<double> latencies; latencies.reserve (producerCount * perProducer);
        target.onInvoke = [&] { latencies.push_back (juce::Time::getMillisecondCounterHiRes() - shortcuts.currentInvocation()->observedTimeMs); };
        auto callbacks = router.inputCallbacks();
        auto receive = callbacks.receive;
        callbacks.receive = [&] (const MidiInputEvent& e, const juce::String& id, bool execute)
        {
            const auto index = static_cast<size_t> (id.substring (4).getIntValue());
            if (e.value != (received[index] % 2 == 0 ? 0 : 127) || e.eventID <= serials[index]) ++orderErrors;
            serials[index] = e.eventID; ++received[index];
            return receive (e, id, execute);
        };
        MidiInputService input (shortcuts, callbacks, backend.backend (false), true);
        std::vector<std::thread> producers;
        std::atomic<int> finished { 0 };
        const auto start = std::chrono::steady_clock::now() + std::chrono::milliseconds (25);
        for (int p = 0; p < producerCount; ++p)
        {
            auto* callback = input.callbackFor ("port" + juce::String (p));
            producers.emplace_back ([&, callback]
            {
                for (int i = 0; i < perProducer; ++i)
                {
                    std::this_thread::sleep_until (start + std::chrono::microseconds (static_cast<int64_t> (i) * 800));
                    callback->handleIncomingMidiMessage (nullptr, control (i % 2 == 0 ? 0 : 127));
                }
                ++finished;
            });
        }
        while (finished.load() != producerCount || input.hasPending())
        {
           #if JUCE_WINDOWS
            MSG message {};
            while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage (&message); DispatchMessageW (&message); }
            MsgWaitForMultipleObjectsEx (0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
           #else
            input.drain(); std::this_thread::sleep_for (std::chrono::milliseconds (1));
           #endif
        }
        for (auto& producer : producers) producer.join();
        const auto counters = input.counters(); input.shutdown();
        logMessage ("MIDI load: received=" + juce::String (counters.received) + " delivered=" + juce::String (counters.delivered)
            + " executed=" + juce::String (target.count) + " dropped=" + juce::String (counters.dropped)
            + " panicDropped=" + juce::String (counters.panicDropped) + " stale=" + juce::String (counters.stale)
            + " capacityDropped=" + juce::String (counters.capacityDropped) + " contentionDropped=" + juce::String (counters.contentionDropped)
            + " orderErrors=" + juce::String (orderErrors));
        expectEquals (static_cast<int> (counters.received), producerCount * perProducer);
        expectEquals (static_cast<int> (counters.delivered), producerCount * perProducer);
        expectEquals (static_cast<int> (counters.dropped + counters.panicDropped + counters.stale), 0);
        expectEquals (orderErrors, 0); expectEquals (target.count, producerCount * (perProducer - 1)); expectEquals (target.unexpected, 0);
        expectEquals (static_cast<int> (counters.capacityDropped + counters.contentionDropped), 0);
        for (int n : received) expectEquals (n, perProducer);
        std::sort (latencies.begin(), latencies.end());
        if (! latencies.empty())
        {
            const auto percentile = [&] (double p) { return latencies[static_cast<size_t> (p * static_cast<double> (latencies.size() - 1))]; };
            logMessage ("MIDI callback->execution ms: p50=" + juce::String (percentile (0.5), 3) + " p95=" + juce::String (percentile (0.95), 3)
                + " p99=" + juce::String (percentile (0.99), 3) + " max=" + juce::String (latencies.back(), 3));
            expect (percentile (0.95) <= 10.0, "p95 target is 10ms on this injected host workload");
        }
    }
};
static MidiLoadTests midiLoadTests;
}
