#include "WebGuestTransport.h"
#include <rtc/rtc.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace gocue::livemix::collab
{
namespace c = contracts;
namespace
{
uint64_t nowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint64_t readLE(const uint8_t* p, size_t bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) value |= uint64_t(p[i]) << (8 * i);
    return value;
}
// Only the envelope and authenticated link binding belong here. R3 validates
// kinds, flags, PCM content, roster revisions and stream epochs.
// Incoming (web -> host): origin = linkSender = this web guest.
// Outgoing audio (host -> web, per-person downlink, contract 2.4): linkSender = host and origin = the speaker,
// the host or a forwarded native participant, never the web guest itself. Outgoing control is the host's own.
enum class Envelope { incoming, outgoingAudio, outgoingControl };
bool envelope(const void* data, size_t bytes, const c::WebConnection& connection, Envelope direction)
{
    if (data == nullptr || bytes < c::v2HeaderBytes || bytes > c::maxPacketBytes) return false;
    const auto* p = static_cast<const uint8_t*>(data);
    if (readLE(p, 4) != c::packetMagic || p[c::offset::version] != c::packetVersion
        || bytes != c::v2HeaderBytes + readLE(p + c::offset::payloadBytes, 2)
        || readLE(p + c::offset::session, 8) != connection.session) return false;
    const auto origin = readLE(p + c::offset::origin, 4), linkSender = readLE(p + c::offset::linkSender, 4);
    switch (direction)
    {
        case Envelope::incoming: return origin == connection.guestId && linkSender == connection.guestId;
        case Envelope::outgoingAudio:
            return linkSender == connection.hostId && origin != c::noParticipant && origin != connection.guestId;
        case Envelope::outgoingControl: return origin == connection.hostId && linkSender == connection.hostId;
    }
    return false;
}
}

uint64_t WebGuestTransport::AudioQueue::push(const void* data, size_t bytes, uint64_t now)
{
    uint64_t dropped = 0;
    if (count == capacity) { pop(); ++dropped; }
    auto& p = packets[(first + count++) % capacity];
    p.bytes = bytes;
    p.queuedAt = now;
    std::memcpy(p.data.data(), data, bytes);
    return dropped;
}
uint64_t WebGuestTransport::AudioQueue::trim(size_t buffered, uint64_t now)
{
    uint64_t dropped = 0;
    while (count != 0 && (buffered + count * packetBytes > capacity * packetBytes
                          || now - packets[first].queuedAt >= maxAgeNs))
    {
        pop();
        ++dropped;
    }
    return dropped;
}
WebGuestTransport::Packet WebGuestTransport::AudioQueue::pop()
{
    auto packet = packets[first];
    first = (first + 1) % capacity;
    --count;
    return packet;
}

struct WebGuestTransport::Impl
{
    struct Connection
    {
        explicit Connection(c::WebConnection value, Options opts, Callbacks cb)
            : context(value), options(opts), callbacks(std::move(cb))
        {
            pendingCandidates.reserve(c::maxCandidatesPerSide);
        }
        const c::WebConnection context;
        const Options options;
        const Callbacks callbacks;
        // No callback captures WebGuestTransport or Impl. close first closes this
        // gate, then joins/resetCallbacks without holding it (lock-order matters).
        std::recursive_mutex callbackGate;
        std::atomic<bool> active { true };
        std::atomic<State> state { State::connecting };
        std::atomic<uint64_t> sent { 0 }, received { 0 }, dropped { 0 }, buffered { 0 };
        std::atomic<bool> ordered { false };
        std::atomic<int> retransmits { 0 };
        bool gatheringComplete = false; // callbackGate
        bool remoteSet = false, remoteEnded = false; // control thread
        juce::String remoteSdp;
        std::string remoteMid;
        std::vector<rtc::Candidate> pendingCandidates;
        std::shared_ptr<rtc::PeerConnection> peer;
        std::shared_ptr<rtc::DataChannel> channel;
        std::thread sender;
        mutable std::mutex queueMutex;
        std::condition_variable wake;
        AudioQueue audio;
        std::array<Packet, 16> control {};
        size_t controlFirst = 0, controlCount = 0, controlBytes = 0;

