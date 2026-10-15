#pragma once
#include "CollabTransport.h"
#include "CollabUpnp.h"
#include <functional>

namespace gocue::livemix::collab
{
// Synchronous HTTP boundary; CollabSession calls it only on its worker.
class CollabRoom
{
public:
    struct Request
    {
        juce::String url, body;
        juce::String headers = "Content-Type: application/json\r\n";
        int timeoutMs = 8000;
    };
    struct Response { int statusCode = 0; juce::String body; };
    using RequestFunction = std::function<Response (const Request&)>;
    struct Reply
    {
        int statusCode = 0;
        juce::String error;
        bool ok() const { return error.isEmpty(); }
    };
    struct Created : Reply
    {
        juce::String code, hostToken, publicIp;
        uint64_t session = 0;
        Nonce nonce {};
    };
    struct Joined : Reply
    {
        uint64_t session = 0;
        juce::String hostNick;
        std::vector<CollabTransport::Address> candidates;
        bool sameNetwork = false;
    };

    explicit CollabRoom (RequestFunction request = {}, juce::String server = {});
    Created create (const juce::String& nick, const juce::String& inst, int port,
                    const juce::StringArray& lan, const CollabUpnp::Result&);
    Reply confirm (const juce::String& code, const juce::String& token, bool reachable, const juce::String& reason);
    Reply heartbeat (const juce::String& code, const juce::String& token, bool connected);
    Joined join (const juce::String& code, const juce::String& nick, const juce::String& inst);
    Reply close (const juce::String& code, const juce::String& token);
    static juce::String errorMessage (int statusCode);

private:
    Reply post (const char* action, const juce::var& body, juce::var& response);
    Reply command (const char* action, const juce::var& body);
    static Response httpRequest (const Request&);
    RequestFunction request;
    juce::String server;
};
}
