#include "ControlSocket.h"

#include <algorithm>
#include <array>
#include <type_traits>

#if JUCE_WINDOWS
 #include <winsock2.h>
#else
 #include <cerrno>
 #include <fcntl.h>
#endif

namespace gocue::livemix
{
namespace
{
    using P = ControlProtocol;
    double now() { return juce::Time::getMillisecondCounterHiRes(); }

    // JUCE read(false) itself sets FIONBIO. Set it before the first write/accept too, so readiness races
    // cannot turn either operation into an unbounded block. All byte transfers still use StreamingSocket.
    bool nonblocking (juce::StreamingSocket& socket)
    {
       #if JUCE_WINDOWS
        u_long value = 1;
        return ioctlsocket ((SOCKET) socket.getRawSocketHandle(), FIONBIO, &value) == 0;
       #else
        const auto handle = socket.getRawSocketHandle();
        const auto flags = fcntl (handle, F_GETFL, 0);
        return flags >= 0 && fcntl (handle, F_SETFL, flags | O_NONBLOCK) == 0;
       #endif
    }

    bool wouldBlock()
    {
       #if JUCE_WINDOWS
        return WSAGetLastError() == WSAEWOULDBLOCK;
       #else
        return errno == EAGAIN || errno == EWOULDBLOCK;
       #endif
    }

    size_t textCost (const juce::String& s)
    {
        // JSON's largest UTF-8 expansion is an ASCII control character -> six bytes. Non-ASCII codepoints
        // remain UTF-8 in JUCE JSON. Quotes and backslashes expand by one byte.
        size_t size = 2;
        for (const auto* p = reinterpret_cast<const unsigned char*> (s.toRawUTF8()); *p != 0; ++p)
            size += *p < 32 ? 6 : (*p == '"' || *p == '\\' ? 2 : 1);
        return size;
    }

    template <typename Channel>
    size_t channelArrays (const Channel& c)
    {
        return c.pluginGroups.size() * 64 + c.sends.size() * 160;
    }

    size_t reservation (const P::ServerMessage& message)
    {
        // These fixed allowances exceed the encoder's field names, punctuation, UUIDs and finite numbers.
        // Reserve before handing off DTOs; a worker checks the actual 64 KiB encoded line limit as well.
        return std::visit ([] (const auto& m) -> size_t
        {
            using T = std::decay_t<decltype (m)>;
            size_t size = 1024;
            if constexpr (std::is_same_v<T, P::State>)
            {
                size += textCost (m.state.session.name);
                for (const auto& c : m.state.channels) size += 512 + textCost (c.name) + channelArrays (c);
                for (const auto& f : m.state.fx) size += 256 + textCost (f.name);
            }
            else if constexpr (std::is_same_v<T, P::StateDelta>)
            {
                if (m.changes.sessionName) size += textCost (*m.changes.sessionName);
                for (const auto& c : m.changes.channels)
                {
                    size += 512 + (c.name ? textCost (*c.name) : 0);
                    if (c.pluginGroups) size += c.pluginGroups->size() * 64;
                    if (c.sends) size += c.sends->size() * 160;
                }
                for (const auto& f : m.changes.fx) size += 256 + (f.name ? textCost (*f.name) : 0);
            }
            else if constexpr (std::is_same_v<T, P::HelloAck>) size += textCost (m.serverVersion);
            return size;
        }, message);
    }

    std::optional<P::Context> stateContext (const P::ServerMessage& message)
    {
        if (const auto* state = std::get_if<P::State> (&message)) return state->context;
        if (const auto* delta = std::get_if<P::StateDelta> (&message)) return delta->context;
        return {};
    }
}

ControlSocket::Connection::Connection (std::unique_ptr<juce::StreamingSocket> stream) : socket (std::move (stream)) {}
ControlSocket::Connection::~Connection() { cancel(); join(); }
void ControlSocket::Connection::join() { if (worker.joinable()) worker.join(); }
void ControlSocket::Connection::validInput() { lastInput = now(); }

void ControlSocket::Connection::launch (Receive onReceive, std::function<void()> onClosed)
{
    receive = std::move (onReceive);
    closed = std::move (onClosed);
    lastInput = now();
    worker = std::thread (&Connection::run, this); // accept worker retains this object until after join
}

bool ControlSocket::Connection::send (const Messages& messages)
{
    size_t bytes = 0;
    for (const auto& message : messages)
    {
        const auto cost = reservation (message);
        if (cost > maxOutgoingBytes && (std::holds_alternative<P::State> (message) || std::holds_alternative<P::StateDelta> (message)))
        {
            send (P::ErrorResponse { { P::ErrorCode::stateTooLarge, {} }, isAuthenticated() ? stateContext (message) : std::nullopt, {}, {} });
            closeAfterFlush();
            return false;
        }
        bytes += cost;
    }
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (! isOpen()) return false;
        if (bytes > maxOutgoingBytes - outgoingBytes || outgoing.size() + messages.size() > 256)
        {
            cancelled = true;
            wake.signal();
            return false;
        }
        for (const auto& message : messages) outgoing.push_back ({ message, reservation (message) });
        outgoingBytes += bytes;
    }
    wake.signal();
    return true;
}

void ControlSocket::Connection::closeAfterFlush (int deadlineMs)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (! closing)
    {
        closeDeadline = now() + deadlineMs;
        closing = true;
    }
    wake.signal();
}

void ControlSocket::Connection::cancel() { cancelled = true; wake.signal(); }

