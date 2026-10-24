#pragma once
#include "app/CueController.h"
#include "remote/RemoteProtocol.h"

namespace gocue::remote
{
/** The PC and web consume one message-thread engine copy per 30 Hz tick. */
struct PlaybackSnapshot
{
    std::vector<AudioEngine::PlayingCue> playing;
    std::vector<FadeRunner::Info> fades;
    std::vector<WaitProgress> waits;
    double waitClock = 0;
    int running = 0, paused = 0;
    bool panic = false, goLocked = false;
    static PlaybackSnapshot capture (AudioEngine&, CueController&);
};

class SnapshotModel : private CueList::Listener, private ProjectDocument::Listener
{
public:
    explicit SnapshotModel (ProjectDocument&);
    ~SnapshotModel() override;
    void changed() { dirty = true; }
    juce::var lists (bool show);
    juce::var state (const PlaybackSnapshot&, bool show) const;
private:
    void cueListStructureChanged() override { changed(); }
    void cueChanged (int) override { changed(); }
    void playheadChanged (int) override { changed(); }
    void documentStateChanged() override { changed(); }
    void containersChanged() override { changed(); }
    void cueListsChanged() override { changed(); }
    ProjectDocument& document;
    bool dirty = true;
    juce::int64 generation = 0;
    juce::var cachedLists;
};
}
