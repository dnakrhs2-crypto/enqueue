#include "MixEngine.h"
#include "ObsSender.h"
#include "TestGainPlugin.h"
#include "lm_obs_protocol.h"

#include <array>
#include <thread>
#include <sddl.h>
#include <aclapi.h>

namespace gocue::livemix
{
struct MixEngineTestAccess
{
    static juce::CriticalSection& graphLock (MixEngine& engine) { return engine.lock; }
};
}

namespace gocue::tests
{
using namespace gocue::livemix;

namespace
{
    juce::String mappingName() { return "Local\\LiveMix.ObsTest." + juce::Uuid().toString(); }
    constexpr size_t ringBytes = sizeof (lm_obs_ring_header) + LM_OBS_CAPACITY_FRAMES * LM_OBS_CHANNELS * sizeof (float);

    struct Mapping
    {
        Mapping (const juce::String& name, size_t bytes, bool create = false, bool readOnly = false)
        {
            const DWORD access = readOnly ? FILE_MAP_READ : FILE_MAP_READ | FILE_MAP_WRITE;
            handle = create ? CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD) bytes, name.toWideCharPointer())
                            : OpenFileMappingW (access, FALSE, name.toWideCharPointer());
            if (handle != nullptr) view = MapViewOfFile (handle, access, 0, 0, bytes);
        }
        ~Mapping() { if (view != nullptr) UnmapViewOfFile (view); if (handle != nullptr) CloseHandle (handle); }
        lm_obs_ring_header* header() const { return static_cast<lm_obs_ring_header*> (view); }
        float* pcm() const { return reinterpret_cast<float*> (static_cast<char*> (view) + sizeof (lm_obs_ring_header)); }
        HANDLE handle = nullptr;
        void* view = nullptr;
    };

    int64_t nowQpc() { LARGE_INTEGER now {}; QueryPerformanceCounter (&now); return now.QuadPart; }
}

class LiveMixObsSenderTests : public juce::UnitTest
{
public:
    LiveMixObsSenderTests() : juce::UnitTest ("LiveMix OBS sender", "LiveMix") {}

