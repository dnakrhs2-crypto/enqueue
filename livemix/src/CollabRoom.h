#pragma once
#include "CollabTransport.h"
#include "CollabUpnp.h"
#include "CollabContracts.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace gocue::livemix::collab
{
// Synchronous HTTP boundary; CollabSession calls it only on its worker.
class CollabRoom
{
public:
    // A request owns this handle until it returns. Injected request functions must
    // also observe cancellation when they block (wait() is provided for fakes).
    class Cancellation
    {
    public:
        void cancel();
        bool isCancelled() const { return cancelled.load(); }
        bool wait (int milliseconds);
        void setHandler (std::function<void()>);
    private:
        std::atomic<bool> cancelled { false };
        std::mutex mutex;
        std::condition_variable condition;
        std::function<void()> handler;
    };
    struct Request
    {
        juce::String url, body;
        juce::String headers = "Content-Type: application/json\r\n";
        int timeoutMs = 8000;
        std::shared_ptr<Cancellation> cancellation;
    };
    struct Response { int statusCode = 0; juce::String body; };
    using RequestFunction = std::function<Response (const Request&)>;
    struct Reply
    {
        int statusCode = 0;
        juce::String error;
        juce::String errorCode; // API data for control flow; never displayed verbatim
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
    struct Beat : Reply
    {
        std::vector<CollabTransport::Address> guest;   // a joined guest's public addresses, to punch toward
    };
    struct Confirmed : Reply
    {
        bool punch = false;   // the server kept the room for hole punching (an older server closes it instead)
    };

    struct ClientInfo
    {
        juce::String appVersion = "0.17.0", connectionId;
        std::vector<int> protocols { 1, 2 };
        juce::StringArray capabilities { "packet_v2", "roster_ack", "star_3", "pcm16_mono", "web_guest" };
        uint32_t wireRate = 48000;
    };
    struct Member
    {
        uint32_t participantId = 0, connectionGeneration = 0, streamEpoch = 0;
        juce::String kind, nick, appVersion, state;
        std::vector<int> protocols;
        juce::StringArray capabilities;
    };
    struct Reservation
    {
        uint32_t participantId = 0, connectionGeneration = 0;
        juce::String kind;
        int64_t expiresAtMs = 0;
    };
    struct MemberState
    {
        uint32_t participantId = 0, connectionGeneration = 0;
        juce::String state;
        uint32_t streamEpoch = 1;
    };
    struct RoomSnapshot
    {
        int apiVersion = 0; // absent extension = old server, v1 negotiation
        juce::String code, roomMode = "legacy2", nativeState;
        uint64_t session = 0;
        uint32_t wireRate = 0, revision = 0;
        juce::StringArray capabilities;
        std::vector<Member> members;
        std::vector<Reservation> reservations;
    };
    struct CreatedV2 : Created
    {
        RoomSnapshot room;
        uint32_t participantId = 0, connectionGeneration = 0, streamEpoch = 0;
    };
    struct JoinedV2 : Joined
    {
        RoomSnapshot room;
        uint32_t participantId = 0, connectionGeneration = 0, streamEpoch = 0;
    };
    struct ConfirmedV2 : Confirmed { RoomSnapshot room; };
    struct Peer
    {
        uint32_t participantId = 0;
        juce::String connectionId;
        std::vector<CollabTransport::Address> candidates;
    };
    struct BeatV2 : Beat { RoomSnapshot room; std::vector<Peer> peers; };
    struct WebReply : Reply { juce::var json; };

    explicit CollabRoom (RequestFunction request = {}, juce::String server = {},
                         std::shared_ptr<Cancellation> cancellation = {});
    // 'stun' (this socket's public address) lets a host whose port is closed keep the room by hole punching.
    Created create (const juce::String& nick, const juce::String& inst, int port,
                    const juce::StringArray& lan, const CollabUpnp::Result&, const CollabTransport::Mapping& stun = {});
    Confirmed confirm (const juce::String& code, const juce::String& token, bool reachable, const juce::String& reason, bool punch = false);
    Beat heartbeat (const juce::String& code, const juce::String& token, bool connected);
    Joined join (const juce::String& code, const juce::String& nick, const juce::String& inst,
                 const CollabTransport::Mapping& stun = {}, int localPort = 0);
    Reply close (const juce::String& code, const juce::String& token);
    CreatedV2 createV2 (const juce::String& nick, const juce::String& inst, int port,
                       const juce::StringArray& lan, const CollabUpnp::Result&, const ClientInfo&,
                       const CollabTransport::Mapping& stun = {});
    ConfirmedV2 confirmV2 (const juce::String& code, const juce::String& token, bool reachable,
                          const juce::String& reason, bool punch = false);
    BeatV2 heartbeatV2 (const juce::String& code, const juce::String& token, bool connected,
                       const std::vector<MemberState>&);
    JoinedV2 joinV2 (const juce::String& code, const juce::String& nick, const juce::String& inst,
                     const ClientInfo&, const CollabTransport::Mapping& stun = {}, int localPort = 0);
    Reply closeV2 (const juce::String& code, const juce::String& token);

    WebReply webInvite (const juce::String& code, const juce::String& hostToken);
    WebReply webAccept (const juce::String& code, const juce::String& hostToken, const juce::String& requestId);
    WebReply webReject (const juce::String& code, const juce::String& hostToken, const juce::String& requestId);
    WebReply webKick (const juce::String& code, const juce::String& hostToken, uint32_t participantId, uint32_t generation);
    WebReply webSignal (const juce::String& code, const juce::String& connectionToken, uint32_t generation,
                        uint32_t signalId, const juce::var& signal);
    WebReply webEvents (const juce::String& code, const juce::String& token, const juce::String& cursor,
                        uint32_t generation = 0, std::shared_ptr<Cancellation> cancellation = {});
    WebReply webLeave (const juce::String& code, const juce::String& connectionToken, uint32_t generation);
    static bool parseRoomSnapshot (const juce::var&, RoomSnapshot&);
    static juce::String errorMessage (int statusCode);

private:
    Reply post (const char* action, const juce::var& body, juce::var& response, int timeoutMs = 8000,
                std::shared_ptr<Cancellation> cancellation = {});
    Reply command (const char* action, const juce::var& body);
    WebReply webPost (const char* action, const juce::var& body, int timeoutMs = 8000,
                      std::shared_ptr<Cancellation> cancellation = {});
    static Response httpRequest (const Request&);
    RequestFunction request;
    juce::String server;
    std::shared_ptr<Cancellation> defaultCancellation;
};
}
