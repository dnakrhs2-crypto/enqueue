#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gocue::remote
{
/** No JUCE or CivetWeb headers cross this boundary. Callbacks never wait for the UI. */
class Server
{
public:
    struct Asset { std::string path, mime; const char* data; size_t size; };
    struct Callbacks
    {
        std::function<void (uint64_t)> opened, closed;
        std::function<void (uint64_t, const std::string&)> received;
    };
    Server (std::vector<Asset>, Callbacks);
    ~Server();
    int start (const std::string& key, bool loopback, int preferredPort = 49820);
    void send (uint64_t connection, std::string message);
    void stop (const std::string& reason);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
