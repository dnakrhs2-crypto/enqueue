#include "ControlServer.h"
#include "ControlDiscovery.h"
#include "ControlDispatcher.h"

#include <juce_cryptography/juce_cryptography.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <deque>
#include <mutex>

namespace gocue::livemix
{
namespace
{
    using P = ControlProtocol;
    using Code = P::ErrorCode;
    double now() { return juce::Time::getMillisecondCounterHiRes(); }
    void messageThread() { jassert (juce::MessageManager::getInstance()->isThisTheMessageThread()); }

    struct Bucket
    {
        double rate, burst, tokens, updated = now();
        Bucket (double perSecond, double capacity) : rate (perSecond), burst (capacity), tokens (capacity) {}
        bool take (double time)
        {
            tokens = std::min (burst, tokens + std::max (0.0, time - updated) * rate / 1000.0);
            updated = time;
            if (tokens < 1.0) return false;
            tokens -= 1.0;
            return true;
        }
    };

    // Object order and whitespace are not request content. Preserve arrays, types and unknown optional fields.
    // Only a SHA-256 digest survives receipt, so a duplicate cache never retains a hello token/raw JSON.
    juce::var canonical (const juce::var& value)
    {
        if (const auto* object = value.getDynamicObject())
        {
            std::vector<juce::String> keys;
            for (const auto& p : object->getProperties()) keys.push_back (p.name.toString());
            std::sort (keys.begin(), keys.end());
            auto result = juce::var (new juce::DynamicObject());
            for (const auto& key : keys) result.getDynamicObject()->setProperty (key, canonical (value[juce::Identifier (key)]));
            return result;
        }
        if (const auto* array = value.getArray())
        {
            juce::Array<juce::var> result;
            for (const auto& v : *array) result.add (canonical (v));
            return result;
        }
        return value;
    }

    juce::String fingerprint (const juce::var& value)
    {
        const auto text = juce::JSON::toString (canonical (value), true, 17);
        return juce::SHA256 (text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString();
    }
}

struct ControlServer::Impl : private juce::Timer
{
    struct Gate { Impl* owner = nullptr; uint64_t generation = 0; }; // read/written ONLY on the message thread
    struct Reply
    {
        ControlSocket::Messages messages;
        std::optional<ControlState::Snapshot> published;
    };
    struct Record
    {
        juce::int64 id;
        juce::String fingerprint;
        std::shared_ptr<const Reply> reply;
    };
    struct Initial { juce::String id; };
    struct Replay { std::shared_ptr<const Reply> reply; };
    struct Work
    {
        std::variant<Initial, P::Command, P::Ping, Replay> request;
        std::shared_ptr<Record> record;
        double receivedAt = 0;
        size_t bytes = 1024;
    };
    struct Client
    {
        explicit Client (ControlSocket::Connection::Ptr stream) : connection (std::move (stream)) {}
        ControlSocket::Connection::Ptr connection;
        std::deque<Work> pending;
        size_t pendingBytes = 0;
        std::deque<std::shared_ptr<Record>> recent; // at most 32 digests + immutable, bounded replies
        juce::int64 highWater = 0;
        Bucket commands { 30, 10 }, requests { 2, 2 }, auxiliary { 2, 4 };
        int violations = 0;
        bool helloQueued = false;
        // Only the message thread uses these publication fields, including when replaying a requested state.
        std::optional<ControlState::Snapshot> published;
        double publishedAt = 0;
    };
    struct Ingress : std::enable_shared_from_this<Ingress>
    {
        const std::shared_ptr<Gate> gate;
        const uint64_t generation;
        const juce::Uuid instance;
        const juce::String token, appVersion;
        std::mutex mutex;
        bool active = true, scheduled = false;
        std::vector<std::shared_ptr<Client>> clients;
        size_t pending = 0, pendingBytes = 0, nextClient = 0;
        Bucket commands { 120, 40 }, requests { 8, 8 }, auxiliary { 16, 16 };
        P::Context context;

