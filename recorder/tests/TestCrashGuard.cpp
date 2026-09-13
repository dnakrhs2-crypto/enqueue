// Crash guard for unattended RecorderTests runs. The build machine is also the CEO's desktop, so a fault must
// never sit behind a Windows "memory could not be read" dialog waiting for a click: the process exits with the
// exception code instead, and a minidump plus a text report naming the running test land in <exe dir>\crash\.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include "TestSupport.h"

namespace recorder_test
{
namespace
{
std::atomic<bool> reporting{false};
wchar_t reportDirectory[MAX_PATH]{};

void writeReport(EXCEPTION_POINTERS* info, const char* origin) noexcept
{
    if (reporting.exchange(true)) return; // a fault inside the reporter (heap damage, a second thread): give up, never recurse
    char name[256]; copyLatestTest(name); // one copy, before the dump: the name must not change under the report
    const char* test = name[0] ? name : "(between tests)";
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t base[MAX_PATH + 64]{};
    swprintf(base, MAX_PATH + 64, L"%s\\RecorderTests-%04u%02u%02u-%02u%02u%02u-%lu", reportDirectory,
             unsigned(now.wYear), unsigned(now.wMonth), unsigned(now.wDay), unsigned(now.wHour), unsigned(now.wMinute), unsigned(now.wSecond),
             static_cast<unsigned long>(GetCurrentProcessId()));
    wchar_t dumpPath[MAX_PATH + 80]{}; swprintf(dumpPath, MAX_PATH + 80, L"%s.dmp", base);
    bool dumped = false;
    HANDLE dump = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION exception{};
        exception.ThreadId = GetCurrentThreadId(); exception.ExceptionPointers = info; exception.ClientPointers = FALSE;
        dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                                   MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithHandleData),
                                   info != nullptr ? &exception : nullptr, nullptr, nullptr) != FALSE;
        CloseHandle(dump);
    }
    const bool haveRecord = info != nullptr && info->ExceptionRecord != nullptr;
    const auto code = haveRecord ? static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode) : 0UL;
    const void* address = haveRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
    char text[1200]{};
    std::snprintf(text, sizeof text, "RecorderTests CRASH (%s) in test: %s\nexception 0x%08lX at %p on thread %lu\ndump: %ls\n",
                  origin, test, code, address, static_cast<unsigned long>(GetCurrentThreadId()), dumped ? dumpPath : L"not written");
    std::fputs(text, stderr); std::fflush(stderr);
    wchar_t textPath[MAX_PATH + 80]{}; swprintf(textPath, MAX_PATH + 80, L"%s.txt", base);
    HANDLE report = CreateFileW(textPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (report != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0; WriteFile(report, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
        CloseHandle(report);
    }
}
LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* info)
{
    writeReport(info, "unhandled exception");
    return EXCEPTION_EXECUTE_HANDLER; // the system ends the process with the exception code as its exit status; no dialog
}
[[noreturn]] void exitAfter(const char* origin) noexcept
{
    writeReport(nullptr, origin);
    TerminateProcess(GetCurrentProcess(), 3);
    std::abort(); // unreachable: TerminateProcess does not return for the calling process
}
void onTerminate() { exitAfter("std::terminate"); }
void onPureCall() { exitAfter("pure virtual call"); }
void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { exitAfter("invalid CRT parameter"); }
}

}

// Hidden subprocess fixture (`--suite inject-access-violation`, excluded from --list/all): faults on purpose so a test can
// prove the guard ends the process with the exception code, prints the report naming the test and shows no dialog.
int runAccessViolationFixture()
{
    std::fputs("inject-access-violation: faulting deliberately\n", stdout); std::fflush(stdout);
    recorder_test::noteLatestTest("Deliberate access violation");
    volatile int* nowhere = nullptr; // volatile keeps the read; the guard reports it and the process exits with STATUS_ACCESS_VIOLATION
    return *nowhere;
}

namespace recorder_test
{
void installCrashGuard()
{
    // Dialogs wait for a click nobody gives during an unattended run: a missing DLL, abort() or a fault must fail fast instead.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(onInvalidParameter);
    _set_purecall_handler(onPureCall);
    std::set_terminate(onTerminate);
    SetUnhandledExceptionFilter(onUnhandledException);
    wchar_t executable[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) { executable[0] = L'.'; executable[1] = 0; }
    else if (auto* slash = wcsrchr(executable, L'\\')) *slash = 0;
    swprintf(reportDirectory, MAX_PATH, L"%s\\crash", executable);
    CreateDirectoryW(reportDirectory, nullptr); // prepared while the process is healthy
}
}
