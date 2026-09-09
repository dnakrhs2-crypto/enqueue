#pragma once
#include "RecorderDocument.h"
#include "RecorderSettings.h"
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
}
