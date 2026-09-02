#pragma once

#include "model/Cue.h"

namespace gocue::CuePropertyPaste
{

/** Which property groups "Paste Cue Properties" copies (QLab's paste-properties dialog). */
struct Selection
{
    bool basics = true;     // colour, second colour, flag, armed, skip, auto-load, notes
    bool timing = true;     // pre-wait, post-wait, continue mode
    bool triggers = true;   // second trigger, wall clock, fade-stop-others, duck (a hotkey is never pasted: it must stay unique)
    bool timeLoops = true;  // trim, loops, rate, envelope
    bool levels = true;     // gain, stop fade
    bool effects = false;   // VST3 chain (the saved slot states; the caller restores the live chain)
};

/** Copies the selected groups from 'source' onto 'target'. Identity, number, name, file and cached file facts stay.
    A trim pasted onto a different file is clamped to that file's length. */
void apply (const Cue& source, Cue& target, const Selection& selection);

} // namespace gocue::CuePropertyPaste
