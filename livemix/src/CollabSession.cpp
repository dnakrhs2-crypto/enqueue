#include "CollabSession.h"
#include "LiveMixSettings.h"
#include "MixEngine.h"
#include "ui/UiUtils.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace gocue::livemix::collab
{
namespace c = contracts;
namespace
{
double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
void messageThread() { jassert (juce::MessageManager::getInstance()->isThisTheMessageThread()); }
uint32_t capabilities (const juce::StringArray& names)
{
    uint32_t result = 0;
    for (const auto& item : c::capabilityNames) if (names.contains (item.name)) result |= (uint32_t) item.bit;
    return result;
}
CollabRoom::ClientInfo clientInfo (const PeerInfo& me)
{
    CollabRoom::ClientInfo result;
    result.connectionId = juce::Uuid().toString().removeCharacters ("-").toLowerCase();
    result.wireRate = me.wireRate;
    return result;
}
juce::var signal (const char* type)
{
    auto* object = new juce::DynamicObject(); object->setProperty ("type", type); return juce::var (object);
}
class NativeWebEndpoint final : public CollabSession::WebEndpoint
{
public:
    void start (const c::WebConnection& connection, WebGuestTransport::Options options, WebGuestTransport::Callbacks callbacks) override
    { transport.startAcceptedGuest (connection, options, std::move (callbacks)); }
    void description (uint32_t g, const juce::String& type, const juce::String& sdp) override { transport.applyRemoteDescription (g, type, sdp); }
    void candidate (uint32_t g, const juce::String& value, const juce::String& mid, int line) override { transport.addRemoteCandidate (g, value, mid, line); }
    void endCandidates (uint32_t g) override { transport.endRemoteCandidates (g); }
    bool send (uint32_t g, const uint8_t* p, size_t n, bool audio) override
    { return audio ? transport.sendAudio (g, p, n) : transport.sendControl (g, p, n); }
    void close() override { transport.close(); }
private:
    WebGuestTransport transport;
};
CollabSession::Options engineOptions (MixEngine& engine, CollabSession::Options options)
{
    options.peerRingNames = engine.getPeerRingNames(); return options;
}
}

struct CollabSession::Impl
{
    Impl (CollabSession& s, std::unique_ptr<CollabRoom> r, std::unique_ptr<CollabUpnp> u)
        : owner (s), room (std::move (r)), upnp (std::move (u)),
          webUpnp (s.options.webUpnpFactory()),
          signaling (s.options.webRequest, s.options.webServer),
          web (s.options.webEndpoint ? s.options.webEndpoint : std::make_shared<NativeWebEndpoint>()),
          worker ([this] { run(); }) {}
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
        signaling.stop();
        web->close();
        owner.transport.stop();
        if (code.isNotEmpty()) { if (apiV2) room->closeV2 (code, token); else room->close (code, token); }
        code.clear(); token.clear();
        apiV2 = false;
        if (upnpUsed) upnp->removeMapping();
        if (webUpnpUsed) webUpnp->removeMapping();
        upnpUsed = false;
        webUpnpUsed = false; webOptions = {};
    }
    void error (unsigned id, const juce::String& text)
    {
        cleanup();
        complete (id, [this, text] { owner.setState (State::error, text); });
    }

    CollabSession& owner;
    std::unique_ptr<CollabRoom> room;
    std::unique_ptr<CollabUpnp> upnp;
    std::unique_ptr<CollabUpnp> webUpnp;
    WebGuestTransport::Options webOptions;
    WebGuestSignaling signaling;
    std::shared_ptr<WebEndpoint> web;
    std::atomic<unsigned> generation { 0 };
    // Worker-owned room credentials and mapping ownership, including cancelled in-flight creates.
    juce::String code, token;
    bool upnpUsed = false, webUpnpUsed = false, apiV2 = false;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> tasks, completions;
    bool stopping = false;
    std::thread worker;
};

CollabSession::CollabSession (MixEngine& engine, LiveMixSettings& prefs)
    : CollabSession (engine, prefs, std::make_unique<CollabRoom>(), CollabUpnp::makeMiniUpnp(), Options {}) {}

