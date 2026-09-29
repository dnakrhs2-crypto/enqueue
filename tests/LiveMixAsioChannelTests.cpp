#include "MixEngine.h"

#include <juce_audio_devices/juce_audio_devices.h>

#if JUCE_WINDOWS && JUCE_ASIO
 #include <windows.h>
 #include <objbase.h>
 #include <malloc.h>
 #include <iasiodrv.h>

 #include <atomic>
 #include <cstdio>
 #include <cstring>
 #include <memory>
 #include <thread>
 #include <vector>
#endif

namespace gocue::tests
{

using namespace gocue::livemix;

#if JUCE_WINDOWS && JUCE_ASIO
namespace
{
    // A test-only driver id and name: nothing is installed, the process alone sees it (see FakeAsioRegistration)
    const CLSID fakeAsioClsid { 0x7c1f0e53, 0x9b2a, 0x4e4b, { 0x8f, 0x2d, 0x4c, 0x1a, 0x5e, 0x0b, 0x9d, 0x11 } };
    const wchar_t* const fakeAsioClsidText = L"{7C1F0E53-9B2A-4E4B-8F2D-4C1A5E0B9D11}";
    const char* const fakeAsioName = "LiveMix Fake ASIO";

    /** The "hardware" behind every driver instance (JUCE loads a new one for each device object): its clock, and the
        channel count of an ADAT interface - fewer channels at 88.2 / 96 kHz than at 44.1 / 48 kHz. */
    struct FakeAsioHardware
    {
        std::atomic<double> rate { 96000.0 };
        std::atomic<int> arraysTooSmall { 0 };   // createBuffers() got more channels than its array holds
        std::atomic<int> biggestRequest { 0 };
        static long inputsAt (double r)  { return r > 48000.0 ? 4 : 18; }
        static long outputsAt (double r) { return r > 48000.0 ? 4 : 20; }
    };
    FakeAsioHardware fakeHardware;

    class FakeAsio : public IASIO
    {
    public:
        ~FakeAsio() { joinCallbacks(); }

        HRESULT STDMETHODCALLTYPE QueryInterface (REFIID iid, void** out) override
        {
            if (iid == IID_IUnknown || iid == fakeAsioClsid)
            {
                *out = this;
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG) ++refs; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const auto left = --refs;
            if (left == 0) delete this;
            return (ULONG) left;
        }

        ASIOBool init (void*) override { return ASIOTrue; }
        void getDriverName (char* name) override { strcpy_s (name, 32, fakeAsioName); }
        long getDriverVersion() override { return 1; }
        void getErrorMessage (char* text) override { text[0] = 0; }

        // A few callbacks right after start are what JUCE waits for; stop never waits for them (JUCE calls it while
        // holding the lock its callback takes), and the buffers they touch live as long as the driver.
        ASIOError start() override
        {
            joinCallbacks();
            if (callbacks == nullptr) return ASE_NotPresent;
            running = true;
            auto* cb = callbacks;
            thread = std::thread ([this, cb]
            {
                for (long half = 0; half < 4 && running; ++half)
                {
                    cb->bufferSwitch (half & 1, ASIOTrue);
                    std::this_thread::sleep_for (std::chrono::milliseconds (2));
                }
            });
            return ASE_OK;
        }
        ASIOError stop() override { running = false; return ASE_OK; }