    void runTest() override
    {
        beginTest ("a denied readers mapping never prevents the audio ring from sending");
        {
            const auto testName = mappingName();
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            expect (ConvertStringSecurityDescriptorToSecurityDescriptorW (L"D:P(D;;GA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr) != FALSE);
            SECURITY_ATTRIBUTES security { sizeof (security), descriptor, FALSE };
            HANDLE denied = CreateFileMappingW (INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
                                               sizeof (lm_obs_readers), (testName + ".Readers").toWideCharPointer());
            expect (denied != nullptr);
            if (descriptor != nullptr) LocalFree (descriptor);
            ObsSender sender (testName);
            sender.setEnabled (true);
            expect (sender.isEnabled());
            expect (sender.readerState() == ObsSender::ReaderState::none);
            Mapping ring (testName, ringBytes, false, true);
            expect (ring.view != nullptr);
            if (ring.view != nullptr)
            {
                const std::array<float, 3> tone { 0.2f, -0.4f, 0.6f };
                sender.write (tone.data(), tone.data(), (int) tone.size());
                expectEquals (lm_obs_load_acquire (&ring.header()->write_frames), int64_t (tone.size()));
                expectEquals (ring.pcm()[4], tone[2]);
            }
            if (denied != nullptr) CloseHandle (denied);
        }

        beginTest ("both mappings grant interactive users access in their protected DACL");
        {
            const auto testName = mappingName();
            ObsSender sender (testName);
            sender.setEnabled (true);
            for (const auto& mapping : { testName, testName + ".Readers" })
            {
                HANDLE handle = OpenFileMappingW (READ_CONTROL, FALSE, mapping.toWideCharPointer());
                expect (handle != nullptr);
                if (handle == nullptr) continue;
                PSECURITY_DESCRIPTOR descriptor = nullptr;
                PACL acl = nullptr;
                expectEquals ((int) GetSecurityInfo (handle, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor), (int) ERROR_SUCCESS);
                BYTE interactive[SECURITY_MAX_SID_SIZE];
                DWORD bytes = sizeof (interactive);
                CreateWellKnownSid (WinInteractiveSid, nullptr, interactive, &bytes);
                bool found = false;
                if (acl != nullptr)
                    for (DWORD i = 0; i < acl->AceCount; ++i)
                    {
                        void* raw = nullptr;
                        if (GetAce (acl, i, &raw))
                        {
                            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*> (raw);
                            found |= ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && EqualSid ((PSID) &ace->SidStart, interactive) != FALSE;
                        }
                    }
                expect (found);
                if (descriptor != nullptr) LocalFree (descriptor);
                CloseHandle (handle);
            }
        }

        beginTest ("read-only mapping exposes the complete v1 header; OFF retains it and ON increments the epoch");
        {
            const auto testName = mappingName();
            ObsSender sender (testName);
            { Mapping absent (testName, ringBytes); expect (absent.handle == nullptr); }
            sender.deviceStarted (44100.0);
            sender.setEnabled (true);
            expect (sender.isEnabled());
            Mapping ring (testName, ringBytes, false, true);
            expect (ring.view != nullptr);
            if (ring.view != nullptr)
            {
                auto* h = ring.header();
                expectEquals ((int) h->magic, (int) LM_OBS_MAGIC);
                expectEquals ((int) h->protocol_major, LM_OBS_PROTOCOL_MAJOR);
                expectEquals ((int) h->protocol_minor, 0);
                expectEquals ((int) h->header_bytes, (int) sizeof (*h));
                expectEquals ((int) h->mapping_bytes, (int) ringBytes);
                expectEquals ((int) h->data_offset, (int) sizeof (*h));
                expectEquals ((int) h->channels, LM_OBS_CHANNELS);
                expectEquals ((int) h->capacity_frames, (int) LM_OBS_CAPACITY_FRAMES);
                expectEquals (lm_obs_load_acquire (&h->sample_rate), int64_t (44100));
                expectEquals (lm_obs_load_acquire (&h->writer_pid), int64_t (GetCurrentProcessId()));
                LARGE_INTEGER frequency {}; QueryPerformanceFrequency (&frequency);
                expectEquals (lm_obs_load_acquire (&h->qpc_frequency), int64_t (frequency.QuadPart));
                const auto epoch = lm_obs_load_acquire (&h->epoch);
                expect (epoch > 0);
                const std::array<float, 3> left { 0.2f, 0.3f, 0.4f }, right { -0.1f, -0.2f, -0.3f };
                sender.write (left.data(), right.data(), 3);
                expectEquals (lm_obs_load_acquire (&h->write_frames), int64_t (3));
                expectEquals (lm_obs_load_acquire ((const volatile int64_t*) h->reserved), int64_t (3));
                const auto beat = lm_obs_load_acquire (&h->heartbeat_qpc);
                expect (beat > 0 && nowQpc() - beat < frequency.QuadPart / 2);
                for (int i = 0; i < 3; ++i)
                {
                    expectEquals (ring.pcm()[i * 2], left[(size_t) i]);
                    expectEquals (ring.pcm()[i * 2 + 1], right[(size_t) i]);
                }
                sender.setEnabled (false);
                expect (! sender.isEnabled());
                sender.write (left.data(), right.data(), 3);
                sender.writeSilence (127);
                expectEquals (lm_obs_load_acquire (&h->send_enabled), int64_t (0));
                expectEquals (lm_obs_load_acquire (&h->write_frames), int64_t (3));
                expectEquals (lm_obs_load_acquire (&h->heartbeat_qpc), beat);
                sender.setEnabled (true);
                expectEquals (lm_obs_load_acquire (&h->epoch), epoch + 1);
                expectEquals (lm_obs_load_acquire (&h->write_frames), int64_t (0));
                expectEquals (lm_obs_load_acquire ((const volatile int64_t*) h->reserved), int64_t (0));
                expectEquals (lm_obs_load_acquire (&h->send_enabled), int64_t (1));
                sender.setEnabled (true); // repeated value edits must not reset the stream
                expectEquals (lm_obs_load_acquire (&h->epoch), epoch + 1);
                sender.deviceStarted (48000.0);
                expectEquals (lm_obs_load_acquire (&h->epoch), epoch + 2);
                expectEquals (lm_obs_load_acquire (&h->sample_rate), int64_t (48000));
            }
        }

        beginTest ("busy graph renders three unequal blocks of OBS silence without stalling the audio clock");
        {
            const auto testName = mappingName();
            MixEngine engine (testName);
            MixSession session;
            session.addChannel();
            engine.prepare (48000.0, 64);
            engine.applySession (session);
            engine.getObsSender().setEnabled (true);
            Mapping ring (testName, ringBytes, false, true);
            expect (ring.view != nullptr);
            if (ring.view != nullptr)
            {
                {
                    const juce::ScopedLock held (MixEngineTestAccess::graphLock (engine));
                    std::thread audio ([&]
                    {
                        for (const int n : { 37, 128, 511 })
                            engine.renderBlock (nullptr, 0, nullptr, 0, n);
                    });
                    audio.join();
                }
                constexpr int total = 37 + 128 + 511;
                expectEquals (lm_obs_load_acquire (&ring.header()->write_frames), int64_t (total));
                expectEquals (lm_obs_load_acquire (&ring.header()->silent_frames), int64_t (total));
                expect (lm_obs_load_acquire (&ring.header()->heartbeat_qpc) > 0);
                bool silent = true;
                for (int i = 0; i < total * 2; ++i) silent = silent && ring.pcm()[i] == 0.0f;
                expect (silent);
                // The ordinary input-only path sends the post-master-chain pair, chunking a large callback.
                engine.getMasterChain().addPlugin (std::make_unique<TestGainPlugin> (0.5f));
                std::array<float, 177> input; input.fill (0.4f);
                const float* inputs[] { input.data() };
                engine.renderBlock (inputs, 1, nullptr, 0, (int) input.size());
                expectEquals (lm_obs_load_acquire (&ring.header()->write_frames), int64_t (total + input.size()));
                for (int i = 0; i < (int) input.size() * 2; ++i)
                    expectWithinAbsoluteError (ring.pcm()[total * 2 + i], 0.2f, 0.00001f);
            }
        }

        beginTest ("a surviving OBS handle preserves the old epoch and reader slots across sender restarts");
        {
            const auto testName = mappingName();
            Mapping oldRing (testName, ringBytes, true), readers (testName + ".Readers", sizeof (lm_obs_readers), true);
            expect (oldRing.view != nullptr && readers.view != nullptr);
            if (oldRing.view != nullptr && readers.view != nullptr)
            {
                lm_obs_store_release (&oldRing.header()->epoch, 81);
                auto* presence = static_cast<lm_obs_readers*> (readers.view);
                presence->magic = LM_OBS_MAGIC; presence->protocol_major = LM_OBS_PROTOCOL_MAJOR;
                presence->slot_count = LM_OBS_MAX_READERS;
                lm_obs_store_release (&presence->slot[3].pid, GetCurrentProcessId());
                lm_obs_store_release (&presence->slot[3].heartbeat_qpc, nowQpc());
                {
                    ObsSender sender (testName);
                    sender.setEnabled (true);
                    expectEquals (lm_obs_load_acquire (&oldRing.header()->epoch), int64_t (82));
                    expect (sender.readerState() == ObsSender::ReaderState::connected);
                    sender.writeSilence (9);
                }
                ObsSender restarted (testName);
                restarted.setEnabled (true);
                expectEquals (lm_obs_load_acquire (&oldRing.header()->epoch), int64_t (83));
                expectEquals (lm_obs_load_acquire (&oldRing.header()->write_frames), int64_t (0));
                expect (restarted.readerState() == ObsSender::ReaderState::connected);
            }
        }

        beginTest ("presence requires a recent heartbeat and a process which is still alive");
        {
            const auto testName = mappingName();
            ObsSender sender (testName); sender.setEnabled (true);
            Mapping readers (testName + ".Readers", sizeof (lm_obs_readers));
            expect (readers.view != nullptr);
            if (readers.view != nullptr)
            {
                auto& slot = static_cast<lm_obs_readers*> (readers.view)->slot[0];
                expect (sender.readerState() == ObsSender::ReaderState::none);
                lm_obs_store_release (&slot.pid, GetCurrentProcessId());
                lm_obs_store_release (&slot.heartbeat_qpc, nowQpc());
                expect (sender.readerState() == ObsSender::ReaderState::connected);
                LARGE_INTEGER frequency {}; QueryPerformanceFrequency (&frequency);
                lm_obs_store_release (&slot.heartbeat_qpc, nowQpc() - 3 * frequency.QuadPart);
                expect (sender.readerState() == ObsSender::ReaderState::none);
                // Hold a finished process handle so Windows cannot reuse its pid during this assertion.
                wchar_t systemPath[MAX_PATH] {}; GetSystemDirectoryW (systemPath, MAX_PATH);
                const auto executable = juce::String (systemPath) + "\\cmd.exe";
                std::wstring command = L"cmd.exe /d /c exit 0";
                STARTUPINFOW startup {}; startup.cb = sizeof (startup);
                PROCESS_INFORMATION process {};
                const bool created = CreateProcessW (executable.toWideCharPointer(), command.data(), nullptr, nullptr, FALSE,
                                                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
                expect (created);
                if (created)
                {
                    expectEquals ((int) WaitForSingleObject (process.hProcess, 5000), (int) WAIT_OBJECT_0);
                    lm_obs_store_release (&slot.pid, process.dwProcessId);
                    lm_obs_store_release (&slot.heartbeat_qpc, nowQpc());
                    expect (sender.readerState() == ObsSender::ReaderState::none);
                    CloseHandle (process.hThread); CloseHandle (process.hProcess);
                }
            }
        }

        beginTest ("an undersized pre-existing audio mapping is rejected without modifying it");
        {
            const auto testName = mappingName();
            Mapping tiny (testName, 64, true);
            expect (tiny.view != nullptr);
            ObsSender sender (testName); sender.setEnabled (true); sender.writeSilence (10);
            expect (! sender.isEnabled());
            if (tiny.view != nullptr) expectEquals (*static_cast<int*> (tiny.view), 0);
        }
    }
};
static LiveMixObsSenderTests liveMixObsSenderTests;
}
