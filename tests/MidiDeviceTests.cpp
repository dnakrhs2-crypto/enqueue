#include "MidiTestHarness.h"
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
        expect (input.devices()[0].status == MidiInputService::Status::waiting); expectEquals (backend.ports["A"].opens, 2);
        for (int i = 0; i < 10; ++i) input.refresh(); expectEquals (backend.ports["A"].opens, 2);
        expectEquals (backend.ports["B"].opens, 0);
        h.service->setMidiTriggers ("transport.preview", { note (60, 1, "A") });
        backend.send ("A", on()); drain (input, backend.now); expectEquals (h.downs (CommandIDs::preview), 0);
        backend.send ("A", off()); backend.now = 1025; backend.send ("A", on()); drain (input, backend.now);
        expectEquals (h.downs (CommandIDs::preview), 1); expect (input.devices()[0].status == MidiInputService::Status::connected);

        beginTest ("disconnect retains selection/mapping; changed identifier never replaces by name; reconnect starts a new generation");
        const auto oldConnection = input.devices()[0].connection;
        backend.send ("A", off()); backend.send ("A", on()); // queued before disappearing
        backend.list = { { "Same name", "B" }, { "Same name", "renamed-id" } }; input.refresh();
        drain (input, backend.now); expectEquals (h.downs (CommandIDs::preview), 1);
        expect (h.service->getMidiInputSettings().selected.count ("A") == 1); expect (! h.service->getMidiTriggers ("transport.preview").empty());
        expectEquals (backend.ports["renamed-id"].opens, 0); expect (input.devices()[0].status == MidiInputService::Status::disconnected);
        backend.list.push_back ({ "New display name", "A" }); input.refresh();
        expect (input.devices()[0].connection != oldConnection); expectEquals (backend.ports["A"].opens, 3);
        backend.send ("A", on()); drain (input, backend.now); expectEquals (h.downs (CommandIDs::preview), 1);
        backend.send ("A", off()); backend.now = 1050; backend.send ("A", on()); drain (input, backend.now);
        expectEquals (h.downs (CommandIDs::preview), 2);
        selected.autoUseAll = true; h.service->setMidiInputSettings (selected);
        expectEquals (backend.ports["B"].opens, 1); expectEquals (backend.ports["renamed-id"].opens, 1);
        backend.list.push_back ({ "New", "C" }); input.deviceListChanged(); input.drain(); expectEquals (backend.ports["C"].opens, 1);
        h.service->setEditingLocked (true); backend.list.pop_back(); input.refresh(); backend.list.push_back ({ "New", "C" }); input.refresh();
        expectEquals (backend.ports["C"].opens, 2); // reconnect is permitted in show mode
        h.service->setEditingLocked (false);

        beginTest ("disabled input invalidates queued packets; shutdown closes every input once and late notifications cannot reopen");
        backend.send ("A", off()); backend.now = 1100; backend.send ("A", on());
        selected.autoUseAll = false; selected.selected.clear(); h.service->setMidiInputSettings (selected);
        drain (input, backend.now); expectEquals (h.downs (CommandIDs::preview), 2);
        input.shutdown(); input.deviceListChanged(); input.drain(); input.refresh(); expect (input.devices().empty());
        expectEquals (backend.ports["A"].stops, 2); expectEquals (backend.ports["B"].stops, 1);
        testOverload();
        testQueue();
        testCaptureRace();
        testShutdown();
    }
private:
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
        expectEquals (h.downs (CommandIDs::go), 0); // old pre-loss offs cannot prepare a still-held input
        backend.send ("A", off (61)); backend.now = 1525; backend.send ("A", on (61)); drain (input, 6000);
        expectEquals (static_cast<int> (h.panics.size()), 2); expect (h.panics.back()); // inclusive 500ms observation gap despite delayed execution
        backend.now = 7000; backend.send ("A", off()); backend.now = 7025; backend.send ("A", on()); drain (input, backend.now);
        expectEquals (h.downs (CommandIDs::go), 1);

        beginTest ("reserved capacity exhaustion is an explicit fault; unsupported messages never enter command routing");
        for (int i = 0; i < 9000; ++i) backend.send ("A", (i % 2 == 0) ? on (61) : off (61));
        expect (input.counters().panicDropped > 0); drain (input, backend.now);
        const auto before = input.counters().unsupported;
        backend.send ("A", juce::MidiMessage::programChange (1, 2)); backend.send ("A", juce::MidiMessage::midiClock());
        const juce::uint8 bytes[] { 0x7d, 0x01 }; backend.send ("A", juce::MidiMessage::createSysExMessage (bytes, 2));
        expectEquals (static_cast<int> (input.counters().unsupported - before), 3);
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
        beginTest ("MPSC simultaneous producers preserve each port's order with no normal loss/duplication");
        auto queue = std::make_unique<MidiEventQueue<8192, 512>>();
        constexpr int producers = 8, perProducer = 2000;
        std::atomic<int> finished { 0 }, rejected { 0 };
        std::atomic<bool> start { false };
        std::vector<std::thread> threads;
        // Batches keep this test within the declared normal burst capacity.
        std::array<std::atomic<int>, producers> consumed {};
        for (int port = 0; port < producers; ++port)
            threads.emplace_back ([&, port]
            {
                while (! start.load()) std::this_thread::yield();
                for (int i = 0; i < perProducer; ++i)
                {
                    while (i - consumed[static_cast<size_t> (port)].load() > 64) std::this_thread::yield();
                    MidiInputEvent e; e.input = static_cast<uint64_t> (port); e.eventID = static_cast<uint64_t> (i);
                    if (! queue->push (e, false)) ++rejected;
                }
                ++finished;
            });
        start = true;
        int total = 0, orderErrors = 0;
        while (finished.load() < producers || queue->pending())
        {
            MidiInputEvent e;
            if (queue->pop (e))
            {
                auto& expected = consumed[static_cast<size_t> (e.input)];
                if (e.eventID != static_cast<uint64_t> (expected.load())) ++orderErrors;
                ++expected; ++total;
            }
            else std::this_thread::yield();
        }
        for (auto& t : threads) t.join();
        expectEquals (rejected.load(), 0); expectEquals (orderErrors, 0); expectEquals (total, producers * perProducer);
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
        input.reset(); expectEquals (backend.ports["A"].stops, 1); expect (stop.load());
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
    MidiLoadTests() : UnitTest ("MIDI 60-second transport load", "Enqueue") {}
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
        expectEquals (static_cast<int> (counters.received), producerCount * perProducer);
        expectEquals (static_cast<int> (counters.delivered), producerCount * perProducer);
        expectEquals (static_cast<int> (counters.dropped + counters.panicDropped + counters.stale), 0);
        expectEquals (orderErrors, 0); expectEquals (target.count, producerCount * (perProducer - 1)); expectEquals (target.unexpected, 0);
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