void ControlSocket::Connection::run()
{
    const auto acceptedAt = now();
    auto lastProgress = acceptedAt;
    P::Decoder decoder;
    std::array<char, 4096> input {};
    std::string frame;
    size_t offset = 0, cost = 0;
    if (! nonblocking (*socket)) cancelled = true;
    try
    {
        while (! cancelled)
        {
            const auto time = now();
            if (closing && time >= closeDeadline) break;
            if (! closing && ! receivedHello && time - acceptedAt >= 3000)
            {
                send (P::ErrorResponse { { P::ErrorCode::handshakeTimeout, {} }, {}, {}, {} });
                closeAfterFlush();
            }
            if (! closing && time - lastInput >= 20000) break;

            if (frame.empty())
            {
                std::optional<Outgoing> next;
                {
                    std::lock_guard<std::mutex> lock (mutex);
                    if (! outgoing.empty()) { next = std::move (outgoing.front()); outgoing.pop_front(); }
                }
                if (next)
                {
                    cost = next->reservation;
                    auto encoded = P::encode (next->message);
                    if (const auto* error = std::get_if<P::Error> (&encoded))
                    {
                        encoded = P::encode (P::ErrorResponse { *error, isAuthenticated() ? stateContext (next->message) : std::nullopt, {}, {} });
                        closeAfterFlush();
                    }
                    frame = std::get<std::string> (std::move (encoded));
                    jassert (frame.size() <= cost);
                    offset = 0;
                }
                else
                {
                    if (closing) break;
                    lastProgress = time; // no outstanding data is not a stalled write
                }
            }

            bool progressed = false;
            if (! frame.empty())
            {
                if (time - lastProgress >= 2000) break;
                const auto ready = socket->waitUntilReady (false, 0);
                if (ready < 0) break;
                if (ready > 0)
                {
                    const auto count = socket->write (frame.data() + offset, (int) std::min<size_t> (4096, frame.size() - offset));
                    if (count > 0)
                    {
                        progressed = true;
                        lastProgress = now();
                        offset += (size_t) count;
                        if (offset == frame.size())
                        {
                            frame.clear();
                            std::lock_guard<std::mutex> lock (mutex);
                            outgoingBytes -= cost;
                        }
                    }
                    else if (count == 0 || ! wouldBlock()) break;
                }
            }

            if (! closing)
            {
                const auto ready = socket->waitUntilReady (true, progressed ? 0 : 10);
                if (ready < 0) break;
                if (ready > 0)
                {
                    const auto count = socket->read (input.data(), (int) input.size(), false);
                    if (count <= 0)
                    {
                        if (const auto error = decoder.finish()) { if (receive) receive (*error); }
                        closeAfterFlush();
                    }
                    else
                    {
                        progressed = true;
                        for (const auto& line : decoder.push (input.data(), (size_t) count))
                        {
                            if (! isOpen()) break;
                            if (receive) receive (line);
                        }
                    }
                }
            }
            if (! progressed) wake.wait (10);
        }
    }
    catch (...) { /* A transport failure terminates only this connection. Never expose exception text. */ }
    socket->close();
    cancelled = true;
    if (closed) closed();
    receive = {};
    closed = {};
    { std::lock_guard<std::mutex> lock (mutex); outgoing.clear(); outgoingBytes = 0; }
    finished = true;
}

ControlSocket::ControlSocket()
{
    // In this JUCE checkout the options constructor skips initSockets(). Its default constructor performs
    // the one-time Winsock initialisation, before we construct our listener with explicit buffer options.
    juce::StreamingSocket initialiseSockets;
}

ControlSocket::~ControlSocket() { stop(); }

int ControlSocket::start (int preferred, Connected handler)
{
    stop();
    if (preferred < 0 || preferred > 65535) return 0;
    listener = std::make_unique<juce::StreamingSocket> (juce::SocketOptions().withSendBufferSize (16384));
    if (! listener->createListener (preferred, "127.0.0.1") && ! listener->createListener (0, "127.0.0.1"))
    {
        listener.reset();
        return 0;
    }
    if (! nonblocking (*listener)) { listener.reset(); return 0; }
    const auto port = listener->getBoundPort();
    if (port <= 0) { listener.reset(); return 0; }
    connected = std::move (handler);
    stopping = accepting = finished = false;
    worker = std::thread (&ControlSocket::run, this); // stop joins before members can be destroyed
    return port;
}

void ControlSocket::allowConnections() { accepting = true; wake.signal(); }
void ControlSocket::beginStop() { stopping = true; accepting = false; wake.signal(); }
void ControlSocket::stop()
{
    beginStop();
    if (worker.joinable()) worker.join();
    listener.reset();
    connected = {};
}

void ControlSocket::run()
{
    const auto reap = [this]
    {
        clients.erase (std::remove_if (clients.begin(), clients.end(), [] (const auto& client)
        {
            if (! client->isFinished()) return false;
            client->join();
            return true;
        }), clients.end());
    };
    while (! stopping)
    {
        reap();
        if (! accepting) { wake.wait (20); continue; }
        const auto ready = listener->waitUntilReady (true, 20);
        if (ready < 0) break;
        if (ready == 0) continue;
        std::unique_ptr<juce::StreamingSocket> stream (listener->waitForNextConnection());
        if (! stream) continue;
        reap(); // a client may have finished while we were in the listener readiness wait
        const auto unauthenticated = std::count_if (clients.begin(), clients.end(), [] (const auto& c) { return ! c->isAuthenticated(); });
        if (stopping || clients.size() >= maxClients || unauthenticated >= maxUnauthenticated)
        {
            stream->close();
            continue;
        }
        auto client = Connection::Ptr (new Connection (std::move (stream)));
        clients.push_back (client);
        auto handler = connected (client);
        client->launch (std::move (handler.receive), std::move (handler.closed));
    }
    listener->close();
    for (const auto& client : clients) client->closeAfterFlush();
    for (const auto& client : clients) client->join();
    clients.clear();
    finished = true;
}

} // namespace gocue::livemix