CollabSession::CollabSession (MixEngine& mix, LiveMixSettings& prefs, std::unique_ptr<CollabRoom> room,
                            std::unique_ptr<CollabUpnp> upnp, Options options)
    : CollabSession (mix.getCollab(), prefs, std::move (room), std::move (upnp), engineOptions (mix, std::move (options)))
{
    engine = &mix;
    peerInfo = [&mix]
    {
        PeerInfo info;
        info.deviceRate = (uint32_t) mix.getCollab().getDeviceRate();
        if (auto* device = mix.getDeviceManager().getCurrentAudioDevice())
            info.inputLatencyMs = 1000.0 * device->getInputLatencyInSamples() / juce::jmax (1.0, device->getCurrentSampleRate());
        info.outputLatencyMs = juce::jmax (0.0, mix.getLatencyMs() - info.inputLatencyMs);
        return info;
    };
    publishPeople();
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
    for (unsigned slot = 0; slot < metadata.size(); ++slot)
        if (options.peerRingNames[slot].isNotEmpty()) metadata[slot].open (options.peerRingNames[slot], slot + 1);
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
    info.appVersion = "0.17.0";
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
    publishPeople();
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
    const auto client = clientInfo (me);
    const auto inst = settings.getCollabInstallId();
    const int port = settings.getCollabPort();
    impl->enqueue ([this, id, me, client, inst, port]
    {
        auto& w = *impl;
        if (w.generation.load() != id) return;
        const auto error = transport.open (port, options.loopbackOnly);
        if (error.isNotEmpty()) { w.error (id, error); return; }
        w.upnpUsed = true;
        const auto upnp = w.upnp->discover (transport.getLocalPort());
        if (w.generation.load() != id) return;
        const int webPort = transport.getLocalPort() + 1;
        // A router the first discovery did not find would only cost room creation a second discovery timeout.
        if (upnp.found && webPort <= 65535)
        {
            w.webUpnpUsed = true;
            const auto webUpnp = w.webUpnp->discover (webPort);
            if (webUpnp.mapped)
            {
                w.webOptions.localPort = (uint16_t) webPort;
                if (WebGuestTransport::isPublicIpv4 (webUpnp.externalIp))
                {
                    w.webOptions.externalIp = webUpnp.externalIp;
                    w.webOptions.lanIp = webUpnp.lanIp;
                }
            }
        }
        if (w.generation.load() != id) return;
        const auto stun = transport.discoverPublic (options.stunServers, options.stunTimeoutMs);
        if (w.generation.load() != id) return;
        auto lan = lanAddresses();
        if (upnp.lanIp.isNotEmpty() && lan.contains (upnp.lanIp))
        {
            lan.removeString (upnp.lanIp); lan.insert (0, upnp.lanIp);
        }
        const auto created = w.room->createV2 (me.nickname, inst, transport.getLocalPort(), lan, upnp, client, stun);
        w.code = created.code; w.token = created.hostToken;
        w.apiV2 = created.room.apiVersion == 2;
        if (w.generation.load() != id) return;
        if (! created.ok()) { w.error (id, created.error); return; }
        transport.expectProbe (created.nonce);
        const auto deadline = nowMs() + options.probeTimeoutMs;
        while (! transport.hasProbe() && nowMs() < deadline && w.generation.load() == id)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        if (w.generation.load() != id) return;
        const auto check = CollabHostCheck::evaluate (transport.hasProbe(), upnp, created.publicIp);
        // A closed port no longer ends the room when this socket's public address is known: the guest's address
        // comes back with the heartbeat and this side knocks first. Only a server that says so keeps the room.
        CollabRoom::ConfirmedV2 confirmed;
        if (w.apiV2) confirmed = w.room->confirmV2 (created.code, created.hostToken, check.reachable, check.reason,
                                                  ! check.reachable && stun.usable());
        else static_cast<CollabRoom::Confirmed&> (confirmed) = w.room->confirm (
            created.code, created.hostToken, check.reachable, check.reason, ! check.reachable && stun.usable());
        const bool punch = ! check.reachable && confirmed.punch;
        if (! w.apiV2 && ! check.reachable && ! punch && confirmed.ok()) { w.code.clear(); w.token.clear(); }
        if (w.generation.load() != id) return;
        if (! w.apiV2 && ! check.reachable && ! punch)
        {
            w.cleanup();
            w.complete (id, [this, check] { setState (State::hostUnavailable, check.message(), check.reason); });
            return;
        }
        if (! confirmed.ok()) { w.error (id, confirmed.error); return; }
        w.complete (id, [this, created, confirmed, me, upnp, punch, mappedWeb = w.webOptions.localPort != 0]
        {
            started = true; mapped = upnp.mapped; punching = punch; snapshot.code = created.code;
            webMapped = mappedWeb;
            snapshot.session = created.session; snapshot.selfId = created.participantId != 0 ? created.participantId : 1;
            snapshot.nativeAvailable = confirmed.room.apiVersion == 2 ? confirmed.room.nativeState != "unavailable" : true;
            applyRoom (confirmed.room.apiVersion == 2 ? confirmed.room : created.room);
            startTransport (me);
            lastHeartbeat = lastRenew = nowMs(); setState (State::hostWaiting);
            if (roomSnapshot.apiVersion == 2) startSignaling();
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
    const auto client = clientInfo (me);
    const auto inst = settings.getCollabInstallId(), roomCode = snapshot.code;
    impl->enqueue ([this, id, me, client, inst, roomCode]
    {
        auto& w = *impl;
        if (w.generation.load() != id) return;
        // The socket first: its public address goes with the join, for a host whose port is closed to knock on.
        const auto error = transport.open (0, options.loopbackOnly);
        if (error.isNotEmpty()) { w.error (id, error); return; }
        const auto stun = transport.discoverPublic (options.stunServers, options.stunTimeoutMs);
        if (w.generation.load() != id) return;
        const auto joined = w.room->joinV2 (roomCode, me.nickname, inst, client, stun, transport.getLocalPort());
        if (w.generation.load() != id) return;
        if (! joined.ok()) { w.error (id, joined.error); return; }
        w.complete (id, [this, joined, me]
        {
            started = true; snapshot.peerNick = joined.hostNick; joinDeadline = nowMs() + options.connectTimeoutMs;
            snapshot.session = joined.session; snapshot.selfId = joined.participantId != 0 ? joined.participantId : 2;
            snapshot.nativeAvailable = true;
            guestGeneration = joined.connectionGeneration;
            applyRoom (joined.room);
            startTransport (me, joined.candidates);
        });
    });
}

void CollabSession::leave()
{
    messageThread();
    ++impl->generation;
    ++transportSerial;
    webGeneration.store (0);
    if (audioAttached) audio.detach();
    audioAttached = started = mapped = heartbeatPending = renewPending = punching = false;
    webMapped = false;
    heartbeatConnected = heartbeatStopped = heartbeatDirty = false;
    heartbeatFailures = 0;
    snapshot.isHost = false;
    snapshot.session = 0; snapshot.selfId = 0; snapshot.personCount = snapshot.webRequestCount = 0;
    snapshot.people = {}; snapshot.webRequests = {};
    snapshot.roomMode = c::RoomMode::legacy2; snapshot.capabilities = snapshot.roomRevision = snapshot.rosterRevision = 0;
    snapshot.webInviteUrl.clear(); snapshot.webInviteQrUrl.clear(); snapshot.inviteGeneration = 0;
    snapshot.nativeAvailable = snapshot.signalingConnected = snapshot.canCreateWebInvite = false;
    snapshot.kickableWebId = 0;
    roomSnapshot = {}; roster = {}; roster.mode = c::RoomMode::legacy2;
    nativePeers.clear(); leftMembers.clear(); webConnection = {};
    transportV2 = switchingTransport = webAcceptPending = false; acceptingNick.clear();
    audio.applyRoster (roster, 0);
    for (auto& item : metadata) item.clear();
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
    for (const auto& person : snapshot.people)
        if (person.slot > 0) audio.setSlotMonitorVolume ((unsigned) person.slot - 1, person.monitorVolume);
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

void CollabSession::applyRoom (const CollabRoom::RoomSnapshot& room, const std::vector<CollabRoom::Peer>& peers)
{
    messageThread();
    if (room.apiVersion == 2)
    {
        if (room.revision < roomSnapshot.revision) return;
        roomSnapshot = room;
        snapshot.roomRevision = room.revision;
        snapshot.roomMode = room.roomMode == "v2" ? c::RoomMode::v2 : c::RoomMode::legacy2;
        snapshot.capabilities = capabilities (room.capabilities);
        snapshot.nativeAvailable = room.nativeState == "reachable" || room.nativeState == "punch";
        // A legacy2 room's rate is the one negotiated in the v1 hello/welcome: a 0.16 host sends no wireRate,
        // so the server reports its 48 kHz default there. Only a v2 room takes the server's value.
        if (snapshot.roomMode == c::RoomMode::v2) snapshot.wireRate = room.wireRate;
    }
    RosterSnapshot next;
    next.session = snapshot.session; next.mode = snapshot.roomMode; next.wireRate = snapshot.wireRate;
    next.revision = roster.revision + 1;
    const auto me = localInfo (snapshot.myNick);
    if (roomSnapshot.apiVersion == 2)
    {
        for (const auto& source : roomSnapshot.members)
        {
            if (std::any_of (leftMembers.begin(), leftMembers.end(), [&] (const auto& m) { return m.participantId == source.participantId; })) continue;
            if (next.memberCount == next.members.size()) break;
            auto& m = next.members[next.memberCount++];
            m.participantId = source.participantId; m.streamEpoch = source.streamEpoch;
            m.kind = source.kind == "web" ? c::ParticipantKind::web : c::ParticipantKind::native;
            m.state = source.state == "connected" ? c::ConnectionState::connected : c::ConnectionState::connecting;
            m.capabilities = capabilities (source.capabilities); m.nickname = source.nick; m.appVersion = source.appVersion;
            m.deviceRate = snapshot.wireRate;
            // The room server owns membership, the audio roster owns epochs and latency.
            if (const auto* old = findMember (roster, m.participantId))
            {
                m.streamEpoch = juce::jmax (m.streamEpoch, old->streamEpoch);
                m.deviceRate = old->deviceRate; m.inputLatencyUs = old->inputLatencyUs; m.outputLatencyUs = old->outputLatencyUs;
                m.state = old->state;
            }
        }
    }
    else
    {
        auto& self = next.members[next.memberCount++];
        self.participantId = snapshot.selfId; self.nickname = snapshot.myNick; self.appVersion = me.appVersion;
        self.state = c::ConnectionState::connected;
        if (snapshot.peerNick.isNotEmpty())
        {
            auto& other = next.members[next.memberCount++];
            other.participantId = snapshot.isHost ? 2 : 1; other.nickname = snapshot.peerNick;
            const auto stats = transport.getStats(); other.appVersion = stats.peer.appVersion;
            other.state = stats.state == CollabTransport::State::connected ? c::ConnectionState::connected : c::ConnectionState::connecting;
        }
        std::sort (next.members.begin(), next.members.begin() + (int) next.memberCount,
                   [] (const auto& a, const auto& b) { return a.participantId < b.participantId; });
    }
    for (size_t n = 0; n < next.memberCount; ++n)
        if (next.members[n].participantId == snapshot.selfId)
        {
            auto& m = next.members[n]; m.deviceRate = me.deviceRate;
            m.inputLatencyUs = (uint32_t) (me.inputLatencyMs * 1000); m.outputLatencyUs = (uint32_t) (me.outputLatencyMs * 1000);
        }
    const auto sameMember = [] (const RosterMember& a, const RosterMember& b)
    {
        return a.participantId == b.participantId && a.streamEpoch == b.streamEpoch && a.kind == b.kind && a.state == b.state
            && a.capabilities == b.capabilities && a.deviceRate == b.deviceRate && a.inputLatencyUs == b.inputLatencyUs
            && a.outputLatencyUs == b.outputLatencyUs && a.nickname == b.nickname && a.appVersion == b.appVersion;
    };
    const bool changed = roster.session != next.session || roster.mode != next.mode || roster.memberCount != next.memberCount
        || ! std::equal (next.members.begin(), next.members.begin() + (int) next.memberCount, roster.members.begin(), sameMember);
    if (changed) applyRoster (next);

    if (snapshot.isHost && roomSnapshot.apiVersion == 2)
    {
        std::vector<CollabTransport::V2Peer> updated;
        for (const auto& m : roomSnapshot.members)
        {
            if (m.participantId == snapshot.selfId || m.kind != "native" || findMember (roster, m.participantId) == nullptr) continue;
            CollabTransport::V2Peer p; p.participantId = m.participantId; p.generation = m.connectionGeneration;
            for (const auto& old : nativePeers) if (old.participantId == p.participantId) p.candidates = old.candidates;
            for (const auto& fromServer : peers) if (fromServer.participantId == p.participantId) p.candidates = fromServer.candidates;
            updated.push_back (std::move (p));
        }
        nativePeers = std::move (updated);
        if (started && transport.getState() != CollabTransport::State::off && ! switchingTransport
            && transportV2 != (snapshot.roomMode == c::RoomMode::v2)) switchHostTransport();
        else if (transportV2 && ! switchingTransport)
        {
            if (changed) transport.updateRoster (roster);
            transport.updatePeers (nativePeers);
        }
    }
    refreshWebActions();
}

void CollabSession::applyRoster (const RosterSnapshot& next)
{
    messageThread();
    if (next.session != snapshot.session || next.revision < roster.revision) return;
    const auto previous = snapshot.people;
    const bool sameRoom = next.session == roster.session;
    roster = next; snapshot.rosterRevision = next.revision;
    snapshot.people = {}; snapshot.personCount = 0;
    std::array<c::ParticipantId, 2> seats {};
    if (sameRoom)
        for (const auto& p : previous)
            if (p.slot > 0 && findMember (next, p.participantId)) seats[(size_t) p.slot - 1] = p.participantId;
    const auto seat = [&] (c::ParticipantId pid)
    {
        if (pid == snapshot.selfId || pid == 0 || findMember (next, pid) == nullptr) return;
        if (std::find (seats.begin(), seats.end(), pid) != seats.end()) return;
        for (auto& empty : seats) if (empty == 0) { empty = pid; return; }
    };
    seat (1);
    for (size_t n = 0; n < next.memberCount; ++n) seat (next.members[n].participantId);
    for (size_t n = 0; n < next.memberCount; ++n)
    {
        const auto& m = next.members[n]; auto& p = snapshot.people[snapshot.personCount++];
        p.participantId = m.participantId; p.isSelf = m.participantId == snapshot.selfId; p.isHost = m.participantId == 1;
        p.kind = m.kind; p.state = m.state; p.streamEpoch = m.streamEpoch; p.nickname = m.nickname; p.appVersion = m.appVersion;
        for (size_t s = 0; s < seats.size(); ++s) if (seats[s] == m.participantId) p.slot = (int) s + 1;
        if (p.slot > 0)
        {
            const auto old = std::find_if (previous.begin(), previous.end(), [&] (const auto& value) { return value.participantId == p.participantId; });
            p.monitorVolume = sameRoom && old != previous.end() ? old->monitorVolume : settings.getCollabPersonVolume (p.nickname);
            if (! sameRoom || old == previous.end()) settings.setCollabPersonVolume (p.nickname, p.monitorVolume);
            audio.setSlotMonitorVolume ((unsigned) p.slot - 1, p.monitorVolume);
        }
    }
    audio.applyRoster (next, snapshot.selfId);
    settings.saveIfNeeded();
    publishPeople(); refreshWebActions(); sendChangeMessage();
}

void CollabSession::publishPeople()
{
    std::array<recording::TrackIdentity, 3> identities {};
    identities[0] = { snapshot.selfId, c::ParticipantKind::native, snapshot.myNick.toStdString() };
    snapshot.peerNick.clear();
    for (unsigned slot = 1; slot <= 2; ++slot)
    {
        const PersonSnapshot* occupant = nullptr;
        for (const auto& person : snapshot.people) if (person.slot == (int) slot) occupant = &person;
        if (occupant != nullptr)
        {
            identities[slot] = { occupant->participantId, occupant->kind, occupant->nickname.toStdString() };
            metadata[slot - 1].publish (snapshot.session, occupant->participantId, occupant->streamEpoch,
                                        occupant->kind, occupant->state, occupant->nickname);
            if (slot == 1) snapshot.peerNick = occupant->nickname;
        }
        else metadata[slot - 1].clear();
    }
    const bool open = snapshot.state != State::off && snapshot.state != State::error && snapshot.state != State::hostUnavailable;
    if (engine != nullptr) engine->setRecordingContext (open, identities);
}

void CollabSession::startTransport (const PeerInfo& local, const std::vector<CollabTransport::Address>& candidates)
{
    transportV2 = snapshot.roomMode == c::RoomMode::v2;
    const auto id = impl->generation.load(); const auto serial = ++transportSerial;
    auto me = local;
    if (roomSnapshot.apiVersion == 2) me.wireRate = snapshot.wireRate;
    if (transportV2)
    {
        if (! snapshot.isHost) nativePeers = { { 1, guestGeneration, candidates } };
        CollabTransport::V2Callbacks callbacks;
        callbacks.onAudio = [this, id] (const auto& h, const auto* p, auto time)
        { if (impl->generation.load() == id) audio.receiveV2 (h, p, time); };
        callbacks.onRoster = [this, id, serial] (const auto& r)
        { impl->complete (id, [this, serial, r] { if (serial == transportSerial) applyRoster (r); }); };
        callbacks.isRosterInstalled = [this, id] (const auto& r)
        { return impl->generation.load() == id && audio.hasInstalledRoster (r.session, r.revision); };
        callbacks.onPeerState = [this, id, serial] (auto pid, auto state)
        { impl->complete (id, [this, serial, pid, state] { if (serial == transportSerial) peerChanged (pid, state); }); };
        transport.configureV2 (snapshot.selfId, roster, nativePeers, std::move (callbacks));
    }
    if (snapshot.isHost) transport.startHost (snapshot.session, me);
    else transport.startGuest (snapshot.session, candidates, me);
    // v2's source clock must run even while waiting for its first native/web link.
    if (transportV2)
    {
        audio.setWireRate (snapshot.wireRate); audio.attach (transport); audioAttached = true;
    }
}

void CollabSession::switchHostTransport()
{
    if (switchingTransport) return;
    switchingTransport = true; ++transportSerial;
    if (audioAttached) audio.detach(); audioAttached = false;
    const auto id = impl->generation.load(); const int port = transport.getLocalPort();
    impl->enqueue ([this, id, port]
    {
        if (impl->generation.load() != id) return;
        transport.stop();
        const auto error = transport.open (port, options.loopbackOnly);
        impl->complete (id, [this, error]
        {
            switchingTransport = false;
            if (error.isNotEmpty()) { fail (error); return; }
            startTransport (localInfo (snapshot.myNick));
        });
    });
}

void CollabSession::peerChanged (c::ParticipantId pid, CollabTransport::State state)
{
    const auto* member = findMember (roster, pid);
    if (member == nullptr) return;
    if (state == CollabTransport::State::peerLeft || state == CollabTransport::State::disconnected)
    {
        if (! snapshot.isHost) { leave(); setState (State::off, ko ("방장이 방을 닫았습니다")); return; }
        for (const auto& m : roomSnapshot.members)
            if (m.participantId == pid && std::none_of (leftMembers.begin(), leftMembers.end(), [&] (const auto& left) { return left.participantId == pid; }))
                leftMembers.push_back ({ pid, m.connectionGeneration, "left", member->streamEpoch });
        if (pid == webConnection.guestId) endWeb (true);
        auto next = roster; size_t count = 0;
        for (size_t n = 0; n < next.memberCount; ++n) if (next.members[n].participantId != pid) next.members[count++] = next.members[n];
        next.memberCount = count; ++next.revision;
        applyRoster (next); transport.updateRoster (next);
        sendHeartbeat();
    }
    else if (state == CollabTransport::State::connected && snapshot.isHost)
    {
        if (member->state != c::ConnectionState::connected)
        {
            auto next = roster; ++next.revision;
            auto& changed = next.members[(size_t) (member - roster.members.data())];
            changed.state = c::ConnectionState::connected;
            const auto stats = transport.getStats (pid);
            changed.deviceRate = stats.peer.deviceRate; changed.inputLatencyUs = (uint32_t) (stats.peer.inputLatencyMs * 1000);
            changed.outputLatencyUs = (uint32_t) (stats.peer.outputLatencyMs * 1000);
            applyRoster (next); transport.updateRoster (next);
        }
        if (pid == webConnection.guestId) sendWebSignal (webConnection.generation, signal ("connected"));
        sendHeartbeat();
    }
}

void CollabSession::refreshWebActions()
{
    bool webPresent = webConnection.guestId != 0;
    for (const auto& person : snapshot.people) webPresent |= person.kind == c::ParticipantKind::web;
    snapshot.canCreateWebInvite = started && snapshot.isHost && snapshot.signalingConnected
        && snapshot.roomMode == c::RoomMode::v2 && snapshot.capabilities == c::allCapabilities
        && snapshot.personCount < 3 && ! webPresent && ! webAcceptPending;
    snapshot.kickableWebId = snapshot.isHost ? webConnection.guestId : 0;
    for (size_t n = 0; n < snapshot.webRequestCount; ++n)
    {
        snapshot.webRequests[n].canAccept = snapshot.canCreateWebInvite;
        snapshot.webRequests[n].canReject = snapshot.isHost && snapshot.signalingConnected;
    }
}

void CollabSession::startSignaling()
{
    const auto id = impl->generation.load();
    WebGuestSignaling::Callbacks callbacks;
    callbacks.onInvite = [this, id] (const auto& invite)
    {
        impl->complete (id, [this, invite]
        {
            snapshot.inviteGeneration = invite.inviteGeneration;
            snapshot.webInviteUrl = juce::String (c::webOrigin) + c::joinPath + "#v=2&code=" + snapshot.code
                + "&invite=" + invite.inviteToken + "&generation=" + juce::String (invite.inviteGeneration);
            snapshot.webInviteQrUrl = snapshot.webInviteUrl;
            sendChangeMessage();
        });
    };
    callbacks.onRequest = [this, id] (const auto& request)
    {
        impl->complete (id, [this, request]
        {
            for (size_t n = 0; n < snapshot.webRequestCount; ++n) if (snapshot.webRequests[n].requestId == request.requestId) return;
            if (snapshot.webRequestCount == snapshot.webRequests.size() || request.expiresAtMs <= juce::Time::currentTimeMillis()) return;
            snapshot.webRequests[snapshot.webRequestCount++] = { request.requestId, request.nickname, request.expiresAtMs };
            refreshWebActions(); sendChangeMessage();
        });
    };
    callbacks.onRequestEnded = [this, id] (const auto& request)
    { impl->complete (id, [this, request] { removeWebRequest (request); }); };
    callbacks.onAccepted = [this, id] (const WebGuestSignaling::Accepted& accepted)
    {
        // Only the token-free transport context crosses into the session/UI queue.
        impl->complete (id, [this, connection = accepted.connection] { webAcceptPending = false; startWeb (connection); });
    };
    callbacks.onSignal = [this, id] (uint32_t generation, const juce::var& incoming)
    {
        const auto copy = incoming.clone();
        impl->enqueue ([this, id, generation, copy]
        {
            if (impl->generation.load() != id || webGeneration.load() != generation) return;
            const auto type = copy["type"].toString();
            if (type == "answer") impl->web->description (generation, type, copy["sdp"].toString());
            else if (type == "candidate") impl->web->candidate (generation, copy["candidate"].toString(), copy["sdpMid"].toString(),
                                                                 copy["sdpMLineIndex"].isVoid() ? -1 : (int) copy["sdpMLineIndex"]);
            else if (type == "endOfCandidates") impl->web->endCandidates (generation);
        });
    };
    callbacks.onEnded = [this, id] (uint32_t generation, const auto& reason)
    {
        impl->complete (id, [this, generation, reason]
        {
            if (generation == webConnection.generation && generation != 0) endWeb (false);
            if (reason == "closed")
            {
                snapshot.signalingConnected = false; snapshot.webRequestCount = 0;
                snapshot.webRequests = {};
                snapshot.webInviteUrl.clear(); snapshot.webInviteQrUrl.clear();
                refreshWebActions(); sendChangeMessage();
            }
        });
    };
    callbacks.onError = [this, id] (int status, const auto&)
    {
        impl->complete (id, [this, status]
        {
            webAcceptPending = false;
            snapshot.message = CollabRoom::errorMessage (status);
            refreshWebActions(); sendChangeMessage();
        });
    };
    impl->enqueue ([this, id, callbacks = std::move (callbacks)] () mutable
    {
        if (impl->generation.load() != id) return;
        impl->signaling.startHost (impl->code, impl->token, std::move (callbacks));
        impl->complete (id, [this] { snapshot.signalingConnected = true; refreshWebActions(); sendChangeMessage(); });
    });
}

void CollabSession::removeWebRequest (const juce::String& requestId)
{
    const auto id = requestId; // may refer to the array being compacted
    size_t count = 0;
    for (size_t n = 0; n < snapshot.webRequestCount; ++n)
        if (snapshot.webRequests[n].requestId != id) snapshot.webRequests[count++] = snapshot.webRequests[n];
    for (size_t n = count; n < snapshot.webRequests.size(); ++n) snapshot.webRequests[n] = {};
    snapshot.webRequestCount = count; refreshWebActions(); sendChangeMessage();
}

void CollabSession::createWebInvite()
{
    messageThread();
    if (! snapshot.canCreateWebInvite) return;
    const auto id = impl->generation.load();
    impl->enqueue ([this, id] { if (impl->generation.load() == id) impl->signaling.createInvite(); });
}
void CollabSession::acceptWebRequest (const juce::String& requestId)
{
    messageThread();
    for (size_t n = 0; n < snapshot.webRequestCount; ++n)
        if (snapshot.webRequests[n].requestId == requestId && snapshot.webRequests[n].canAccept)
        {
            acceptingNick = snapshot.webRequests[n].nickname; webAcceptPending = true;
            refreshWebActions(); sendChangeMessage();
            const auto id = impl->generation.load();
            impl->enqueue ([this, id, requestId] { if (impl->generation.load() == id) impl->signaling.accept (requestId); });
            return;
        }
}
void CollabSession::rejectWebRequest (const juce::String& requestId)
{
    messageThread();
    for (size_t n = 0; n < snapshot.webRequestCount; ++n)
        if (snapshot.webRequests[n].requestId == requestId && snapshot.webRequests[n].canReject)
        {
            const auto id = impl->generation.load();
            impl->enqueue ([this, id, requestId] { if (impl->generation.load() == id) impl->signaling.reject (requestId); });
            return;
        }
}
void CollabSession::kickWebGuest (c::ParticipantId pid)
{
    messageThread();
    if (pid == 0 || pid != snapshot.kickableWebId) return;
    const auto id = impl->generation.load(); const auto generation = webConnection.generation;
    impl->enqueue ([this, id, pid, generation]
    { if (impl->generation.load() == id) impl->signaling.kick (pid, generation); });
}
void CollabSession::sendWebSignal (uint32_t generation, const juce::var& value)
{
    const auto id = impl->generation.load(); const auto copy = value.clone();
    impl->enqueue ([this, id, generation, copy]
    { if (impl->generation.load() == id && webGeneration.load() == generation) impl->signaling.sendSignal (generation, copy); });
}

void CollabSession::startWeb (c::WebConnection connection)
{
    if (! snapshot.isHost || ! transportV2 || connection.session != snapshot.session) return;
    webConnection = connection; webGeneration.store (connection.generation);
    if (findMember (roster, connection.guestId) == nullptr && roster.memberCount < 3)
    {
        auto next = roster; ++next.revision;
        auto& m = next.members[next.memberCount++];
        m.participantId = connection.guestId; m.streamEpoch = connection.guestEpoch; m.kind = c::ParticipantKind::web;
        m.nickname = acceptingNick; m.capabilities = c::allCapabilities; m.deviceRate = connection.wireRate;
        applyRoster (next); transport.updateRoster (next);
        CollabRoom::Member serverMember; serverMember.participantId = m.participantId;
        serverMember.connectionGeneration = connection.generation; serverMember.streamEpoch = m.streamEpoch;
        serverMember.kind = "web"; serverMember.nick = m.nickname; serverMember.state = "connecting";
        serverMember.capabilities = CollabRoom::ClientInfo {}.capabilities;
        roomSnapshot.members.push_back (serverMember);
    }
    connection.rosterRevision = roster.revision;
    refreshWebActions(); sendChangeMessage(); sendHeartbeat();
    const auto id = impl->generation.load();
    WebGuestTransport::Callbacks callbacks;
    callbacks.onLocalDescription = [this, id] (uint32_t g, const auto& type, const auto& sdp)
    {
        if (impl->generation.load() != id || webGeneration.load() != g) return;
        auto value = signal ("offer"); value.getDynamicObject()->setProperty ("type", type); value.getDynamicObject()->setProperty ("sdp", sdp);
        sendWebSignal (g, value);
    };
    callbacks.onLocalCandidate = [this, id] (uint32_t g, const auto& candidate, const auto& mid, int line)
    {
        if (impl->generation.load() != id || webGeneration.load() != g) return;
        auto value = signal ("candidate"); auto* object = value.getDynamicObject();
        object->setProperty ("candidate", candidate); object->setProperty ("sdpMid", mid.isEmpty() ? juce::var() : juce::var (mid));
        object->setProperty ("sdpMLineIndex", line < 0 ? juce::var() : juce::var (line)); sendWebSignal (g, value);
    };
    callbacks.onGatheringComplete = [this, id] (uint32_t g)
    { if (impl->generation.load() == id && webGeneration.load() == g) sendWebSignal (g, signal ("endOfCandidates")); };
    callbacks.onPacket = [this, id] (uint32_t g, const uint8_t* bytes, size_t size, uint64_t arrival)
    {
        if (impl->generation.load() != id || webGeneration.load() != g) return;
        c::HeaderV2 h; const uint8_t* payload = nullptr;
        if (readPacketV2 (bytes, size, h, payload)) transport.receiveWebPacket (h, payload, arrival);
    };
    callbacks.onState = [this, id] (uint32_t g, WebGuestTransport::State state)
    {
        impl->complete (id, [this, g, state]
        {
            if (webGeneration.load() != g) return;
            if (state == WebGuestTransport::State::open)
            {
                const auto endpoint = impl->web;
                transport.setWebLink (webConnection.guestId, g, {
                    [endpoint, g] (const uint8_t* p, size_t n) { return endpoint->send (g, p, n, true); },
                    [endpoint, g] (const uint8_t* p, size_t n) { return endpoint->send (g, p, n, false); } });
            }
            else if (state == WebGuestTransport::State::closed || state == WebGuestTransport::State::failed) endWeb (true);
        });
    };
    impl->enqueue ([this, id, connection, callbacks = std::move (callbacks)] () mutable
    {
        if (impl->generation.load() == id && webGeneration.load() == connection.generation)
        {
            auto webOptions = impl->webOptions;
            webOptions.loopbackOnly = options.loopbackOnly;
            impl->web->start (connection, webOptions, std::move (callbacks));
        }
    });
}

void CollabSession::endWeb (bool notifyServer)
{
    if (webConnection.guestId == 0) return;
    const auto old = webConnection; webGeneration.store (0); webConnection = {};
    transport.setWebLink (old.guestId, old.generation, {});
    auto next = roster; size_t count = 0;
    for (size_t n = 0; n < roster.memberCount; ++n) if (roster.members[n].participantId != old.guestId) next.members[count++] = roster.members[n];
    next.memberCount = count; ++next.revision; applyRoster (next); transport.updateRoster (next);
    roomSnapshot.members.erase (std::remove_if (roomSnapshot.members.begin(), roomSnapshot.members.end(),
        [&] (const auto& m) { return m.participantId == old.guestId; }), roomSnapshot.members.end());
    leftMembers.erase (std::remove_if (leftMembers.begin(), leftMembers.end(),
        [&] (const auto& m) { return m.participantId == old.guestId; }), leftMembers.end());
    // Web leave/kick already releases its server reservation. Keep a local tombstone
    // until heartbeat catches up, but do not report a now-invalid generation again.
    leftMembers.push_back ({ old.guestId, 0, "left", old.guestEpoch });
    const auto id = impl->generation.load();
    impl->enqueue ([this, id, old, notifyServer]
    {
        if (impl->generation.load() != id) return;
        impl->web->close();
        if (notifyServer) impl->signaling.leave (old.generation);
    });
    refreshWebActions(); sendHeartbeat(); sendChangeMessage();
}

void CollabSession::setPersonMonitorVolume (c::ParticipantId pid, float volume)
{
    messageThread();
    const auto id = impl->generation.load();
    impl->complete (id, [this, pid, volume]
    {
        for (auto& person : snapshot.people)
            if (person.participantId == pid && person.slot > 0)
            {
                person.monitorVolume = juce::jlimit (0.0f, 1.0f, std::isfinite (volume) ? volume : 0.0f);
                audio.setSlotMonitorVolume ((unsigned) person.slot - 1, person.monitorVolume);
                settings.setCollabPersonVolume (person.nickname, person.monitorVolume); settings.saveIfNeeded();
                sendChangeMessage(); return;
            }
    });
}

CollabSession::Snapshot CollabSession::getSnapshot() const
{
    messageThread();
    auto result = snapshot;
    result.myNick = settings.getCollabNick();
    const auto local = localInfo (snapshot.myNick);
    for (auto& person : result.people)
        if (person.slot > 0)
        {
            const auto stats = audio.getStats ((unsigned) person.slot - 1);
            const auto link = transportV2 ? transport.getStats (snapshot.isHost ? person.participantId : 1) : transport.getStats();
            person.rttMs = link.rttMs; person.bufferMs = stats.targetMs; person.lossPercent = stats.lossPercent10s;
            person.estimatedOneWayMs = link.rttMs * 0.5 + stats.targetMs + 0.5 * packetDurationMs (snapshot.wireRate);
            if (const auto* m = findMember (roster, person.participantId))
                person.estimatedOneWayMs += m->inputLatencyUs / 1000.0
                    + (m->deviceRate != snapshot.wireRate ? 16000.0 / m->deviceRate : 0.0);
            person.estimatedOneWayMs += 16000.0 / snapshot.wireRate + local.outputLatencyMs;
            if (! transportV2) person.estimatedOneWayMs = transport.estimatedOneWayMs();
            if (engine != nullptr)
            {
                const auto peak = engine->readCollabMeter ((unsigned) person.slot - 1);
                person.peakLeft = peak.left; person.peakRight = peak.right;
            }
        }
    if (transportV2)
    {
        result.rttMs = result.estimatedOneWayMs = result.bufferMs = result.lossPercent = 0;
        for (const auto& person : result.people) if (person.slot == 1)
        {
            result.rttMs = person.rttMs; result.estimatedOneWayMs = person.estimatedOneWayMs;
            result.bufferMs = person.bufferMs; result.lossPercent = person.lossPercent;
        }
    }
    else if (started && (result.state == State::connected || result.state == State::disconnected))
    {
        result.rttMs = transport.getStats().rttMs; result.estimatedOneWayMs = transport.estimatedOneWayMs();
        const auto stats = audio.getStats(); result.bufferMs = stats.targetMs; result.lossPercent = stats.lossPercent10s;
    }
    return result;
}

void CollabSession::sendHeartbeat()
{
    if (heartbeatStopped || switchingTransport) return;
    if (heartbeatPending) { heartbeatDirty = true; return; }
    heartbeatPending = true; lastHeartbeat = nowMs();
    const auto id = impl->generation.load();
    const bool connected = snapshot.state == State::connected;
    heartbeatConnected = connected;
    const bool knock = punching && ! connected;
    std::vector<CollabRoom::MemberState> states;
    for (const auto& left : leftMembers) if (left.connectionGeneration != 0) states.push_back (left);
    for (const auto& m : roomSnapshot.members)
        if (m.participantId != snapshot.selfId)
            if (const auto* active = findMember (roster, m.participantId))
                states.push_back ({ m.participantId, m.connectionGeneration,
                    active->state == c::ConnectionState::connected ? "connected" : "connecting", active->streamEpoch });
    impl->enqueue ([this, id, connected, knock, states]
    {
        if (impl->generation.load() != id) return;
        CollabRoom::BeatV2 reply;
        if (impl->apiV2) reply = impl->room->heartbeatV2 (impl->code, impl->token, connected, states);
        else static_cast<CollabRoom::Beat&> (reply) = impl->room->heartbeat (impl->code, impl->token, connected);
        // the joined guest's public address: knock on it through this side's router until its hello gets in
        if (knock && reply.ok() && ! reply.guest.empty()) transport.punch (reply.guest, options.punchMs);
        impl->complete (id, [this, reply, knock]
        {
            heartbeatPending = false;
            if (reply.ok())
            {
                heartbeatFailures = 0;
                if (reply.room.apiVersion == 2)
                {
                    applyRoom (reply.room, reply.peers);
                    leftMembers.erase (std::remove_if (leftMembers.begin(), leftMembers.end(), [&] (const auto& left)
                    { return std::none_of (reply.room.members.begin(), reply.room.members.end(), [&] (const auto& m) { return m.participantId == left.participantId; }); }), leftMembers.end());
                    if (snapshot.roomMode == c::RoomMode::legacy2 && knock && ! reply.peers.empty())
                        transport.punch (reply.peers.front().candidates, options.punchMs);
                }
                if (heartbeatDirty) { heartbeatDirty = false; sendHeartbeat(); }
                return;
            }
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
    const auto utc = juce::Time::currentTimeMillis();
    for (size_t n = snapshot.webRequestCount; n > 0; --n)
        if (snapshot.webRequests[n - 1].expiresAtMs <= utc) removeWebRequest (snapshot.webRequests[n - 1].requestId);
    if (! started || switchingTransport) return;
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
        if (snapshot.state != State::connected || (! transportV2 && snapshot.peerNick != stats.peer.nickname))
        {
            if (! transportV2)
            {
                snapshot.peerNick = stats.peer.nickname;
                applyRoom (roomSnapshot);
                auto next = roster; ++next.revision;
                for (size_t n = 0; n < next.memberCount; ++n)
                    if (next.members[n].participantId != snapshot.selfId)
                    {
                        auto& m = next.members[n]; m.state = c::ConnectionState::connected;
                        m.deviceRate = stats.peer.deviceRate; m.inputLatencyUs = (uint32_t) (stats.peer.inputLatencyMs * 1000);
                        m.outputLatencyUs = (uint32_t) (stats.peer.outputLatencyMs * 1000);
                    }
                applyRoster (next);
            }
            setState (State::connected);
        }
    }
    else if (stats.state == CollabTransport::State::disconnected)
    {
        if (snapshot.state != State::disconnected) setState (State::disconnected);
    }
    else if (stats.state == CollabTransport::State::peerLeft)
    {
        if (! snapshot.isHost) { leave(); setState (State::off, ko ("방장이 방을 닫았습니다")); return; }
        if (snapshot.state != State::hostWaiting)
        {
            if (roomSnapshot.apiVersion == 2)
            {
                for (const auto& m : roomSnapshot.members)
                    if (m.participantId != snapshot.selfId) leftMembers.push_back ({ m.participantId, m.connectionGeneration, "left", m.streamEpoch });
            }
            snapshot.peerNick.clear(); applyRoom (roomSnapshot); setState (State::hostWaiting);
        }
    }
    else if (transportV2 && snapshot.isHost && stats.state == CollabTransport::State::waiting && snapshot.state != State::hostWaiting)
        setState (State::hostWaiting);
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
            || now - lastHeartbeat >= ((punching && snapshot.state == State::hostWaiting)
                || (roomSnapshot.apiVersion == 2 && (snapshot.personCount < 3
                    || std::any_of (snapshot.people.begin(), snapshot.people.end(), [] (const auto& p)
                       { return ! p.isSelf && p.kind == c::ParticipantKind::native && p.state == c::ConnectionState::connecting; })))
                ? juce::jmin (options.punchHeartbeatMs, options.heartbeatIntervalMs) : options.heartbeatIntervalMs)) sendHeartbeat();
        if ((mapped || webMapped) && ! renewPending && now - lastRenew >= options.renewIntervalMs)
        {
            renewPending = true; lastRenew = now;
            const auto id = impl->generation.load();
            impl->enqueue ([this, id, renewNative = mapped, renewWeb = webMapped]
            {
                if (impl->generation.load() != id) return;
                if (renewNative) impl->upnp->renew();
                if (renewWeb) impl->webUpnp->renew();
                impl->complete (id, [this] { renewPending = false; });
            });
        }
    }
    if (now - lastNotification >= 250) { lastNotification = now; sendChangeMessage(); }
}
}
