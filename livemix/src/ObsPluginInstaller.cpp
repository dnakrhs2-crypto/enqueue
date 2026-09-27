#include "ObsPluginInstaller.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <atomic>
#include <vector>

namespace gocue::livemix
{
namespace
{
    using Result = ObsPluginInstaller::Result;
    using Roots = ObsPluginInstaller::Roots;
    constexpr auto dllName = "livemix-obs.dll";

    juce::String ko (const char* text) { return juce::String::fromUTF8 (text); }
    juce::String failure (const juce::String& reason) { return ko ("OBS 플러그인을 설치하지 못했습니다: ") + reason; }

    juce::String windowsError (DWORD error)
    {
        wchar_t* text = nullptr;
        FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                        nullptr, error, 0, reinterpret_cast<wchar_t*> (&text), 0, nullptr);
        const auto description = text != nullptr ? juce::String (text).trim() : juce::String();
        LocalFree (text);
        return description + " (Windows " + juce::String (error) + ")";
    }

    Result fileFailure (DWORD error, const juce::File& file, juce::String& message)
    {
        message = failure (file.getFullPathName() + ": " + windowsError (error));
        return error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_WRITE_PROTECT
                   ? Result::needsElevation : Result::failed;
    }

    juce::uint64 fileVersion (const juce::File& file)
    {
        const auto path = file.getFullPathName();
        DWORD unused = 0;
        const auto size = GetFileVersionInfoSizeW (path.toWideCharPointer(), &unused);
        if (size == 0) return 0;
        std::vector<BYTE> bytes (size);
        if (! GetFileVersionInfoW (path.toWideCharPointer(), 0, size, bytes.data())) return 0;
        void* info = nullptr;
        UINT length = 0;
        if (! VerQueryValueW (bytes.data(), L"\\", &info, &length) || length < sizeof (VS_FIXEDFILEINFO)) return 0;
        const auto& version = *static_cast<const VS_FIXEDFILEINFO*> (info);
        if (version.dwSignature != 0xfeef04bd) return 0;
        return (static_cast<juce::uint64> (version.dwFileVersionMS) << 32) | version.dwFileVersionLS;
    }

    juce::File pluginRoot (const Roots& roots) { return roots.programData.getChildFile ("obs-studio/plugins/livemix-obs"); }
    juce::File legacyDll (const Roots& roots) { return pluginRoot (roots).getChildFile ("bin/64bit/livemix-obs.dll"); }
    juce::File modernDll (const Roots& roots) { return pluginRoot (roots).getChildFile (dllName); }
    juce::File targetDll (const Roots& roots, int major) { return major >= 33 ? modernDll (roots) : legacyDll (roots); }

    juce::Array<juce::File> bundledLocales (const Roots& roots)
    {
        return roots.bundledPlugin.getChildFile ("data/locale").findChildFiles (juce::File::findFiles, false, "*.ini");
    }

    bool localesPresent (const Roots& roots, const juce::Array<juce::File>& locales)
    {
        if (locales.isEmpty()) return false;
        for (const auto& file : locales)
            if (! pluginRoot (roots).getChildFile ("data/locale").getChildFile (file.getFileName()).existsAsFile()) return false;
        return true;
    }

    // Unlike File::createDirectory, this retains the original Win32 error for the UAC decision.
    DWORD createDirectory (const juce::File& directory)
    {
        if (directory.isDirectory()) return ERROR_SUCCESS;
        const auto parent = directory.getParentDirectory();
        if (parent == directory) return ERROR_PATH_NOT_FOUND;
        const auto error = createDirectory (parent);
        if (error != ERROR_SUCCESS) return error;
        if (CreateDirectoryW (directory.getFullPathName().toWideCharPointer(), nullptr)) return ERROR_SUCCESS;
        const auto lastError = GetLastError();
        return lastError == ERROR_ALREADY_EXISTS && directory.isDirectory() ? ERROR_SUCCESS : lastError;
    }

    juce::File uniqueSibling (const juce::File& file, const char* suffix)
    {
        static std::atomic<juce::uint32> sequence { 0 };
        const auto number = (static_cast<juce::uint64> (GetCurrentProcessId()) << 32) | ++sequence;
        return file.getSiblingFile (file.getFileName() + suffix + juce::String (number));
    }

