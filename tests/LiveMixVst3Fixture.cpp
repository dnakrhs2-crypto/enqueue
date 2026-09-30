// Raw VST3 interfaces only: no JUCE plugin client or SDK implementation sources.
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/vstspeaker.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <windows.h>
#include <intrin.h>

namespace
{
using namespace Steinberg;
using namespace Steinberg::Vst;

// 0 = changing shell, 1 = crash, 2 = hang, 3 = empty shell.
constexpr TUID effects[] = {
    INLINE_UID (0x4C4D5300 + LM_FIXTURE_KIND, 0x12345678, 0xA0123456, 0x00000001),
    INLINE_UID (0x4C4D5300 + LM_FIXTURE_KIND, 0x12345678, 0xA0123456, 0x00000002),
    INLINE_UID (0x4C4D5300 + LM_FIXTURE_KIND, 0x12345678, 0xA0123456, 0x00000003),
    INLINE_UID (0x4C4D5300 + LM_FIXTURE_KIND, 0x12345678, 0xA0123456, 0x00000004)
};
constexpr TUID controllers[] = {
    INLINE_UID (0x4C4D5343, 0x12345678, 0xA0123456, 0x00000001),
    INLINE_UID (0x4C4D5343, 0x12345678, 0xA0123456, 0x00000002),
    INLINE_UID (0x4C4D5343, 0x12345678, 0xA0123456, 0x00000003)
};
constexpr const char* names[] = { "Alpha", "Beta", "Gamma", "Delta" };
constexpr const char* controllerNames[] = { "AlphaCtrl", "BetaCtrl", "GammaCtrl" };

bool same (const char* a, const char* b) { return std::memcmp (a, b, sizeof (TUID)) == 0; }

template <typename Char, size_t N>
void copyText (Char (&dest)[N], const char* source)
{
    std::fill (std::begin (dest), std::end (dest), Char {});
    for (size_t i = 0; i + 1 < N && source[i] != 0; ++i) dest[i] = (Char) source[i];
}

class Effect final : public IComponent, public IAudioProcessor, public IEditController
{
public:
    Effect (int identity, bool onlyController) : index (identity), controllerOnly (onlyController) {}
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
    {
        *obj = nullptr;
        if (same (iid, FUnknown_iid)) *obj = static_cast<IEditController*> (this);
        else if (same (iid, IPluginBase_iid)) *obj = static_cast<IEditController*> (this);
        else if (same (iid, IEditController_iid)) *obj = static_cast<IEditController*> (this);
        else if (! controllerOnly && same (iid, IComponent_iid)) *obj = static_cast<IComponent*> (this);
        else if (! controllerOnly && same (iid, IAudioProcessor_iid)) *obj = static_cast<IAudioProcessor*> (this);
        if (*obj == nullptr) return kNoInterface;
        addRef();
        return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return ++refs; }
    uint32 PLUGIN_API release() override
    {
        const auto count = --refs;
        if (count == 0) delete this;
        return count;
    }
    tresult PLUGIN_API initialize (FUnknown*) override
    {
       #if LM_FIXTURE_KIND == 1
        __fastfail (FAST_FAIL_FATAL_APP_EXIT);
       #elif LM_FIXTURE_KIND == 2
        for (;;) Sleep (INFINITE);
       #endif
        return kResultOk;
    }
    tresult PLUGIN_API terminate() override { return kResultOk; }
    tresult PLUGIN_API getControllerClassId (TUID) override { return kResultFalse; } // single component
    tresult PLUGIN_API setIoMode (IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount (MediaType type, BusDirection) override { return type == kAudio ? 1 : 0; }
    tresult PLUGIN_API getBusInfo (MediaType type, BusDirection direction, int32 bus, BusInfo& info) override
    {
        if (type != kAudio || bus != 0) return kInvalidArgument;
        info = {};
        info.mediaType = kAudio;
        info.direction = direction;
        info.channelCount = 2;
        info.busType = kMain;
        info.flags = BusInfo::kDefaultActive;
        copyText (info.name, direction == kInput ? "Stereo Input" : "Stereo Output");
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo (RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus (MediaType type, BusDirection, int32 bus, TBool) override
    { return type == kAudio && bus == 0 ? kResultOk : kInvalidArgument; }
    tresult PLUGIN_API setActive (TBool) override { return kResultOk; }
    tresult PLUGIN_API setState (IBStream* stream) override
    {
        int32 read = 0;
        return stream != nullptr && stream->read (&value, sizeof (value), &read) == kResultOk && read == sizeof (value)
                   ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API getState (IBStream* stream) override
    { return stream != nullptr ? stream->write (&value, sizeof (value), nullptr) : kInvalidArgument; }
    tresult PLUGIN_API setComponentState (IBStream* stream) override { return setState (stream); }
    tresult PLUGIN_API setBusArrangements (SpeakerArrangement* inputs, int32 ni, SpeakerArrangement* outputs, int32 no) override
    { return ni == 1 && no == 1 && inputs[0] == SpeakerArr::kStereo && outputs[0] == SpeakerArr::kStereo ? kResultOk : kResultFalse; }
    tresult PLUGIN_API getBusArrangement (BusDirection, int32 bus, SpeakerArrangement& arrangement) override
    {
        arrangement = SpeakerArr::kStereo;
        return bus == 0 ? kResultOk : kInvalidArgument;
    }
    tresult PLUGIN_API canProcessSampleSize (int32 size) override { return size == kSample32 ? kResultOk : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return (uint32) (101 + index); }
    tresult PLUGIN_API setupProcessing (ProcessSetup&) override { return kResultOk; }
    tresult PLUGIN_API setProcessing (TBool) override { return kResultOk; }
    tresult PLUGIN_API process (ProcessData& data) override
    {
        if (data.symbolicSampleSize != kSample32) return kResultFalse;
        if (data.numInputs > 0 && data.numOutputs > 0)
        {
            auto& in = data.inputs[0];
            auto& out = data.outputs[0];
            for (int32 c = 0; c < out.numChannels; ++c)
            {
                if (c < in.numChannels)
                    std::memmove (out.channelBuffers32[c], in.channelBuffers32[c], (size_t) data.numSamples * sizeof (float));
                else
                    std::fill_n (out.channelBuffers32[c], data.numSamples, 0.0f);
            }
            out.silenceFlags = in.silenceFlags;
        }
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return 0; }
    int32 PLUGIN_API getParameterCount() override { return 1; }
    tresult PLUGIN_API getParameterInfo (int32 param, ParameterInfo& info) override
    {
        if (param != 0) return kInvalidArgument;
        info = {};
        info.id = 0;
        info.defaultNormalizedValue = 0.5;
        info.flags = ParameterInfo::kCanAutomate;
        copyText (info.title, names[index]);
        copyText (info.shortTitle, names[index]);
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue (ParamID, ParamValue v, String128 string) override
    {
        string[0] = v >= 0.5 ? u'1' : u'0';
        string[1] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString (ParamID, TChar* text, ParamValue& v) override
    { v = text != nullptr && text[0] == u'1' ? 1.0 : 0.0; return kResultOk; }
    ParamValue PLUGIN_API normalizedParamToPlain (ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API plainParamToNormalized (ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API getParamNormalized (ParamID) override { return value; }
    tresult PLUGIN_API setParamNormalized (ParamID, ParamValue v) override { value = v; return kResultOk; }
    tresult PLUGIN_API setComponentHandler (IComponentHandler*) override { return kResultOk; }
    IPlugView* PLUGIN_API createView (FIDString) override { return nullptr; }

private:
    std::atomic<uint32> refs { 1 };
    int index;
    bool controllerOnly;
    ParamValue value = 0.5;
};

class Factory final : public IPluginFactory3
{
public:
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
    {
        *obj = nullptr;
        if (same (iid, FUnknown_iid) || same (iid, IPluginFactory_iid)
            || same (iid, IPluginFactory2_iid) || same (iid, IPluginFactory3_iid))
        {
            *obj = static_cast<IPluginFactory3*> (this);
            addRef();
            return kResultOk;
        }
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs; }
    uint32 PLUGIN_API release() override
    {
        const auto count = --refs;
        if (count == 0) delete this;
        return count;
    }
    tresult PLUGIN_API getFactoryInfo (PFactoryInfo* info) override
    {
        if (info == nullptr) return kInvalidArgument;
        *info = {};
        copyText (info->vendor, "LiveMix Test Fixtures");
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override
    {
       #if LM_FIXTURE_KIND == 0
        return hasContext ? 7 : 3;
       #elif LM_FIXTURE_KIND == 3
        return 0;
       #else
        return 1;
       #endif
    }
    tresult PLUGIN_API getClassInfo (int32 i, PClassInfo* info) override
    {
        if (info == nullptr || i < 0 || i >= countClasses()) return kInvalidArgument;
        const bool ctrl = LM_FIXTURE_KIND == 0 && hasContext && i < 6 && i % 2 != 0;
        const int effect = LM_FIXTURE_KIND == 0 && hasContext ? i / 2 : i;
        *info = {};
        std::memcpy (info->cid, ctrl ? controllers[effect] : effects[effect], sizeof (TUID));
        info->cardinality = PClassInfo::kManyInstances;
        copyText (info->category, ctrl ? kVstComponentControllerClass : kVstAudioEffectClass);
        const auto name = LM_FIXTURE_KIND == 1 ? "LiveMix Test Crasher" : LM_FIXTURE_KIND == 2 ? "LiveMix Test Hang"
                          : ctrl ? controllerNames[effect] : names[effect];
        copyText (info->name, name);
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2 (int32 i, PClassInfo2* info) override
    {
        PClassInfo basic {};
        if (info == nullptr || getClassInfo (i, &basic) != kResultOk) return kInvalidArgument;
        *info = {};
        std::memcpy (info->cid, basic.cid, sizeof (TUID));
        info->cardinality = basic.cardinality;
        copyText (info->category, basic.category);
        copyText (info->name, basic.name);
        copyText (info->subCategories, "Fx");
        copyText (info->vendor, "LiveMix Test Fixtures");
        copyText (info->version, "1.0.0");
        copyText (info->sdkVersion, "VST 3");
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfoUnicode (int32 i, PClassInfoW* info) override
    {
        PClassInfo2 ascii {};
        if (info == nullptr || getClassInfo2 (i, &ascii) != kResultOk) return kInvalidArgument;
        *info = {};
        std::memcpy (info->cid, ascii.cid, sizeof (TUID));
        info->cardinality = ascii.cardinality;
        copyText (info->category, ascii.category);
        copyText (info->name, ascii.name);
        copyText (info->subCategories, ascii.subCategories);
        copyText (info->vendor, ascii.vendor);
        copyText (info->version, ascii.version);
        copyText (info->sdkVersion, ascii.sdkVersion);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance (FIDString cid, FIDString iid, void** obj) override
    {
        *obj = nullptr;
        for (int i = 0; i < countClasses(); ++i)
        {
            PClassInfo info {};
            getClassInfo (i, &info);
            if (! same (cid, info.cid)) continue;
            const bool ctrl = std::strcmp (info.category, kVstComponentControllerClass) == 0;
            const int index = LM_FIXTURE_KIND == 0 && hasContext ? i / 2 : i;
            auto* effect = new Effect (index, ctrl);
            const auto result = effect->queryInterface (iid, obj);
            effect->release();
            return result;
        }
        return kNoInterface;
    }
    tresult PLUGIN_API setHostContext (FUnknown* context) override
    {
        hasContext = context != nullptr; // rebuilding is idempotent; null is safe and restores the initial list
        return kResultOk;
    }

private:
    std::atomic<uint32> refs { 1 };
    bool hasContext = false;
};
}

extern "C" __declspec(dllexport) Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory() { return new Factory; }
extern "C" __declspec(dllexport) bool PLUGIN_API InitDll() { return true; }
extern "C" __declspec(dllexport) bool PLUGIN_API ExitDll() { return true; }
