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
    runtime) downloads the best AAC stream (m4a), the app decodes it to a WAV and LAME encodes the mp3. One job at
    a time on its own thread; progress arrives on the message thread. When yt-dlp fails because YouTube changed,
    yt-dlp updates itself (once an hour at most) and the download is tried once more. Every tool runs through
    ToolProcess, so a cancel or a deadline ends it. */
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
    /** Stops the running job: the tool running at that moment is ended, the partial files are deleted. */
    void cancel();
    bool isBusy() const noexcept { return busy.load(); }

    /** Called on the message thread with the latest state (a burst of changes arrives as its last one). */
    std::function<void (const Progress&)> onProgress;

    //==============================================================================
    // Pure helpers (unit-tested)

    /** A link to ONE YouTube video, trimmed and unquoted, "https://" added when the scheme is missing: a watch
        link with v=, youtu.be/<id>, /shorts/<id>, /live/<id>, /embed/<id> on youtube.com or youtu.be (any
        subdomain). Empty for anything else - a playlist, a channel, another site. */
    static juce::String normaliseUrl (const juce::String& text);

    /** The percentage in a yt-dlp progress line ("[download]  45.3% of ...") as 0..1, or -1 for other lines. */
    static double parseProgress (const juce::String& line);

    /** What went wrong, from yt-dlp's output: a Korean message and the kind of problem - the video itself
        (private, removed, no audio format, a bad address), the network (offline, YouTube unreachable), or the
        tool (anything else: what a yt-dlp update may fix). */
    struct Failure
    {
        enum class Kind { video, network, tool };
        juce::String message;
        Kind kind = Kind::tool;
    };
    static Failure classifyFailure (const juce::StringArray& outputLines);

    /** yt-dlp's arguments for one download (the executable first): one video, its best AAC audio, into
        'outputDirectory', the final path printed last. */
    static juce::StringArray ytDlpArguments (const YouTubeTools::Paths& tools, const juce::String& url, const juce::File& outputDirectory);

    /** LAME's arguments for the mp3 (the executable first): 320 kbps CBR from 'wav' into 'mp3'. */
    static juce::StringArray lameArguments (const juce::File& lame, const juce::File& wav, const juce::File& mp3);

private:
    void run() override;
    void handleAsyncUpdate() override;
    void report (Stage stage, double fraction, const juce::String& message, const juce::String& detail = {}, const juce::File& file = {});
    void fail (const juce::String& message, const juce::String& detail = {}) { report (Stage::failed, 0.0, message, detail); }

    /** Runs yt-dlp; returns the downloaded file (or none) with the outcome and the output lines. */
    juce::File runYtDlp (const YouTubeTools::Paths& tools, const juce::File& workDirectory, ToolProcess::Result& outcome, juce::StringArray& lines);
    /** Decodes 'source' to a WAV next to it and has LAME write 'target'; an error text when it could not. */
    juce::String convertToMp3 (const juce::File& source, const juce::File& target, const juce::File& lame);

    const juce::String appVersion;
    juce::String videoUrl;
    juce::File directory;

    juce::CriticalSection lock;
    Progress pending;                          // under 'lock'
    std::atomic<bool> cancelRequested { false };
    std::atomic<bool> busy { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (YouTubeDownloader)
};

} // namespace gocue