    DWORD renameAside (const juce::File& file, juce::File& aside)
    {
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            auto candidate = uniqueSibling (file, ".old-");
            // Never replace another retired DLL, including one still mapped by OBS.
            if (MoveFileExW (file.getFullPathName().toWideCharPointer(), candidate.getFullPathName().toWideCharPointer(), MOVEFILE_WRITE_THROUGH))
            {
                aside = candidate;
                return ERROR_SUCCESS;
            }
            const auto error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) return error;
        }
        return ERROR_FILE_EXISTS;
    }

    void cleanupOldDlls (const Roots& roots)
    {
        for (const auto& file : { legacyDll (roots), modernDll (roots) })
            for (const auto& old : file.getParentDirectory().findChildFiles (juce::File::findFiles, false, "livemix-obs.dll.old-*"))
                DeleteFileW (old.getFullPathName().toWideCharPointer()); // a still-mapped file may need another attempt later
    }

    struct Transaction
    {
        struct Change
        {
            juce::File destination, staged, previous;
            bool dll = false, published = false;
        };
        std::vector<Change> changes;

        ~Transaction()
        {
            for (const auto& change : changes)
                if (change.staged != juce::File()) DeleteFileW (change.staged.getFullPathName().toWideCharPointer());
        }

        DWORD stage (const Roots& roots, const juce::File& source, const juce::File& destination, bool dll)
        {
            auto error = createDirectory (destination.getParentDirectory());
            if (error != ERROR_SUCCESS) return error;
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                auto temporary = uniqueSibling (destination, ".tmp-");
                error = roots.copyFile ? roots.copyFile (source, temporary)
                                      : CopyFileW (source.getFullPathName().toWideCharPointer(), temporary.getFullPathName().toWideCharPointer(), TRUE)
                                            ? ERROR_SUCCESS : GetLastError();
                if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) continue;
                if (error != ERROR_SUCCESS)
                {
                    DeleteFileW (temporary.getFullPathName().toWideCharPointer()); // CopyFile may have left partial bytes
                    return error;
                }
                changes.push_back ({ destination, temporary, {}, dll, false });
                return ERROR_SUCCESS;
            }
            return ERROR_FILE_EXISTS;
        }

        bool rollback()
        {
            bool ok = true;
            for (auto i = changes.rbegin(); i != changes.rend(); ++i)
            {
                if (i->published) ok &= DeleteFileW (i->destination.getFullPathName().toWideCharPointer()) != FALSE;
                if (i->previous != juce::File())
                    ok &= MoveFileExW (i->previous.getFullPathName().toWideCharPointer(), i->destination.getFullPathName().toWideCharPointer(),
                                       MOVEFILE_WRITE_THROUGH) != FALSE;
            }
            return ok;
        }
    };

    juce::File registryObsDirectory (REGSAM view)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW (HKEY_LOCAL_MACHINE, L"SOFTWARE\\OBS Studio", 0, KEY_READ | view, &key) != ERROR_SUCCESS) return {};
        DWORD bytes = 0;
        constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
        juce::File result;
        if (RegGetValueW (key, nullptr, nullptr, flags, nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes > 0)
        {
            std::vector<wchar_t> value (bytes / sizeof (wchar_t) + 1, 0);
            if (RegGetValueW (key, nullptr, nullptr, flags, nullptr, value.data(), &bytes) == ERROR_SUCCESS)
            {
                auto path = juce::String (value.data()).trim().unquoted();
                if (juce::File::isAbsolutePath (path)) result = juce::File (path);
            }
        }
        RegCloseKey (key);
        return result;
    }

    juce::uint64 processCreationTime (HANDLE process)
    {
        FILETIME created {}, exited {}, kernel {}, user {};
        if (! GetProcessTimes (process, &created, &exited, &kernel, &user)) return 0;
        return (juce::uint64 (created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }

    juce::String processCommandLine (const RunningObsDetector::Process& info)
    {
        using QueryProcess = NTSTATUS (NTAPI*) (HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
        static const auto query = reinterpret_cast<QueryProcess> (GetProcAddress (GetModuleHandleW (L"ntdll.dll"), "NtQueryInformationProcess"));
        if (query == nullptr) return {};
        HANDLE process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, info.pid);
        if (process == nullptr) return {};
        juce::String result;
        if (processCreationTime (process) == info.creationTime)
        {
            constexpr auto commandLineInformation = static_cast<PROCESSINFOCLASS> (60); // Windows 8.1+
            ULONG bytes = 0;
            query (process, commandLineInformation, nullptr, 0, &bytes);
            if (bytes >= sizeof (UNICODE_STRING) && bytes <= 1024 * 1024)
            {
                std::vector<unsigned char> storage (bytes);
                if (query (process, commandLineInformation, storage.data(), bytes, &bytes) >= 0)
                {
                    const auto& text = *reinterpret_cast<const UNICODE_STRING*> (storage.data());
                    const auto begin = reinterpret_cast<uintptr_t> (storage.data());
                    const auto end = begin + storage.size();
                    const auto buffer = reinterpret_cast<uintptr_t> (text.Buffer);
                    if (buffer >= begin + sizeof (UNICODE_STRING) && buffer <= end
                        && text.Length <= end - buffer && text.Length % sizeof (wchar_t) == 0)
                        result = juce::String (text.Buffer, text.Length / sizeof (wchar_t));
                }
            }
        }
        CloseHandle (process);
        return result;
    }

    std::vector<RunningObsDetector::Process> runningObsProcesses()
    {
        std::vector<RunningObsDetector::Process> processes;
        HANDLE snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return processes;
        PROCESSENTRY32W entry {};
        entry.dwSize = sizeof (entry);
        for (BOOL found = Process32FirstW (snapshot, &entry); found; found = Process32NextW (snapshot, &entry))
        {
            if (_wcsicmp (entry.szExeFile, L"obs64.exe") != 0) continue;
            HANDLE process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process == nullptr) continue;
            std::vector<wchar_t> path (32768);
            DWORD length = static_cast<DWORD> (path.size());
            if (QueryFullProcessImageNameW (process, 0, path.data(), &length))
            {
                auto image = juce::String (path.data());
                if (image.startsWith ("\\\\?\\")) image = image.substring (4);
                const auto created = processCreationTime (process);
                if (created != 0 && juce::File (image).getFileName().equalsIgnoreCase ("obs64.exe"))
                    processes.push_back ({ juce::File (image), entry.th32ProcessID, created });
            }
            CloseHandle (process);
        }
        CloseHandle (snapshot);
        return processes;
    }

    bool obsRunningIn (const juce::File& directory)
    {
        for (const auto& process : runningObsProcesses())
            if (process.image.isAChildOf (directory)) return true;
        return false;
    }
}

