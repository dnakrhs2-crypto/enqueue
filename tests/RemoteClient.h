#pragma once
#include <memory>
#include <string>
#include <vector>

namespace gocue::tests
{
/** Real CivetWeb client; kept out of JUCE translation units like the server. */
class RemoteClient
{
public:
    RemoteClient();
    ~RemoteClient();
    bool connect (int port, const std::string& key);
    void send (const std::string&);
    void close();
    std::vector<std::string> take();
    std::string error;
    static std::string http (int port, const std::string& path, int& status);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
