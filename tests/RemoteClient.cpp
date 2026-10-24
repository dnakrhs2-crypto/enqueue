#include "RemoteClient.h"
#include "civetweb.h"
#include <mutex>

namespace gocue::tests
{
struct RemoteClient::Impl
{
    mg_connection* socket = nullptr;
    std::mutex mutex;
    std::vector<std::string> received;
    static int data (mg_connection*, int bits, char* bytes, size_t size, void* user)
    {
        if ((bits & 15) == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) return 0;
        if ((bits & 15) == MG_WEBSOCKET_OPCODE_TEXT)
        {
            auto& self = *static_cast<Impl*> (user);
            std::lock_guard<std::mutex> lock (self.mutex);
            self.received.emplace_back (bytes, size);
        }
        return 1;
    }
};
RemoteClient::RemoteClient() : impl (std::make_unique<Impl>()) {}
RemoteClient::~RemoteClient() { close(); }
bool RemoteClient::connect (int port, const std::string& key)
{
    char message[256] {};
    impl->socket = mg_connect_websocket_client ("127.0.0.1", port, 0, message, sizeof (message), ("/ws?k=" + key).c_str(),
                                                nullptr, &Impl::data, nullptr, impl.get());
    error = message;
    return impl->socket != nullptr;
}
void RemoteClient::send (const std::string& text) { if (impl->socket != nullptr) mg_websocket_client_write (impl->socket, MG_WEBSOCKET_OPCODE_TEXT, text.data(), text.size()); }
void RemoteClient::close() { if (impl->socket != nullptr) { mg_close_connection (impl->socket); impl->socket = nullptr; } }
std::vector<std::string> RemoteClient::take()
{
    std::lock_guard<std::mutex> lock (impl->mutex);
    std::vector<std::string> result;
    result.swap (impl->received);
    return result;
}
std::string RemoteClient::http (int port, const std::string& path, int& status)
{
    char error[256] {};
    auto* connection = mg_download ("127.0.0.1", port, 0, error, sizeof (error), "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path.c_str());
    if (connection == nullptr) { status = 0; return error; }
    status = mg_get_response_info (connection)->status_code;
    std::string response;
    for (const char* header : { "Content-Type", "Cache-Control" })
        if (const auto* value = mg_get_header (connection, header)) response += std::string (header) + ": " + value + "\n";
    char buffer[4096];
    int size;
    while ((size = mg_read (connection, buffer, sizeof (buffer))) > 0) response.append (buffer, static_cast<size_t> (size));
    mg_close_connection (connection);
    return response;
}
}
