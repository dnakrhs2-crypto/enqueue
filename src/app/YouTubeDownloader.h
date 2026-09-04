#pragma once

#include "app/YouTubeTools.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>

namespace gocue
{

/** Fetches a YouTube video's audio on this PC and makes an mp3 (320 kbps) out of it: yt-dlp (with the QuickJS
    runtime) downloads the best audio stream (m4a), the app decodes it and LAME encodes the mp3. One job at a
    time on its own thread; progress arrives on the message thread. When yt-dlp fails because YouTube changed,
    yt-dlp updates itself (once an hour at most) and the download is tried once more. */
class YouTubeDownloader : private juce::Thread,
                          private juce::AsyncUpdater
{
public:
    enum class Stage { idle, preparing, fetching, updatingTool, converting, done, failed, cancelled };

    struct Progress
    {
        Stage stage = Stage::idle;
        double fraction = -1.0;   // 0..1 when known (fetching: yt-dlp's percentage; converting: samples), else -1
        juce::String message;     // for the status line
        juce::String detail;      // extra lines for the log (yt-dlp's last words on a failure), may be empty
        juce::File file;          // the mp3, once done
    };

    /** 'appVersion' tells the tools folder which app version seeded it. */
    explicit YouTubeDownloader (juce::String appVersion);
    ~YouTubeDownloader() override;

    /** Starts one download into 'directory' (created when missing). False while a job is still running. */
    bool start (const juce::String& videoUrl, const juce::File& directory);
    /** Stops the running job (the partial files are deleted). */
    void cancel();
    bool isBusy() const noexcept { return busy.load(); }

    /** Called on the message thread with the latest state (a burst of changes arrives as its last one). */
    std::function<void (const Progress&)> onProgress;

    //==============================================================================
    // Pure helpers (unit-tested)

    /** A YouTube link, trimmed and unquoted, "https://" added when the scheme is missing; empty when the text is
        not a YouTube address (youtube.com or youtu.be, any subdomain). */
    static juce::String normaliseUrl (const juce::String& text);

    /** The percentage in a yt-dlp progress line ("[download]  45.3% of ...") as 0..1, or -1 for other lines. */
    static double parseProgress (const juce::String& line);

    /** What went wrong, from yt-dlp's output: a Korean message, and whether it is about the video itself
        (private, removed, no audio format, a bad address) rather than about the tool - the latter is what a
        yt-dlp update can fix. */
    struct Failure
    {
        juce::String message;
        bool aboutTheVideo = false;
    };
    static Failure classifyFailure (const juce::StringArray& outputLines);

    /** yt-dlp's arguments for one download (the executable first): audio only, best m4a, into 'outputTemplate'. */
    static juce::StringArray ytDlpArguments (const YouTubeTools::Paths& tools, const juce::String& url, const juce::File& outputDirectory);

private:
    void run() override;
    void handleAsyncUpdate() override;
    void report (Stage stage, double fraction, const juce::String& message, const juce::String& detail = {}, const juce::File& file = {});
    void fail (const juce::String& message, const juce::String& detail = {}) { report (Stage::failed, 0.0, message, detail); }

    /** Runs yt-dlp; returns the downloaded file (or none) with the exit code and the output lines. */
    juce::File runYtDlp (const YouTubeTools::Paths& tools, const juce::File& workDirectory, int& exitCode, juce::StringArray& lines);
    /** Decodes 'source' and writes the mp3 to 'target'; an error text when it could not. */
    juce::String convertToMp3 (const juce::File& source, const juce::File& target, const juce::File& lame);

    const juce::String appVersion;
    juce::String videoUrl;
    juce::File directory;

    juce::CriticalSection lock;
    Progress pending;                          // under 'lock'
    juce::ChildProcess* activeProcess = nullptr;   // under 'lock': killed by cancel()
    std::atomic<bool> busy { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (YouTubeDownloader)
};

} // namespace gocue
