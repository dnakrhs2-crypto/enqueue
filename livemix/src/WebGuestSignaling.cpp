#include "WebGuestSignaling.h"
#include <chrono>
#include <deque>
#include <future>
#include <map>
#include <thread>

namespace gocue::livemix::collab
{
namespace
{
bool transient (int status) { return status == 0 || status == 408 || status == 429 || status >= 500; }
bool hexToken (const juce::String& token, int size)
{
    return token.length() == size && token.containsOnly ("0123456789abcdef");
}
bool cursorValue (const juce::var& value, uint64_t& result)
{
    if (! value.isString()) return false;
    const auto text = value.toString();
    if (text.isEmpty() || text.length() > 20 || ! text.containsOnly ("0123456789")
        || (text.length() > 1 && text[0] == '0')) return false;
    result = 0;
    for (auto p = text.getCharPointer(); ! p.isEmpty();)
    {
        const auto digit = (uint64_t) (p.getAndAdvance() - '0');
        if (result > (UINT64_MAX - digit) / 10) return false;
        result = result * 10 + digit;
    }
    return true;
}
uint32_t positive (const juce::var& value)
{
    if (! value.isInt() && ! value.isInt64()) return 0;
    const auto number = (juce::int64) value;
    return number > 0 && number <= 0xffffffffLL ? (uint32_t) number : 0;
}
}

struct WebGuestSignaling::Impl
{
    CollabRoom::RequestFunction request;
    juce::String server, code, hostToken;
    Callbacks callbacks;
    std::atomic<bool> stopping { true };
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> commands;
    std::thread controlThread, hostThread, connectionThread;
    std::shared_ptr<CollabRoom::Cancellation> controlCancel, hostCancel, connectionCancel;
    // The poll thread reads only this published copy under mutex.
    Accepted pollConnection;
    // The remaining state belongs exclusively to controlThread.
    Accepted active;
    uint32_t nextSignalId = 1;
    std::map<juce::String, PendingRequest> pending;
    std::unique_ptr<CollabRoom> room;

    Impl (CollabRoom::RequestFunction fn, juce::String base) : request (std::move (fn)), server (std::move (base)) {}