        bool live() const
        {
            const auto s = state.load();
            return active.load() && (s == State::connecting || s == State::open);
        }
        void changeState(State next)
        {
            std::lock_guard<std::recursive_mutex> lock(callbackGate);
            if (!live() || state.load() == next) return;
            state.store(next);
            wake.notify_one();
            if (callbacks.onState) callbacks.onState(context.generation, next);
        }
        void runSender()
        {
            // This is the only production call site of DataChannel::send.
            while (live())
            {
                Packet packet;
                bool havePacket = false;
                try
                {
                    const auto amount = channel->bufferedAmount();
                    buffered.store(amount);
                    {
                        std::unique_lock<std::mutex> lock(queueMutex);
                        dropped.fetch_add(audio.trim(amount, nowNs()));
                        if (state.load() == State::open)
                        {
                            // Controls get the next available SCTP space. They
                            // cannot grow SCTP's queue without bound either.
                            if (controlCount != 0)
                            {
                                const auto& head = control[controlFirst];
                                if (amount + head.bytes <= AudioQueue::capacity * AudioQueue::packetBytes)
                                {
                                    packet = head;
                                    controlFirst = (controlFirst + 1) % control.size();
                                    --controlCount;
                                    controlBytes -= packet.bytes;
                                    havePacket = true;
                                }
                            }
                            else if (audio.count != 0)
                            {
                                packet = audio.pop();
                                havePacket = true;
                            }
                        }
                        if (!havePacket) wake.wait_for(lock, std::chrono::milliseconds(2));
                    }
                    if (havePacket && live())
                    {
                        // false means SCTP queued it. Sending it again would
                        // duplicate audio/control messages.
                        channel->send(reinterpret_cast<const rtc::byte*>(packet.data.data()), packet.bytes);
                        sent.fetch_add(1);
                        buffered.store(channel->bufferedAmount());
                    }
                }
                catch (const std::exception&)
                {
                    if (havePacket) dropped.fetch_add(1);
                    changeState(State::failed);
                }
            }
            std::lock_guard<std::mutex> lock(queueMutex);
            dropped.fetch_add(audio.count + controlCount);
            audio.count = controlCount = controlBytes = 0;
        }
    };

    std::shared_ptr<Connection> current;
    std::shared_ptr<Connection> get() const { return std::atomic_load(&current); }

    template<typename Fn>
    static void callback(const std::weak_ptr<Connection>& weak, Fn&& fn)
    {
        if (auto connection = weak.lock())
        {
            std::lock_guard<std::recursive_mutex> lock(connection->callbackGate);
            if (connection->live()) fn(*connection);
        }
    }

    bool enqueue(uint32_t generation, const void* data, size_t bytes, bool audioPacket)
    {
        auto connection = get();
        if (!connection) return false;
        auto reject = [&] { connection->dropped.fetch_add(1); return false; };
        if (generation != connection->context.generation || !connection->live()
            || !envelope(data, bytes, connection->context,
                         audioPacket ? Envelope::outgoingAudio : Envelope::outgoingControl)) return reject();
        const auto* p = static_cast<const uint8_t*>(data);
        if ((p[c::offset::kind] == static_cast<uint8_t>(c::Kind::audio)) != audioPacket) return reject();
        if (audioPacket && (bytes != AudioQueue::packetBytes
            || readLE(p + c::offset::flags, 2) != static_cast<uint16_t>(c::Flags::stereoPcm24)
            || readLE(p + c::offset::frames, 2) != c::packetFrames)) return reject();
        {
            std::lock_guard<std::mutex> lock(connection->queueMutex);
            if (!connection->live()) return reject();
            if (audioPacket)
            {
                const auto now = nowNs();
                connection->dropped.fetch_add(connection->audio.push(data, bytes, now));
                connection->dropped.fetch_add(connection->audio.trim(connection->buffered.load(), now));
            }
            else
            {
                if (connection->controlCount == connection->control.size()) return reject();
                auto& packet = connection->control[(connection->controlFirst + connection->controlCount++)
                                                   % connection->control.size()];
                std::memcpy(packet.data.data(), data, bytes);
                packet.bytes = bytes;
                packet.queuedAt = nowNs();
                connection->controlBytes += bytes;
            }
        }
        connection->wake.notify_one();
        return true;
    }
};

WebGuestTransport::WebGuestTransport() : impl(std::make_unique<Impl>()) {}
WebGuestTransport::~WebGuestTransport() { close(); }

