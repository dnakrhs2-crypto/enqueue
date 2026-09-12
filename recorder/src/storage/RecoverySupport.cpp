#include "RecoverySupport.h"
#include "StorageEncoding.h"
#include "model/RecorderModel.h"
#include <bcrypt.h>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace gocue::recorder::recovery
{
void require(bool b, const juce::String& s) { if (!b) throw std::runtime_error(s.toStdString()); }
void check(const juce::Result& r) { require(r.wasOk(), r.getErrorMessage()); }
juce::var object() { return new juce::DynamicObject(); }
void set(juce::var& v, const char* k, const juce::var& x) { v.getDynamicObject()->setProperty(k, x); }
juce::var integer(std::int64_t n) { return juce::var(static_cast<juce::int64>(n)); }
std::int64_t number(const juce::var& v)
{ require(v.isInt() || v.isInt64(), "Expected integer in recovery metadata"); return static_cast<juce::int64>(v); }
void hit(const Hook& h, const char* name) { if (h) h(name); }
void writeNew(const juce::File& f, const void* data, std::size_t bytes, FileIoFaultAdapter* faults)
{
    check(f.getParentDirectory().createDirectory()); DurableFile out(faults);
    check(out.open(f, DurableFile::OpenMode::createNew)); check(out.write(data, bytes));
    check(out.flushApplicationBuffers()); check(out.flushData()); check(out.close());
}
void writeJson(const juce::File& f, const juce::var& v, FileIoFaultAdapter* faults)
{ const auto s = juce::JSON::toString(v, false); writeNew(f, s.toRawUTF8(), s.getNumBytesAsUTF8(), faults); }
juce::File child(const juce::File& root, const juce::String& relative)
{
    require(isProjectRelativePath(relative), "Unsafe project-relative recovery path: " + relative);
    auto result = root.getChildFile(relative);
    // Do not follow junctions/symlinks out of a project, including output parents.
    for (auto f = result; f != root; f = f.getParentDirectory())
    {
        require(f.isAChildOf(root), "Recovery path escapes project");
        const auto attributes = GetFileAttributesW(f.getFullPathName().toWideCharPointer());
        require(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Recovery path contains a reparse point");
    }
    return result;
}
WriterLock::WriterLock(const juce::File& path)
{
    check(path.getParentDirectory().createDirectory());
    handle = CreateFileW(path.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Project/journal writer lock unavailable (Win32 " + juce::String(int(GetLastError())) + "): " + path.getFullPathName());
}
WriterLock::~WriterLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
juce::String sha256(const juce::File& path)
{
    struct Hash
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE value = nullptr;
        ~Hash() { if (value) BCryptDestroyHash(value); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } h;
    require(BCryptOpenAlgorithmProvider(&h.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "Open SHA256");
    require(BCryptCreateHash(h.algorithm, &h.value, nullptr, 0, nullptr, 0, 0) >= 0, "Create SHA256");
    juce::FileInputStream input(path); check(input.getStatus());
    std::array<unsigned char, 65536> bytes{};
    while (input.getPosition() < input.getTotalLength())
    {
        const auto n = input.read(bytes.data(), int(bytes.size()));
        require(n > 0, "SHA256 short read: " + path.getFullPathName());
        require(BCryptHashData(h.value, bytes.data(), ULONG(n), 0) >= 0, "Update SHA256");
    }
    check(input.getStatus()); std::array<unsigned char, 32> digest{};
    require(BCryptFinishHash(h.value, digest.data(), ULONG(digest.size()), 0) >= 0, "Finish SHA256");
    return juce::String::toHexString(digest.data(), int(digest.size()), 0);
}
namespace
{
constexpr std::uint32_t magic = 0x31564352;
constexpr std::uint64_t commit = 0x313054494d4d4f43;
constexpr std::size_t header = 48, trailer = 8, limit = 16 * 1024 * 1024;
}
Replay Log::read(const juce::File& path, const std::function<bool(const Record&)>& visit)
{
    using namespace storageEncoding;
    Replay r; if (!path.exists()) return r;
    juce::FileInputStream in(path); check(in.getStatus());
    std::set<juce::String> seen;
    const auto stop = [&](const char* why) { r.ignoredTail = true; r.reason = why; return r; };
    while (in.getPosition() < in.getTotalLength())
    {
        std::array<std::uint8_t, header> h{};
        if (in.getTotalLength() - in.getPosition() < std::int64_t(header)) return stop("Truncated header");
        require(in.read(h.data(), int(header)) == header, "Log header read failure");
        const auto size = get<std::uint32_t>(h.data() + 4), payload = get<std::uint32_t>(h.data() + 36);
        const auto seq = get<std::uint64_t>(h.data() + 12);
        if (get<std::uint32_t>(h.data()) != magic || crc32(h.data(), 44) != get<std::uint32_t>(h.data() + 44)) return stop("Header CRC/magic");
        const auto k = get<std::uint16_t>(h.data() + 10);
        if (get<std::uint16_t>(h.data() + 8) != 1 || k < 1 || k > 4) return stop("Unsupported schema/kind");
        if (size < header + trailer || size > limit || payload != size - header - trailer) return stop("Record length");
        if (seq == 0 || seq != r.sequence + 1) return stop("Sequence gap");
        if (in.getTotalLength() - in.getPosition() < size - header) return stop("Truncated payload/commit");
        std::vector<std::uint8_t> body(size - header);
        require(in.read(body.data(), int(body.size())) == int(body.size()), "Log body read failure");
        if (get<std::uint64_t>(body.data() + payload) != commit || crc32(body.data(), payload) != get<std::uint32_t>(h.data() + 40)) return stop("Payload CRC/commit");
        juce::var value;
        if (juce::JSON::parse(juce::String::fromUTF8(reinterpret_cast<const char*>(body.data()), int(payload)), value).failed() || !value.isObject()) return stop("Payload JSON schema");
        juce::Uuid txn(h.data() + 20); if (txn.isNull()) return stop("Null transaction");
        if (seen.insert(txn.toString()).second)
        {
            if (!visit({static_cast<Kind>(k), seq, txn, value})) return stop("Payload semantic validation");
        }
        else ++r.duplicates;
        r.sequence = seq; r.bytes = std::uint64_t(in.getPosition());
    }
    check(in.getStatus()); return r;
}
void Log::open(const juce::File& path, bool createNew)
{
    check(path.getParentDirectory().createDirectory());
    const auto r = read(path, [](const Record&) { return true; });
    require(!r.ignoredTail, "Cannot append to damaged log; preserve original"); sequence = r.sequence;
    check(file.open(path, createNew ? DurableFile::OpenMode::createNew : DurableFile::OpenMode::appendOrCreate));
}
void Log::append(Kind k, const juce::var& value, const juce::Uuid& txn, const Hook& hook)
{
    using namespace storageEncoding;
    require(!txn.isNull() && value.isObject() && sequence < std::uint64_t(INT64_MAX), "Invalid log record");
    const auto json = juce::JSON::toString(value, true); const auto bytes = json.getNumBytesAsUTF8();
    require(bytes <= limit - header - trailer, "Recovery log record exceeds 16 MiB");
    std::vector<std::uint8_t> out(header + bytes + trailer); auto* h = out.data();
    put(h, magic); put(h + 4, std::uint32_t(out.size())); put(h + 8, std::uint16_t(1)); put(h + 10, std::uint16_t(k)); put(h + 12, sequence + 1);
    std::memcpy(h + 20, txn.getRawData(), 16); put(h + 36, std::uint32_t(bytes)); put(h + 40, crc32(json.toRawUTF8(), bytes)); put(h + 44, crc32(h, 44));
    std::memcpy(h + header, json.toRawUTF8(), bytes); put(h + header + bytes, commit);
    check(file.write(h, header + bytes)); hit(hook, "journal-before-commit");
    check(file.write(h + header + bytes, trailer)); check(file.flushData()); ++sequence;
    hit(hook, "journal-after-commit");
}
void Log::close() { check(file.close()); }
}
