#pragma once

#include "CollabContracts.h"
#include <juce_core/juce_core.h>

struct lm_obs_metadata;

namespace gocue::livemix
{
/** Single control-thread writer. Roster/slot owners supply the resolved audio ring name.
    No method belongs on the audio callback. close() publishes an empty slot first. */
class ObsMetadata
{
public:
    ObsMetadata() = default;
    ~ObsMetadata();

    bool open (const juce::String& resolvedPeerRing, unsigned slot);
    void publish (uint64_t session, collab::contracts::ParticipantId,
                  collab::contracts::StreamEpoch, collab::contracts::ParticipantKind,
                  collab::contracts::ConnectionState, const juce::String& nickname);
    void clear();
    void close();

private:
    void store (const lm_obs_metadata&);

    void* mapping = nullptr;
    lm_obs_metadata* view = nullptr;
    unsigned slotNumber = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ObsMetadata)
};
}