        Ingress (std::shared_ptr<Gate> g, uint64_t gen, juce::Uuid id, juce::String secret, juce::String version)
            : gate (std::move (g)), generation (gen), instance (id), token (std::move (secret)), appVersion (std::move (version)) {}
        ControlSocket::Handler connect (ControlSocket::Connection::Ptr);
        void receive (const std::shared_ptr<Client>&, const P::DecodedLine&);
        void remove (const std::shared_ptr<Client>&);
        void scheduleLocked();
        bool permitLocked (Client&, const P::ClientMessage&, double receivedAt);
        void rejectLocked (Client&, P::Error, const std::shared_ptr<Record>& = {}, bool close = false);
        bool enqueueLocked (Client&, Work);
    };

    Impl (MixDocument& doc, MuteGroups& mute, std::function<bool()> running)
        : document (doc), groups (mute), audioRunning (std::move (running)), gate (std::make_shared<Gate>())
    {
        messageThread();
        jassert (audioRunning);
        gate->owner = this;
    }
    ~Impl() override { stop(); gate->owner = nullptr; }
    void start (Options);
    void setEnabled (bool);
    void beginShutdown();
    void stop();
    void changed();
    Status getStatus() const;
    void drain();
    bool capture();
    void finish (const std::shared_ptr<Client>&, const std::shared_ptr<Record>&, Reply, bool initial);
    void timerCallback() override;
    void publishDiscovery (ControlDiscovery::State);
    void discoveryPublished (const ControlDiscovery::Status&, bool);
    void openListener();
    void quiesce (P::StatusReason);
    void fail (const juce::String&);