        ASIOError getChannels (long* ins, long* outs) override
        {
            const double r = fakeHardware.rate;
            *ins = FakeAsioHardware::inputsAt (r);
            *outs = FakeAsioHardware::outputsAt (r);
            return ASE_OK;
        }
        ASIOError getLatencies (long* in, long* out) override { *in = *out = 256; return ASE_OK; }
        ASIOError getBufferSize (long* smallest, long* largest, long* preferred, long* granularity) override
        {
            *smallest = 64;
            *largest = 1024;
            *preferred = 256;
            *granularity = -1;
            return ASE_OK;
        }
        ASIOError canSampleRate (ASIOSampleRate r) override
        {
            return r == 44100.0 || r == 48000.0 || r == 88200.0 || r == 96000.0 ? ASE_OK : ASE_NoClock;
        }
        ASIOError getSampleRate (ASIOSampleRate* r) override { *r = fakeHardware.rate; return ASE_OK; }
        ASIOError setSampleRate (ASIOSampleRate r) override
        {
            if (canSampleRate (r) != ASE_OK) return ASE_NoClock;
            fakeHardware.rate = r;
            return ASE_OK;
        }
        ASIOError getClockSources (ASIOClockSource* clocks, long* numSources) override
        {
            clocks[0] = {};
            clocks[0].associatedChannel = clocks[0].associatedGroup = -1;
            clocks[0].isCurrentSource = ASIOTrue;
            strcpy_s (clocks[0].name, sizeof (clocks[0].name), "Internal");
            *numSources = 1;
            return ASE_OK;
        }
        ASIOError setClockSource (long) override { return ASE_OK; }
        ASIOError getSamplePosition (ASIOSamples* position, ASIOTimeStamp* stamp) override
        {
            std::memset (position, 0, sizeof (*position));
            std::memset (stamp, 0, sizeof (*stamp));
            return ASE_OK;
        }
        ASIOError getChannelInfo (ASIOChannelInfo* info) override
        {
            const double r = fakeHardware.rate;
            if (info->channel < 0 || info->channel >= (info->isInput ? FakeAsioHardware::inputsAt (r) : FakeAsioHardware::outputsAt (r)))
                return ASE_InvalidParameter;
            info->isActive = ASIOFalse;
            info->channelGroup = 0;
            info->type = ASIOSTFloat32LSB;
            sprintf_s (info->name, sizeof (info->name), "%s %ld", info->isInput ? "In" : "Out", info->channel + 1);
            return ASE_OK;
        }
        ASIOError createBuffers (ASIOBufferInfo* infos, long numChannels, long bufferSize, ASIOCallbacks* cb) override
        {
            // JUCE filled 'infos' for every channel before this call: the array must hold them all
            const auto capacity = (long) (_msize (infos) / sizeof (ASIOBufferInfo));
            if (numChannels > fakeHardware.biggestRequest) fakeHardware.biggestRequest = (int) numChannels;
            if (numChannels > capacity)
            {
                ++fakeHardware.arraysTooSmall;
                return ASE_InvalidMode;
            }
            for (long i = 0; i < numChannels; ++i)
                for (int half = 0; half < 2; ++half)
                {
                    buffers.push_back (std::make_unique<float[]> ((size_t) bufferSize));
                    infos[i].buffers[half] = buffers.back().get();
                }
            callbacks = cb;
            return ASE_OK;
        }
        ASIOError disposeBuffers() override { callbacks = nullptr; return ASE_OK; }   // kept until the driver goes
        ASIOError controlPanel() override { return ASE_OK; }
        ASIOError future (long, void*) override { return ASE_InvalidParameter; }
        ASIOError outputReady() override { return ASE_NotPresent; }

    private:
        void joinCallbacks()
        {
            running = false;
            if (thread.joinable()) thread.join();
        }

        std::atomic<long> refs { 1 };
        std::atomic<bool> running { false };
        std::thread thread;
        ASIOCallbacks* callbacks = nullptr;
        std::vector<std::unique_ptr<float[]>> buffers;
    };

    class FakeAsioFactory : public IClassFactory
    {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface (REFIID iid, void** out) override
        {
            if (iid == IID_IUnknown || iid == IID_IClassFactory)
            {
                *out = this;
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return 2; }    // lives as long as the test
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE CreateInstance (IUnknown* outer, REFIID iid, void** out) override
        {
            if (outer != nullptr) return CLASS_E_NOAGGREGATION;
            auto* driver = new FakeAsio();
            const auto result = driver->QueryInterface (iid, out);
            driver->Release();
            return result;
        }
        HRESULT STDMETHODCALLTYPE LockServer (BOOL) override { return S_OK; }
    };

    /** JUCE finds ASIO drivers under HKLM\software\asio and checks HKCR\clsid: this makes the fake visible to this
        process only - volatile keys under HKCU mapped over both roots while JUCE scans - and serves its class from
        memory. Everything is removed again at the end. */
    struct FakeAsioRegistration
    {
        FakeAsioRegistration()
        {
            const bool comReady = SUCCEEDED (comResult = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED)) || comResult == RPC_E_CHANGED_MODE;
            ok = comReady
                 && createKey (root + L"\\LM\\software\\asio\\LiveMix Fake ASIO",
                               { { L"clsid", fakeAsioClsidText }, { L"description", L"LiveMix Fake ASIO" } })
                 && createKey (root + L"\\CR\\clsid\\" + fakeAsioClsidText + L"\\InprocServer32", { { L"", L"livemix-fake-asio.dll" } })
                 && RegOpenKeyExW (HKEY_CURRENT_USER, (root + L"\\LM").c_str(), 0, KEY_READ, &machine) == ERROR_SUCCESS
                 && RegOpenKeyExW (HKEY_CURRENT_USER, (root + L"\\CR").c_str(), 0, KEY_READ, &classes) == ERROR_SUCCESS
                 && SUCCEEDED (CoRegisterClassObject (fakeAsioClsid, &factory, CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &cookie));
        }
        ~FakeAsioRegistration()
        {
            if (cookie != 0) CoRevokeClassObject (cookie);
            if (machine != nullptr) RegCloseKey (machine);
            if (classes != nullptr) RegCloseKey (classes);
            RegDeleteTreeW (HKEY_CURRENT_USER, root.c_str());
            if (SUCCEEDED (comResult)) CoUninitialize();
        }

        void scan (juce::AudioIODeviceType& type) const
        {
            RegOverridePredefKey (HKEY_LOCAL_MACHINE, machine);
            RegOverridePredefKey (HKEY_CLASSES_ROOT, classes);
            type.scanForDevices();
            RegOverridePredefKey (HKEY_CLASSES_ROOT, nullptr);
            RegOverridePredefKey (HKEY_LOCAL_MACHINE, nullptr);
        }

