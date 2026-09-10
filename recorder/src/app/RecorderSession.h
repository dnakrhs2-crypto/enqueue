#pragma once
#include "RecorderDocument.h"
#include "RecorderSettings.h"
#include "RecorderLifecycle.h"
#include "record/TakeController.h"
#include "playback/TimelineTransport.h"
#include "playback/VideoPlaybackEngine.h"
#include "video/PreviewPresenter.h"
#include "capture/MfCameraCapture.h"
#include "media/ThumbnailCache.h"
#include <map>

namespace gocue::recorder
{
// App coordinator. Commands/tick run on the document owner; capture, present,
// indexing and derived media have independent workers. No GUI dependency.
class RecorderSession
{
public:
    explicit RecorderSession(RecorderDocument&);
    ~RecorderSession();
    void setHosts(std::array<void*, 2>);
    juce::Result configure(UserSettings); // completion through onConfigured
    void enterTimeline(bool);
    juce::Result record();
    juce::Result setCalibrationProfiles(std::vector<CalibrationProfile>); // outside an active take
    juce::Result stopRecording();
    void play(bool latestTake = false);
    void pause();
    void stopPlayback();
    void goToStart();
    void scrub(Sample, bool released);
    void addMarker();
    void setMonitoring(std::uint8_t mask);
    void updateMicrophoneSettings(const UserSettings&);
    void refreshPlaybackPlan();
    void projectChanged();
    void tick();
    bool busy() const;
    bool configuring() const;
    bool recording() const;
    bool readyToRecord() const;
    bool playing() const;
    Sample playhead() const;
    Sample elapsed() const;
    TakeController& takeController() { return take; }
    RecorderAudioEngine& audioEngine() { return audio; }
    const RecorderAudioEngine::DeviceInfo& deviceInfo() const { return device; }
    std::shared_ptr<RecorderLifecycle> lifecycleState() const { return lifecycle; }
    void requestShutdown();
    bool readyForShutdownCommit() const;
    void releaseForShutdown(); // only after the owner has committed its document/settings
    bool shutdownComplete() const;
    void resumeFromSleep();
    // Export/dubbing coordinators acquire this gate before launching work and
    // release it only after their callback/worker checkpoint barrier has completed.
    juce::Result beginExclusive(RecorderLifecycle::Activity, std::function<void()> requestStop = {});
    void endExclusive(RecorderLifecycle::Activity);
    bool cameraReady(unsigned) const;
    juce::String cameraCaption(unsigned) const;
    bool showingPlayback() const { return playback != nullptr; }
    juce::String error, notice;
    std::function<void(const juce::Result&, const UserSettings&)> onConfigured;
    std::function<void(const Id&, std::shared_ptr<PeakCache>, unsigned)> onPeaks;
    std::function<void(const Id&, PeakSnapshot, unsigned)> onLoadedPeaks;
    std::function<void(const Id&, std::vector<ThumbnailFrame>)> onThumbnails;
    std::int64_t firstPlaybackVideoQpc = 0, firstPlaybackAudioQpc = 0, firstPlaybackAudibleQpc = 0;
    std::int64_t playbackButtonQpc = 0;
    // Worker-only adapter shared with the headless writer-to-playback regression.
    static std::shared_ptr<const WavSource> indexRecordedAudio(const MediaAsset&, const juce::File& folder, unsigned Fs, const Id& track);
private:
    struct LiveCamera;
    struct Playback;
    struct PreparedPlan;
    class SharedOutput;
    void clearPlayback();
    void presentLive();
    void preparePlayback();
    void scheduleDerived();
    RecorderDocument& document;
    std::shared_ptr<RecorderLifecycle> lifecycle = std::make_shared<RecorderLifecycle>();
    RecorderAudioEngine audio;
    TakeController take;
    std::array<std::unique_ptr<LiveCamera>, 2> cameras;
    std::array<void*, 2> hosts{};
    RecorderAudioEngine::DeviceInfo device;
    UserSettings current;
    std::vector<CalibrationProfile> calibrationProfiles;
    struct DeviceResult { juce::Result result = juce::Result::ok(); UserSettings settings; };
    std::future<DeviceResult> deviceWork;
    std::future<std::unique_ptr<PreparedPlan>> planWork;
    std::unique_ptr<Playback> playback;
    bool timeline = false, wantPlay = false, pendingLatest = false, autoStart = false;
    bool shuttingDown = false, resourcesReleased = false, permitRelease = false;
    bool recordAfterExport = false;
    std::future<void> releaseWork;
    std::map<RecorderLifecycle::Activity, std::function<void()>> exclusiveStops;
    Sample cursor = 0;
    Id peaksPublished, derivedProject;
    std::vector<Marker> recordedMarkers;
    std::map<juce::String, std::shared_ptr<const VideoIndex>> videoIndexes;
    std::map<juce::String, std::shared_ptr<const WavSource>> wavIndexes;
    std::set<juce::String> derivedKeys;
    struct Derived { Id asset; std::vector<ThumbnailFrame> thumbs; PeakSnapshot peaks; unsigned channel = 0; };
    std::mutex derivedMutex;
    std::deque<Derived> derivedResults;
    ThumbnailCache derivedWorker; // destroyed/joined before callback result storage
};
// First-run audio defaults for a freshly chosen device: microphone 1 on input 1, playback on outputs 1/2 (mono on a
// single-output device). Runs once per device choice (UserSettings::audioDefaultsApplied); later explicit "none"
// choices are kept. Returns whether any mapping changed.
bool applyAudioDefaults(UserSettings&, const RecorderAudioEngine::DeviceInfo&);
}