bool WebGuestTransport::isPublicIpv4(const juce::String& text)
{
    const auto parts = juce::StringArray::fromTokens(text, ".", "");
    if (parts.size() != 4) return false;
    for (const auto& part : parts)
        if (part.isEmpty() || part.length() > 3 || !part.containsOnly("0123456789")
            || part.getIntValue() > 255 || juce::String(part.getIntValue()) != part) return false;
    const int a = parts[0].getIntValue(), b = parts[1].getIntValue(), c = parts[2].getIntValue();
    // Exclude private/shared, link-local, loopback, documentation, benchmark and reserved networks.
    return a != 0 && a != 10 && a != 127 && a < 224
        && !(a == 100 && b >= 64 && b <= 127) && !(a == 169 && b == 254)
        && !(a == 172 && b >= 16 && b <= 31)
        && !(a == 192 && (b == 168 || (b == 0 && (c == 0 || c == 2)) || (b == 88 && c == 99)))
        && !(a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100)))
        && !(a == 203 && b == 0 && c == 113);
}

void WebGuestTransport::startAcceptedGuest(const c::WebConnection& context, Options options, Callbacks callbacks)
{
    close();
    options.loopbackOnly = options.loopbackOnly
        || juce::SystemStats::getEnvironmentVariable("LIVEMIX_COLLAB_LOOPBACK", {}) == "1";
    auto connection = std::make_shared<Impl::Connection>(context, options, std::move(callbacks));
    std::atomic_store(&impl->current, connection);
    const std::weak_ptr<Impl::Connection> weak(connection);
    if (connection->callbacks.onState) connection->callbacks.onState(context.generation, State::connecting);
    if (context.generation == 0 || context.session == 0 || context.hostId != c::hostParticipant
        || context.guestId <= c::hostParticipant || (context.wireRate != 44100 && context.wireRate != 48000))
    {
        connection->changeState(State::failed);
        return;
    }
    // 0.24.6 binds libjuice's UDP socket during setLocalDescription's gathering, not in the constructor.
    // Hold callbacks until that succeeds so a busy mapped port cannot publish an obsolete offer.
    std::unique_lock<std::recursive_mutex> startup(connection->callbackGate);
    try
    {
        rtc::Configuration configuration;
        configuration.disableAutoNegotiation = true;
        configuration.maxMessageSize = c::maxPacketBytes;
        if (options.localPort != 0)
        {
            configuration.portRangeBegin = configuration.portRangeEnd = options.localPort;
            // UPnP forwards IPv4. Windows can bind IPv6 on a port already occupied by an IPv4 socket;
            // bind IPv4 explicitly so that collision takes the unrestricted-port retry below.
            configuration.bindAddress = "0.0.0.0";
        }
        if (!options.loopbackOnly)
            for (const auto* server : c::iceServers) configuration.iceServers.emplace_back(server);
        auto peer = std::make_shared<rtc::PeerConnection>(configuration);
        connection->peer = peer;
        peer->onLocalDescription([weak](rtc::Description description)
        {
            Impl::callback(weak, [&](auto& value)
            {
                if (value.callbacks.onLocalDescription)
                    value.callbacks.onLocalDescription(value.context.generation, description.typeString(),
                                                       std::string(description));
            });
        });
        peer->onLocalCandidate([weak, mappedCandidateSent = false](rtc::Candidate candidate) mutable
        {
            Impl::callback(weak, [&](auto& value)
            {
                if (value.options.loopbackOnly && candidate.type() != rtc::Candidate::Type::Host) return;
                if (value.callbacks.onLocalCandidate)
                {
                    value.callbacks.onLocalCandidate(value.context.generation, candidate.candidate(), candidate.mid(), 0);
                    const auto& opts = value.options;
                    if (!mappedCandidateSent && opts.localPort != 0 && isPublicIpv4(opts.externalIp)
                        && candidate.type() == rtc::Candidate::Type::Host
                        && candidate.transportType() == rtc::Candidate::TransportType::Udp
                        && candidate.family() == rtc::Candidate::Family::Ipv4 && candidate.port() == opts.localPort
                        && (opts.lanIp.isEmpty() || candidate.address() == opts.lanIp.toStdString()))
                    {
                        mappedCandidateSent = true;
                        // RFC 8445: srflx type preference 100, retaining the base candidate's local preference/component.
                        const auto priority = (100u << 24) | (candidate.priority() & 0x00ffffffu);
                        const auto mapped = "candidate:livemixupnp 1 udp " + juce::String(priority) + " "
                            + opts.externalIp + " " + juce::String(opts.localPort) + " typ srflx raddr "
                            + juce::String(*candidate.address()) + " rport " + juce::String(opts.localPort);
                        value.callbacks.onLocalCandidate(value.context.generation, mapped, candidate.mid(), 0);
                    }
                }
            });
        });
        peer->onGatheringStateChange([weak](rtc::PeerConnection::GatheringState state)
        {
            Impl::callback(weak, [&](auto& value)
            {
                if (state == rtc::PeerConnection::GatheringState::Complete && !value.gatheringComplete)
                {
                    value.gatheringComplete = true;
                    if (value.callbacks.onGatheringComplete) value.callbacks.onGatheringComplete(value.context.generation);
                }
            });
        });
        peer->onStateChange([weak](rtc::PeerConnection::State state)
        {
            Impl::callback(weak, [&](auto& value)
            {
                if (state == rtc::PeerConnection::State::Failed) value.changeState(State::failed);
                else if (state == rtc::PeerConnection::State::Closed || state == rtc::PeerConnection::State::Disconnected)
                    value.changeState(State::closed);
            });
        });
        // The accepted guest must use the single host-created in-band channel.
        peer->onDataChannel([weak](std::shared_ptr<rtc::DataChannel>)
        {
            Impl::callback(weak, [](auto& value) { value.changeState(State::failed); });
        });
        rtc::DataChannelInit init;
        init.protocol = c::dataChannelProtocol;
        init.reliability.unordered = true;
        init.reliability.maxRetransmits = 0u;
        auto channel = peer->createDataChannel(c::dataChannelLabel, init);
        connection->channel = channel;
        const std::weak_ptr<rtc::DataChannel> weakChannel(channel);
        channel->onOpen([weak, weakChannel]
        {
            Impl::callback(weak, [&](auto& value)
            {
                const auto opened = weakChannel.lock();
                if (!opened) return;
                const auto reliability = opened->reliability();
                value.ordered.store(!reliability.unordered);
                value.retransmits.store(reliability.maxRetransmits ? int(*reliability.maxRetransmits) : -1);
                const bool valid = opened->label() == c::dataChannelLabel && opened->protocol() == c::dataChannelProtocol
                    && reliability.unordered && reliability.maxRetransmits == 0u && !reliability.maxPacketLifeTime;
                value.changeState(valid ? State::open : State::failed);
            });
        });
        channel->onClosed([weak] { Impl::callback(weak, [](auto& value) { value.changeState(State::closed); }); });
        channel->onError([weak](std::string) { Impl::callback(weak, [](auto& value) { value.changeState(State::failed); }); });
        channel->onMessage([weak](rtc::message_variant message)
        {
            Impl::callback(weak, [&](auto& value)
            {
                const auto* binary = std::get_if<rtc::binary>(&message);
                if (value.state.load() != State::open || !binary
                    || !envelope(binary->data(), binary->size(), value.context, Envelope::incoming))
                {
                    value.dropped.fetch_add(1);
                    return;
                }
                value.received.fetch_add(1);
                if (value.callbacks.onPacket)
                    value.callbacks.onPacket(value.context.generation, reinterpret_cast<const uint8_t*>(binary->data()),
                                             binary->size(), nowNs());
            });
        });
        peer->setLocalDescription(rtc::Description::Type::Offer);
        connection->sender = std::thread([connection] { connection->runSender(); });
    }
    catch (const std::exception&)
    {
        if (options.localPort == 0) { connection->changeState(State::failed); return; }
        // Retire queued callbacks without reporting a failed/closed guest. close() still releases this attempt.
        connection->state.store(State::closed);
        startup.unlock();
        options.localPort = 0;
        options.externalIp.clear();
        options.lanIp.clear();
        startAcceptedGuest(context, options, connection->callbacks); // one retry with the original ICE configuration
    }
}

