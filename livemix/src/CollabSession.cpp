#include "CollabSession.h"
#include "LiveMixSettings.h"
#include "MixEngine.h"
#include "ui/UiUtils.h"
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace gocue::livemix::collab
{
namespace
{
double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
void messageThread() { jassert (juce::MessageManager::getInstance()->isThisTheMessageThread()); }
}

struct CollabSession::Impl
{
    Impl (CollabSession& s, std::unique_ptr<CollabRoom> r, std::unique_ptr<CollabUpnp> u)
        : owner (s), room (std::move (r)), upnp (std::move (u)), worker ([this] { run(); }) {}
    ~Impl() { finish(); }
    void finish()
    {
        if (! worker.joinable()) return;
        { std::lock_guard<std::mutex> lock (mutex); stopping = true; }
        wake.notify_one();
        worker.join(); // drains leave, including a create response which arrived after cancellation
    }

    void enqueue (std::function<void()> task)
    {
        { std::lock_guard<std::mutex> lock (mutex); tasks.push_back (std::move (task)); }
        wake.notify_one();
    }
    void complete (unsigned id, std::function<void()> task)
    {
        std::lock_guard<std::mutex> lock (mutex);
        completions.push_back ([this, id, task = std::move (task)] { if (generation.load() == id) task(); });
    }
    void dispatch()
    {
        std::deque<std::function<void()>> ready;
        { std::lock_guard<std::mutex> lock (mutex); ready.swap (completions); }
        for (auto& callback : ready) callback();
    }
    void run()
    {
        for (;;)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock (mutex);
                wake.wait (lock, [this] { return stopping || ! tasks.empty(); });
                if (tasks.empty()) return;
                task = std::move (tasks.front()); tasks.pop_front();
            }
            task();
        }
    }
    void cleanup()
    {
        owner.transport.stop();
        if (code.isNotEmpty()) room->close (code, token);
        code.clear(); token.clear();
        if (upnpUsed) upnp->removeMapping();
        upnpUsed = false;
    }
    void error (unsigned id, const juce::String& text)
    {
        cleanup();
        complete (id, [this, text] { owner.setState (State::error, text); });
    }

    CollabSession& owner;
    std::unique_ptr<CollabRoom> room;
    std::unique_ptr<CollabUpnp> upnp;
    std::atomic<unsigned> generation { 0 };
    // Worker-owned room credentials and mapping ownership, including cancelled in-flight creates.
    juce::String code, token;
    bool upnpUsed = false;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> tasks, completions;
    bool stopping = false;
    std::thread worker;
};

CollabSession::CollabSession (MixEngine& engine, LiveMixSettings& prefs)
    : CollabSession (engine.getCollab(), prefs, std::make_unique<CollabRoom>(), CollabUpnp::makeMiniUpnp(), Options {})
{
    peerInfo = [&engine]
    {
        PeerInfo info;
        info.deviceRate = (uint32_t) engine.getCollab().getDeviceRate();
        if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
            info.inputLatencyMs = 1000.0 * device->getInputLatencyInSamples() / juce::jmax (1.0, device->getCurrentSampleRate());
        info.outputLatencyMs = juce::jmax (0.0, engine.getLatencyMs() - info.inputLatencyMs);
        return info;
    };
}

CollabSession::CollabSession (CollabAudio& a, LiveMixSettings& prefs, std::unique_ptr<CollabRoom> room,
                              std::unique_ptr<CollabUpnp> upnp, Options opt)
    : audio (a), settings (prefs), options (opt), impl (std::make_unique<Impl> (*this, std::move (room), std::move (upnp)))
{
    messageThread();
    options.loopbackOnly = options.loopbackOnly
        || juce::SystemStats::getEnvironmentVariable ("LIVEMIX_COLLAB_LOOPBACK", {}) == "1";
    snapshot.myNick = settings.getCollabNick();
    snapshot.monitorVolume = settings.getCollabMonitorVolume(); snapshot.monitorOn = settings.getCollabMonitorOn();
    audio.setMonitor (snapshot.monitorVolume, snapshot.monitorOn);
    snapshot.route = (CollabAudio::Route) settings.getCollabRoute(); snapshot.routeFirst = settings.getCollabRouteFirst();
    audio.setRoute (snapshot.route, snapshot.routeFirst);
    startTimer (50);
}

CollabSession::~CollabSession()
{
    leave(); stopTimer(); impl->finish();
}

juce::StringArray CollabSession::lanAddresses()
{
    juce::StringArray result;
    for (const auto& ip : juce::IPAddress::getAllAddresses (false))
        if (! ip.isIPv6 && ! ip.isNull() && ip.address[0] != 127
            && ! (ip.address[0] == 169 && ip.address[1] == 254))
        {
            result.addIfNotAlreadyThere (ip.toString());
            if (result.size() == 8) break;
        }
    return result;
}

