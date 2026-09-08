#pragma once

#include "ControlProtocol.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace gocue::livemix
{

class ControlLog;

/** Loopback-only transport. One accept worker and at most eight connection workers. Workers own all socket
    operations; readiness waits are bounded, and stop wakes/joins them without waiting for a message callback. */
class ControlSocket
{
public:
    static constexpr int preferredPort = 49721, maxClients = 8, maxUnauthenticated = 2;
    static constexpr size_t maxOutgoingBytes = 256 * 1024;
    using Messages = std::vector<ControlProtocol::ServerMessage>;

    class Connection : public std::enable_shared_from_this<Connection>
    {
    public:
        using Ptr = std::shared_ptr<Connection>;
        using Receive = std::function<void (const ControlProtocol::DecodedLine&)>;
        ~Connection();
        /** Copies immutable DTOs to the ordered FIFO. Serialization/writes happen only on this worker.
            A conservative encoded-byte reservation bounds the DTO stage as well as partially sent bytes. */
        bool send (const Messages&);
        bool send (ControlProtocol::ServerMessage message) { return send (Messages { std::move (message) }); }
        void closeAfterFlush (int deadlineMs = 100);
        void cancel();
        void helloReceived() { receivedHello = true; }
        void authenticated() { authorised = true; }
        void validInput();
        bool isAuthenticated() const { return authorised; }
        bool isOpen() const { return ! closing && ! cancelled && ! finished; }
        bool isFinished() const { return finished; }

    private:
        friend class ControlSocket;
        Connection (std::unique_ptr<juce::StreamingSocket>, std::shared_ptr<ControlLog>);
        void launch (Receive, std::function<void()> closed);
        void run();
        void join();
        void failure (const char* code);
        struct Outgoing { ControlProtocol::ServerMessage message; size_t reservation; };
        std::unique_ptr<juce::StreamingSocket> socket;
        std::shared_ptr<ControlLog> log;
        Receive receive;
        std::function<void()> closed;
        std::mutex mutex;
        std::deque<Outgoing> outgoing;
        size_t outgoingBytes = 0;
        juce::WaitableEvent wake;
        std::atomic<bool> authorised { false }, receivedHello { false }, closing { false }, cancelled { false }, finished { false };
        std::atomic<double> closeDeadline { 0 }, lastInput { 0 };
        std::thread worker;
    };

    struct Handler { Connection::Receive receive; std::function<void()> closed; };
    using Connected = std::function<Handler (Connection::Ptr)>;
    explicit ControlSocket (std::shared_ptr<ControlLog> = {});
    ~ControlSocket();
    int start (int preferred, Connected); // binds exactly 127.0.0.1, falling back to port 0; 0 means failure
    void allowConnections();             // only after ready discovery was successfully replaced
    void stopAccepting();                // leave clients open for queued errors and serverStatus
    void beginStop();                    // asynchronous; workers flush best effort and close their sockets
    void stop();                         // final join; may also be used after isStopped() on re-enable
    bool isStopped() const { return finished; }

private:
    void run();
    std::unique_ptr<juce::StreamingSocket> listener;
    std::shared_ptr<ControlLog> log;
    Connected connected;
    std::vector<Connection::Ptr> clients; // accept-worker-only
    std::atomic<bool> accepting { false }, stopping { false }, finished { true };
    juce::WaitableEvent wake;
    std::thread worker;
};

} // namespace gocue::livemix