bool ObsPluginInstaller::RunningObs::needsUpdate() const noexcept
{
    return version != 0 && version < ((juce::uint64 (31) << 48) | (juce::uint64 (1) << 32));
}

juce::String ObsPluginInstaller::RunningObs::versionString() const
{
    auto text = juce::String (int (version >> 48)) + "." + juce::String (int ((version >> 32) & 0xffff))
        + "." + juce::String (int ((version >> 16) & 0xffff));
    if ((version & 0xffff) != 0) text += "." + juce::String (int (version & 0xffff));
    return text;
}

ObsPluginInstaller::RunningObs ObsPluginInstaller::runningObsInfo (const juce::File& image, juce::uint64 version, const juce::String& commandLine)
{
    RunningObs info { image, version, false };
    if (commandLine.isNotEmpty())
    {
        int count = 0;
        if (auto** arguments = CommandLineToArgvW (commandLine.toWideCharPointer(), &count))
        {
            for (int i = 1; i < count; ++i)
                info.portable |= _wcsicmp (arguments[i], L"--portable") == 0 || _wcsicmp (arguments[i], L"-p") == 0;
            LocalFree (arguments);
        }
    }
    const auto root = image.getParentDirectory().getParentDirectory().getParentDirectory();
    for (const auto* marker : { "portable_mode", "obs_portable_mode", "portable_mode.txt", "obs_portable_mode.txt" })
        info.portable |= root.getChildFile (marker).existsAsFile();
    return info;
}

