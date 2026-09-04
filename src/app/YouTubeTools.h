#pragma once

#include <juce_core/juce_core.h>

namespace gocue
{

/** The command-line tools the YouTube download runs on this PC: yt-dlp (fetches the audio), QuickJS (the
    JavaScript runtime yt-dlp needs for YouTube) and LAME (the mp3 encoder). The installer puts them in
    <app folder>/tools; the app keeps its working copies in a per-user folder so yt-dlp can update itself there
    without administrator rights. */
class YouTubeTools
{
public:
    struct Paths
    {
        juce::File directory;   // the per-user tools folder
        juce::File ytDlp, qjs, lame;
    };

    /** <app folder>/tools, where the installer puts the tools (may not exist: a build run from the build tree). */
    static juce::File bundledDirectory();
    /** %LOCALAPPDATA%/Enqueue/tools: the working copies. */
    static juce::File userDirectory();

    /** Makes sure the per-user copies exist, refreshing them from the bundled folder once per app version
        (a bundled yt-dlp that is older than a self-updated copy is left alone). Returns an empty string and the
        paths, or an error text for the user. */
    static juce::String prepare (Paths& paths, const juce::String& appVersion);

    /** "2026.08.19" from `yt-dlp --version`; empty when the program cannot run. */
    static juce::String ytDlpVersion (const juce::File& exe);

    /** Runs yt-dlp's own updater (it replaces its executable). At most once an hour: a later call within the
        hour does nothing. True when the version changed; 'note' says what happened, for the log. */
    static bool selfUpdateYtDlp (const juce::File& exe, juce::String& note);

    static constexpr juce::int64 selfUpdateMinIntervalMs = 60 * 60 * 1000;

    //==============================================================================
    // Pure helpers (unit-tested)

    /** Whether the bundled yt-dlp replaces the per-user copy: no copy yet, or a bundled version ("YYYY.MM.DD")
        that is not older than the copy's. An unreadable version counts as old (a broken file is replaced). */
    static bool shouldReplaceWithBundled (const juce::String& userVersion, const juce::String& bundledVersion);

    /** True when an update was attempted less than 'minIntervalMs' ago. */
    static bool isRateLimited (juce::int64 lastAttemptMs, juce::int64 nowMs, juce::int64 minIntervalMs);

    /** The version out of yt-dlp's `--version` output (the first token that looks like YYYY.MM.DD, else empty). */
    static juce::String versionFromOutput (const juce::String& output);

private:
    static juce::File markerFile (const juce::File& directory, const juce::String& appVersion);
    static juce::File lastUpdateFile (const juce::File& directory);
};

} // namespace gocue
