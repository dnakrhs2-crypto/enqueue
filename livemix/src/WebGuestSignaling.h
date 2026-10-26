#pragma once
#include "CollabRoom.h"

namespace gocue::livemix::collab
{
// Host only. Commands and callbacks run on our control thread; the two event
// streams each have their own poll thread. R8 queues callbacks to its session.
// startHost/stop/destruction belong to the owner thread, outside callbacks.
class WebGuestSignaling
{
public:
    struct Invite
    {
        juce::String inviteToken, hostNick;
        uint32_t inviteGeneration = 0;
    };
    struct PendingRequest { juce::String requestId, nickname; int64_t expiresAtMs = 0; };
    struct Accepted
    {
        contracts::WebConnection connection;
        juce::String requestId, connectionToken;
        int64_t reservationExpiresAtMs = 0;
    };
    struct Callbacks
    {
        std::function<void(const Invite&)> onInvite;
        std::function<void(const PendingRequest&)> onRequest;
        std::function<void(const juce::String&)> onRequestEnded;
        std::function<void(const Accepted&)> onAccepted;
        std::function<void(uint32_t, const juce::var&)> onSignal;
        std::function<void(uint32_t, const juce::String&)> onEnded;
        std::function<void(int, const juce::String&)> onError;
    };
    explicit WebGuestSignaling (CollabRoom::RequestFunction = {}, juce::String server = {});
    ~WebGuestSignaling();
    void startHost (const juce::String& code, const juce::String& hostToken, Callbacks);
    void createInvite();
    void accept (const juce::String& requestId);
    void reject (const juce::String& requestId);
    void kick (contracts::ParticipantId, uint32_t generation);
    void sendSignal (uint32_t generation, const juce::var& signal);
    void leave (uint32_t generation);
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
