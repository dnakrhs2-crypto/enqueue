#include "MfMp3Sink.h"
#include "../../shared/lm_asrc.h"
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace gocue::livemix::recording
{
using Microsoft::WRL::ComPtr;
struct MfMp3Sink::Impl
{
    ComPtr<IMFTransform> encoder;
    std::unique_ptr<RecordingRecovery::Stream> stream;
    RecordingRecovery recovery;
    MFT_OUTPUT_STREAM_INFO outputInfo {};
    MFT_INPUT_STREAM_INFO inputInfo {};
    std::vector<double> asrcMemory;
    lm_asrc* asrc = nullptr;
    std::array<float, 4096 * 2> resampled {};
    std::uint64_t inputFrames = 0, pcmFrames = 0, encodedFrames = 0, clips = 0;
    std::uint32_t random = 0x7e35a19b;
    unsigned rate = 48000;
    double sourceRate = 48000;
    int skip = 0;
    bool com = false, mf = false, closed = false;
    Error failure = Error::none;
    juce::String detail;
    HMODULE mfModule = nullptr;

    bool fail(Error code, const juce::String& message)
    {
        if (failure == Error::none) { failure = code; detail = message; }
        return false;
    }
    bool check(HRESULT hr, const char* operation)
    {
        return SUCCEEDED(hr) || fail(Error::encoderFailed, juce::String(operation) + " HRESULT 0x"
                                     + juce::String::toHexString(static_cast<int>(hr)));
    }
    float uniform() noexcept
    {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        return static_cast<float>(random >> 8) / 16777216.0f;
    }
    std::int16_t pcm(float x, float noise)
    {
        bool clipped = !std::isfinite(x);
        if (clipped) x = 0;
        clipped = clipped || x < -1.0f || x > 32767.0f / 32768.0f;
        const auto q = std::lround(std::clamp(static_cast<double>(x), -1.0, 1.0) * 32768.0 + noise);
        if (clipped || q < -32768 || q > 32767) ++clips;
        return static_cast<std::int16_t>(std::clamp(q, -32768L, 32767L));
    }
    bool output(bool& produced)
    {
        produced = false;
        for (;;)
        {
            ComPtr<IMFSample> sample;
            ComPtr<IMFMediaBuffer> buffer;
            if ((outputInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
            {
                if (!check(MFCreateSample(&sample), "create output sample")
                    || !check(MFCreateAlignedMemoryBuffer(std::max<DWORD>(outputInfo.cbSize, 16384),
                              outputInfo.cbAlignment ? outputInfo.cbAlignment - 1 : 0, &buffer), "output buffer")
                    || !check(sample->AddBuffer(buffer.Get()), "add output buffer")) return false;
            }
            MFT_OUTPUT_DATA_BUFFER out {};
            out.pSample = sample.Get();
            DWORD status = 0;
            const auto hr = encoder->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents) out.pEvents->Release();
            if (out.pSample != sample.Get()) sample.Attach(out.pSample);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (!check(hr, "ProcessOutput") || !sample) return fail(Error::encoderFailed, "empty encoder output");
            if (!check(sample->ConvertToContiguousBuffer(&buffer), "contiguous output")) return false;
            BYTE* data = nullptr; DWORD bytes = 0;
            if (!check(buffer->Lock(&data, nullptr, &bytes), "lock output")) return false;
            // MP3 ACM emits complete MPEG-1 Layer III frames. Count their actual headers, not PCM submitted.
            std::uint64_t frames = 0;
            DWORD pos = 0;
            const unsigned bitrates[] { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
            const unsigned rates[] { 44100,48000,32000,0 };
            bool valid = true;
            while (pos < bytes)
            {
                if (bytes - pos < 4 || data[pos] != 0xff || (data[pos+1] & 0xfe) != 0xfa)
                { valid = false; break; }
                const auto kbps = bitrates[data[pos+2] >> 4];
                const auto hz = rates[(data[pos+2] >> 2) & 3];
                if (!hz || !kbps) { valid = false; break; }
                const auto length = 144000 * kbps / hz + ((data[pos+2] >> 1) & 1);
                if (length > bytes - pos) { valid = false; break; }
                pos += length; frames += 1152;
            }
            const bool saved = valid && stream->write(data, bytes);
            buffer->Unlock();
            if (!valid) return fail(Error::encoderFailed, "incomplete MP3 frame from encoder");
            if (!saved) return fail(Error::writeFailed, stream->error());
            encodedFrames += frames;
            produced = produced || bytes != 0;
        }
    }
    bool submit(const float* data, int n)
    {
        if (n == 0) return true;
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        const auto bytes = static_cast<DWORD>(n * 4);
        if (!check(MFCreateSample(&sample), "create input sample")
            || !check(MFCreateAlignedMemoryBuffer(std::max(bytes, inputInfo.cbSize),
                       inputInfo.cbAlignment ? inputInfo.cbAlignment - 1 : 0, &buffer), "input buffer")) return false;
        BYTE* address = nullptr;
        if (!check(buffer->Lock(&address, nullptr, nullptr), "lock input")) return false;
        auto* dest = reinterpret_cast<std::int16_t*>(address);
        for (int i = 0; i < n; ++i)
        {
            const float noise = uniform() - uniform(); // TPDF, +/- one PCM16 LSB
            dest[2*i] = pcm(data[2*i], noise);
            // Preserve dual mono exactly through quantisation (web seats).
            dest[2*i+1] = pcm(data[2*i+1], data[2*i] == data[2*i+1] ? noise : uniform() - uniform());
        }
        buffer->Unlock();
        if (!check(buffer->SetCurrentLength(bytes), "input length")
            || !check(sample->AddBuffer(buffer.Get()), "add input buffer")
            || !check(sample->SetSampleTime(static_cast<LONGLONG>(pcmFrames * 10000000 / rate)), "input time")
            || !check(sample->SetSampleDuration(static_cast<LONGLONG>((pcmFrames + n) * 10000000 / rate
                                       - pcmFrames * 10000000 / rate)), "input duration")) return false;
        auto hr = encoder->ProcessInput(0, sample.Get(), 0);
        while (hr == MF_E_NOTACCEPTING)
        {
            bool made = false;
            if (!output(made)) return false;
            if (!made) return fail(Error::encoderFailed, "encoder refused input without output");
            hr = encoder->ProcessInput(0, sample.Get(), 0);
        }
        if (!check(hr, "ProcessInput")) return false;
        pcmFrames += static_cast<unsigned>(n);
        bool made = false;
        return output(made);
    }
    bool resample(const float* data, int n, std::uint64_t target)
    {
        int used = 0;
        do
        {
            int consumed = 0;
            const int made = lm_asrc_process(asrc, data + used * 2, n - used, &consumed,
                                             resampled.data(), 4096);
            used += consumed;
            const int discard = std::min(skip, made);
            skip -= discard;
            const auto count = static_cast<int>(std::min<std::uint64_t>(made - discard, target - pcmFrames));
            if (!submit(resampled.data() + discard * 2, count)) return false;
            if (!consumed && !made) break;
        } while (used < n);
        return true;
    }
    bool close()
    {
        if (closed) return failure == Error::none;
        closed = true;
        if (encoder && failure == Error::none)
        {
            if (asrc)
            {
                const auto target = static_cast<std::uint64_t>(std::llround(inputFrames * rate / sourceRate));
                std::array<float, 512> silence {};
                while (pcmFrames < target && failure == Error::none)
                    resample(silence.data(), 256, target);
            }
            if (failure == Error::none
                && check(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "end of stream")
                && check(encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "drain"))
            { bool made = false; output(made); }
            encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        encoder.Reset();
        if (stream)
        {
            if (!recovery.checkpoint(*stream, encodedFrames)) fail(Error::writeFailed, stream->error());
            stream.reset();
        }
        if (mf) { MFShutdown(); mf = false; }
        if (com) { CoUninitialize(); com = false; }
        if (mfModule) { FreeLibrary(mfModule); mfModule = nullptr; }
        return failure == Error::none;
    }
};
MfMp3Sink::MfMp3Sink() : impl(std::make_unique<Impl>()) {}
MfMp3Sink::~MfMp3Sink() { impl->close(); }
bool MfMp3Sink::open(const juce::File& file, double sourceRate, unsigned kbps, std::uint64_t failBytes, bool unavailable)
{
    auto& p = *impl;
    if (p.encoder || p.closed || !std::isfinite(sourceRate) || sourceRate <= 0
        || std::find(mp3BitratesKbps.begin(), mp3BitratesKbps.end(), kbps) == mp3BitratesKbps.end())
        return p.fail(Error::invalidOptions, "invalid MP3 format");
    p.sourceRate = sourceRate;
    p.rate = sourceRate == 32000 || sourceRate == 44100 || sourceRate == 48000
        ? static_cast<unsigned>(sourceRate) : mp3FallbackRate;
    if (unavailable || !(p.mfModule = LoadLibraryW(L"mfplat.dll")))
        return p.fail(Error::encoderUnavailable, "Media Foundation is unavailable");
    const auto co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    p.com = SUCCEEDED(co);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) return p.fail(Error::encoderUnavailable, "COM startup failed");
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return p.fail(Error::encoderUnavailable, "MF startup failed");
    p.mf = true;
    if (FAILED(CoCreateInstance(CLSID_MP3ACMCodecWrapper, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&p.encoder))))
        return p.fail(Error::encoderUnavailable, "MP3 ACM encoder is unavailable");
    ComPtr<IMFMediaType> output;
    for (DWORD index = 0;; ++index)
    {
        ComPtr<IMFMediaType> candidate;
        const auto hr = p.encoder->GetOutputAvailableType(0, index, &candidate);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (!p.check(hr, "enumerate MP3 types")) return false;
        UINT32 hz = 0, channels = 0, bytes = 0;
        candidate->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &hz);
        candidate->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        candidate->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes);
        if (hz == p.rate && channels == 2 && bytes == kbps * 125) { output = candidate; break; }
    }
    if (!output) return p.fail(Error::encoderUnavailable, "requested stereo MP3 bitrate/rate is unavailable");
    // Microsoft MP3 MFT requires output first, then matching stereo PCM16 input.
    if (!p.check(p.encoder->SetOutputType(0, output.Get(), 0), "set MP3 output")) return false;
    ComPtr<IMFMediaType> input;
    if (!p.check(MFCreateMediaType(&input), "create PCM type")) return false;
    if (!p.check(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "PCM major type")
        || !p.check(input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM), "PCM subtype")
        || !p.check(input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2), "PCM channels")
        || !p.check(input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, p.rate), "PCM rate")
        || !p.check(input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16), "PCM bits")
        || !p.check(input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4), "PCM alignment")
        || !p.check(input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, p.rate * 4), "PCM bytes")
        || !p.check(p.encoder->SetInputType(0, input.Get(), 0), "set PCM input")
        || !p.check(p.encoder->GetOutputStreamInfo(0, &p.outputInfo), "output stream info")
        || !p.check(p.encoder->GetInputStreamInfo(0, &p.inputInfo), "input stream info")) return false;
    if (file.exists()) return p.fail(Error::openFailed, "MP3 destination already exists");
    p.stream = std::make_unique<RecordingRecovery::Stream>(file, failBytes);
    if (!p.stream->ok()) return p.fail(Error::openFailed, p.stream->error());
    if (p.rate != sourceRate)
    {
        p.asrcMemory.resize((lm_asrc_size(2) + sizeof(double) - 1) / sizeof(double));
        p.asrc = reinterpret_cast<lm_asrc*>(p.asrcMemory.data());
        lm_asrc_init(p.asrc, 2, sourceRate, p.rate);
        p.skip = static_cast<int>(std::llround(lm_asrc_latency_input_frames(p.asrc) * p.rate / sourceRate));
    }
    return p.check(p.encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "begin streaming")
        && p.check(p.encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "start stream") && checkpoint();
}
bool MfMp3Sink::write(const float* samples, int frames)
{
    auto& p = *impl;
    if (!p.encoder || p.closed || p.failure != Error::none || frames < 0) return false;
    p.inputFrames += static_cast<unsigned>(frames);
    if (p.asrc) return p.resample(samples, frames, RecordingRecovery::unlimited);
    return p.submit(samples, frames);
}
bool MfMp3Sink::checkpoint()
{
    auto& p = *impl;
    return p.stream && (p.recovery.checkpoint(*p.stream, p.encodedFrames) || p.fail(Error::writeFailed, p.stream->error()));
}
bool MfMp3Sink::close() { return impl->close(); }
unsigned MfMp3Sink::sampleRate() const noexcept { return impl->rate; }
std::uint64_t MfMp3Sink::writtenFrames() const noexcept { return impl->encodedFrames; }
std::uint64_t MfMp3Sink::checkpointFrames() const noexcept { return impl->recovery.completedFrames(); }
std::uint64_t MfMp3Sink::clippedSamples() const noexcept { return impl->clips; }
Error MfMp3Sink::errorCode() const noexcept { return impl->failure; }
juce::String MfMp3Sink::error() const { return impl->detail; }
}
