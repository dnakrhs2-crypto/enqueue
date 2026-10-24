#include "remote/RemoteServer.h"
#include "civetweb.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace gocue::remote
{
struct Server::Impl
{
    struct Connection
    {
        uint64_t id = 0;
        mg_connection* socket = nullptr;
        std::mutex mutex; // pins the socket through a write; close waits before CivetWeb frees it
    };
    struct Outgoing { std::shared_ptr<Connection> connection; std::string text; bool close = false; };
    std::vector<Asset> assets;
    Callbacks callbacks;
    mg_context* context = nullptr;
    std::string key;
    std::atomic<bool> accepting { false };
    std::mutex mutex;
    std::condition_variable wake;
    std::map<const mg_connection*, std::shared_ptr<Connection>> connections;
    std::deque<Outgoing> outgoing;
    std::thread writer;
    bool stopping = false;
    static std::atomic<uint64_t> nextConnection;

    static int http (mg_connection* socket, void* user)
    {
        auto& self = *static_cast<Impl*> (user);
        const auto* request = mg_get_request_info (socket);
        const std::string path = request->local_uri != nullptr ? request->local_uri : "";
        const auto found = std::find_if (self.assets.begin(), self.assets.end(), [&] (const auto& a) { return a.path == path; });
        const bool get = std::strcmp (request->request_method, "GET") == 0;
        const bool head = std::strcmp (request->request_method, "HEAD") == 0;
        if (found == self.assets.end() || (! get && ! head))
        {
            mg_printf (socket, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nCache-Control: no-store\r\n\r\n");
            return 404;
        }
        mg_printf (socket, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %llu\r\nCache-Control: no-store\r\n\r\n",
                   found->mime.c_str(), static_cast<unsigned long long> (found->size));
        if (get) mg_write (socket, found->data, found->size);
        return 200;
    }
    static int connect (const mg_connection*, void* user) { return static_cast<Impl*> (user)->accepting ? 0 : 1; }
    static void ready (mg_connection* socket, void* user)
    {
        auto& self = *static_cast<Impl*> (user);
        const auto* query = mg_get_request_info (socket)->query_string;
        char supplied[128] {};
        if (query != nullptr) mg_get_var (query, std::strlen (query), "k", supplied, sizeof (supplied));
        if (! self.accepting || self.key != supplied)
        {
            const std::string reason = self.accepting ? "key" : "off";
            const auto message = "{\"t\":\"bye\",\"reason\":\"" + reason + "\"}";
            mg_websocket_write (socket, MG_WEBSOCKET_OPCODE_TEXT, message.data(), message.size());
            mg_websocket_write (socket, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, "", 0);
            return;
        }
        auto connection = std::make_shared<Connection>();
        connection->id = ++nextConnection;
        connection->socket = socket;
        {
            std::lock_guard<std::mutex> lock (self.mutex);
            self.connections[socket] = connection;
        }
        if (self.callbacks.opened) self.callbacks.opened (connection->id);
    }
    static int data (mg_connection* socket, int bits, char* bytes, size_t length, void* user)
    {
        auto& self = *static_cast<Impl*> (user);
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard<std::mutex> lock (self.mutex);
            const auto found = self.connections.find (socket);
            if (found != self.connections.end()) connection = found->second;
        }
        if (! self.accepting || connection == nullptr || (bits & 15) == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) return 0;
        if ((bits & 15) == MG_WEBSOCKET_OPCODE_TEXT && self.callbacks.received)
            self.callbacks.received (connection->id, (bits & 128) != 0 ? std::string (bytes, length) : std::string());
        return 1;
    }
    static void closed (const mg_connection* socket, void* user)
    {
        auto& self = *static_cast<Impl*> (user);
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard<std::mutex> lock (self.mutex);
            const auto found = self.connections.find (socket);
            if (found == self.connections.end()) return;
            connection = found->second;
            self.connections.erase (found);
        }
        {
            std::lock_guard<std::mutex> lock (connection->mutex);
            connection->socket = nullptr;
        }
        if (self.callbacks.closed) self.callbacks.closed (connection->id);
    }
    void writeLoop()
    {
        for (;;)
        {
            Outgoing message;
            {
                std::unique_lock<std::mutex> lock (mutex);
                wake.wait (lock, [&] { return stopping || ! outgoing.empty(); });
                if (outgoing.empty() && stopping) return;
                message = std::move (outgoing.front());
                outgoing.pop_front();
            }
            std::lock_guard<std::mutex> lock (message.connection->mutex);
            if (auto* socket = message.connection->socket)
            {
                // mg_websocket_write itself takes mg_lock_connection, including against ping/pong writes.
                mg_websocket_write (socket, MG_WEBSOCKET_OPCODE_TEXT, message.text.data(), message.text.size());
                if (message.close) mg_websocket_write (socket, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, "", 0);
            }
        }
    }
};
std::atomic<uint64_t> Server::Impl::nextConnection { 0 };
Server::Server (std::vector<Asset> assets, Callbacks callbacks) : impl (std::make_unique<Impl>())
{
    impl->assets = std::move (assets);
    impl->callbacks = std::move (callbacks);
    mg_init_library (0);
}
Server::~Server() { stop ("off"); mg_exit_library(); }
int Server::start (const std::string& key, bool loopback, int preferredPort)
{
    auto& self = *impl;
    self.key = key;
    self.stopping = false;
    for (int offset = 0; offset < 10; ++offset)
    {
        const int port = 49820 + (preferredPort - 49820 + offset) % 10;
        const auto address = (loopback ? "127.0.0.1:" : "0.0.0.0:") + std::to_string (port);
        const char* options[] { "listening_ports", address.c_str(), "num_threads", "12",
            "request_timeout_ms", "1000", "websocket_timeout_ms", "1000",
            "enable_websocket_ping_pong", "yes", nullptr };
        self.context = mg_start (nullptr, nullptr, options);
        if (self.context == nullptr) continue;
        mg_set_request_handler (self.context, "/", &Impl::http, &self);
        mg_set_websocket_handler (self.context, "/ws", &Impl::connect, &Impl::ready, &Impl::data, &Impl::closed, &self);
        self.accepting = true;
        self.writer = std::thread ([&self] { self.writeLoop(); });
        return port;
    }
    return 0;
}
void Server::send (uint64_t id, std::string text)
{
    auto& self = *impl;
    std::lock_guard<std::mutex> lock (self.mutex);
    if (self.stopping) return;
    for (const auto& entry : self.connections)
        if (entry.second->id == id) { self.outgoing.push_back ({ entry.second, std::move (text), false }); self.wake.notify_one(); break; }
}
void Server::stop (const std::string& reason)
{
    auto& self = *impl;
    self.accepting = false;
    if (self.context == nullptr) return;
    {
        std::lock_guard<std::mutex> lock (self.mutex);
        self.outgoing.clear();
        for (const auto& entry : self.connections)
            self.outgoing.push_back ({ entry.second, "{\"t\":\"bye\",\"reason\":\"" + reason + "\"}", true });
        self.stopping = true;
        self.wake.notify_all();
    }
    if (self.writer.joinable()) self.writer.join();
    // Workers only enqueue callbacks; none waits on the message thread during mg_stop.
    mg_stop (self.context);
    self.context = nullptr;
    std::lock_guard<std::mutex> lock (self.mutex);
    self.connections.clear();
    self.outgoing.clear();
}
}
