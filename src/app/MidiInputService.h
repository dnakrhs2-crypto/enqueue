#pragma once

#include "app/ShortcutService.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <atomic>

namespace gocue
{
/** Bounded MPSC queue. The reservation CAS defines cross-port order; a consumer
    never skips an unpublished reservation. Producers never wait for one another.
    Normal admission leaves Reserved slots available for panic addresses. */
template <size_t Capacity, size_t Reserved> class MidiEventQueue
{
public:
    MidiEventQueue() { for (size_t i = 0; i < Capacity; ++i) cells[i].sequence.store (i); }
    bool push (const MidiInputEvent& event, bool panic) noexcept
    {
        auto position = write.load (std::memory_order_relaxed);
        for (int attempt = 0; attempt < 64; ++attempt)
        {
            const auto readPosition = read.load (std::memory_order_acquire);
            if (position < readPosition) { position = write.load (std::memory_order_relaxed); continue; }
            if (position - readPosition >= (panic ? Capacity : Capacity - Reserved)) return false;
            auto& cell = cells[position % Capacity];
            if (cell.sequence.load (std::memory_order_acquire) != position) { position = write.load (std::memory_order_relaxed); continue; }
            if (write.compare_exchange_weak (position, position + 1, std::memory_order_relaxed))
            {
                cell.event = event;
                cell.sequence.store (position + 1, std::memory_order_release);
                return true;
            }
        }
        return false; // bounded contention is also an explicit loss, never a callback spinlock
    }
    bool pop (MidiInputEvent& event) noexcept
    {
        const auto position = read.load (std::memory_order_relaxed);
        auto& cell = cells[position % Capacity];
        if (cell.sequence.load (std::memory_order_acquire) != position + 1) return false;
        event = cell.event;
        cell.sequence.store (position + Capacity, std::memory_order_release);
        read.store (position + 1, std::memory_order_release);
        return true;
    }
    bool pending() const noexcept { return read.load (std::memory_order_relaxed) != write.load (std::memory_order_acquire); }
private:
    static_assert (Reserved < Capacity && std::atomic<uint64_t>::is_always_lock_free);
    struct Cell { std::atomic<uint64_t> sequence { 0 }; MidiInputEvent event; };
    std::array<Cell, Capacity> cells;
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
        bool overloaded = false; // diagnostic for this connection; does not gate routing
    };
    struct Counters
    {
        // stale counts observations older than 100ms; reserved panic remains executable.
        uint64_t received = 0, delivered = 0, dropped = 0, panicDropped = 0, stale = 0, unsupported = 0;
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
    // Heap allocation happens at service creation, never in a callback.
    std::unique_ptr<MidiEventQueue<8704, 512>> queue;
    std::atomic<bool> accepting { true };
    std::atomic<uint64_t> routing { 0 };
    std::atomic<uint64_t> received { 0 }, delivered { 0 }, dropped { 0 }, panicDropped { 0 }, stale { 0 }, unsupported { 0 };
    uint64_t nextInput = 0, nextConnection = 0;
    bool stopped = false;
};
}
