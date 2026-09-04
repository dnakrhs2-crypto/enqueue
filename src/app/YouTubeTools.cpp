#include "app/YouTubeTools.h"

#include <string>
#include <vector>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue
{

namespace
{
    juce::String k (const char* utf8) { return juce::String::fromUTF8 (utf8); }

    const char* const toolNames[] = { "yt-dlp.exe", "qjs.exe", "lame.exe", "libsndfile-1.dll" };   // lame needs the dll

    /** Splits the bytes collected so far into whole lines (\\n or \\r), keeping the unfinished tail. */
    void takeLines (juce::MemoryBlock& partial, bool flushTail, const std::function<void (const juce::String&)>& onLine)
    {
        for (;;)
        {
            const auto* data = (const char*) partial.getData();
            const auto size = partial.getSize();
            size_t cut = 0;

            while (cut < size && data[cut] != '\n' && data[cut] != '\r')
                ++cut;

            if (cut >= size)
                break;

            if (cut > 0 && onLine)
                onLine (juce::String::fromUTF8 (data, (int) cut));

            partial.removeSection (0, cut + 1);
        }

        if (flushTail && partial.getSize() > 0)
        {
            if (onLine)
                onLine (juce::String::fromUTF8 ((const char*) partial.getData(), (int) partial.getSize()));

            partial.reset();
        }
    }

    /** Runs a tool and returns its whole output; 'finished' says whether it ended by itself. */
    juce::String runAndRead (const juce::StringArray& args, int timeoutMs, const std::atomic<bool>* cancel, bool* finished = nullptr)
    {
        juce::String collected;
        const auto result = ToolProcess::run (args, timeoutMs, cancel, [&collected] (const juce::String& line) { collected << line << '\n'; });

        if (finished != nullptr)
            *finished = result.finished;

        return collected;
    }
}

//==============================================================================
juce::String ToolProcess::quoteArgument (const juce::String& argument)
{
    if (argument.isNotEmpty() && ! argument.containsAnyOf (" \t\""))
        return argument;

    juce::String out ("\"");
    int backslashes = 0;

    for (auto c : argument)
    {
        if (c == '\\')
        {
            ++backslashes;
            continue;
        }

        if (c == '"')
        {
            out << juce::String::repeatedString ("\\", backslashes * 2 + 1) << '"';   // the backslashes before a quote double, the quote escapes
            backslashes = 0;
            continue;
        }

        out << juce::String::repeatedString ("\\", backslashes) << juce::String::charToString (c);
        backslashes = 0;
    }

    out << juce::String::repeatedString ("\\", backslashes * 2) << '"';   // backslashes before the closing quote double
    return out;
}

ToolProcess::Result ToolProcess::run (const juce::StringArray& arguments, int timeoutMs, const std::atomic<bool>* cancel,
                                      const std::function<void (const juce::String&)>& onLine)
{
    Result result;

   #if JUCE_WINDOWS
    juce::String commandLine;

    for (int i = 0; i < arguments.size(); ++i)
    {
        if (i > 0)
            commandLine << ' ';

        commandLine << quoteArgument (arguments[i]);
    }

    SECURITY_ATTRIBUTES inheritable = {};
    inheritable.nLength = sizeof (inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr, writePipe = nullptr;

    if (! CreatePipe (&readPipe, &writePipe, &inheritable, 0))
        return result;

    SetHandleInformation (readPipe, HANDLE_FLAG_INHERIT, 0);   // the child gets the write end only
    HANDLE nul = CreateFileW (L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW startup = {};
    startup.cb = sizeof (startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION process = {};

    std::wstring wide (commandLine.toWideCharPointer());
    std::vector<wchar_t> command (wide.begin(), wide.end());
    command.push_back (0);

    const BOOL created = CreateProcessW (nullptr, command.data(), nullptr, nullptr, TRUE,
                                         CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &process);
    CloseHandle (writePipe);   // the parent's copy: the pipe breaks when the child is gone

    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle (nul);

    if (! created)
    {
        CloseHandle (readPipe);
        return result;
    }

    result.started = true;
    CloseHandle (process.hThread);

    const auto deadline = juce::Time::currentTimeMillis() + (juce::int64) timeoutMs;
    juce::MemoryBlock partial;
    char buffer[4096];
    bool running = true;

    for (;;)
    {
        DWORD available = 0;

        if (PeekNamedPipe (readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0)
        {
            DWORD n = 0;

            if (ReadFile (readPipe, buffer, (DWORD) juce::jmin ((DWORD) sizeof (buffer), available), &n, nullptr) && n > 0)
            {
                partial.append (buffer, (size_t) n);
                takeLines (partial, false, onLine);
                continue;
            }
        }

        if (! running)
            break;   // the child ended and its output is drained

        if (WaitForSingleObject (process.hProcess, 30) == WAIT_OBJECT_0)
        {
            running = false;   // drain what is left, then leave
            continue;
        }

        const bool cancelled = cancel != nullptr && cancel->load();
        const bool late = juce::Time::currentTimeMillis() > deadline;

        if (cancelled || late)
        {
            TerminateProcess (process.hProcess, 1);
            WaitForSingleObject (process.hProcess, 5000);
            result.cancelled = cancelled;
            result.timedOut = ! cancelled;
            running = false;
        }
    }

    takeLines (partial, true, onLine);

    DWORD code = 0;

    if (GetExitCodeProcess (process.hProcess, &code))
        result.exitCode = (int) code;

    result.finished = ! result.cancelled && ! result.timedOut;
    CloseHandle (process.hProcess);
    CloseHandle (readPipe);
   #else
    juce::ignoreUnused (arguments, timeoutMs, cancel, onLine);
   #endif

    return result;
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
            return k ("도구를 복사하지 못했습니다 (다른 다운로드가 쓰는 중일 수 있습니다): ") + target.getFullPathName();
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

juce::String YouTubeTools::ytDlpVersion (const juce::File& exe, const std::atomic<bool>* cancel)
{
    if (! exe.existsAsFile())
        return {};

    return versionFromOutput (runAndRead ({ exe.getFullPathName(), "--ignore-config", "--version" }, 30000, cancel));
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

bool YouTubeTools::selfUpdateYtDlp (const juce::File& exe, juce::String& note, const std::atomic<bool>* cancel)
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
    const auto before = ytDlpVersion (exe, cancel);
    bool finished = false;
    runAndRead ({ exe.getFullPathName(), "--ignore-config", "-U" }, 180000, cancel, &finished);

    if (! finished)
    {
        note = k ("yt-dlp 갱신이 끝나지 않았습니다 (시간 초과 또는 취소).");
        return false;
    }

    const auto after = ytDlpVersion (exe, cancel);

    if (after.isNotEmpty() && after != before)
    {
        note = k ("yt-dlp를 ") + (before.isEmpty() ? juce::String ("?") : before) + " -> " + after + k (" 로 갱신했습니다.");
        return true;
    }

    note = k ("yt-dlp는 이미 최신입니다 (") + (after.isEmpty() ? before : after) + ").";
    return false;
}

} // namespace gocue
