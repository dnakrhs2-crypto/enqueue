#include "ControlDiscovery.h"

#include <array>
#include <chrono>
#include <ctime>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <bcrypt.h>
#endif

namespace gocue::livemix
{

juce::String ControlDiscovery::createToken()
{
    std::array<unsigned char, 32> bytes {};
   #if JUCE_WINDOWS
    if (BCryptGenRandom (nullptr, bytes.data(), (ULONG) bytes.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return {};
   #else
    // The product is Windows-only. Never silently substitute JUCE Random for a cryptographic generator.
    return {};
   #endif
    return juce::Base64::toBase64 (bytes.data(), bytes.size()).replaceCharacter ('+', '-')
        .replaceCharacter ('/', '_').removeCharacters ("=");
}

bool ControlDiscovery::tokensEqual (const juce::String& expected, const juce::String& supplied) noexcept
{
    const auto size = expected.getNumBytesAsUTF8(), otherSize = supplied.getNumBytesAsUTF8();
    const auto* a = reinterpret_cast<const unsigned char*> (expected.toRawUTF8());
    const auto* b = reinterpret_cast<const unsigned char*> (supplied.toRawUTF8());
    // Length is public (a token is always 43 bytes); never return at the first mismatching secret byte.
    volatile size_t difference = size ^ otherSize;
    for (size_t i = 0; i < size; ++i) difference |= (size_t) (a[i] ^ (i < otherSize ? b[i] : 0));
    return size != 0 && difference == 0;
}

ControlDiscovery::ControlDiscovery (juce::File folder, Status initial, Published callback)
    : directory (std::move (folder)), published (std::move (callback)), latest (std::move (initial)),
      worker (&ControlDiscovery::run, this) // joined before any worker-owned data is destroyed
{
}

ControlDiscovery::~ControlDiscovery()
{
    Status final;
    { std::lock_guard<std::mutex> lock (mutex); final = latest; }
    final.state = State::stopped;
    stop (std::move (final));
}

void ControlDiscovery::publish (Status status)
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (stopping) return;
        latest = std::move (status);
        dirty = true;
    }
    wake.notify_one();
}

void ControlDiscovery::stop (Status tombstone)
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (! stopping)
        {
            tombstone.state = State::stopped;
            tombstone.port = 0;
            tombstone.token.clear();
            latest = std::move (tombstone);
            dirty = stopping = true;
        }
    }
    wake.notify_one();
    if (worker.joinable()) worker.join();
}

void ControlDiscovery::run()
{
    if (directory == juce::File())
        directory = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("LiveMix/control");
    for (;;)
    {
        Status status;
        bool final;
        {
            std::unique_lock<std::mutex> lock (mutex);
            wake.wait_for (lock, std::chrono::seconds (5), [this] { return dirty; });
            status = latest;
            final = stopping;
            dirty = false;
        }
        const auto success = write (status);
        if (published) published (status, success);
        if (final) return;
    }
}

bool ControlDiscovery::write (const Status& status)
{
    if (directory.createDirectory().failed()) return false;
    auto json = juce::var (new juce::DynamicObject());
    auto& o = *json.getDynamicObject();
    o.setProperty ("schemaVersion", 1);
    o.setProperty ("app", "LiveMix");
    o.setProperty ("appVersion", status.appVersion);
    o.setProperty ("instanceId", status.instanceId.toString());
   #if JUCE_WINDOWS
    o.setProperty ("pid", (juce::int64) GetCurrentProcessId());
   #else
    o.setProperty ("pid", (juce::int64) 0);
   #endif
    constexpr const char* states[] { "starting", "ready", "disabled", "error", "stopped" };
    o.setProperty ("state", states[(size_t) status.state]);
    o.setProperty ("host", "127.0.0.1");
    if (status.state == State::ready)
    {
        o.setProperty ("port", status.port);
        o.setProperty ("token", status.token);
    }
    const auto timestamp = std::time (nullptr);
    std::tm utc {};
   #if JUCE_WINDOWS
    gmtime_s (&utc, &timestamp);
   #else
    gmtime_r (&timestamp, &utc);
   #endif
    char updatedAt[32] {};
    std::strftime (updatedAt, sizeof (updatedAt), "%Y-%m-%dT%H:%M:%SZ", &utc);
    o.setProperty ("updatedAt", juce::String (updatedAt));
    o.setProperty ("heartbeat", ++heartbeat);
    o.setProperty ("leaseMs", 15000);
    if (status.state == State::error) o.setProperty ("errorCode", status.errorCode);

    const auto target = directory.getChildFile ("discovery.json");
    juce::TemporaryFile temporary (target);
    {
        juce::FileOutputStream output (temporary.getFile());
        if (! output.openedOk()) return false;
        const auto text = juce::JSON::toString (json, true) + "\n";
        if (! output.write (text.toRawUTF8(), text.getNumBytesAsUTF8())) return false;
        output.flush();
        if (output.getStatus().failed()) return false;
    }
   #if JUCE_WINDOWS
    // One same-directory rename handles both first publication and replacement. Never delete the old file first.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        if (MoveFileExW (temporary.getFile().getFullPathName().toWideCharPointer(),
                         target.getFullPathName().toWideCharPointer(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
            return true;
        const auto error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) return false;
        // Readers/virus scanners may briefly deny a rename. Retain the old complete file and retry only here,
        // on the worker, with a bounded deadline. Persistent failure still withdraws the service.
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return false;
   #else
    return temporary.overwriteTargetFileWithTemporary();
   #endif
}

} // namespace gocue::livemix
