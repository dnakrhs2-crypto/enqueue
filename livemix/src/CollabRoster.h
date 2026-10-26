#pragma once
#include "CollabProtocol.h"

namespace gocue::livemix::collab
{
struct RosterMember
{
    contracts::ParticipantId participantId = 0;
    contracts::StreamEpoch streamEpoch = 1;
    contracts::ParticipantKind kind = contracts::ParticipantKind::native;
    contracts::ConnectionState state = contracts::ConnectionState::connecting;
    uint32_t capabilities = 0, deviceRate = 48000, inputLatencyUs = 0, outputLatencyUs = 0;
    juce::String nickname, appVersion;
};
struct RosterSnapshot
{
    uint64_t session = 0;
    contracts::Revision revision = 0;
    uint32_t wireRate = 48000;
    contracts::RoomMode mode = contracts::RoomMode::v2;
    std::array<RosterMember, 3> members {};
    size_t memberCount = 0;
};
const RosterMember* findMember (const RosterSnapshot&, contracts::ParticipantId) noexcept;
bool validateRoster (const RosterSnapshot&, contracts::ParticipantId self,
                     const RosterSnapshot* previous = nullptr);
size_t writeRoster (uint8_t*, size_t capacity, const RosterSnapshot&);
bool readRoster (const uint8_t*, size_t bytes, uint64_t session, contracts::Revision,
                 contracts::ParticipantId self, RosterSnapshot&);

class CollabRoster
{
public:
    enum class Result { rejected, ignored, unchanged, replaced };
    Result apply (const RosterSnapshot&, contracts::ParticipantId self);
    const RosterSnapshot& snapshot() const noexcept { return installed; }
private:
    RosterSnapshot installed;
};
}
