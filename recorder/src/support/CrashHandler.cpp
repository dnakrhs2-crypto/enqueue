#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include "CrashHandler.h"
#include "app/ProductIdentity.h"
#include <cstdlib>
#include <exception>

namespace gocue::recorder
{
namespace
{
void writeReport(EXCEPTION_POINTERS* info, const char* origin)
{
    const auto dir = CrashHandler::directory(); dir.createDirectory();
    const auto base = dir.getChildFile("Recorder-" + ProductIdentity::version() + "-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S"));
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
    juce::SystemStats::setApplicationCrashHandler(onCrash); // SetUnhandledExceptionFilter underneath
    std::set_terminate(onTerminate);
}
juce::File CrashHandler::directory() { return ProductIdentity::settingsDirectory().getChildFile("crash"); }
juce::File CrashHandler::latestUnseenReport()
{
    juce::File newest;
    for (const auto& f : directory().findChildFiles(juce::File::findFiles, false, "*.txt"))
        if (!f.getFileName().endsWith(".seen.txt") && (newest == juce::File() || f.getLastModificationTime() > newest.getLastModificationTime())) newest = f;
    return newest;
}
void CrashHandler::markSeen(const juce::File& report)
{ if (report.existsAsFile()) report.moveFileTo(report.getSiblingFile(report.getFileNameWithoutExtension() + ".seen.txt")); }
}
