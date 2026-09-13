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
#include "playback/ImportedAudioCache.h"
#include <map>

namespace gocue::recorder
{
// App coordinator. Commands/tick run on the document owner; capture, present,
// indexing and derived media have independent workers. No GUI dependency.
class RecorderSession
{
public:
    explicit RecorderSession(RecorderDocument&, TakeController::VideoFactory = {});
    ~RecorderSession();
    void setHosts(std::array<void*, 2>);
    juce::Result configure(UserSettings); // completion through onConfigured
    void enterTimeline(bool);
    juce::Result record();
    juce::Result setCalibrationProfiles(std::vector<CalibrationProfile>); // outside an active take
    CalibrationMatch calibrationMatches(const UserSettings&) const;
    juce::Result stopRecording();
    void play(bool latestTake = false);
    juce::Result pause();
    void stopPlayback();
    void goToStart();
    void scrub(Sample, bool released);
    void addMarker();
    void addMarker(const juce::String& name, Sample at, const juce::String& colour = "#4c8dff");
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
    friend struct StabilityTestAccess;
    friend struct ShortcutExceptionTestAccess;
    friend struct ImportUiTestAccess;
    friend struct RecordingPlacementTestAccess;
    struct LiveCamera;
    struct Playback;
    struct PreparedPlan
    {
        std::vector<PlaybackVideoClip> videos;
        std::vector<PlaybackAudioTrack> tracks;
        std::shared_ptr<const CompiledRenderPlan> importedPlan;
        std::vector<AudioSourceBinding> audioSources;
        Sample end = 0, revision = 0;
        Id project;
        std::uint64_t generation = 0;
        juce::String error;
    };
    class SharedOutput;
    void clearPlayback();
    void presentLive();
    void preparePlayback();
    void configurePlacement(TakeController::Config&);
    std::unique_ptr<PreparedPlan> collectPreparedPlan();
    void playbackPreparationFailed(const juce::String&);
    juce::Result configurationFailed(const juce::String&);
    void finishDeviceRelease();
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
    juce::Result startDeviceWork(std::function<DeviceResult()>);
    std::function<void(const char*)> beforeWorkerStart; // owner-thread failure injection; empty in the application
    std::future<DeviceResult> deviceWork;
    std::future<std::unique_ptr<PreparedPlan>> planWork;
    std::shared_ptr<AudioImportControl> planImportControl;
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
    std::map<juce::String, CachedImportedAudio> importedIndexes; // plan worker only; cleared after its join
    std::set<juce::String> derivedKeys;
    struct Derived
    {
        Id asset; std::vector<ThumbnailFrame> thumbs; PeakSnapshot peaks; unsigned channel = 0;
        std::uint64_t requestGeneration = 0;
        Id project;
        Sample mediaGeneration = 0;
    };
    std::mutex derivedMutex;
    std::deque<Derived> derivedResults;
    std::atomic<std::uint64_t> derivedGeneration{1};
    ThumbnailCache derivedWorker; // destroyed/joined before callback result storage
};
// First-run audio defaults for a freshly chosen device: microphone 1 on input 1, playback on outputs 1/2 (mono on a
// single-output device). Runs once per device choice (UserSettings::audioDefaultsApplied); later explicit "none"
// choices are kept. Returns whether any mapping changed.
bool applyAudioDefaults(UserSettings&, const RecorderAudioEngine::DeviceInfo&);
// A project without media is provisional: it follows the open device's sample rate (the first take fixes it). Returns whether Fs changed.
bool adoptDeviceSampleRate(RecorderDocument&, unsigned deviceFs);
// Banner for a fixed project whose device opened at another rate: both numbers and the way back (device panel, then 적용).
juce::String rateMismatchText(unsigned projectFs, unsigned deviceFs);
}
