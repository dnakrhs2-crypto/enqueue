#include "app/YouTubeTools.h"

namespace gocue
{

namespace
{
    juce::String k (const char* utf8) { return juce::String::fromUTF8 (utf8); }

    const char* const toolNames[] = { "yt-dlp.exe", "qjs.exe", "lame.exe", "libsndfile-1.dll" };   // lame needs the dll

    /** Runs a program and returns its output (stdout + stderr), waiting at most 'timeoutMs'. */
    juce::String runAndRead (const juce::StringArray& args, int timeoutMs, bool* finished = nullptr)
    {
        juce::ChildProcess process;

        if (! process.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            if (finished != nullptr)
                *finished = false;

            return {};
        }

        juce::MemoryOutputStream collected;
        const auto deadline = juce::Time::currentTimeMillis() + timeoutMs;
        char buffer[4096];

        for (;;)
        {
            const int n = process.readProcessOutput (buffer, (int) sizeof (buffer));

            if (n > 0)
                collected.write (buffer, (size_t) n);
            else if (! process.isRunning())
                break;
            else
                juce::Thread::sleep (20);

            if (juce::Time::currentTimeMillis() > deadline)
            {
                process.kill();

                if (finished != nullptr)
                    *finished = false;

                return juce::String::fromUTF8 ((const char*) collected.getData(), (int) collected.getDataSize());
            }
        }

        if (finished != nullptr)
            *finished = true;

        return juce::String::fromUTF8 ((const char*) collected.getData(), (int) collected.getDataSize());
    }
}

//==============================================================================
juce::File YouTubeTools::bundledDirectory()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getChildFile ("tools");
}

juce::File YouTubeTools::userDirectory()
{
    return juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getChildFile ("Enqueue").getChildFile ("tools");
}

juce::File YouTubeTools::markerFile (const juce::File& directory, const juce::String& appVersion)
{
    return directory.getChildFile ("seeded_by_" + juce::File::createLegalFileName (appVersion) + ".txt");
}

juce::File YouTubeTools::lastUpdateFile (const juce::File& directory)
{
    return directory.getChildFile ("ytdlp_update_attempt.txt");
}

juce::String YouTubeTools::prepare (Paths& paths, const juce::String& appVersion)
{
    const auto bundled = bundledDirectory();
    const auto user = userDirectory();
    user.createDirectory();

    if (! user.isDirectory())
        return k ("도구 폴더를 만들 수 없습니다: ") + user.getFullPathName();

    paths.directory = user;
    paths.ytDlp = user.getChildFile ("yt-dlp.exe");
    paths.qjs = user.getChildFile ("qjs.exe");
    paths.lame = user.getChildFile ("lame.exe");

    const auto marker = markerFile (user, appVersion);
    const bool freshVersion = ! marker.existsAsFile();   // this app version has not seeded the folder yet

    for (const auto* name : toolNames)
    {
        const auto source = bundled.getChildFile (name);
        const auto target = user.getChildFile (name);

        if (! source.existsAsFile())
            continue;   // nothing bundled (a build tree): whatever is in the user folder is used

        bool replace = ! target.existsAsFile();

        if (! replace && freshVersion)
        {
            // yt-dlp may have updated itself since: keep it when it is newer than the bundled one
            replace = juce::String (name) != "yt-dlp.exe"
                      || shouldReplaceWithBundled (ytDlpVersion (target), ytDlpVersion (source));
        }

        if (replace && ! source.copyFileTo (target))
            return k ("도구를 복사하지 못했습니다: ") + target.getFullPathName();
    }

    if (freshVersion)
        marker.replaceWithText (juce::Time::getCurrentTime().toISO8601 (true));

    for (const auto* name : toolNames)
        if (! user.getChildFile (name).existsAsFile())
            return k ("다운로드 도구가 없습니다 (") + juce::String (name) + k ("). 설치 파일로 설치한 앤큐에 들어 있습니다. 백신이 격리했다면 복원해 주세요: ") + user.getFullPathName();

    return {};
}

//==============================================================================
juce::String YouTubeTools::versionFromOutput (const juce::String& output)
{
    for (const auto& token : juce::StringArray::fromTokens (output, " \r\n\t", {}))
    {
        const auto t = token.trim();

        if (t.length() >= 10 && t.length() <= 12 && t.containsOnly ("0123456789.")
            && t.substring (4, 5) == "." && t.substring (7, 8) == ".")
            return t;
    }

    return {};
}

juce::String YouTubeTools::ytDlpVersion (const juce::File& exe)
{
    if (! exe.existsAsFile())
        return {};

    return versionFromOutput (runAndRead ({ exe.getFullPathName(), "--version" }, 30000));
}

bool YouTubeTools::shouldReplaceWithBundled (const juce::String& userVersion, const juce::String& bundledVersion)
{
    if (userVersion.isEmpty())
        return true;   // no copy, or one that cannot even report its version

    if (bundledVersion.isEmpty())
        return false;

    return bundledVersion.compare (userVersion) >= 0;   // "YYYY.MM.DD" compares as text
}

bool YouTubeTools::isRateLimited (juce::int64 lastAttemptMs, juce::int64 nowMs, juce::int64 minIntervalMs)
{
    return lastAttemptMs > 0 && nowMs >= lastAttemptMs && nowMs - lastAttemptMs < minIntervalMs;
}

bool YouTubeTools::selfUpdateYtDlp (const juce::File& exe, juce::String& note)
{
    const auto stamp = lastUpdateFile (exe.getParentDirectory());
    const auto now = juce::Time::currentTimeMillis();
    const auto last = stamp.existsAsFile() ? stamp.loadFileAsString().trim().getLargeIntValue() : 0;

    if (isRateLimited (last, now, selfUpdateMinIntervalMs))
    {
        note = k ("yt-dlp 갱신은 한 시간에 한 번만 시도합니다 (이미 시도함).");
        return false;
    }

    stamp.replaceWithText (juce::String (now));
    const auto before = ytDlpVersion (exe);
    bool finished = false;
    const auto output = runAndRead ({ exe.getFullPathName(), "-U" }, 180000, &finished);

    if (! finished)
    {
        note = k ("yt-dlp 갱신이 끝나지 않았습니다 (시간 초과).");
        return false;
    }

    const auto after = ytDlpVersion (exe);

    if (after.isNotEmpty() && after != before)
    {
        note = k ("yt-dlp를 ") + (before.isEmpty() ? juce::String ("?") : before) + " -> " + after + k (" 로 갱신했습니다.");
        return true;
    }

    note = k ("yt-dlp는 이미 최신입니다 (") + (after.isEmpty() ? before : after) + ").";
    return false;
}

} // namespace gocue
