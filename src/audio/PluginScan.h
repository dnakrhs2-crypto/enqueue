#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <mutex>
#include <queue>
#include <vector>

namespace gocue
{
/** Installed on the application's PluginHost list; plugin code runs in its scan worker. */
class PluginScanCoordinator final : public juce::KnownPluginList::CustomScanner
{
public:
    struct Options
    {
        juce::File executable;
        juce::File logFile;
        int timeoutMs = 60 * 60 * 1000;   // Waves 17's WaveShell (718 plugins) took about 6 minutes on 2026-09-30; the scan window cancels
    };
    enum class Outcome { completed, crashed, timedOut, cancelled, workerFailed };
    struct Result
    {
        juce::String format, file;
        Outcome outcome = Outcome::workerFailed;
        int numTypes = 0;
        juce::uint32 exitCode = 0, workerPid = 0;
        juce::int64 elapsedMs = 0;
        bool onMessageThread = false;
    };

    explicit PluginScanCoordinator (Options);
    ~PluginScanCoordinator() override;
    bool findPluginTypesFor (juce::AudioPluginFormat&, juce::OwnedArray<juce::PluginDescription>&,
                             const juce::String& file) override;
    void scanFinished() override;
    void cancel() noexcept { cancelRequested = true; }
    /** Before constructing the directory scanner: a stale crash marker can re-blacklist files. */
    void prepareForScan (juce::KnownPluginList&, const juce::File& crashMarker = {});
    std::vector<Result> getResults() const;
    juce::String getSkippedMessage() const;

private:
    class Connection;
    void record (const Result&);
    Options options;
    std::unique_ptr<Connection> connection;
    mutable std::mutex resultsMutex;
    std::vector<Result> results;
    std::atomic<bool> cancelRequested { false };
};

/** No application services. Every format call runs in handleAsyncUpdate. */
class PluginScanWorker final : private juce::ChildProcessWorker, private juce::AsyncUpdater
{
public:
    static bool isWorkerCommandLine (const juce::String&);
    static void suppressCrashDialogs();
    bool start (const juce::String& commandLine);

private:
    void handleMessageFromCoordinator (const juce::MemoryBlock&) override;
    void handleConnectionLost() override;
    void handleAsyncUpdate() override;
    void send (const juce::XmlElement&);
    juce::AudioPluginFormatManager formats;
    std::mutex mutex;
    std::queue<juce::MemoryBlock> pending;
    bool readySent = false;
};
} // namespace gocue