void WebGuestTransport::applyRemoteDescription(uint32_t generation, const juce::String& type, const juce::String& sdp)
{
    auto connection = impl->get();
    if (!connection || !connection->live()) return;
    if (generation != connection->context.generation) { connection->dropped.fetch_add(1); return; }
    if (type != "answer" || sdp.isEmpty() || sdp.getNumBytesAsUTF8() > c::maxSdpBytes)
    {
        connection->changeState(State::failed);
        return;
    }
    if (connection->remoteSet)
    {
        if (connection->remoteSdp != sdp) connection->changeState(State::failed); // no renegotiation
        return;
    }
    try
    {
        rtc::Description description(sdp.toStdString(), "answer");
        if (!description.hasApplication() || description.mediaCount() != 1)
            throw std::invalid_argument("Expected one SCTP application");
        if (connection->options.loopbackOnly)
            for (const auto& candidate : description.candidates())
                if (candidate.type() != rtc::Candidate::Type::Host)
                    throw std::invalid_argument("Expected host candidate");
        if (connection->remoteEnded) description.endCandidates();
        connection->remoteMid = description.application()->mid();
        connection->peer->setRemoteDescription(description);
        connection->remoteSet = true;
        connection->remoteSdp = sdp;
        for (const auto& candidate : connection->pendingCandidates)
        {
            if (candidate.mid() != connection->remoteMid) throw std::invalid_argument("Candidate mid mismatch");
            connection->peer->addRemoteCandidate(candidate);
        }
        connection->pendingCandidates.clear();
    }
    catch (const std::exception&) { connection->changeState(State::failed); }
}