PeerInfo CollabSession::localInfo (const juce::String& nick) const
{
    auto info = peerInfo ? peerInfo() : PeerInfo {};
    info.nickname = nick; info.deviceRate = (uint32_t) audio.getDeviceRate();
    info.wireRate = (uint32_t) settings.getCollabRate();
    return info;
}

void CollabSession::setState (State state, const juce::String& message, const juce::String& reason)
{
    messageThread();
    snapshot.state = state; snapshot.message = message; snapshot.reason = reason;
    const bool closed = state == State::off || state == State::error || state == State::hostUnavailable;
    if (closed)
    {
        snapshot.code.clear(); snapshot.peerNick.clear(); started = false;
    }
    audio.setCollabOpen (! closed);   // the partner's OBS source works while a room is open, 'OBS로 보내기' or not
    sendChangeMessage();
}

void CollabSession::createRoom (const juce::String& nick)
{
    messageThread(); leave();
    snapshot.isHost = true; snapshot.myNick = nick.trim();
    snapshot.wireRate = (uint32_t) settings.getCollabRate();
    audio.setWireRate (snapshot.wireRate);
    settings.setCollabNick (snapshot.myNick); settings.saveIfNeeded();
    setState (State::checkingHost);
    const auto id = impl->generation.load();
    const auto me = localInfo (snapshot.myNick);
    const auto inst = settings.getCollabInstallId();
    const int port = settings.getCollabPort();
    impl->enqueue ([this, id, me, inst, port]
    {
        auto& w = *impl;
        if (w.generation.load() != id) return;
        const auto error = transport.open (port, options.loopbackOnly);
        if (error.isNotEmpty()) { w.error (id, error); return; }
        w.upnpUsed = true;
        const auto upnp = w.upnp->discover (transport.getLocalPort());
        if (w.generation.load() != id) return;
        auto lan = lanAddresses();
        if (upnp.lanIp.isNotEmpty() && lan.contains (upnp.lanIp))
        {
            lan.removeString (upnp.lanIp); lan.insert (0, upnp.lanIp);
        }
        const auto created = w.room->create (me.nickname, inst, transport.getLocalPort(), lan, upnp);
        w.code = created.code; w.token = created.hostToken;
        if (w.generation.load() != id) return;
        if (! created.ok()) { w.error (id, created.error); return; }
        transport.expectProbe (created.nonce);
        const auto deadline = nowMs() + options.probeTimeoutMs;
        while (! transport.hasProbe() && nowMs() < deadline && w.generation.load() == id)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        if (w.generation.load() != id) return;
        const auto check = CollabHostCheck::evaluate (transport.hasProbe(), upnp, created.publicIp);
        const auto confirmed = w.room->confirm (created.code, created.hostToken, check.reachable, check.reason);
        if (! check.reachable && confirmed.ok()) { w.code.clear(); w.token.clear(); } // confirm(false) closes the room
        if (w.generation.load() != id) return;
        if (! check.reachable)
        {
            w.cleanup();
            w.complete (id, [this, check] { setState (State::hostUnavailable, check.message(), check.reason); });
            return;
        }
        if (! confirmed.ok()) { w.error (id, confirmed.error); return; }
        transport.startHost (created.session, me);
        w.complete (id, [this, created, upnp]
        {
            started = true; mapped = upnp.mapped; snapshot.code = created.code;
            lastHeartbeat = lastRenew = nowMs(); setState (State::hostWaiting);
        });
    });
}

void CollabSession::joinRoom (const juce::String& code, const juce::String& nick)
{
    messageThread(); leave();
    snapshot.myNick = nick.trim(); snapshot.code = code.trim();
    settings.setCollabNick (snapshot.myNick); settings.saveIfNeeded();
    setState (State::joining);
    const auto id = impl->generation.load();
    const auto me = localInfo (snapshot.myNick);
    const auto inst = settings.getCollabInstallId(), roomCode = snapshot.code;
    impl->enqueue ([this, id, me, inst, roomCode]
    {
        auto& w = *impl;
        if (w.generation.load() != id) return;
        const auto joined = w.room->join (roomCode, me.nickname, inst);
        if (w.generation.load() != id) return;
        if (! joined.ok()) { w.error (id, joined.error); return; }
        const auto error = transport.open (0, options.loopbackOnly);
        if (error.isNotEmpty()) { w.error (id, error); return; }
        transport.startGuest (joined.session, joined.candidates, me);
        w.complete (id, [this, joined]
        {
            started = true; snapshot.peerNick = joined.hostNick; joinDeadline = nowMs() + options.connectTimeoutMs;
        });
    });
}

void CollabSession::leave()
{
    messageThread();
    ++impl->generation;
    if (audioAttached) audio.detach();
    audioAttached = started = mapped = heartbeatPending = renewPending = false;
    heartbeatConnected = heartbeatStopped = false;
    heartbeatFailures = 0;
    snapshot.isHost = false;
    impl->enqueue ([this] { impl->cleanup(); });
    setState (State::off);
}

void CollabSession::fail (const juce::String& message)
{
    leave(); setState (State::error, message);
}

