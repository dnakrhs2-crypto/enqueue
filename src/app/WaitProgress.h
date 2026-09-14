#pragma once

#include <juce_core/juce_core.h>

namespace gocue
{

/** A wait that runs right now, for the UI (the cue table's pre-wait / post-wait columns, the active cues panel):
    a cue's pre-wait counting down to its start, an auto-continue cue's post-wait counting down to the next cue's
    pre-wait (or start), or a wait cue's own wait. Times are the controller's clock (seconds). */
struct WaitProgress
{
    enum class Kind { preWait, postWait, waitCue };

    juce::Uuid cueId = juce::Uuid::null();   // (a default-constructed Uuid would be a random one)
    Kind kind = Kind::preWait;
    double startedAt = 0.0;   // when this wait began
    double endsAt = 0.0;      // when it is over (the start fires / the next cue's pre-wait begins / the wait cue continues)
    bool audition = false;    // put on by an audition GO
    /** The cue whose scheduled start this wait leads to: the cue itself for a pre-wait, the next cue for a post-wait
        (cancelling that start is what "stopping" a post-wait means), null for a wait cue (its follow is its own). */
    juce::Uuid startsCueId = juce::Uuid::null();

    double total() const noexcept { return endsAt - startedAt; }
    double remaining (double now) const noexcept { return endsAt > now ? endsAt - now : 0.0; }

    /** 0..1 through the wait at 'now'. */
    double fraction (double now) const noexcept
    {
        const double length = total();

        if (! (length > 0.0))
            return 1.0;

        const double f = (now - startedAt) / length;
        return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
    }
};

} // namespace gocue
