#pragma once
#include "app/AppSettings.h"
#include "remote/RemoteNetwork.h"
#include "remote/RemoteServer.h"
#include "remote/RemoteSnapshot.h"
#include <juce_events/juce_events.h>
#include <deque>
#include <mutex>

namespace gocue::remote
{
/** Message-thread lifetime; the transport's callbacks only parse and enqueue. */
class Service : public juce::ChangeBroadcaster, private juce::AsyncUpdater
{
public:
    struct Callbacks
    {
        std::function<juce::Result (uint64_t, const Request&, double)> execute;
        std::function<void (uint64_t)> release;
        std::function<bool()> showMode;
    };
    struct Device { uint64_t connection; juce::String name; juce::Time since; Session session; };
    using NetworkDiscovery = std::function<std::vector<NetworkAddress>()>;
    Service (AppSettings&, ProjectDocument&, Callbacks, NetworkDiscovery = {});
    ~Service() override;
    void restoreEnabled (bool safeMode);
    void setEnabled (bool);
    bool isEnabled() const noexcept { return enabled; }
    bool failedToOpenPort() const noexcept { return enabled && port == 0; }
    int getPort() const noexcept { return port; }
    juce::String address() const;
    juce::String link() const;
    juce::String key() const { return accessKey; }
    juce::String installId() const { return serverId; }
    void renewKey();
    void refreshNetworks();
    void selectNetwork (int index);
    int selectedNetwork() const noexcept { return selected; }
    const std::vector<NetworkAddress>& networks() const noexcept { return addresses; }
    const std::map<uint64_t, Device>& devices() const noexcept { return connected; }
    void publish (const PlaybackSnapshot&);
    void modelChanged() { model.changed(); }
    /** Also used by the PC timer: drain input before taking the shared snapshot. */
    void dispatchPending() { handleUpdateNowIfNeeded(); }
private:
    enum class EventKind { opened, closed, request };
    struct Event { EventKind kind; uint64_t connection; double timeMs; Request request; };
    void enqueue (Event);
    void handleAsyncUpdate() override;
    void stop (const char* reason);
    void start (int preferredPort = 49820);
    void send (uint64_t, const juce::var&);
    void broadcast (const juce::var&);
    juce::var stateMessage();
    AppSettings& settings;
    ProjectDocument& document;
    Callbacks callbacks;
    NetworkDiscovery discoverNetworks;
    SnapshotModel model;
    std::unique_ptr<Server> server;
    std::mutex queueMutex;
    std::deque<Event> incoming;
    bool accepting = false; // protected by queueMutex; workers never inspect model/UI members
    bool enabled = false, loopback = false;
    int port = 0, selected = -1;
    juce::String accessKey, serverId;
    std::vector<NetworkAddress> addresses;
    std::map<uint64_t, Device> connected;
    PlaybackSnapshot snapshot;
    juce::String lastState;
    juce::int64 sequence = 0, sentGeneration = 0;
    double lastStateMs = -1e9, lastNetworkMs = -1e9;
};
}