    MixDocument& document;
    MuteGroups& groups;
    std::function<bool()> audioRunning;
    std::shared_ptr<Gate> gate;
    Options options;
    bool started = false, enabled = false, shuttingDown = false, startingWritten = false, captureScheduled = false;
    int port = 0;
    juce::String error;
    double lastPoll = 0;
    ControlDiscovery::Status discoveryStatus;
    std::unique_ptr<ControlDiscovery> discovery;
    std::unique_ptr<ControlSocket> socket;
    std::shared_ptr<Ingress> ingress;
    std::unique_ptr<ControlState> state;
    std::unique_ptr<ControlDispatcher> dispatcher;
};

ControlSocket::Handler ControlServer::Impl::Ingress::connect (ControlSocket::Connection::Ptr connection)
{
    auto client = std::make_shared<Client> (std::move (connection));
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (! active) client->connection->cancel();
        else clients.push_back (client);
    }
    auto self = shared_from_this(); // only owned DTOs/transport here; this object never reads the document
    return { [self, client] (const auto& line) { self->receive (client, line); },
             [self, client] { self->remove (client); } };
}

void ControlServer::Impl::Ingress::remove (const std::shared_ptr<Client>& client)
{
    std::lock_guard<std::mutex> lock (mutex);
    pending -= client->pending.size();
    pendingBytes -= client->pendingBytes;
    client->pending.clear();
    client->pendingBytes = 0;
    clients.erase (std::remove (clients.begin(), clients.end(), client), clients.end());
}

void ControlServer::Impl::Ingress::scheduleLocked()
{
    if (scheduled || ! active) return;
    scheduled = true;
    if (! juce::MessageManager::callAsync ([lifetime = gate, gen = generation]
    {
        if (lifetime->owner != nullptr && lifetime->generation == gen) lifetime->owner->drain();
    }))
    {
        scheduled = false;
        for (const auto& client : clients) client->connection->cancel();
    }
}

void ControlServer::Impl::Ingress::rejectLocked (Client& client, P::Error failure, const std::shared_ptr<Record>& record, bool close)
{
    P::ErrorResponse response { failure, {}, {}, {} };
    if (client.connection->isAuthenticated()) response.context = context;
    if (failure.code == Code::unsupportedVersion) response.supportedVersions = { 1 };
    if (failure.code == Code::rateLimited) response.retryAfterMs = 500;
    if (failure.code == Code::serverBusy) response.retryAfterMs = 100;
    Reply reply { { response }, {} };
    if (record) record->reply = std::make_shared<const Reply> (reply);
    client.connection->send (reply.messages);
    if (close) client.connection->closeAfterFlush();
}

bool ControlServer::Impl::Ingress::enqueueLocked (Client& client, Work work)
{
    if (client.pending.size() >= 16 || pending >= 64
        || client.pendingBytes + work.bytes > 16 * P::maxLineBytes || pendingBytes + work.bytes > 64 * P::maxLineBytes)
    {
        // A replay that cannot be queued must not overwrite the original cached completion.
        rejectLocked (client, { Code::serverBusy, juce::String (work.record->id) },
                      std::holds_alternative<Replay> (work.request) ? nullptr : work.record);
        return false;
    }
    ++pending;
    pendingBytes += work.bytes;
    client.pendingBytes += work.bytes;
    client.pending.push_back (std::move (work));
    scheduleLocked();
    return true;
}

bool ControlServer::Impl::Ingress::permitLocked (Client& client, const P::ClientMessage& message, double receivedAt)
{
    if (const auto* command = std::get_if<P::Command> (&message))
    {
        if (! client.commands.take (receivedAt) || ! commands.take (receivedAt)) return false;
        return ! std::holds_alternative<P::RequestState> (command->args)
            || (client.requests.take (receivedAt) && requests.take (receivedAt));
    }
    return client.auxiliary.take (receivedAt) && auxiliary.take (receivedAt);
}

void ControlServer::Impl::Ingress::receive (const std::shared_ptr<Client>& client, const P::DecodedLine& line)
{
    const auto receivedAt = now();
    // Framing/shape validation and digesting run on the connection worker, before the bounded DTO queue.
    const auto* json = std::get_if<juce::var> (&line);
    auto validated = json != nullptr ? P::validate (*json) : P::ValidationResult { std::get<P::Error> (line) };
    const auto id = json != nullptr && (*json)["id"].isString() && P::isRequestId ((*json)["id"].toString())
        ? (*json)["id"].toString() : juce::String();
    const auto digest = id.isNotEmpty() ? fingerprint (*json) : juce::String();
    std::lock_guard<std::mutex> lock (mutex);
    if (! active || ! client->connection->isOpen()) { client->connection->cancel(); return; }

    std::shared_ptr<Record> record;
    if (id.isNotEmpty())
    {
        const auto number = id.getLargeIntValue();
        if (number <= client->highWater)
        {
            const auto* message = std::get_if<P::ClientMessage> (&validated);
            const auto permitted = message != nullptr ? permitLocked (*client, *message, receivedAt)
                : client->auxiliary.take (receivedAt) && auxiliary.take (receivedAt);
            if (! permitted)
            { rejectLocked (*client, { Code::rateLimited, id }); return; }
            const auto found = std::find_if (client->recent.begin(), client->recent.end(), [number] (const auto& r) { return r->id == number; });
            if (found == client->recent.end() || (*found)->fingerprint != digest)
                rejectLocked (*client, { Code::duplicateId, id });
            else if ((*found)->reply)
            {
                const auto reply = (*found)->reply;
                enqueueLocked (*client, { Replay { reply }, *found, receivedAt, reply->published ? 2 * P::maxLineBytes : 1024 });
            }
            // Identical in-flight requests share the original result and never create another edit.
            return;
        }
        client->highWater = number;
        record = std::make_shared<Record> (Record { number, digest, {} });
        client->recent.push_back (record);
        if (client->recent.size() > 32) client->recent.pop_front();
    }

    const auto authenticated = client->connection->isAuthenticated();
    if (! authenticated && json != nullptr && (*json)["type"].toString() != "hello")
    { rejectLocked (*client, { Code::authRequired, id }, record, true); return; }
    if (const auto* failure = std::get_if<P::Error> (&validated))
    {
        const auto close = json == nullptr || ! authenticated || failure->code == Code::unsupportedVersion || ++client->violations >= 3;
        rejectLocked (*client, *failure, record, close);
        return;
    }

    auto message = std::get<P::ClientMessage> (std::move (validated));
    if (const auto* hello = std::get_if<P::Hello> (&message))
    {
        if (authenticated || client->helloQueued) { rejectLocked (*client, { Code::invalidArgument, id }, record, true); return; }
        if (! ControlDiscovery::tokensEqual (token, hello->token)) { rejectLocked (*client, { Code::authFailed, id }, record, true); return; }
        if (! permitLocked (*client, message, receivedAt)) { rejectLocked (*client, { Code::rateLimited, id }, record, true); return; }
        client->connection->helloReceived();
        client->connection->validInput();
        client->helloQueued = true;
        if (! enqueueLocked (*client, { Initial { id }, record, receivedAt })) client->connection->closeAfterFlush();
        return;
    }
    const auto permitted = permitLocked (*client, message, receivedAt);
    if (permitted)
    {
        if (const auto* command = std::get_if<P::Command> (&message)) enqueueLocked (*client, { *command, record, receivedAt });
        else enqueueLocked (*client, { std::get<P::Ping> (message), record, receivedAt });
    }
    if (! permitted) rejectLocked (*client, { Code::rateLimited, id }, record);
    else client->connection->validInput();
}

void ControlServer::Impl::start (Options settings)
{
    messageThread();
    stop();
    options = std::move (settings);
    if (options.appVersion.isEmpty()) options.appVersion = JUCE_APPLICATION_VERSION_STRING;
    started = true;
    shuttingDown = false;
    enabled = options.enabled;
    error.clear();
    ++gate->generation;
    discoveryStatus = {};
    discoveryStatus.appVersion = options.appVersion;
    discoveryStatus.generation = gate->generation;
    discoveryStatus.state = enabled ? ControlDiscovery::State::starting : ControlDiscovery::State::disabled;
    discovery = std::make_unique<ControlDiscovery> (options.discoveryDirectory, discoveryStatus,
        [lifetime = gate] (const auto& status, bool success)
        {
            juce::MessageManager::callAsync ([lifetime, status, success]
            {
                if (lifetime->owner != nullptr && lifetime->generation == status.generation)
                    lifetime->owner->discoveryPublished (status, success);
            });
        });
    startTimer (20);
}

void ControlServer::Impl::publishDiscovery (ControlDiscovery::State value)
{
    discoveryStatus.state = value;
    discoveryStatus.generation = gate->generation;
    discoveryStatus.port = value == ControlDiscovery::State::ready ? port : 0;
    if (value != ControlDiscovery::State::ready) discoveryStatus.token.clear();
    discoveryStatus.errorCode = error;
    if (discovery) discovery->publish (discoveryStatus);
}

void ControlServer::Impl::discoveryPublished (const ControlDiscovery::Status& status, bool success)
{
    if (! started || shuttingDown || status.state != discoveryStatus.state) return;
    if (! success)
    {
        if (error != "DISCOVERY_FAILED") fail ("DISCOVERY_FAILED");
        return;
    }
    if (status.state == ControlDiscovery::State::starting)
    {
        startingWritten = true;
        if (! socket || socket->isStopped()) openListener();
    }
    else if (status.state == ControlDiscovery::State::ready && socket && ingress)
        socket->allowConnections();
}

void ControlServer::Impl::openListener()
{
    if (! enabled || shuttingDown || ! startingWritten || error.isNotEmpty()) return;
    startingWritten = false;
    socket.reset(); // any previous accept/connection workers have already finished
    const auto token = ControlDiscovery::createToken();
    if (token.isEmpty()) { fail ("RANDOM_FAILED"); return; }
    state = std::make_unique<ControlState> (ControlState::capture (document, groups, audioRunning()));
    ingress = std::make_shared<Ingress> (gate, gate->generation, discoveryStatus.instanceId, token, options.appVersion);
    ingress->context = { ingress->instance, state->getCurrent().sessionId, state->getCurrent().revision };
    dispatcher = std::make_unique<ControlDispatcher> (document, groups, *state, ingress->instance, audioRunning);
    socket = std::make_unique<ControlSocket>();
    port = socket->start (options.preferredPort, [queue = ingress] (auto connection) { return queue->connect (std::move (connection)); });
    if (port == 0) { fail ("BIND_FAILED"); return; }
    discoveryStatus.token = token;
    publishDiscovery (ControlDiscovery::State::ready);
    lastPoll = now();
}

void ControlServer::Impl::quiesce (P::StatusReason reason)
{
    ++gate->generation; // invalidate queued drains/captures before any socket or document teardown
    captureScheduled = startingWritten = false;
    if (ingress)
    {
        std::lock_guard<std::mutex> lock (ingress->mutex);
        ingress->active = false;
        const auto disabled = reason == P::StatusReason::controlDisabled;
        for (const auto& client : ingress->clients)
        {
            for (const auto& work : client->pending)
                ingress->rejectLocked (*client, { disabled ? Code::controlDisabled : Code::serverStopping, juce::String (work.record->id) });
            client->pending.clear();
            client->pendingBytes = 0;
            if (client->connection->isAuthenticated())
                client->connection->send (P::ServerStatus { ingress->instance, disabled ? P::Status::disabled : P::Status::stopping, reason });
        }
        ingress->pending = ingress->pendingBytes = 0;
        ingress->scheduled = false;
    }
    if (socket) socket->beginStop();
    port = 0;
    dispatcher.reset();
    state.reset();
}

void ControlServer::Impl::setEnabled (bool value)
{
    messageThread();
    if (! started || shuttingDown || enabled == value) return;
    quiesce (P::StatusReason::controlDisabled);
    enabled = value;
    error.clear();
    if (enabled) discoveryStatus.instanceId = juce::Uuid();
    publishDiscovery (enabled ? ControlDiscovery::State::starting : ControlDiscovery::State::disabled);
}

void ControlServer::Impl::beginShutdown()
{
    messageThread();
    if (! started || shuttingDown) return;
    shuttingDown = true;
    quiesce (P::StatusReason::shutdown);
}

void ControlServer::Impl::stop()
{
    messageThread();
    beginShutdown();
    stopTimer();
    if (socket) socket->stop();
    socket.reset();
    ingress.reset();
    if (discovery)
    {
        discoveryStatus.generation = ++gate->generation;
        discovery->stop (discoveryStatus);
        discovery.reset();
    }
    started = enabled = false;
    port = 0;
}

void ControlServer::Impl::fail (const juce::String& code)
{
    quiesce (P::StatusReason::restart);
    error = code;
    publishDiscovery (ControlDiscovery::State::error);
}

bool ControlServer::Impl::capture()
{
    if (! enabled || shuttingDown || ! state || ! ingress) return false;
    if (! state->update (ControlState::capture (document, groups, audioRunning())))
    {
        // A new instance is required if the JSON-safe revision space is exhausted; never wrap revisions.
        quiesce (P::StatusReason::restart);
        discoveryStatus.instanceId = juce::Uuid();
        publishDiscovery (ControlDiscovery::State::starting);
        return false;
    }
    std::lock_guard<std::mutex> lock (ingress->mutex);
    ingress->context = { ingress->instance, state->getCurrent().sessionId, state->getCurrent().revision };
    return true;
}

void ControlServer::Impl::changed()
{
    messageThread();
    if (! enabled || shuttingDown || ! state || captureScheduled) return;
    captureScheduled = true;
    juce::MessageManager::callAsync ([lifetime = gate, gen = gate->generation]
    {
        if (lifetime->owner != nullptr && lifetime->generation == gen)
        {
            lifetime->owner->captureScheduled = false;
            lifetime->owner->capture(); // callback stack has finished, including MuteGroups reset/apply
        }
    });
}

void ControlServer::Impl::finish (const std::shared_ptr<Client>& client, const std::shared_ptr<Record>& record, Reply reply, bool initial)
{
    std::lock_guard<std::mutex> lock (ingress->mutex);
    if (! ingress->active || ! client->connection->isOpen()) return;
    if (! client->connection->send (reply.messages)) return;
    if (initial) client->connection->authenticated(); // receive sees this before it can process a post-hello request
    if (reply.published) { client->published = reply.published; client->publishedAt = now(); }
    record->reply = std::make_shared<const Reply> (std::move (reply));
}

void ControlServer::Impl::drain()
{
    messageThread();
    if (! enabled || shuttingDown || ! ingress || ! state) return;
    const auto queue = ingress;
    const auto began = now();
    for (int count = 0; count < 8 && now() - began < 2.0; ++count)
    {
        std::shared_ptr<Client> client;
        std::optional<Work> work;
        {
            std::lock_guard<std::mutex> lock (queue->mutex);
            if (! queue->active || queue->clients.empty()) break;
            for (size_t i = 0; i < queue->clients.size(); ++i)
            {
                queue->nextClient %= queue->clients.size();
                auto candidate = queue->clients[queue->nextClient++];
                if (candidate->pending.empty()) continue;
                client = std::move (candidate);
                work = std::move (client->pending.front());
                client->pending.pop_front();
                --queue->pending;
                queue->pendingBytes -= work->bytes;
                client->pendingBytes -= work->bytes;
                break;
            }
        }
        if (! work) break;
        if (! client->connection->isOpen()) continue;
        if (! capture()) return;
        const auto context = queue->context;
        Reply reply;
        if (const auto* command = std::get_if<P::Command> (&work->request))
        {
            if (now() - work->receivedAt > 1000)
                reply.messages.push_back (P::ErrorResponse { { Code::commandExpired, command->id }, context, {}, {} });
            else
            {
                const auto result = dispatcher->dispatch (*command);
                std::visit ([&reply] (const auto& r) { reply.messages.push_back (r); }, result);
                if (std::holds_alternative<P::Ack> (result) && std::holds_alternative<P::RequestState> (command->args))
                {
                    reply.published = state->getCurrent();
                    reply.messages.push_back (P::State { context, P::StateReason::requested, command->id, reply.published->projection });
                }
                if (! capture()) return;
            }
        }
        else if (const auto* initial = std::get_if<Initial> (&work->request))
        {
            reply.published = state->getCurrent();
            reply.messages = { P::HelloAck { initial->id, queue->instance, queue->appVersion },
                               P::State { context, P::StateReason::initial, {}, reply.published->projection } };
        }
        else if (const auto* ping = std::get_if<P::Ping> (&work->request))
        {
            if (ping->instanceId != queue->instance)
                reply.messages.push_back (P::ErrorResponse { { Code::instanceChanged, ping->id }, context, {}, {} });
            else reply.messages.push_back (P::Pong { ping->id, context });
        }
        else reply = *std::get<Replay> (work->request).reply;
        finish (client, work->record, std::move (reply), std::holds_alternative<Initial> (work->request));
    }
    std::lock_guard<std::mutex> lock (queue->mutex);
    queue->scheduled = false;
    if (queue->pending != 0) queue->scheduleLocked();
}

void ControlServer::Impl::timerCallback()
{
    if (startingWritten && (! socket || socket->isStopped())) openListener();
    if (! enabled || shuttingDown || ! state) return;
    if (socket && socket->isStopped()) { fail ("SOCKET_FAILED"); return; }
    const auto time = now();
    if (time - lastPoll < 100) return;
    lastPoll = time;
    if (! capture()) return; // also covers callback-free dirty/audio changes
    std::vector<std::shared_ptr<Client>> clients;
    { std::lock_guard<std::mutex> lock (ingress->mutex); clients = ingress->clients; }
    for (const auto& client : clients)
    {
        if (! client->connection->isOpen() || ! client->connection->isAuthenticated()
            || ! client->published || time - client->publishedAt < 100) continue;
        const auto& current = state->getCurrent();
        const auto difference = ControlState::diff (*client->published, current);
        if (difference.kind == ControlState::ChangeKind::none) continue;
        const P::Context context { ingress->instance, current.sessionId, current.revision };
        P::ServerMessage message;
        if (difference.needsFullState())
            message = P::State { context, difference.kind == ControlState::ChangeKind::session ? P::StateReason::sessionChanged
                                                                                            : P::StateReason::structureChanged, {}, current.projection };
        else message = P::StateDelta { context, difference.baseRevision, difference.changes };
        if (client->connection->send (std::move (message))) { client->published = current; client->publishedAt = time; }
    }
}

ControlServer::Status ControlServer::Impl::getStatus() const
{
    messageThread();
    Status result { enabled && ! shuttingDown, port, 0, error };
    if (ingress)
    {
        std::lock_guard<std::mutex> lock (ingress->mutex);
        if (ingress->active)
            for (const auto& client : ingress->clients)
                if (client->connection->isOpen() && client->connection->isAuthenticated()) ++result.connectedCount;
    }
    return result;
}

ControlServer::ControlServer (MixDocument& document, MuteGroups& groups, std::function<bool()> running)
    : impl (std::make_unique<Impl> (document, groups, std::move (running))) {}
ControlServer::~ControlServer() = default;
void ControlServer::start (Options options) { impl->start (std::move (options)); }
void ControlServer::setEnabled (bool enabled) { impl->setEnabled (enabled); }
void ControlServer::beginShutdown() { impl->beginShutdown(); }
void ControlServer::stop() { impl->stop(); }
void ControlServer::documentChanged (ChangeKind) { impl->changed(); }
void ControlServer::muteGroupsChanged() { impl->changed(); }
ControlServer::Status ControlServer::getStatus() const { return impl->getStatus(); }

} // namespace gocue::livemix