    void enqueue (std::function<void()> command)
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            if (stopping.load()) return;
            commands.push_back (std::move (command));
        }
        wake.notify_all();
    }

    void requestStop()
    {
        stopping.store (true);
        std::shared_ptr<CollabRoom::Cancellation> connection;
        {
            std::lock_guard<std::mutex> guard (mutex);
            connection = connectionCancel;
        }
        if (controlCancel) controlCancel->cancel();
        if (hostCancel) hostCancel->cancel();
        if (connection) connection->cancel();
        wake.notify_all();
    }

    void stop()
    {
        requestStop();
        for (auto* thread : { &hostThread, &connectionThread, &controlThread })
            if (thread->joinable()) thread->join();
        std::lock_guard<std::mutex> guard (mutex);
        commands.clear(); pending.clear();
        active = {}; pollConnection = {}; callbacks = {};
        code.clear(); hostToken.clear(); room.reset();
        controlCancel.reset(); hostCancel.reset(); connectionCancel.reset();
    }

    void start (const juce::String& newCode, const juce::String& token, Callbacks cb)
    {
        stop();
        code = newCode; hostToken = token; callbacks = std::move (cb);
        controlCancel = std::make_shared<CollabRoom::Cancellation>();
        hostCancel = std::make_shared<CollabRoom::Cancellation>();
        room = std::make_unique<CollabRoom> (request, server, controlCancel);
        stopping.store (false);
        controlThread = std::thread ([this]
        {
            while (! stopping.load())
            {
                std::function<void()> command;
                {
                    std::unique_lock<std::mutex> guard (mutex);
                    wake.wait (guard, [this] { return stopping.load() || ! commands.empty(); });
                    if (stopping.load()) break;
                    command = std::move (commands.front()); commands.pop_front();
                }
                command();
            }
        });
        hostThread = std::thread ([this] { poll (hostToken, 0, hostCancel); });
        connectionThread = std::thread ([this]
        {
            while (! stopping.load())
            {
                Accepted connection;
                std::shared_ptr<CollabRoom::Cancellation> cancellation;
                {
                    std::unique_lock<std::mutex> guard (mutex);
                    wake.wait (guard, [this] { return stopping.load() || pollConnection.connection.generation != 0; });
                    if (stopping.load()) break;
                    connection = pollConnection; cancellation = connectionCancel;
                }
                poll (connection.connectionToken, connection.connection.generation, cancellation);
                // Wait until the control thread retires this generation.
                std::unique_lock<std::mutex> guard (mutex);
                wake.wait (guard, [&] { return stopping.load() || pollConnection.connection.generation != connection.connection.generation; });
            }
        });
    }

    void error (const CollabRoom::Reply& reply)
    {
        if (! stopping.load() && callbacks.onError)
            callbacks.onError (reply.statusCode, reply.errorCode.isNotEmpty() ? reply.errorCode : juce::String ("response"));
    }

    void removePending (juce::String id)
    {
        if (pending.erase (id) != 0 && callbacks.onRequestEnded) callbacks.onRequestEnded (id);
    }

    void endConnection (uint32_t generation, const juce::String& reason)
    {
        if (generation == 0 || active.connection.generation != generation) return;
        std::shared_ptr<CollabRoom::Cancellation> cancellation;
        {
            std::lock_guard<std::mutex> guard (mutex);
            pollConnection = {};
            cancellation = connectionCancel;
        }
        if (cancellation) cancellation->cancel();
        active = {};
        wake.notify_all();
        if (callbacks.onEnded) callbacks.onEnded (generation, reason);
    }

    void closed()
    {
        const auto generation = active.connection.generation;
        endConnection (generation, "closed");
        while (! pending.empty()) removePending (pending.begin()->first);
        if (generation == 0 && callbacks.onEnded) callbacks.onEnded (0, "closed");
        requestStop();
    }

    bool check (const CollabRoom::Reply& reply)
    {
        if (stopping.load()) return false;
        if (reply.ok()) return true;
        error (reply);
        if (reply.errorCode == "closed" || reply.errorCode == "room") closed();
        return false;
    }

    CollabRoom::WebReply retry (const std::function<CollabRoom::WebReply()>& fn)
    {
        CollabRoom::WebReply reply;
        for (int attempt = 0; attempt < 3 && ! stopping.load(); ++attempt)
        {
            reply = fn();
            if (reply.ok() || ! transient (reply.statusCode) || attempt == 2 || controlCancel->wait (1000)) break;
        }
        return reply;
    }

    bool addPending (const juce::var& data)
    {
        PendingRequest p { data["requestId"].toString(), data["nick"].toString(), (juce::int64) data["expiresAtMs"] };
        if (! hexToken (p.requestId, 32) || p.nickname.isEmpty() || p.expiresAtMs <= 0) return false;
        const bool inserted = pending.emplace (p.requestId, p).second;
        if (inserted && callbacks.onRequest) callbacks.onRequest (p);
        return true;
    }

    bool process (const juce::var& event, uint32_t generation)
    {
        const auto type = event["type"].toString();
        const auto& data = event["data"];
        if (type == "snapshot" && generation == 0)
        {
            CollabRoom::RoomSnapshot snapshot;
            const auto* list = data["pendingWebRequests"].getArray();
            if (! CollabRoom::parseRoomSnapshot (data["room"], snapshot) || snapshot.apiVersion != 2 || list == nullptr) return false;
            for (const auto& p : *list) if (! addPending (p)) return false;
        }
        else if (type == "request" && generation == 0) return addPending (data);
        else if (type == "requestState" && generation == 0)
        {
            if (data["state"].toString() != "pending") removePending (data["requestId"].toString());
        }
        else if (type == "signal" && generation != 0)
        {
            if (active.connection.generation == generation && positive (data["connectionGeneration"]) == generation
                && data["from"].toString() == "web" && callbacks.onSignal)
                callbacks.onSignal (generation, data["signal"]);
        }
        else if (type == "ended")
        {
            removePending (data["requestId"].toString());
            const auto reason = data["reason"].toString();
            if (reason == "closed") { closed(); return true; }
            endConnection (positive (data["connectionGeneration"]), reason);
        }
        return true;
    }

    void poll (const juce::String& token, uint32_t generation, const std::shared_ptr<CollabRoom::Cancellation>& cancellation)
    {
        CollabRoom pollRoom (request, server);
        juce::String cursor = "0";
        uint64_t processed = 0;
        while (! stopping.load() && ! cancellation->isCancelled())
        {
            auto reply = pollRoom.webEvents (code, token, cursor, generation, cancellation);
            if (stopping.load() || cancellation->isCancelled()) break;
            auto done = std::make_shared<std::promise<bool>>();
            auto completion = done->get_future();
            auto nextCursor = cursor;
            uint64_t next = processed;
            if (reply.ok())
            {
                const auto* events = reply.json["events"].getArray();
                uint64_t last = processed, responseCursor = 0;
                bool valid = events != nullptr && events->size() <= 32 && cursorValue (reply.json["cursor"], responseCursor)
                             && responseCursor >= processed;
                if (valid)
                    for (const auto& event : *events)
                    {
                        uint64_t id = 0;
                        if (! cursorValue (event["id"], id) || id == 0 || id > responseCursor || ! event["data"].isObject())
                        { valid = false; break; }
                        if (id > processed && id <= last) { valid = false; break; }
                        if (id > processed) last = id;
                    }
                if (! valid)
                {
                    reply.error = "response"; reply.errorCode = "response";
                }
                else { nextCursor = reply.json["cursor"].toString(); next = responseCursor; }
            }
            enqueue ([this, reply, generation, processed, done]
            {
                if (generation != 0 && active.connection.generation != generation)
                {
                    done->set_value (true);
                    return;
                }
                if (! reply.ok())
                {
                    error (reply);
                    if (reply.errorCode == "closed" || reply.errorCode == "room") closed();
                    else if (! transient (reply.statusCode))
                    {
                        if (generation != 0) endConnection (generation, "expired");
                        else requestStop();
                    }
                    done->set_value (false);
                    return;
                }
                bool ok = true;
                for (const auto& event : *reply.json["events"].getArray())
                {
                    uint64_t id = 0;
                    cursorValue (event["id"], id);
                    if (id > processed && ! stopping.load() && ! process (event, generation)) { ok = false; break; }
                }
                if (! ok) { error ({ 200, "response", "response" }); requestStop(); }
                done->set_value (ok);
            });
            while (completion.wait_for (std::chrono::milliseconds (100)) != std::future_status::ready)
                if (stopping.load() || cancellation->isCancelled()) return;
            if (completion.get()) { cursor = nextCursor; processed = next; }
            else if (! transient (reply.statusCode)) break;
            else if (cancellation->wait (1000)) break;
        }
    }

    void invite()
    {
        const auto reply = room->webInvite (code, hostToken);
        if (! check (reply)) return;
        Invite invite { reply.json["inviteToken"].toString(), reply.json["hostNick"].toString(), positive (reply.json["inviteGeneration"]) };
        if (! hexToken (invite.inviteToken, 64) || invite.inviteGeneration == 0 || invite.hostNick.isEmpty())
        { error ({ 200, "response", "response" }); return; }
        if (callbacks.onInvite) callbacks.onInvite (invite);
    }

    void accept (const juce::String& id)
    {
        const auto reply = retry ([&] { return room->webAccept (code, hostToken, id); });
        if (! check (reply)) return;
        Accepted accepted;
        auto& snapshot = accepted.room;
        accepted.requestId = reply.json["requestId"].toString();
        accepted.connectionToken = reply.json["connectionToken"].toString();
        accepted.reservationExpiresAtMs = (juce::int64) reply.json["reservationExpiresAtMs"];
        auto& connection = accepted.connection;
        connection.guestId = positive (reply.json["participantId"]);
        connection.generation = positive (reply.json["connectionGeneration"]);
        connection.guestEpoch = positive (reply.json["streamEpoch"]);
        if (! CollabRoom::parseRoomSnapshot (reply.json["room"], snapshot) || snapshot.apiVersion != 2
            || accepted.requestId != id || ! hexToken (accepted.connectionToken, 64) || accepted.reservationExpiresAtMs <= 0
            || connection.guestId < 2 || connection.generation == 0 || connection.guestEpoch == 0)
        { error ({ 200, "response", "response" }); return; }
        bool foundGuest = false;
        for (const auto& member : snapshot.members)
        {
            if (member.participantId == 1) connection.hostEpoch = member.streamEpoch;
            if (member.participantId == connection.guestId && member.kind == "web"
                && member.connectionGeneration == connection.generation && member.streamEpoch == connection.guestEpoch) foundGuest = true;
        }
        if (! foundGuest) { error ({ 200, "response", "response" }); return; }
        if (active.connection.generation == connection.generation) return;
        connection.session = snapshot.session; connection.wireRate = snapshot.wireRate;
        // Server revision is deliberately NOT copied into the UDP roster revision.
        connection.rosterRevision = 0;
        active = accepted; nextSignalId = 1;
        removePending (id);
        {
            std::lock_guard<std::mutex> guard (mutex);
            pollConnection = accepted;
            connectionCancel = std::make_shared<CollabRoom::Cancellation>();
        }
        if (callbacks.onAccepted) callbacks.onAccepted (accepted);
        wake.notify_all();
    }
};

