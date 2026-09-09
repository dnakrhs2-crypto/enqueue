#include "DurableFile.h"
#include <algorithm>
#include <limits>

namespace gocue::recorder
{
namespace { struct IoScope { FileIoFaultAdapter* adapter; explicit IoScope(FileIoFaultAdapter* a) : adapter(a) { if (a) a->ioStarted(); } ~IoScope() { if (adapter) adapter->ioFinished(); } }; }
DurableFile::~DurableFile() { if (isOpen()) CloseHandle(handle); }
juce::Result DurableFile::remember(juce::Result result)
{
    if (error.wasOk() && result.failed()) error = result;
    return error;
}
juce::Result DurableFile::windowsError(const char* operation, DWORD code)
{
    return remember(juce::Result::fail(juce::String(operation) + " failed (Win32 "
        + juce::String(static_cast<juce::int64>(code)) + "): " + path.getFullPathName()));
}
juce::Result DurableFile::check(FileIoOperation operation, std::uint64_t offset, std::size_t bytes)
{
    if (error.failed()) return error;
    if (!isOpen()) return windowsError("File not open", ERROR_INVALID_HANDLE);
    if (faults) return remember(faults->beforeIo(operation, path, faults->observedOffset(offset), bytes));
    return error;
}
juce::Result DurableFile::open(const juce::File& target, OpenMode mode)
{
    IoScope operation(faults);
    if (isOpen()) return remember(juce::Result::fail("DurableFile is already open"));
    error = juce::Result::ok(); path = target; length.store(0); durable.store(0);
    if (faults && remember(faults->beforeIo(FileIoOperation::open, path, 0, 0)).failed()) return error;
    handle = CreateFileW(path.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, mode == OpenMode::createNew ? CREATE_NEW : OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!isOpen()) return windowsError("CreateFileW", GetLastError());
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size))
    {
        windowsError("GetFileSizeEx", GetLastError());
        return close();
    }
    length.store(static_cast<std::uint64_t>(size.QuadPart), std::memory_order_release);
    return error;
}
juce::Result DurableFile::writeImpl(std::uint64_t offset, const void* data, std::size_t count, FileIoOperation operation)
{
    IoScope observation(faults);
    if (check(operation, offset, count).failed()) return error;
    const auto maxOffset = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if ((count && !data) || offset > maxOffset || count > maxOffset - offset)
        return windowsError("Invalid write range", ERROR_INVALID_PARAMETER);
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) return windowsError("SetFilePointerEx", GetLastError());
    auto* bytes = static_cast<const std::uint8_t*>(data);
    while (count)
    {
        const auto requested = static_cast<DWORD>(std::min<std::size_t>(faults && faults->splitWritesForTesting() && count > 1 ? (count + 1) / 2 : count, 1u << 30));
        DWORD written = 0;
        const auto success = WriteFile(handle, bytes, requested, &written, nullptr);
        // Account for an actual partial write even when the remainder fails.
        offset += written; bytes += written; count -= written;
        length.store(std::max(writtenBytes(), offset), std::memory_order_release);
        if (!success) return windowsError("WriteFile", GetLastError());
        if (written == 0) return windowsError("WriteFile made no progress", ERROR_WRITE_FAULT);
        if (faults && count && check(operation == FileIoOperation::patch ? FileIoOperation::patchProgress
            : FileIoOperation::appendProgress, offset, count).failed()) return error;
    }
    return error;
}
juce::Result DurableFile::write(const void* bytes, std::size_t count)
{
    return writeImpl(writtenBytes(), bytes, count, FileIoOperation::append);
}
juce::Result DurableFile::writeAt(std::uint64_t offset, const void* bytes, std::size_t count)
{
    const auto end = writtenBytes();
    if (offset > end || count > end - offset)
        return windowsError("Header patch outside existing file", ERROR_INVALID_PARAMETER);
    return writeImpl(offset, bytes, count, FileIoOperation::patch);
}
juce::Result DurableFile::flushApplicationBuffers()
{
    if (error.failed()) return error;
    return isOpen() ? error : windowsError("File not open", ERROR_INVALID_HANDLE);
}
juce::Result DurableFile::flushData()
{
    IoScope operation(faults);
    if (check(FileIoOperation::flushData, writtenBytes(), 0).failed()) return error;
    if (!FlushFileBuffers(handle)) return windowsError("FlushFileBuffers", GetLastError());
    durable.store(writtenBytes(), std::memory_order_release);
    return error;
}
juce::Result DurableFile::close()
{
    if (isOpen())
    {
        const auto closed = CloseHandle(handle);
        const auto code = closed ? ERROR_SUCCESS : GetLastError();
        handle = INVALID_HANDLE_VALUE;
        if (!closed) windowsError("CloseHandle", code);
    }
    return error;
}
}
