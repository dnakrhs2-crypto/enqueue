#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include "CrashHandler.h"
#include "app/ProductIdentity.h"
#include <atomic>
#include <cstdlib>
#include <exception>

namespace gocue::recorder
{
namespace
{
std::atomic<bool> reporting{false};
void writeReport(EXCEPTION_POINTERS* info, const char* origin)
{
    if (reporting.exchange(true)) return; // a crash inside the reporter (heap damage, second thread): give up, do not recurse
    const auto dir = CrashHandler::directory(); dir.createDirectory();
    const auto base = dir.getChildFile("Recorder-" + ProductIdentity::version() + "-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + "-" + juce::String((juce::int64) GetCurrentProcessId()));
    const auto dump = juce::File(base.getFullPathName() + ".dmp");
    HANDLE file = CreateFileW(dump.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool dumped = false;
    if (file != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION exception{}; exception.ThreadId = GetCurrentThreadId(); exception.ExceptionPointers = info; exception.ClientPointers = FALSE;
        dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                   MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithHandleData),
                                   info ? &exception : nullptr, nullptr, nullptr) != FALSE;
        CloseHandle(file);
    }
    juce::String text;
    text << ProductIdentity::displayName() << " " << ProductIdentity::version() << " crash (" << origin << ") " << juce::Time::getCurrentTime().toString(true, true) << "\n";
    if (info != nullptr && info->ExceptionRecord != nullptr)
        text << "exception 0x" << juce::String::toHexString((juce::int64) info->ExceptionRecord->ExceptionCode)
             << " at 0x" << juce::String::toHexString((juce::int64) (juce::pointer_sized_int) info->ExceptionRecord->ExceptionAddress) << "\n";
    text << "dump: " << (dumped ? dump.getFullPathName() : juce::String("not written")) << "\n\n" << juce::SystemStats::getStackBacktrace();
    juce::File(base.getFullPathName() + ".txt").replaceWithText(text);
}
void onCrash(void* pointers) { writeReport(static_cast<EXCEPTION_POINTERS*>(pointers), "unhandled exception"); }
void onTerminate() { writeReport(nullptr, "std::terminate"); std::abort(); }
}
void CrashHandler::install()
{
    directory().createDirectory(); // prepared while the process is healthy
    // A crashed Tally.exe must never wait behind a Windows dialog (automation runs it unattended on the CEO's desktop):
    // the report written below is the evidence, and the next start shows it.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    juce::SystemStats::setApplicationCrashHandler(onCrash); // SetUnhandledExceptionFilter underneath
    std::set_terminate(onTerminate);
}
juce::File CrashHandler::directory() { return ProductIdentity::settingsDirectory().getChildFile("crash"); }
juce::File CrashHandler::writeExceptionReport(const juce::String& what, const juce::String& source, int line,
                                             const juce::File& reportDirectory) noexcept
{
    try
    {
        const auto dir = reportDirectory == juce::File() ? directory() : reportDirectory;
        if (dir.createDirectory().failed()) return {};
        static std::atomic<unsigned> serial{0};
        const auto now = juce::Time::getCurrentTime();
        const auto stamp = now.formatted("%Y%m%d-%H%M%S") + "." + juce::String(now.toMilliseconds() % 1000).paddedLeft('0', 3)
            + "-" + juce::String(serial.fetch_add(1));
        const auto report = dir.getChildFile("Recorder-" + ProductIdentity::version() + "-" + stamp + "-"
            + juce::String((juce::int64) GetCurrentProcessId()) + "-exception.txt");
        juce::String text;
        text << ProductIdentity::displayName() << " " << ProductIdentity::version() << " handled exception\n"
             << "time: " << now.toString(true, true) << "\nwhat: " << what << "\nfile: " << source << "\nline: " << line
             << "\n\nstack:\n" << juce::SystemStats::getStackBacktrace();
        return report.replaceWithText(text) ? report : juce::File();
    }
    catch (...) { return {}; }
}
void CrashHandler::handleException(const std::exception* exception, const juce::String& source, int line,
                                  const std::function<void(const juce::File&)>& notify, const juce::File& reportDirectory) noexcept
{
    static thread_local bool handling = false;
    if (handling) return;
    struct Guard { bool& value; explicit Guard(bool& v) : value(v) { value = true; } ~Guard() { value = false; } } guard(handling);
    try
    {
        const auto report = writeExceptionReport(exception ? juce::String::fromUTF8(exception->what()) : juce::String("Unknown non-standard exception"), source, line, reportDirectory);
        if (notify) notify(report);
    }
    catch (...) { OutputDebugStringW(L"Recorder: exception notification failed\n"); }
}
juce::File CrashHandler::latestUnseenReport()
{
    juce::File newest;
    for (const auto& f : directory().findChildFiles(juce::File::findFiles, false, "*.txt"))
        if (!f.getFileName().endsWith(".seen.txt") && !f.getSiblingFile(f.getFileName() + ".seen").existsAsFile()
            && (newest == juce::File() || f.getLastModificationTime() > newest.getLastModificationTime())) newest = f;
    return newest;
}
void CrashHandler::markSeen(const juce::File& report)
{ if (report.existsAsFile()) report.getSiblingFile(report.getFileName() + ".seen").create(); }
}
