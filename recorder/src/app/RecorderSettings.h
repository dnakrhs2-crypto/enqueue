#pragma once
#include "ProductIdentity.h"
#include "../model/RecorderModel.h"
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace gocue::recorder
{
struct OutputMapping
{
    bool mono = false;
    int left = -1, right = -1, monoChannel = -1; // -1 = explicitly unselected
    juce::Result validate() const;
};
struct UserSettings
{
    juce::String asioDeviceId;
    int bufferSize = 256;
    std::uint32_t preferredSampleRate = 48000;
    std::array<juce::String, 2> cameraDeviceIds, cameraModes;
    std::array<bool, 2> cameraEnabled {true, false};
    std::vector<int> physicalInputs;
    std::array<juce::String, 8> microphoneNames;
    std::array<bool, 8> microphoneArmed {true, true, true, true, true, true, true, true};
    OutputMapping output;
    bool audioDefaultsApplied = false; // first-run input/output defaults were applied for asioDeviceId
    CaptureSnapshot calibration;
    juce::StringArray recentProjects;
    juce::String windowState;
    juce::Result validate() const;
};
class RecorderSettings
{
public:
    explicit RecorderSettings(const juce::File& testRoot = {});
    ~RecorderSettings(); // drains queued writes, joins the worker; never writes on the caller
    juce::Result load(); // startup read; failure preserves the current settings
    const UserSettings& get() const { return state; }
    juce::Result set(UserSettings);
    void rememberProject(const juce::File&);
    std::future<juce::Result> save(); // immutable JUCE PropertySet XML snapshot, queued FIFO
    const juce::File& getFile() const { return settingsFile; }
private:
    struct Request { juce::String xml; std::promise<juce::Result> completion; };
    void run();
    UserSettings state;
    const juce::File settingsFile;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Request> pending;
    bool stopping = false;
    std::thread worker;
};
}
