#pragma once

#include "ControlProtocol.h"

namespace gocue::livemix
{

class MixDocument;
class MuteGroups;

/** Message-thread-only capture and revision tracker. No locks. The server calls update before/after dispatch
    and on its publish poll, AFTER existing document/MuteGroups callbacks have finished. Callbacks only mark
    pending work; they must not capture intermediate setter state. Copies of Snapshot own all their values and
    can be handed to the encoder on a worker. One tracker per service instance, one published copy per client. */
class ControlState
{
public:
    struct Snapshot
    {
        juce::Uuid sessionId = juce::Uuid::null();
        ControlProtocol::Projection projection;
        juce::int64 revision = 0;
        juce::int64 structureRevision = 0;   // remembers structural edits even if coalescing restores the old shape
    };
    enum class ChangeKind { none, values, structure, session };
    struct Difference
    {
        ChangeKind kind = ChangeKind::none;
        juce::int64 baseRevision = 0, revision = 0;
        ControlProtocol::Changes changes;
        bool needsFullState() const { return kind == ChangeKind::structure || kind == ChangeKind::session; }
    };

    static Snapshot capture (const MixDocument&, const MuteGroups&, bool audioRunning);
    static bool sameStructure (const ControlProtocol::Projection&, const ControlProtocol::Projection&);
    /** Compares a client's last published snapshot with the current one. Advanced revision with equal values
        yields kind=values and empty changes. A new client uses a full initial snapshot, not diff. */
    static Difference diff (const Snapshot& published, const Snapshot& current);

    explicit ControlState (Snapshot initial);
    const Snapshot& getCurrent() const noexcept { return current; }
    /** A complete observation advances revision once iff the projection or session generation changed.
        False means the safe-integer revision space is exhausted: the server must restart its instance.
        In that case the previous snapshot is retained; revision never wraps or silently saturates. */
    bool update (Snapshot latest);

private:
    Snapshot current;
};

} // namespace gocue::livemix