void CollabSession::rebuffer() { messageThread(); audio.rebuffer(); sendChangeMessage(); }
void CollabSession::setMonitor (float volume, bool on)
{
    messageThread();
    snapshot.monitorVolume = juce::jlimit (0.0f, 1.0f, std::isfinite (volume) ? volume : 0.0f); snapshot.monitorOn = on;
    audio.setMonitor (snapshot.monitorVolume, on);
    settings.setCollabMonitorVolume (snapshot.monitorVolume); settings.setCollabMonitorOn (on); settings.saveIfNeeded();
    sendChangeMessage();
}
void CollabSession::setRoute (CollabAudio::Route route, int first)
{
    messageThread();
    settings.setCollabRoute ((int) route); settings.setCollabRouteFirst (first); settings.saveIfNeeded();
    snapshot.route = (CollabAudio::Route) settings.getCollabRoute(); snapshot.routeFirst = settings.getCollabRouteFirst();
    audio.setRoute (snapshot.route, snapshot.routeFirst);
    sendChangeMessage();
}

CollabSession::Snapshot CollabSession::getSnapshot() const
{
    messageThread();
    auto result = snapshot;
    result.myNick = settings.getCollabNick();
    if (started && (result.state == State::connected || result.state == State::disconnected))
    {
        result.rttMs = transport.getStats().rttMs; result.estimatedOneWayMs = transport.estimatedOneWayMs();
        const auto stats = audio.getStats(); result.bufferMs = stats.targetMs; result.lossPercent = stats.lossPercent10s;
    }
    return result;
}

void CollabSession::sendHeartbeat()
{
    if (heartbeatPending || heartbeatStopped) return;
    heartbeatPending = true; lastHeartbeat = nowMs();
    const auto id = impl->generation.load();
    const bool connected = snapshot.state == State::connected;
    heartbeatConnected = connected;
    impl->enqueue ([this, id, connected]
    {
        if (impl->generation.load() != id) return;
        const auto reply = impl->room->heartbeat (impl->code, impl->token, connected);
        impl->complete (id, [this, reply]
        {
            heartbeatPending = false;
            if (reply.ok()) { heartbeatFailures = 0; return; }
            const bool roomGone = reply.statusCode == 404 || reply.statusCode == 410;
            const auto peerState = transport.getState();
            if (snapshot.state == State::connected || snapshot.state == State::disconnected
                || peerState == CollabTransport::State::connected || peerState == CollabTransport::State::disconnected)
            {
                // Audio is peer-to-peer: a room-server restart must not stop the transport.
                heartbeatFailures = 0;
                if (roomGone) heartbeatStopped = true;
                return;
            }
            if (roomGone) { createRoom (snapshot.myNick); return; }
            const bool temporary = reply.statusCode == 0 || reply.statusCode >= 500;
            if (! temporary || ++heartbeatFailures >= 3) fail (reply.error);
        });
    });
}

void CollabSession::timerCallback()
{
    impl->dispatch();
    if (! started) return;
    const auto now = nowMs();
    const auto stats = transport.getStats();
    const auto previous = snapshot.state;
    if (stats.state == CollabTransport::State::connected)
    {
        if (! audioAttached)
        {
            snapshot.wireRate = stats.wireRate;
            audio.setWireRate (snapshot.wireRate);
            audio.attach (transport); audioAttached = true;
        }
        if (snapshot.state != State::connected || snapshot.peerNick != stats.peer.nickname)
        {
            snapshot.peerNick = stats.peer.nickname; setState (State::connected);
        }
    }
    else if (stats.state == CollabTransport::State::disconnected)
    {
        if (snapshot.state != State::disconnected) setState (State::disconnected);
    }
    else if (stats.state == CollabTransport::State::peerLeft)
    {
        if (! snapshot.isHost) { leave(); setState (State::off, ko ("방장이 방을 닫았습니다")); return; }
        if (snapshot.state != State::hostWaiting) { snapshot.peerNick.clear(); setState (State::hostWaiting); }
    }
    else if (! snapshot.isHost && snapshot.state == State::joining && now >= joinDeadline)
    {
        fail (ko ("방장 PC에 연결하지 못했습니다")); return;
    }
    if (snapshot.isHost)
    {
        if (snapshot.state == State::hostWaiting && heartbeatStopped)
        {
            createRoom (snapshot.myNick);
            return;
        }
        if (previous != snapshot.state || heartbeatConnected != (snapshot.state == State::connected)
            || now - lastHeartbeat >= options.heartbeatIntervalMs) sendHeartbeat();
        if (mapped && ! renewPending && now - lastRenew >= options.renewIntervalMs)
        {
            renewPending = true; lastRenew = now;
            const auto id = impl->generation.load();
            impl->enqueue ([this, id]
            {
                if (impl->generation.load() != id) return;
                impl->upnp->renew();
                impl->complete (id, [this] { renewPending = false; });
            });
        }
    }
    if (now - lastNotification >= 250) { lastNotification = now; sendChangeMessage(); }
}
}
