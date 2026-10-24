#include "remote/RemoteSnapshot.h"
#include "ui/UiUtils.h"

namespace gocue::remote
{
PlaybackSnapshot PlaybackSnapshot::capture (AudioEngine& engine, CueController& controller)
{
    PlaybackSnapshot value;
    value.playing = engine.getPlayingCues(); // engine lock ends here, before any JSON work
    value.fades = controller.getFadeRunner().getRunning();
    value.waits = controller.getRunningWaits();
    value.waitClock = controller.clock();
    value.panic = controller.isPanicLatched();
    value.goLocked = controller.isGoLocked();
    for (const auto& fade : value.fades)
    {
        AudioEngine::PlayingCue p;
        p.id = fade.fadeId;
        p.positionSeconds = fade.elapsedSeconds;
        p.lengthSeconds = fade.durationSeconds;
        p.remainingSeconds = juce::jmax (0.0, fade.durationSeconds - fade.elapsedSeconds);
        p.progress = fade.durationSeconds > 0 ? juce::jlimit (0.0, 1.0, fade.elapsedSeconds / fade.durationSeconds) : 1.0;
        p.startOrder = 9007199254740991LL; // after audio, also exactly representable by the browser
        value.playing.push_back (p);
    }
    for (const auto& p : value.playing)
        if (! p.loaded) { ++value.running; if (p.paused) ++value.paused; }
    return value;
}
namespace
{
juce::var nullable (const juce::Uuid& id) { return id.isNull() ? juce::var() : juce::var (id.toString()); }
juce::String lengthText (const CueList& list, int index)
{
    const double effective = list.effectiveLengthOf (index);
    return effective < 0 ? ko ("∞") : formatSeconds (effective > 0 ? effective : list.get (index).durationSeconds);
}
const char* cueKind (const Cue& cue)
{
    if (cue.isControl())
    {
        static const char* kinds[] { "start", "stop", "pause", "load", "reset", "goto", "wait", "memo", "arm", "disarm", "target" };
        return kinds[juce::jlimit (0, 10, static_cast<int> (cue.control.kind))];
    }
    static const char* kinds[] { "audio", "fade", "devamp", "group", "control", "mic" };
    return kinds[juce::jlimit (0, 5, static_cast<int> (cue.type))];
}
}
SnapshotModel::SnapshotModel (ProjectDocument& d) : document (d)
{
    document.addListener (this);
    document.cues.addListener (this);
}
SnapshotModel::~SnapshotModel()
{
    document.cues.removeListener (this);
    document.removeListener (this);
}
juce::var SnapshotModel::lists (bool show)
{
    if (! dirty && cachedLists["mode"].toString() == (show ? "show" : "edit")) return cachedLists;
    dirty = false;
    juce::Array<juce::var> tabs;
    auto cues = object ({}), heads = object ({});
    for (int tab = 0; tab < document.getNumContainers(); ++tab)
    {
        const auto info = document.getContainerInfo (tab);
        const auto tabId = info.id.toString();
        tabs.add (object ({ { "id", tabId }, { "name", info.name }, { "kind", info.isCart ? "cart" : "list" },
                           { "cols", info.cartCols }, { "rows", info.cartRows } }));
        juce::Array<juce::var> rows;
        juce::var head;
        for (const auto& id : document.cueIdsOf (tab))
        {
            int index = -1;
            const auto* list = document.listContaining (id, &index);
            if (list == nullptr) continue;
            const auto& cue = list->get (index);
            rows.add (object ({ { "id", id.toString() }, { "no", cue.number }, { "name", cue.name }, { "kind", cueKind (cue) },
                { "depth", list->depthOf (index) }, { "parent", nullable (cue.parentId) }, { "len", lengthText (*list, index) },
                { "armed", cue.armed }, { "color", cue.color }, { "slot", info.isCart ? juce::var (index) : juce::var() } }));
            if (list->getPlayheadIndex() == index) head = id.toString();
        }
        cues.getDynamicObject()->setProperty (tabId, rows);
        heads.getDynamicObject()->setProperty (tabId, head);
    }
    cachedLists = object ({ { "t", "lists" }, { "gen", ++generation },
        { "active", document.getContainerInfo (document.getActiveContainer()).id.toString() }, { "mode", show ? "show" : "edit" },
        { "tabs", tabs }, { "cues", cues }, { "playhead", heads } });
    return cachedLists;
}
juce::var SnapshotModel::state (const PlaybackSnapshot& snapshot, bool show) const
{
    juce::var next;
    if (const auto* cue = document.cues.getPlayhead())
        next = object ({ { "id", cue->id.toString() }, { "no", cue->number }, { "name", cue->name },
                        { "len", lengthText (document.cues, document.cues.getPlayheadIndex()) } });
    juce::Array<juce::var> playing, waits;
    for (const auto& p : snapshot.playing)
    {
        if (p.loaded) continue;
        const auto* cue = document.findCueAnywhere (p.id);
        const bool fade = p.fadingOut || std::any_of (snapshot.fades.begin(), snapshot.fades.end(), [&] (const auto& f) { return f.fadeId == p.id; });
        playing.add (object ({ { "id", p.id.toString() }, { "no", cue != nullptr ? cue->number : juce::String() },
            { "name", cue != nullptr ? cue->name : juce::String() }, { "remain", p.remainingSeconds }, { "elapsed", p.positionSeconds },
            { "dur", p.lengthSeconds }, { "progress", p.progress }, { "st", p.paused ? "pause" : fade ? "fade" : "play" }, { "order", p.startOrder } }));
    }
    for (const auto& w : snapshot.waits)
    {
        const auto* cue = document.findCueAnywhere (w.cueId);
        waits.add (object ({ { "cue", w.cueId.toString() }, { "no", cue != nullptr ? cue->number : juce::String() },
            { "name", cue != nullptr ? cue->name : juce::String() },
            { "kind", w.kind == WaitProgress::Kind::preWait ? "pre" : w.kind == WaitProgress::Kind::postWait ? "post" : "wait" },
            { "remain", w.remaining (snapshot.waitClock) }, { "startId", w.startId } }));
    }
    return object ({ { "t", "state" }, { "mode", show ? "show" : "edit" }, { "next", next }, { "playing", playing }, { "waits", waits },
        { "counts", object ({ { "playing", snapshot.running }, { "paused", snapshot.paused }, { "waiting", waits.size() } }) },
        { "panic", snapshot.panic }, { "goLocked", snapshot.goLocked } });
}
}
