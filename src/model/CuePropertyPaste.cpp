#include "model/CuePropertyPaste.h"

namespace gocue::CuePropertyPaste
{

void apply (const Cue& source, Cue& target, const Selection& sel)
{
    if (sel.basics)
    {
        target.color = source.color;
        target.secondColor = source.secondColor;
        target.useSecondColor = source.useSecondColor;
        target.flagged = source.flagged;
        target.armed = source.armed;
        target.skipIfDisarmed = source.skipIfDisarmed;
        target.autoLoad = source.autoLoad;
        target.notes = source.notes;
    }

    if (sel.timing)
    {
        target.preWaitSeconds = source.preWaitSeconds;
        target.postWaitSeconds = source.postWaitSeconds;
        target.continueMode = source.continueMode;
    }

    if (sel.triggers)
    {
        target.secondTrigger = source.secondTrigger;
        target.wallClock = source.wallClock;
        target.fadeStopOthers = source.fadeStopOthers;
        target.duck = source.duck;
    }

    if (sel.timeLoops)
    {
        target.audio = source.audio;

        if (target.file != source.file && target.durationSeconds > 0.0)   // a different file: keep the trim inside it
        {
            target.audio.startSeconds = juce::jlimit (0.0, juce::jmax (0.0, target.durationSeconds - 0.01), source.audio.startSeconds);

            if (target.audio.endSeconds > target.durationSeconds || (target.audio.endSeconds >= 0.0 && target.audio.endSeconds <= target.audio.startSeconds))
                target.audio.endSeconds = -1.0;
        }
    }

    if (sel.levels)
    {
        target.gainDb = source.gainDb;
        target.fadeOutMs = source.fadeOutMs;
    }

    if (sel.effects)
        target.plugins = source.plugins;

    if (sel.fade && source.isFade() && target.isFade())
    {
        const auto keepTarget = target.fade.targetId;
        target.fade = source.fade;
        target.fade.targetId = keepTarget;
    }

    if (sel.fade && source.isDevamp() && target.isDevamp())
    {
        const auto keepTarget = target.devamp.targetId;
        target.devamp = source.devamp;
        target.devamp.targetId = keepTarget;
    }

    if (sel.fade && source.isControl() && target.isControl())
    {
        const auto keepTarget = target.control.targetId;
        target.control = source.control;
        target.control.targetId = keepTarget;
    }

    if (sel.fade && source.isMic() && target.isMic())
        target.mic = source.mic;

    if (sel.fade && source.isGroup() && target.isGroup())
    {
        const bool keepCollapsed = target.group.collapsed;
        target.group = source.group;
        target.group.collapsed = keepCollapsed;
    }

    target.sanitise();
}

} // namespace gocue::CuePropertyPaste