        static bool createKey (const std::wstring& path, std::initializer_list<std::pair<const wchar_t*, const wchar_t*>> values)
        {
            HKEY key = nullptr;
            if (RegCreateKeyExW (HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &key, nullptr) != ERROR_SUCCESS)
                return false;
            bool written = true;
            for (const auto& [name, text] : values)
                written = written && RegSetValueExW (key, name, 0, REG_SZ, (const BYTE*) text, (DWORD) ((wcslen (text) + 1) * sizeof (wchar_t))) == ERROR_SUCCESS;
            RegCloseKey (key);
            return written;
        }

        const std::wstring root = L"Software\\LiveMixFakeAsio_" + std::to_wstring (GetCurrentProcessId());
        HRESULT comResult = E_FAIL;
        HKEY machine = nullptr, classes = nullptr;
        DWORD cookie = 0;
        FakeAsioFactory factory;
        bool ok = false;
    };

    /** JUCE's real ASIO device type, scanned once with the fake visible (MixEngine rescans before every open). */
    class FakeScannedAsioType : public juce::AudioIODeviceType
    {
    public:
        explicit FakeScannedAsioType (const FakeAsioRegistration& r)
            : AudioIODeviceType ("ASIO"), registration (r), inner (juce::AudioIODeviceType::createAudioIODeviceType_ASIO()) {}

        void scanForDevices() override
        {
            if (! scanned)
                registration.scan (*inner);
            scanned = true;
        }
        juce::StringArray getDeviceNames (bool input) const override { return inner->getDeviceNames (input); }
        int getDefaultDeviceIndex (bool input) const override { return inner->getDefaultDeviceIndex (input); }
        int getIndexOfDevice (juce::AudioIODevice* device, bool input) const override { return inner->getIndexOfDevice (device, input); }
        bool hasSeparateInputsAndOutputs() const override { return inner->hasSeparateInputsAndOutputs(); }
        juce::AudioIODevice* createDevice (const juce::String& output, const juce::String& input) override { return inner->createDevice (output, input); }

    private:
        const FakeAsioRegistration& registration;
        std::unique_ptr<juce::AudioIODeviceType> inner;
        bool scanned = false;
    };
}
#endif

/** JUCE sized its ASIO buffer arrays once, when the driver loaded: a rate change that gives an ADAT interface more
    channels (96 -> 48 kHz) wrote past them (LiveMix patch in juce_ASIO_windows.cpp, 2026-09-29). A fake driver whose
    channel count follows its rate checks the array JUCE hands it. */
class LiveMixAsioChannelTests : public juce::UnitTest
{
public:
    LiveMixAsioChannelTests() : juce::UnitTest ("LiveMix ASIO channel count across rates", "LiveMix") {}

    void runTest() override
    {
        beginTest ("a rate change that adds channels opens them all, with names, without outgrowing JUCE's arrays");
       #if JUCE_WINDOWS && JUCE_ASIO
        FakeAsioRegistration registration;
        expect (registration.ok, "the fake ASIO driver could not be registered for this process");
        if (! registration.ok) return;
        fakeHardware.rate = 96000.0;
        fakeHardware.arraysTooSmall = 0;
        fakeHardware.biggestRequest = 0;
        {
            MixEngine engine;
            auto& manager = engine.getDeviceManager();
            while (! manager.getAvailableDeviceTypes().isEmpty())
                manager.removeAudioDeviceType (manager.getAvailableDeviceTypes().getLast());
            manager.addAudioDeviceType (std::make_unique<FakeScannedAsioType> (registration));

            const MixDevice at96 { "ASIO", fakeAsioName, fakeAsioName, 256, 96000.0 };
            auto at48 = at96;
            at48.sampleRate = 48000.0;
            auto check = [&] (const MixDevice& wanted, int ins, int outs)
            {
                const auto error = engine.openDevice (wanted);
                expect (error.isEmpty(), error);
                auto* device = manager.getCurrentAudioDevice();
                expect (device != nullptr && device->isOpen());
                if (device == nullptr) return;
                expectEquals (juce::roundToInt (device->getCurrentSampleRate()), juce::roundToInt (wanted.sampleRate));
                expectEquals (device->getInputChannelNames().size(), ins);
                expectEquals (device->getOutputChannelNames().size(), outs);
                expectEquals (device->getActiveInputChannels().countNumberOfSetBits(), ins);
                expectEquals (device->getActiveOutputChannels().countNumberOfSetBits(), outs);
                expectEquals (engine.getOpenDevice().input, juce::String (fakeAsioName));
                expectEquals (fakeHardware.arraysTooSmall.load(), 0);
            };

            check (at96, 4, 4);
            check (at48, 18, 20);   // the driver loads at 96 kHz (8 channels), then opens at 48 kHz with 38
            expectGreaterOrEqual (fakeHardware.biggestRequest.load(), 38);
            check (at96, 4, 4);
            check (at48, 18, 20);
            engine.shutdown();
        }
       #else
        expect (true);   // no ASIO in this build
       #endif
    }
};

static LiveMixAsioChannelTests liveMixAsioChannelTests;

} // namespace gocue::tests