void WebGuestTransport::addRemoteCandidate(uint32_t generation, const juce::String& text, const juce::String& mid, int index)
{
    auto connection = impl->get();
    if (!connection || !connection->live()) return;
    if (generation != connection->context.generation) { connection->dropped.fetch_add(1); return; }
    if (text.isEmpty() || text.getNumBytesAsUTF8() > c::maxCandidateBytes
        || (index != 0 && index != -1) || (mid.isEmpty() && index != 0))
    {
        connection->dropped.fetch_add(1);
        return;
    }
    try
    {
        const auto local = connection->peer->localDescription();
        rtc::Candidate candidate(text.toStdString(), mid.isEmpty() && local ? local->bundleMid() : mid.toStdString());
        if (connection->options.loopbackOnly && candidate.type() != rtc::Candidate::Type::Host)
        {
            connection->dropped.fetch_add(1);
            return;
        }
        if (!connection->remoteSet)
        {
            auto& pending = connection->pendingCandidates;
            if (std::find(pending.begin(), pending.end(), candidate) != pending.end()) return;
            if (pending.size() == c::maxCandidatesPerSide) { connection->dropped.fetch_add(1); return; }
            pending.push_back(std::move(candidate));
        }
        else if (candidate.mid() == connection->remoteMid) connection->peer->addRemoteCandidate(std::move(candidate));
        else connection->dropped.fetch_add(1);
    }
    catch (const std::exception&) { connection->dropped.fetch_add(1); }
}

void WebGuestTransport::endRemoteCandidates(uint32_t generation)
{
    auto connection = impl->get();
    if (!connection || !connection->live()) return;
    if (generation != connection->context.generation) { connection->dropped.fetch_add(1); return; }
    // v0.24.6 exposes no PeerConnection end-remote-candidates API. Remember the
    // marker (and apply it to an answer not yet installed). ICE can complete
    // without it; do not reapply SDP or restart ICE for a trickled end marker.
    connection->remoteEnded = true;
}

bool WebGuestTransport::sendAudio(uint32_t generation, const void* packet, size_t bytes)
{
    return impl->enqueue(generation, packet, bytes, true);
}
bool WebGuestTransport::sendControl(uint32_t generation, const void* packet, size_t bytes)
{
    return impl->enqueue(generation, packet, bytes, false);
}

void WebGuestTransport::close()
{
    auto connection = impl->get();
    if (!connection) return;
    {
        std::lock_guard<std::recursive_mutex> lock(connection->callbackGate);
        if (!connection->active.exchange(false)) return;
        const auto previous = connection->state.exchange(State::closed);
        if (previous != State::closed && connection->callbacks.onState)
            connection->callbacks.onState(connection->context.generation, State::closed);
    }
    connection->wake.notify_one();
    if (connection->sender.joinable()) connection->sender.join();
    if (connection->channel) connection->channel->resetCallbacks();
    if (connection->peer) connection->peer->resetCallbacks();
    if (connection->channel) connection->channel->close();
    if (connection->peer) connection->peer->close();
    connection->channel.reset();
    connection->peer.reset();
    connection->pendingCandidates.clear();
    connection->buffered.store(0);
}

WebGuestTransport::Snapshot WebGuestTransport::snapshot() const
{
    Snapshot result;
    auto connection = impl->get();
    if (!connection) return result;
    result.state = connection->state.load();
    result.generation = connection->context.generation;
    result.sent = connection->sent.load();
    result.received = connection->received.load();
    result.dropped = connection->dropped.load();
    result.ordered = connection->ordered.load();
    result.maxRetransmits = connection->retransmits.load();
    std::lock_guard<std::mutex> lock(connection->queueMutex);
    result.bufferedBytes = connection->buffered.load() + connection->audio.count * AudioQueue::packetBytes
        + connection->controlBytes;
    return result;
}
}
