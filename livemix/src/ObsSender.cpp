#include "ObsSender.h"
#include "lm_obs_protocol.h"

#include <sddl.h>
#include <cmath>
#include <vector>

namespace gocue::livemix
{
namespace
{
    constexpr DWORD ringBytes = sizeof (lm_obs_ring_header) + LM_OBS_CAPACITY_FRAMES * LM_OBS_CHANNELS * sizeof (float);
    static_assert (std::atomic<lm_obs_ring_header*>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free);

    struct MappingSecurity
    {
        MappingSecurity()
        {
            HANDLE token = nullptr;
            if (! OpenProcessToken (GetCurrentProcess(), TOKEN_QUERY, &token)) return;
            DWORD bytes = 0;
            GetTokenInformation (token, TokenUser, nullptr, 0, &bytes);
            std::vector<unsigned char> storage (bytes);
            LPWSTR sid = nullptr;
            if (bytes > 0 && GetTokenInformation (token, TokenUser, storage.data(), bytes, &bytes)
                && ConvertSidToStringSidW (reinterpret_cast<TOKEN_USER*> (storage.data())->User.Sid, &sid))
            {
                const auto sddl = juce::String (L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GA;;;") + juce::String (sid) + ")S:(ML;;NW;;;ME)";
                ConvertStringSecurityDescriptorToSecurityDescriptorW (sddl.toWideCharPointer(), SDDL_REVISION_1, &descriptor, nullptr);
                LocalFree (sid);
            }
            CloseHandle (token);
        }
        ~MappingSecurity() { if (descriptor != nullptr) LocalFree (descriptor); }
        PSECURITY_DESCRIPTOR descriptor = nullptr;
    };