WebGuestSignaling::WebGuestSignaling (CollabRoom::RequestFunction fn, juce::String server)
    : impl (std::make_unique<Impl> (std::move (fn), std::move (server))) {}
WebGuestSignaling::~WebGuestSignaling() { stop(); }
void WebGuestSignaling::startHost (const juce::String& code, const juce::String& token, Callbacks callbacks)
{ impl->start (code, token, std::move (callbacks)); }
void WebGuestSignaling::stop() { impl->stop(); }
void WebGuestSignaling::createInvite() { impl->enqueue ([this] { impl->invite(); }); }
void WebGuestSignaling::accept (const juce::String& id) { impl->enqueue ([this, id] { impl->accept (id); }); }
void WebGuestSignaling::reject (const juce::String& id)
{
    impl->enqueue ([this, id]
    {
        if (impl->check (impl->room->webReject (impl->code, impl->hostToken, id))) impl->removePending (id);
    });
}
void WebGuestSignaling::kick (contracts::ParticipantId pid, uint32_t generation)
{
    impl->enqueue ([this, pid, generation]
    {
        if (impl->check (impl->room->webKick (impl->code, impl->hostToken, pid, generation)))
            impl->endConnection (generation, "kicked");
    });
}
void WebGuestSignaling::sendSignal (uint32_t generation, const juce::var& signal)
{
    // var objects are shared/mutable; freeze the callback-local payload at enqueue.
    auto copy = signal.clone();
    impl->enqueue ([this, generation, copy]
    {
        if (impl->active.connection.generation != generation || generation == 0) return;
        const auto id = impl->nextSignalId++;
        const auto reply = impl->retry ([&] { return impl->room->webSignal (impl->code, impl->active.connectionToken, generation, id, copy); });
        if (! impl->check (reply) && (reply.statusCode == 410 || reply.errorCode == "generation"))
            impl->endConnection (generation, "expired");
    });
}
void WebGuestSignaling::leave (uint32_t generation)
{
    impl->enqueue ([this, generation]
    {
        if (impl->active.connection.generation != generation || generation == 0) return;
        if (impl->check (impl->room->webLeave (impl->code, impl->active.connectionToken, generation)))
            impl->endConnection (generation, "left");
    });
}
}