RunningObsDetector::RunningObsDetector (ProcessQuery processQuery, VersionQuery versionQuery, CommandLineQuery commandLineQuery)
    : processes (processQuery ? std::move (processQuery) : runningObsProcesses),
      version (versionQuery ? std::move (versionQuery) : fileVersion),
      commandLine (commandLineQuery ? std::move (commandLineQuery) : processCommandLine)
{
}

const std::vector<ObsPluginInstaller::RunningObs>& RunningObsDetector::scan (double nowMs)
{
    if (nowMs < nextScan) return running;
    nextScan = nowMs + 2000.0;
    running.clear();
    std::map<ProcessKey, ObsPluginInstaller::RunningObs> active;
    for (const auto& process : processes())
    {
        const auto& image = process.image;
        const ProcessKey key { process.pid, process.creationTime };
        auto cached = byProcess.find (key);
        if (cached == byProcess.end())
            cached = byProcess.emplace (key, ObsPluginInstaller::runningObsInfo (image, version (image), commandLine (process))).first;
        running.push_back (cached->second);
        active.emplace (key, cached->second);
    }
    byProcess = std::move (active);
    return running;
}

ObsPluginInstaller::Roots ObsPluginInstaller::systemRoots()
{
    Roots roots;
    wchar_t* commonData = nullptr;
    if (SUCCEEDED (SHGetKnownFolderPath (FOLDERID_ProgramData, KF_FLAG_DONT_VERIFY, nullptr, &commonData)))
    {
        roots.programData = juce::File (juce::String (commonData));
        CoTaskMemFree (commonData);
    }
    else
        roots.programData = juce::File (juce::SystemStats::getEnvironmentVariable ("ProgramData", "C:\\ProgramData"));

    const auto native = registryObsDirectory (KEY_WOW64_64KEY);
    const auto wow = registryObsDirectory (KEY_WOW64_32KEY);
    roots.obsInstallDir = juce::File ("C:\\Program Files\\obs-studio");
    for (const auto& directory : { native, wow })
        if (directory != juce::File() && directory.getChildFile ("bin/64bit/obs64.exe").existsAsFile())
        {
            roots.obsInstallDir = directory;
            break;
        }
    roots.bundledPlugin = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory()
                             .getChildFile ("obs-plugin/livemix-obs");
    roots.isObsRunning = [directory = roots.obsInstallDir] { return obsRunningIn (directory); };
    return roots;
}

int ObsPluginInstaller::obsMajorVersion (const Roots& roots)
{
    return static_cast<int> (fileVersion (roots.obsInstallDir.getChildFile ("bin/64bit/obs64.exe")) >> 48);
}

bool ObsPluginInstaller::isInstalledAndCurrent (const Roots& roots)
{
    const auto version = fileVersion (roots.bundledPlugin.getChildFile (dllName));
    if (version == 0) return false;
    const auto major = obsMajorVersion (roots);
    const auto other = major >= 33 ? legacyDll (roots) : modernDll (roots);
    return fileVersion (targetDll (roots, major)) >= version && ! other.exists()
           && localesPresent (roots, bundledLocales (roots));
}

