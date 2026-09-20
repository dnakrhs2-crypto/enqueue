#pragma once

#include "app/ShortcutService.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <atomic>

namespace gocue
{
/** Bounded per-port SPSC queue: one serial MIDI callback producer and one message
    thread consumer. Admission has no CAS/retry path and fails only at capacity.
    Normal admission leaves Reserved slots available for that port's panic. */
template <size_t Capacity, size_t Reserved> class MidiEventQueue
{
public:
    bool push (const MidiInputEvent& event, bool panic) noexcept
    {
        const auto position = write.load (std::memory_order_relaxed);
        if (position - read.load (std::memory_order_acquire) >= (panic ? Capacity : Capacity - Reserved)) return false;
        cells[position % Capacity] = event;
        write.store (position + 1, std::memory_order_release);
        return true;
    }
    bool pop (MidiInputEvent& event) noexcept
    {
        const auto position = read.load (std::memory_order_relaxed);
        if (position == write.load (std::memory_order_acquire)) return false;
        event = cells[position % Capacity];
        read.store (position + 1, std::memory_order_release);
        return true;
    }
    bool pending() const noexcept { return read.load (std::memory_order_relaxed) != write.load (std::memory_order_acquire); }
private:
    static_assert (Reserved < Capacity && std::atomic<uint64_t>::is_always_lock_free);
    std::array<MidiInputEvent, Capacity> cells;
    std::atomic<uint64_t> read { 0 };
    std::array<char, 64> counterSeparation {};
    std::atomic<uint64_t> write { 0 };
};

class MidiInputService : private juce::AsyncUpdater, private ShortcutService::Listener
{
public:
    enum class Status { connected, notSelected, disconnected, unavailable, waiting };
    struct Device
    {
        juce::String identifier, name;
        Status status = Status::notSelected;
        uint64_t input = 0, connection = 0;
    };
    struct Counters
    {
        // stale counts observations older than 100ms; reserved panic remains executable.
        uint64_t received = 0, delivered = 0, dropped = 0, panicDropped = 0, stale = 0, unsupported = 0;
        // Cause totals across ordinary/panic losses. SPSC has no contention
        // exhaustion path, so contentionDropped is always zero.
        uint64_t capacityDropped = 0, contentionDropped = 0;
    };
    struct Handle
    {
        virtual ~Handle() = default;
        virtual void start() = 0;
        /** Returns only after callbacks finish; no later callbacks until start(). */
        virtual void stop() = 0;
    };
    struct Backend
    {
        std::function<std::vector<juce::MidiDeviceInfo>()> enumerate;
        // Each opened handle must deliver serial callbacks; different handles
        // may call concurrently. Queues are allocated before open/start.
        std::function<std::unique_ptr<Handle> (const juce::String&, juce::MidiInputCallback*)> open;
        bool nativeNotifications = true;
        std::function<double()> clockMs; // deterministic tests; callback-safe, monotonic and nonblocking
    };
    struct Callbacks
    {
        /** Message thread. false=state cleanup only; return per-address readiness. */
        std::function<bool (const MidiInputEvent&, const juce::String&, bool execute)> receive;
        std::function<void (uint64_t input, uint64_t connection, bool connected)> connection;
        std::function<void (uint64_t input, bool panic)> fault;
        std::function<void()> devicesChanged;
    };
    MidiInputService (ShortcutService&, Callbacks, Backend = {}, bool runNotifier = true);
    ~MidiInputService() override;
    void refresh(); // message thread, explicit retry of unavailable devices
    void deviceListChanged() noexcept; // notification coalescing, also used by injected backends
    void drain (double nowMs = -1.0); // deterministic message-thread entry, bounded per call
    void shutdown();
    std::vector<Device> devices() const;
    Counters counters() const noexcept;
    bool hasPending() const noexcept;
    bool hasInputFault() const noexcept { return dropped.load() != 0 || panicDropped.load() != 0 || stale.load() != 0; }
    /** Inject through the same callback path; the returned callback is valid until
        that connection is stopped. Tests own their producer thread lifetime. */
    juce::MidiInputCallback* callbackFor (const juce::String&) const;

private:
    struct Port;
    struct Notifier;
    struct Signals { std::atomic<bool> deviceChange { false }, stopped { false }, faultPending { false }; };
    struct FaultEpochs { uint64_t ordinary, panic; };
    FaultEpochs deliverFaults (Port&);
    Port* popNext (MidiInputEvent&);
    void accept (Port&, const juce::MidiMessage&) noexcept;
    void rebuildPanicAddresses();
    void closePort (Port&);
    void handleAsyncUpdate() override;
    void shortcutsChanged() override;
    void captureStateChanged() override;
    void midiInputSettingsChanged() override;

    ShortcutService& shortcuts;
    Callbacks callbacks;
    Backend backend;
    std::shared_ptr<Signals> signals = std::make_shared<Signals>();
    juce::MidiDeviceListConnection deviceConnection;
    std::unique_ptr<Notifier> notifier;
    std::map<juce::String, std::unique_ptr<Port>> ports;
    std::vector<juce::MidiDeviceInfo> available;
    juce::String lastDrainedPort; // round-robin cursor, message thread only
    // Increment before publishing to a port queue. The notifier only reads
    // this count, never the message thread's mutable port list/queue pointers.
    std::atomic<uint64_t> queued { 0 };
    std::atomic<bool> accepting { true };
    std::atomic<uint64_t> routing { 0 };
    std::atomic<uint64_t> received { 0 }, delivered { 0 }, dropped { 0 }, panicDropped { 0 }, stale { 0 }, unsupported { 0 };
    uint64_t nextInput = 0, nextConnection = 0;
    bool stopped = false;
};
}
