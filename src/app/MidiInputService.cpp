#include "app/MidiInputService.h"
#include "app/Commands.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue
{
namespace
{
struct NativeMidiHandle final : MidiInputService::Handle
{
    explicit NativeMidiHandle (std::unique_ptr<juce::MidiInput> p) : input (std::move (p)) {}
    void start() override { input->start(); }
    void stop() override { input->stop(); }
    std::unique_ptr<juce::MidiInput> input;
};
}
struct MidiInputService::Port final : juce::MidiInputCallback
{
    Port (MidiInputService& s, juce::String i, uint64_t token) : service (s), identifier (std::move (i)), input (token)
    { for (auto& word : panicAddresses) word.store (0); }
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message) override { service.accept (*this, message); }
    MidiInputService& service;
    juce::String identifier, name;
    const uint64_t input;
    uint64_t connection = 0, observedFault = 0, observedPanicFault = 0;
    std::atomic<bool> enabled { false };
    std::atomic<uint64_t> faultEpoch { 0 }, inFlight { 0 };
    std::atomic<uint64_t> panicFaultEpoch { 0 };
    // 2 kinds * 16 channels * 128 numbers, atomically published words. Mapping
    // boundaries invalidate routing before a new table is installed.
    std::array<std::atomic<uint64_t>, 64> panicAddresses;
    std::unique_ptr<MidiEventQueue<8704, 512>> queue;
    std::unique_ptr<Handle> handle;
    Status status = Status::disconnected;
    bool staleObserved = false;
};
struct MidiInputService::Notifier final : juce::Thread
{
    explicit Notifier (MidiInputService& s) : Thread ("MIDI input notification"), service (s) { startThread(); }
    ~Notifier() override { stopThread (-1); }
    void run() override
    {
       #if JUCE_WINDOWS
        // High-resolution waitable timer avoids raising the process/system timer
        // period. Only this worker touches an OS wait/message-posting API.
        HANDLE timer = CreateWaitableTimerExW (nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
       #endif
        while (! threadShouldExit())
        {
            if (service.hasPending() || service.signals->deviceChange.load (std::memory_order_acquire))
                service.triggerAsyncUpdate();
           #if JUCE_WINDOWS
            if (timer != nullptr)
            {
                LARGE_INTEGER due {};
                due.QuadPart = -10000;
                if (SetWaitableTimer (timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject (timer, 10);
                else wait (1);
            }
            else
           #endif
                wait (1);
        }
       #if JUCE_WINDOWS
        if (timer != nullptr) CloseHandle (timer);
       #endif
    }
    MidiInputService& service;
};

MidiInputService::MidiInputService (ShortcutService& s, Callbacks c, Backend b, bool runNotifier)
    : shortcuts (s), callbacks (std::move (c)), backend (std::move (b))
{
    if (! backend.enumerate) backend.enumerate = []
    {
        std::vector<juce::MidiDeviceInfo> result;
        for (const auto& device : juce::MidiInput::getAvailableDevices()) result.push_back (device);
        return result;
    };
    if (! backend.open) backend.open = [] (const juce::String& id, juce::MidiInputCallback* callback) -> std::unique_ptr<Handle>
    {
        auto input = juce::MidiInput::openDevice (id, callback);
        if (! input) return {};
        return std::make_unique<NativeMidiHandle> (std::move (input));
    };
    routing.store (shortcuts.getInputGeneration());
    shortcuts.addListener (this);
    if (backend.nativeNotifications)
    {
        const auto shared = signals;
        deviceConnection = juce::MidiDeviceListConnection::make ([shared] { shared->deviceChange.store (true, std::memory_order_release); });
    }
    refresh();
    if (runNotifier) notifier = std::make_unique<Notifier> (*this);
}
MidiInputService::~MidiInputService() { shutdown(); }
void MidiInputService::deviceListChanged() noexcept { signals->deviceChange.store (true, std::memory_order_release); }
void MidiInputService::closePort (Port& port)
{
    port.enabled.store (false, std::memory_order_release);
    if (port.handle)
    {
        port.handle->stop();
        port.handle.reset(); // driver destruction also joins any driver-owned callback
        while (port.inFlight.load (std::memory_order_acquire) != 0) juce::Thread::sleep (1);
        // The sole producer has stopped. Remove its backlog before reconnecting
        // so old packets cannot consume the next connection's capacity.
        MidiInputEvent ignored;
        while (port.queue->pop (ignored)) queued.fetch_sub (1, std::memory_order_release);
        port.queue.reset();
        if (callbacks.connection) callbacks.connection (port.input, port.connection, false);
    }
}
void MidiInputService::shutdown()
{
    if (stopped) return;
    stopped = true;
    signals->stopped.store (true);
    accepting.store (false, std::memory_order_release);
    deviceConnection = {};
    for (auto& [id, port] : ports) { juce::ignoreUnused (id); closePort (*port); }
    notifier.reset();
    cancelPendingUpdate();
    shortcuts.removeListener (this);
    callbacks = {};
    ports.clear();
    signals->faultPending.store (false, std::memory_order_release);
}
void MidiInputService::refresh()
{
    if (stopped) return;
    signals->deviceChange.store (false, std::memory_order_release);
    available = backend.enumerate();
    const auto& settings = shortcuts.getMidiInputSettings();
    std::map<juce::String, juce::String> present;
    for (const auto& d : available) if (d.identifier.isNotEmpty()) present.emplace (d.identifier, d.name);
    for (const auto& [id, name] : settings.selected)
        if (ports.count (id) == 0) { auto p = std::make_unique<Port> (*this, id, ++nextInput); p->name = name; ports[id] = std::move (p); }
    for (const auto& [id, name] : present)
    {
        if (ports.count (id) == 0) ports[id] = std::make_unique<Port> (*this, id, ++nextInput);
        ports[id]->name = name;
    }
    shortcuts.setAvailableMidiDevices (present);
    for (auto& [id, p] : ports)
    {
        const bool selected = settings.autoUseAll || settings.selected.count (id) != 0;
        if (present.count (id) == 0 || ! selected)
        {
            closePort (*p);
            p->status = present.count (id) == 0 ? Status::disconnected : Status::notSelected;
            continue;
        }
        if (p->handle) continue; // notifications never duplicate an open connection
        p->connection = ++nextConnection;
        p->faultEpoch.store (0);
        p->panicFaultEpoch.store (0);
        p->observedFault = 0;
        p->observedPanicFault = 0;
        p->staleObserved = false;
        p->queue = std::make_unique<MidiEventQueue<8704, 512>>();
        p->handle = backend.open (id, p.get());
        if (! p->handle) { p->queue.reset(); p->status = Status::unavailable; continue; }
        p->status = Status::connected;
        rebuildPanicAddresses();
        if (callbacks.connection) callbacks.connection (p->input, p->connection, true);
        p->enabled.store (true, std::memory_order_release);
        p->handle->start();
    }
    if (callbacks.devicesChanged) callbacks.devicesChanged();
}
void MidiInputService::rebuildPanicAddresses()
{
    const auto* panic = ShortcutCatalog::get().find (CommandIDs::panicAll);
    for (auto& [id, port] : ports)
    {
        std::array<uint64_t, 64> table {};
        if (panic != nullptr)
            for (const auto& t : shortcuts.getMidiTriggers (panic->id))
                if (t.source == "any" || t.source == id)
                    for (int channel = 1; channel <= 16; ++channel)
                        if (t.channel == 0 || channel == t.channel)
                        {
                            const auto address = static_cast<size_t> ((t.kind == MidiTrigger::Kind::cc ? 2048 : 0) + (channel - 1) * 128 + t.number);
                            table[address / 64] |= uint64_t (1) << (address % 64);
                        }
        for (size_t i = 0; i < table.size(); ++i) port->panicAddresses[i].store (table[i], std::memory_order_release);
    }
}
void MidiInputService::shortcutsChanged()
{
    routing.store (0, std::memory_order_release); // a concurrent observation cannot acquire a partially updated address table
    rebuildPanicAddresses();
    routing.store (shortcuts.getInputGeneration(), std::memory_order_release);
}
void MidiInputService::captureStateChanged() { routing.store (shortcuts.getInputGeneration(), std::memory_order_release); }
void MidiInputService::midiInputSettingsChanged() { refresh(); }
void MidiInputService::accept (Port& port, const juce::MidiMessage& message) noexcept
{
    port.inFlight.fetch_add (1, std::memory_order_acquire);
    if (accepting.load (std::memory_order_acquire) && port.enabled.load (std::memory_order_acquire))
    {
        received.fetch_add (1, std::memory_order_relaxed);
        MidiInputEvent event;
        if (MidiInputEvent::copyMessage (message, event))
        {
            event.input = port.input;
            event.connection = port.connection;
            event.routing = routing.load (std::memory_order_acquire);
            event.observedTimeMs = backend.clockMs ? backend.clockMs() : juce::Time::getMillisecondCounterHiRes();
            event.eventID = InputInvocation::nextEventID();
            const auto address = static_cast<size_t> (event.token().control);
            event.panicReserved = (port.panicAddresses[address / 64].load (std::memory_order_acquire) & (uint64_t (1) << (address % 64))) != 0;
            event.ordinaryFaultEpoch = port.faultEpoch.load (std::memory_order_acquire);
            event.faultEpoch = event.panicReserved ? port.panicFaultEpoch.load (std::memory_order_acquire) : event.ordinaryFaultEpoch;
            queued.fetch_add (1, std::memory_order_relaxed);
            if (! port.queue->push (event, event.panicReserved))
            {
                queued.fetch_sub (1, std::memory_order_release);
                (event.panicReserved ? panicDropped : dropped).fetch_add (1, std::memory_order_relaxed);
                // Reserved addresses may also drive ordinary commands on the
                // opposite CC edge. Discard their pre-loss ordinary state too.
                port.faultEpoch.fetch_add (1, std::memory_order_release);
                if (event.panicReserved) port.panicFaultEpoch.fetch_add (1, std::memory_order_release);
                signals->faultPending.store (true, std::memory_order_release);
            }
        }
        else unsupported.fetch_add (1, std::memory_order_relaxed);
    }
    port.inFlight.fetch_sub (1, std::memory_order_release);
}
MidiInputService::FaultEpochs MidiInputService::deliverFaults (Port& port)
{
    // Read panic first: its loss also publishes the preceding ordinary epoch.
    const auto panic = port.panicFaultEpoch.load (std::memory_order_acquire);
    const auto ordinary = port.faultEpoch.load (std::memory_order_acquire);
    const bool ordinaryChanged = ordinary != port.observedFault, panicChanged = panic != port.observedPanicFault;
    if (! ordinaryChanged && ! panicChanged) return { ordinary, panic };
    port.observedFault = ordinary;
    port.observedPanicFault = panic;
    port.status = Status::waiting;
    const auto input = port.input;
    const auto lifetime = signals;
    const auto fault = callbacks.fault;
    if (ordinaryChanged && fault) fault (input, false);
    if (! lifetime->stopped.load() && panicChanged && fault) fault (input, true);
    return { ordinary, panic };
}
MidiInputService::Port* MidiInputService::popNext (MidiInputEvent& event)
{
    auto next = ports.upper_bound (lastDrainedPort);
    for (size_t checked = 0; checked < ports.size(); ++checked, ++next)
    {
        if (next == ports.end()) next = ports.begin();
        auto& port = *next->second;
        if (port.queue && port.queue->pop (event))
        {
            queued.fetch_sub (1, std::memory_order_release);
            lastDrainedPort = next->first;
            return &port;
        }
    }
    return nullptr;
}
void MidiInputService::drain (double nowMs)
{
    if (stopped) return;
    const auto lifetime = signals;
    if (signals->deviceChange.load (std::memory_order_acquire)) refresh();
    const auto start = juce::Time::getMillisecondCounterHiRes();
    // A lost release may be the last packet a port ever sends. Deliver its fault
    // independently of dequeue; a concurrent loss leaves the flag set for the
    // next notifier turn even if the last queued packet is consumed below.
    if (signals->faultPending.exchange (false, std::memory_order_acq_rel))
        for (auto& [id, port] : ports)
        {
            juce::ignoreUnused (id);
            if (port->enabled.load (std::memory_order_acquire)) deliverFaults (*port);
            if (lifetime->stopped.load()) return;
        }
    const bool realtime = nowMs < 0.0;
    if (nowMs < 0.0) nowMs = start;
    MidiInputEvent event;
    for (int count = 0; count < 256; ++count)
    {
        auto* port = popNext (event);
        if (port == nullptr) break;
        if (! port->enabled.load() || port->connection != event.connection) continue;
        // Also catch losses published during this bounded drain before routing
        // any post-loss packet from the same port.
        const auto faults = deliverFaults (*port);
        if (lifetime->stopped.load()) return;
        const bool currentFaultEpoch = event.faultEpoch == (event.panicReserved ? faults.panic : faults.ordinary);
        bool execute = currentFaultEpoch;
        event.ordinaryStateValid = event.ordinaryFaultEpoch == faults.ordinary;
        event.ordinaryAllowed = event.ordinaryStateValid;
        if ((realtime ? juce::Time::getMillisecondCounterHiRes() : nowMs) - event.observedTimeMs > 100.0)
        {
            ++stale;
            event.ordinaryAllowed = false;
            port->staleObserved = true;
            if (! event.panicReserved) execute = false;
            port->status = Status::waiting;
        }
        // An off preceding a lost packet cannot prove the input is now released.
        // Unlike a routing boundary, a loss boundary discards that old state too.
        if (! currentFaultEpoch) { ++delivered; continue; }
        if (callbacks.receive)
        {
            auto receive = callbacks.receive;
            receive (event, port->identifier, execute);
            if (lifetime->stopped.load()) return; // a command may have closed the application
        }
        // Connection health is independent of mapping/CC baseline readiness.
        // Only a fresh observation after the loss epoch clears the port warning.
        if (event.ordinaryStateValid && event.ordinaryAllowed && port->enabled.load() && port->connection == event.connection)
            port->status = Status::connected;
        ++delivered;
        if (juce::Time::getMillisecondCounterHiRes() - start >= 2.0) break;
    }
}
void MidiInputService::handleAsyncUpdate() { drain(); }
bool MidiInputService::hasPending() const noexcept { return queued.load (std::memory_order_acquire) != 0 || signals->faultPending.load (std::memory_order_acquire); }
std::vector<MidiInputService::Device> MidiInputService::devices() const
{
    std::vector<Device> result;
    for (const auto& [id, p] : ports) result.push_back ({ id, p->name, p->status, p->input, p->connection,
        p->staleObserved || p->faultEpoch.load() != 0 || p->panicFaultEpoch.load() != 0 });
    return result;
}
MidiInputService::Counters MidiInputService::counters() const noexcept
{
    const auto ordinaryLoss = dropped.load(), panicLoss = panicDropped.load();
    return { received.load(), delivered.load(), ordinaryLoss, panicLoss, stale.load(), unsupported.load(), ordinaryLoss + panicLoss, 0 };
}
juce::MidiInputCallback* MidiInputService::callbackFor (const juce::String& id) const
{
    const auto found = ports.find (id);
    return found != ports.end() && found->second->handle ? found->second.get() : nullptr;
}
}