ObsPluginInstaller::Result ObsPluginInstaller::install (const Roots& roots, juce::String& message)
{
    const bool running = roots.isObsRunning && roots.isObsRunning();
    if (! running) cleanupOldDlls (roots);
    const auto bundle = roots.bundledPlugin.getChildFile (dllName);
    const auto locales = bundledLocales (roots);
    if (! bundle.existsAsFile() || locales.isEmpty())
    {
        message = failure (ko ("LiveMix에 포함된 플러그인 파일을 찾지 못했습니다. LiveMix를 다시 설치해 주세요."));
        return Result::noBundledFiles;
    }
    const auto version = fileVersion (bundle);
    if (version == 0)
    {
        message = failure (ko ("포함된 플러그인의 버전 정보를 읽지 못했습니다."));
        return Result::failed;
    }
    const auto major = obsMajorVersion (roots);
    const auto target = targetDll (roots, major);
    const auto other = major >= 33 ? legacyDll (roots) : modernDll (roots);
    const bool updateDll = fileVersion (target) < version;
    if (! updateDll && ! other.exists() && localesPresent (roots, locales))
    {
        message = ko ("OBS 플러그인이 이미 설치되어 있습니다.");
        return Result::alreadyCurrent;
    }

    // Prepare EVERY byte before retiring either active layout. A denied locale copy cannot strand the old DLL.
    Transaction transaction;
    if (updateDll)
    {
        const auto error = transaction.stage (roots, bundle, target, true);
        if (error != ERROR_SUCCESS) return fileFailure (error, target, message);
    }
    for (const auto& locale : locales)
    {
        const auto destination = pluginRoot (roots).getChildFile ("data/locale").getChildFile (locale.getFileName());
        if (! updateDll && destination.existsAsFile()) continue;
        const auto error = transaction.stage (roots, locale, destination, false);
        if (error != ERROR_SUCCESS) return fileFailure (error, destination, message);
    }
    if (other.exists()) transaction.changes.push_back ({ other, {}, {}, true, false });

    auto abort = [&] (Result result)
    {
        if (! transaction.rollback())
        {
            message = failure (ko ("기존 파일을 복원하지 못했습니다. OBS를 종료하고 다시 시도해 주세요. 보관 파일: ")
                               + pluginRoot (roots).getFullPathName());
            return Result::failed;
        }
        return result;
    };

    for (auto& change : transaction.changes)
    {
        if (! change.destination.exists()) continue;
        const auto error = renameAside (change.destination, change.previous);
        if (error != ERROR_SUCCESS)
        {
            if (change.dll)
            {
                message = ko ("OBS가 예전 플러그인을 쓰고 있어 바꾸지 못했습니다. OBS를 종료한 뒤 다시 눌러 주세요.");
                return abort (Result::obsBusyCloseIt);
            }
            return abort (fileFailure (error, change.destination, message));
        }
    }
    for (auto& change : transaction.changes)
    {
        if (change.staged == juce::File()) continue; // only removing the other layout
        if (! MoveFileExW (change.staged.getFullPathName().toWideCharPointer(), change.destination.getFullPathName().toWideCharPointer(), MOVEFILE_WRITE_THROUGH))
            return abort (fileFailure (GetLastError(), change.destination, message));
        change.staged = {};
        change.published = true;
    }
    for (const auto& change : transaction.changes)
        if (change.previous != juce::File() && (! change.dll || ! running))
            DeleteFileW (change.previous.getFullPathName().toWideCharPointer());

    message = major == 0 ? ko ("OBS를 찾지 못했습니다. OBS를 설치하면 바로 쓸 수 있게 플러그인을 넣어 두었습니다.")
            : running ? ko ("OBS 플러그인을 설치했습니다. OBS를 다시 시작하면 연결됩니다.")
                      : ko ("OBS 플러그인을 설치했습니다. OBS를 켜고 소스(+)에서 'LiveMix'를 추가하세요.");
    return running ? Result::installedRestartObs : Result::installed;
}

bool ObsPluginInstaller::isInstallCommandLine (const juce::String& commandLine)
{
    const juce::ArgumentList args ("LiveMix", commandLine);
    for (int i = 0; i < args.size(); ++i)
        if (args[i].text == "--install-obs-plugin") return true;
    return false;
}

const char* ObsPluginInstaller::resultName (Result result)
{
    switch (result)
    {
        case Result::alreadyCurrent: return "alreadyCurrent";
        case Result::installed: return "installed";
        case Result::installedRestartObs: return "installedRestartObs";
        case Result::needsElevation: return "needsElevation";
        case Result::obsBusyCloseIt: return "obsBusyCloseIt";
        case Result::noBundledFiles: return "noBundledFiles";
        case Result::failed: return "failed";
    }
    return "failed";
}

juce::File ObsPluginInstaller::resultFile (const juce::String& commandLine)
{
    const juce::ArgumentList args ("LiveMix", commandLine);
    for (int i = 0; i < args.size(); ++i)
        if (args[i].text == "--result")
            return i + 1 < args.size() && juce::File::isAbsolutePath (args[i + 1].text)
                ? juce::File (args[i + 1].text) : juce::File();
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LiveMix/obs-install-result.txt");
}