    bool map (const juce::String& name, DWORD bytes, PSECURITY_DESCRIPTOR security, void*& handle, void*& view, bool& existed,
              juce::String& reason)
    {
        SECURITY_ATTRIBUTES attributes { sizeof (attributes), security, FALSE };
        handle = CreateFileMappingW (INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, bytes, name.toWideCharPointer());
        existed = GetLastError() == ERROR_ALREADY_EXISTS;
        if (handle != nullptr)
            view = MapViewOfFile (handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, bytes);
        if (view != nullptr) return true; // Windows checked the full requested layout, including on an existing object
        const auto error = GetLastError();
        if (handle != nullptr) CloseHandle (handle);
        handle = nullptr;
        reason = juce::String::fromUTF8 ("공유 메모리를 열지 못했습니다: ") + name + " (Win32 " + juce::String ((int) error) + ")";
        juce::Logger::writeToLog ("OBS sender: " + reason);
        return false;
    }
}

ObsSender::ObsSender (const juce::String& name)
    : ringName (name.isEmpty() ? juce::String (LM_OBS_RING_NAME) : name), readersName (ringName + ".Readers")
{
    LARGE_INTEGER value {};
    QueryPerformanceFrequency (&value);
    frequency = value.QuadPart;
}

ObsSender::~ObsSender()
{
    setEnabled (false);
    if (auto* view = published.exchange (nullptr, std::memory_order_acq_rel)) UnmapViewOfFile (view);
    if (ringHandle != nullptr) CloseHandle (ringHandle);
    if (readers != nullptr) UnmapViewOfFile (readers);
    if (readersHandle != nullptr) CloseHandle (readersHandle);
}

bool ObsSender::openMappings()
{
    if (published.load (std::memory_order_acquire) != nullptr) return true;
    lastError.clear();
    MappingSecurity security;
    if (security.descriptor == nullptr)
    {
        lastError = juce::String::fromUTF8 ("공유 메모리의 접근 권한을 만들지 못했습니다.");
        juce::Logger::writeToLog ("OBS sender: " + lastError);
        return false; // do not fall back to an ACL which breaks cross-integrity OBS connections
    }
    if (readers == nullptr)
    {
        void* view = nullptr;
        bool existed = false;
        juce::String presenceError; // presence is advisory; a denied mapping must never gate audio
        if (map (readersName, sizeof (lm_obs_readers), security.descriptor, readersHandle, view, existed, presenceError))
        {
            auto* presence = static_cast<lm_obs_readers*> (view);
            if (! existed)
            {
                std::memset (presence, 0, sizeof (*presence));
                presence->protocol_major = LM_OBS_PROTOCOL_MAJOR;
                presence->slot_count = LM_OBS_MAX_READERS;
                InterlockedExchange (reinterpret_cast<volatile LONG*> (&presence->magic), (LONG) LM_OBS_MAGIC);
            }
            MemoryBarrier();
            if (presence->magic != LM_OBS_MAGIC || presence->protocol_major != LM_OBS_PROTOCOL_MAJOR || presence->slot_count != LM_OBS_MAX_READERS)
            {
                juce::Logger::writeToLog ("OBS sender: incompatible readers mapping");
                UnmapViewOfFile (view); CloseHandle (readersHandle); readersHandle = nullptr;
            }
            else
                readers = presence; // preserve existing slots, including those owned by elevated OBS
        }
    }

    void* view = nullptr;
    bool existed = false;
    if (! map (ringName, ringBytes, security.descriptor, ringHandle, view, existed, lastError)) return false;
    auto* h = static_cast<lm_obs_ring_header*> (view);
    // Never zero an existing epoch: a reader may retain this exact object across LiveMix restarts.
    lm_obs_store_release (&h->send_enabled, 0);
    if (! existed) std::memset (view, 0, ringBytes);
    h->magic = LM_OBS_MAGIC;
    h->protocol_major = LM_OBS_PROTOCOL_MAJOR;
    h->protocol_minor = 0;
    h->header_bytes = sizeof (*h);
    h->mapping_bytes = ringBytes;
    h->data_offset = sizeof (*h);
    h->channels = LM_OBS_CHANNELS;
    h->capacity_frames = LM_OBS_CAPACITY_FRAMES;
    std::memset (h->reserved + sizeof (int64_t), 0, sizeof (h->reserved) - sizeof (int64_t));
    published.store (h, std::memory_order_release); // retained even when sending is switched off
    return true;
}

void ObsSender::stopWrites()
{
    writerState.fetch_or (2, std::memory_order_acq_rel);
    while ((writerState.load (std::memory_order_acquire) & 1) != 0)
        juce::Thread::yield(); // only the owner waits; audio never takes ownerLock or waits on this gate
}

void ObsSender::resetEpoch (lm_obs_ring_header& h)
{
    lm_obs_store_release (&h.send_enabled, 0);
    const auto nextEpoch = static_cast<int64_t> (static_cast<uint64_t> (lm_obs_load_acquire (&h.epoch)) + 1u);
    lm_obs_store_release (&h.write_frames, 0);
    lm_obs_store_release (reinterpret_cast<volatile int64_t*> (h.reserved), 0);
    lm_obs_store_release (&h.silent_frames, 0);
    lm_obs_store_release (&h.heartbeat_qpc, 0); // freshness begins at the first actual audio block
    lm_obs_store_release (&h.sample_rate, (int64_t) std::llround (sampleRate));
    lm_obs_store_release (&h.qpc_frequency, frequency);
    lm_obs_store_release (&h.writer_pid, GetCurrentProcessId());
    lm_obs_store_release (&h.epoch, nextEpoch);
}

void ObsSender::setEnabled (bool on)
{
    const juce::ScopedLock lock (ownerLock);
    if (! on) lastError.clear();
    if (on == enabled.load (std::memory_order_acquire)) return;
    stopWrites();
    if (on && ! openMappings()) return; // gate remains disabled; a later enable can retry
    if (auto* h = published.load (std::memory_order_acquire))
    {
        if (on) resetEpoch (*h);
        lm_obs_store_release (&h->send_enabled, on ? 1 : 0);
    }
    enabled.store (on, std::memory_order_release);
    writerState.store (on ? 0u : 2u, std::memory_order_release);
}

juce::String ObsSender::getError() const
{
    const juce::ScopedLock lock (ownerLock);
    return lastError;
}

void ObsSender::deviceStarted (double rate)
{
    const juce::ScopedLock lock (ownerLock);
    stopWrites();
    sampleRate = std::isfinite (rate) && rate > 0.0 ? rate : 48000.0;
    const bool on = enabled.load (std::memory_order_acquire);
    if (auto* h = published.load (std::memory_order_acquire))
    {
        resetEpoch (*h);
        lm_obs_store_release (&h->send_enabled, on ? 1 : 0);
    }
    writerState.store (on ? 0u : 2u, std::memory_order_release);
}

void ObsSender::writeBlock (const float* left, const float* right, int frames) noexcept
{
    if (frames <= 0) return;
    unsigned idle = 0;
    if (! writerState.compare_exchange_strong (idle, 1, std::memory_order_acquire, std::memory_order_relaxed)) return;
    if (auto* h = published.load (std::memory_order_acquire))
    {
        auto* pcm = reinterpret_cast<float*> (reinterpret_cast<unsigned char*> (h) + sizeof (*h));
        if (left != nullptr && right != nullptr) lm_obs_write (h, pcm, left, right, (uint32_t) frames);
        else lm_obs_write_silence (h, pcm, (uint32_t) frames);
        LARGE_INTEGER now {};
        QueryPerformanceCounter (&now);
        lm_obs_store_release (&h->heartbeat_qpc, now.QuadPart);
    }
    writerState.fetch_and (~1u, std::memory_order_release);
}

void ObsSender::write (const float* left, const float* right, int frames) noexcept { writeBlock (left, right, frames); }
void ObsSender::writeSilence (int frames) noexcept { writeBlock (nullptr, nullptr, frames); }

ObsSender::ReaderState ObsSender::readerState() const
{
    if (readers == nullptr || frequency <= 0) return ReaderState::none;
    LARGE_INTEGER now {}; QueryPerformanceCounter (&now);
    for (const auto& slot : readers->slot)
    {
        const auto pid = lm_obs_load_acquire (&slot.pid);
        const auto beat = lm_obs_load_acquire (&slot.heartbeat_qpc);
        if (pid <= 0 || pid > MAXDWORD || beat <= 0 || std::abs (double (now.QuadPart) - double (beat)) >= 2.0 * double (frequency)) continue;
        if (HANDLE process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid))
        {
            DWORD code = 0;
            const bool alive = GetExitCodeProcess (process, &code) && code == STILL_ACTIVE;
            CloseHandle (process);
            if (alive && lm_obs_load_acquire (&slot.pid) == pid) return ReaderState::connected;
        }
    }
    return ReaderState::none;
}
}