juce::String ObsPluginInstaller::elevatedCommandLine (const juce::File& report)
{
    return "--install-obs-plugin --elevated-helper --result " + report.getFullPathName().quoted();
}

ObsPluginInstaller::Result ObsPluginInstaller::runInstallCommandLine (const juce::String& commandLine, const Roots& roots,
    juce::String& message, bool& reported, const std::function<Result (juce::String&)>& elevate)
{
    const juce::ArgumentList args ("LiveMix", commandLine);
    bool allowElevation = false, elevatedHelper = false;
    for (int i = 0; i < args.size(); ++i)
    {
        allowElevation |= args[i].text == "--allow-elevation";
        elevatedHelper |= args[i].text == "--elevated-helper";
    }
    auto result = install (roots, message);
    if (result == Result::needsElevation && allowElevation && ! elevatedHelper)
        result = elevate (message);
    reported = writeResult (resultFile (commandLine), result, message);
    return result;
}

bool ObsPluginInstaller::writeResult (const juce::File& file, Result result, const juce::String& message)
{
    if (file == juce::File()) return false;
    if (file.getParentDirectory().createDirectory().failed()) return false;
    const auto line = juce::String (resultName (result)) + "\t" + message.replaceCharacters ("\r\n\t", "   ") + "\n";
    return file.replaceWithData (line.toRawUTF8(), line.getNumBytesAsUTF8());
}

ObsPluginInstaller::Result ObsPluginInstaller::readResult (const juce::File& file, juce::String& message)
{
    message = failure (ko ("설치 결과를 읽지 못했습니다. 다시 시도해 주세요."));
    if (! file.existsAsFile() || file.getSize() > 65536) return Result::failed;
    const auto line = file.loadFileAsString().trimEnd();
    const auto tab = line.indexOfChar ('\t');
    if (tab <= 0 || tab == line.length() - 1 || line.containsAnyOf ("\r\n")) return Result::failed;
    for (auto result : { Result::alreadyCurrent, Result::installed, Result::installedRestartObs, Result::needsElevation,
                         Result::obsBusyCloseIt, Result::noBundledFiles, Result::failed })
        if (line.substring (0, tab) == resultName (result))
        {
            message = line.substring (tab + 1);
            return result;
        }
    return Result::failed;
}

ObsPluginInstaller::Result ObsPluginInstaller::installElevated (juce::String& message)
{
    // The caller owns a unique reply file even when runas uses another administrator's TEMP.
    const juce::TemporaryFile reply (".obs-install-result.txt");
    const auto report = reply.getFile();
    if (! writeResult (report, Result::failed, ko ("설치 결과를 받지 못했습니다.")))
        return fileFailure (GetLastError(), report, message);

    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
    const auto parameters = elevatedCommandLine (report);
    const auto com = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
    SHELLEXECUTEINFOW execute {};
    execute.cbSize = sizeof (execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    execute.lpVerb = L"runas";
    execute.lpFile = exe.toWideCharPointer();
    execute.lpParameters = parameters.toWideCharPointer();
    execute.nShow = SW_HIDE;
    const bool launched = ShellExecuteExW (&execute) != FALSE;
    const auto launchError = GetLastError();
    if (SUCCEEDED (com)) CoUninitialize();
    if (! launched)
    {
        if (launchError == ERROR_CANCELLED)
        {
            message = ko ("설치를 취소했습니다");
            return Result::needsElevation;
        }
        return fileFailure (launchError, juce::File (exe), message);
    }
    if (execute.hProcess == nullptr)
    {
        message = failure (ko ("설치 프로세스를 기다릴 수 없습니다."));
        return Result::failed;
    }
    const auto waited = WaitForSingleObject (execute.hProcess, INFINITE);
    const auto waitError = GetLastError();
    CloseHandle (execute.hProcess);
    if (waited != WAIT_OBJECT_0) return fileFailure (waitError, report, message);
    return readResult (report, message);
}
}
