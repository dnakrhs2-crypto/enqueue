#include "AudioImport.h"
#include "audio/MediaFoundationAudioFormat.h"
#include "model/SafeFileWrite.h"
#include "storage/DurableFile.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/sha.h>
}
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>

namespace gocue::recorder
{
namespace
{
using Stage = AudioImportControl::Stage;
void require(bool ok, const juce::String& message)
{ if (!ok) throw std::runtime_error(message.toStdString()); }
void checked(const juce::Result& r) { require(r.wasOk(), r.getErrorMessage()); }

std::unique_ptr<juce::AudioFormatReader> openNativeMf(const juce::File& file)
{
    gocue::MediaFoundationAudioFormat format;
    auto stream = file.createInputStream(); require(stream && stream->openedOk(), "MF 원본 파일 읽기 실패");
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(stream.release(), true));
    require(reader != nullptr, "Media Foundation codec/DRM으로 오디오 파일을 열 수 없습니다."); return reader;
}
// MF's two-frame seek preroll still differed from a fresh sequential decode on the
// local AAC fixture (max 0.01594776). Import never needs realtime source seeking:
// preserve the existing decoder's sequential state and recreate it for backwards reads.
// Timeline seeks use the project-Fs PCM cache. Shared MediaFoundationAudioFormat stays unchanged.
class SequentialMfImportReader final : public juce::AudioFormatReader
{
public:
    explicit SequentialMfImportReader(const juce::File& file)
        : AudioFormatReader(nullptr, "Media Foundation (sequential import)"), source(file), native(openNativeMf(file))
    {
        sampleRate = native->sampleRate; numChannels = native->numChannels; bitsPerSample = native->bitsPerSample;
        usesFloatingPointData = native->usesFloatingPointData; lengthInSamples = native->lengthInSamples;
        metadataValues = native->metadataValues;
    }
    bool readSamples(int* const* dest, int channels, int offset, juce::int64 start, int count) override
    {
        clearSamplesBeyondAvailableLength(dest, channels, offset, start, count, lengthInSamples);
        if (count <= 0) return true;
        if (start < position) { native = openNativeMf(source); position = 0; }
        juce::AudioBuffer<float> discard(static_cast<int>(numChannels), 4096);
        while (position < start)
        {
            const int n = static_cast<int>((std::min)(juce::int64(4096), start - position));
            if (!native->read(&discard, 0, n, position, true, numChannels == 2)) return false;
            position += n;
        }
        const bool ok = native->readSamples(dest, channels, offset, position, count); position += count; return ok;
    }
private:
    juce::File source;
    std::unique_ptr<juce::AudioFormatReader> native;
    juce::int64 position = 0;
};
void avChecked(int result, const char* operation)
{
    if (result >= 0) return;
    char text[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(result, text, sizeof(text));
    throw std::runtime_error(std::string(operation) + ": " + text);
}
struct AvInput
{
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    ~AvInput() { av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&decoder); avformat_close_input(&format); }
};

// Validation only: the PCM used for playback is always read by JUCE/MF below.
// Counting AVFrame::nb_samples with SKIP_MANUAL avoids trusting a bitrate/duration estimate,
// and preserves packet priming/padding evidence without modifying the shared JUCE reader.
void auditDecode(const juce::File& file, AudioImportControl& control, ImportedAudioInfo& info)
{
    AvInput in;
    in.format = avformat_alloc_context();
    require(in.format && in.packet && in.frame, "오디오 검증 메모리가 부족합니다.");
    in.format->interrupt_callback = { [](void* p) -> int { return static_cast<AudioImportControl*>(p)->cancelled.load() ? 1 : 0; }, &control };
    avChecked(avformat_open_input(&in.format, file.getFullPathName().toRawUTF8(), nullptr, nullptr), "오디오/codec/DRM 검사 실패");
    avChecked(avformat_find_stream_info(in.format, nullptr), "오디오 스트림 정보를 읽을 수 없습니다");
    const int streamIndex = av_find_best_stream(in.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    avChecked(streamIndex, "지원하는 오디오 스트림이 없습니다");
    const auto* stream = in.format->streams[streamIndex];
    const auto* parameters = stream->codecpar;
    require(parameters->ch_layout.nb_channels == 1 || parameters->ch_layout.nb_channels == 2,
            "완성 오디오는 mono/stereo(1~2채널)만 지원합니다. 자동 downmix는 지원하지 않습니다.");
    require(parameters->sample_rate == static_cast<int>(info.sampleRate)
            && parameters->ch_layout.nb_channels == info.channels, "원본과 reader의 샘플레이트/채널이 다릅니다.");
    info.codec = avcodec_get_name(parameters->codec_id);
    const auto* codec = avcodec_find_decoder(parameters->codec_id);
    require(codec != nullptr, "지원하지 않는 codec이거나 DRM으로 보호된 파일입니다.");
    in.decoder = avcodec_alloc_context3(codec);
    require(in.decoder != nullptr, "오디오 디코더 메모리가 부족합니다.");
    avChecked(avcodec_parameters_to_context(in.decoder, parameters), "오디오 포맷 오류");
    in.decoder->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
    in.decoder->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
    in.decoder->thread_count = 1;
    avChecked(avcodec_open2(in.decoder, codec, nullptr), "codec/DRM으로 오디오를 해독할 수 없습니다");
    bool hadSkip = false;
    const auto receive = [&]
    {
        for (;;)
        {
            const int status = avcodec_receive_frame(in.decoder, in.frame);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return;
            avChecked(status, "손상된 오디오를 해독할 수 없습니다");
            require(in.frame->sample_rate == static_cast<int>(info.sampleRate)
                    && in.frame->ch_layout.nb_channels == info.channels && !in.frame->decode_error_flags,
                    "오디오 도중 포맷이 바뀌었거나 디코드 오류가 있습니다.");
            require(in.frame->nb_samples > 0 && info.rawDecodedSamples <= (std::numeric_limits<Sample>::max)() - in.frame->nb_samples,
                    "오디오 샘플 길이가 범위를 벗어납니다.");
            info.rawDecodedSamples += in.frame->nb_samples;
            av_frame_unref(in.frame);
        }
    };
    int status = 0;
    while ((status = av_read_frame(in.format, in.packet)) >= 0)
    {
        control.checkpoint(Stage::verifying, .25);
        if (in.packet->stream_index == streamIndex)
        {
            require(!(in.packet->flags & AV_PKT_FLAG_CORRUPT), "손상되거나 잘린 오디오 packet입니다.");
            size_t size = 0;
            if (const auto* skip = av_packet_get_side_data(in.packet, AV_PKT_DATA_SKIP_SAMPLES, &size); skip && size >= 10)
            {
                // Only boundary skips are supported; midstream edit/discontinuity must not be collapsed.
                const Sample lead = AV_RL32(skip), tail = AV_RL32(skip + 4);
                require(lead == 0 || info.rawDecodedSamples == 0, "오디오 중간의 priming/edit list는 지원하지 않습니다.");
                info.leadingSkipSamples += lead; info.trailingSkipSamples += tail; hadSkip = true;
            }
            avChecked(avcodec_send_packet(in.decoder, in.packet), "오디오 packet 디코드 실패");
            receive();
        }
        av_packet_unref(in.packet);
    }
    avChecked(status == AVERROR_EOF ? 0 : status, "오디오 파일 읽기 실패");
    avChecked(avcodec_send_packet(in.decoder, nullptr), "오디오 마지막 프레임 디코드 실패");
    receive();
    require(info.rawDecodedSamples > info.leadingSkipSamples + info.trailingSkipSamples, "유효한 오디오 샘플이 없습니다.");
    info.decodedSamples = info.rawDecodedSamples - info.leadingSkipSamples - info.trailingSkipSamples;
    const bool mp4 = file.hasFileExtension("m4a;mp4;m4b");
    if (mp4 && stream->duration != AV_NOPTS_VALUE && stream->duration > 0)
    {
        const auto duration = av_rescale_q(stream->duration, stream->time_base, AVRational{1, static_cast<int>(info.sampleRate)});
        require(duration > 0 && duration <= info.decodedSamples, "컨테이너 길이보다 실제 디코드 오디오가 짧습니다.");
        info.trailingSkipSamples += info.decodedSamples - duration;
        info.decodedSamples = duration;
    }
    info.primingKnown = hadSkip || file.hasFileExtension("wav;wave");
    info.primingEvidence = hadSkip ? "packet skip-samples; MP4 presentation duration when present"
        : mp4 ? "unknown encoder priming: MP4 presentation duration only; no explicit leading skip"
        : info.primingKnown ? "uncompressed PCM; no encoder priming"
        : "unknown: no gapless metadata; preserve decoded boundaries, no silence guessing";
    info.readerStartSample = file.hasFileExtension("mp3") ? info.leadingSkipSamples : 0;
}

// The shared MF reader returns silence on decoder errors and has no EOF/error counters.
// Walk the same native float output independently, without changing that shared reader,
// so an error or premature EOF cannot be accepted as silent valid audio by the importer.
void auditMediaFoundation(const juce::File& file, AudioImportControl& control, ImportedAudioInfo& info)
{
    using Microsoft::WRL::ComPtr;
    const auto hr = [](HRESULT value)
    { require(SUCCEEDED(value), "Media Foundation 오디오 검증 실패 (HRESULT 0x" + juce::String::toHexString(static_cast<int>(value)) + "). 지원하지 않는 codec/DRM 또는 손상된 파일일 수 있습니다."); };
    ComPtr<IMFAttributes> attributes; hr(MFCreateAttributes(&attributes, 1)); hr(attributes->SetUINT32(MF_LOW_LATENCY, FALSE));
    ComPtr<IMFSourceReader> reader; hr(MFCreateSourceReaderFromURL(file.getFullPathName().toWideCharPointer(), attributes.Get(), &reader));
    hr(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE)); hr(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE));
    ComPtr<IMFMediaType> type; hr(MFCreateMediaType(&type)); hr(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)); hr(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float));
    hr(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type.Get()));
    const auto checkFormat = [&]
    {
        ComPtr<IMFMediaType> actual; hr(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual));
        UINT32 rate = 0, channels = 0, bits = 0;
        hr(actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)); hr(actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)); hr(actual->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits));
        require(rate == info.sampleRate && channels == static_cast<UINT32>(info.channels) && bits == 32, "MF 검증 도중 오디오 포맷이 바뀌었습니다.");
    };
    checkFormat();
    PROPVARIANT start{}; start.vt = VT_I8; start.hVal.QuadPart = 0; hr(reader->SetCurrentPosition(GUID_NULL, start));
    bool pending = false, emitted = false; Sample pendingAt = 0, pendingFrames = 0, expected = 0;
    const auto emit = [&]
    {
        const Sample skip = (std::max)(Sample(0), -pendingAt);
        if (pendingFrames <= skip) return;
        const auto at = (std::max)(Sample(0), pendingAt);
        require(std::abs(at - expected) <= 2, "MF 오디오 timestamp에 지원하지 않는 빈 구간 또는 불연속이 있습니다.");
        info.readerDecodedSamples += pendingFrames - skip; expected = info.readerDecodedSamples; emitted = true;
    };
    for (;;)
    {
        control.checkpoint(Stage::verifying, .30);
        DWORD flags = 0; LONGLONG ticks = 0; ComPtr<IMFSample> sample;
        hr(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ticks, &sample));
        require(!(flags & MF_SOURCE_READERF_ERROR), "MF 디코더 오류로 오디오 검증을 중단했습니다.");
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) checkFormat();
        if (sample)
        {
            DWORD bytes = 0; hr(sample->GetTotalLength(&bytes));
            require(bytes % (sizeof(float) * info.channels) == 0, "MF PCM 버퍼 길이가 잘못되었습니다.");
            const auto frames = static_cast<Sample>(bytes / (sizeof(float) * info.channels));
            const auto at = rescaleRound(ticks, info.sampleRate, 10000000);
            if (frames > 0)
            {
                if (pending)
                {
                    if (!emitted && at <= pendingAt) info.readerPrimingDiscardedSamples += pendingFrames;
                    else emit();
                }
                pending = true; pendingAt = at; pendingFrames = frames;
            }
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (pending) emit();
    require(info.readerDecodedSamples > 0, "MF 디코더가 실제 오디오 샘플을 반환하지 않았습니다.");
    const auto readable = (std::min)(info.readerDecodedSamples, info.readerReportedSamples);
    if (!info.primingKnown)
        info.decodedSamples = (std::min)(info.decodedSamples, readable); // no gapless tags: do not invent encoder trimming
    require(info.decodedSamples > 0 && readable >= info.decodedSamples,
            "MF 실제 디코드 길이가 컨테이너의 유효 오디오 길이보다 짧습니다.");
}
}

void AudioImportControl::checkpoint(Stage desired, double fraction)
{
    while (recordingActive.load() && !cancelled.load())
    {
        stage.store(Stage::pausedForRecording);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(!cancelled.load(), "오디오 불러오기를 취소했습니다.");
    stage.store(desired); progress.store(std::clamp(fraction, 0.0, 1.0));
    if (onProgress) onProgress(desired, progress.load());
    require(!cancelled.load(), "오디오 불러오기를 취소했습니다.");
}
juce::var ImportedAudioInfo::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty("version", 1); o->setProperty("codec", codec); o->setProperty("reader", reader);
    o->setProperty("contentHash", contentHash); o->setProperty("sampleRate", static_cast<int>(sampleRate));
    o->setProperty("channels", channels); o->setProperty("bitsPerSample", bitsPerSample);
    o->setProperty("readerReportedSamples", juce::int64(readerReportedSamples));
    o->setProperty("rawDecodedSamples", juce::int64(rawDecodedSamples));
    o->setProperty("readerDecodedSamples", juce::int64(readerDecodedSamples));
    o->setProperty("readerPrimingDiscardedSamples", juce::int64(readerPrimingDiscardedSamples));
    o->setProperty("decodedSamples", juce::int64(decodedSamples));
    o->setProperty("leadingSkipSamples", juce::int64(leadingSkipSamples));
    o->setProperty("trailingSkipSamples", juce::int64(trailingSkipSamples));
    o->setProperty("readerStartSample", juce::int64(readerStartSample));
    o->setProperty("primingKnown", primingKnown); o->setProperty("primingEvidence", primingEvidence);
    o->setProperty("validationDecoder", "FFmpeg " RECORDER_FFMPEG_VERSION " (count/error audit only)");
    return juce::var(o);
}
ImportedAudioInfo ImportedAudioInfo::fromVar(const juce::var& v)
{
    require(static_cast<int>(v["version"]) == 1, "오디오 import 메타데이터 버전을 지원하지 않습니다.");
    ImportedAudioInfo i;
    i.codec = v["codec"].toString(); i.reader = v["reader"].toString(); i.contentHash = v["contentHash"].toString();
    i.sampleRate = static_cast<std::uint32_t>(static_cast<int>(v["sampleRate"]));
    i.channels = v["channels"]; i.bitsPerSample = v["bitsPerSample"];
    i.readerReportedSamples = static_cast<juce::int64>(v["readerReportedSamples"]);
    i.rawDecodedSamples = static_cast<juce::int64>(v["rawDecodedSamples"]);
    i.readerDecodedSamples = static_cast<juce::int64>(v["readerDecodedSamples"]);
    i.readerPrimingDiscardedSamples = static_cast<juce::int64>(v["readerPrimingDiscardedSamples"]);
    i.decodedSamples = static_cast<juce::int64>(v["decodedSamples"]);
    i.leadingSkipSamples = static_cast<juce::int64>(v["leadingSkipSamples"]);
    i.trailingSkipSamples = static_cast<juce::int64>(v["trailingSkipSamples"]);
    i.readerStartSample = static_cast<juce::int64>(v["readerStartSample"]);
    i.primingKnown = v["primingKnown"]; i.primingEvidence = v["primingEvidence"].toString();
    require(i.sampleRate > 0 && (i.channels == 1 || i.channels == 2) && i.decodedSamples > 0
        && i.leadingSkipSamples >= 0 && i.trailingSkipSamples >= 0 && i.readerStartSample >= 0
        && i.rawDecodedSamples >= i.decodedSamples && i.readerDecodedSamples >= i.decodedSamples
        && i.leadingSkipSamples <= i.rawDecodedSamples && i.trailingSkipSamples <= i.rawDecodedSamples - i.leadingSkipSamples
        && i.readerStartSample <= (std::numeric_limits<Sample>::max)() - i.decodedSamples
        && i.contentHash.length() == 64 && i.contentHash.containsOnly("0123456789abcdef"),
        "오디오 import 메타데이터가 손상되었습니다.");
    return i;
}
std::unique_ptr<juce::AudioFormatReader> AudioImport::openReader(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormat> format;
    if (file.hasFileExtension("wav;wave")) format = std::make_unique<juce::WavAudioFormat>();
    else if (file.hasFileExtension("mp3"))
    {
       #if JUCE_USE_MP3AUDIOFORMAT
        format = std::make_unique<juce::MP3AudioFormat>();
       #else
        throw std::runtime_error("JUCE MP3 포맷이 빌드에 포함되지 않았습니다.");
       #endif
    }
    else if (file.hasFileExtension("m4a;aac;mp4;m4b;wma")) return std::make_unique<SequentialMfImportReader>(file);
    else throw std::runtime_error("지원하지 않는 오디오 파일 형식입니다. WAV/MP3/M4A/AAC 파일을 선택하세요.");
    auto input = file.createInputStream();
    require(input && input->openedOk(), "오디오 파일을 열 수 없습니다. 파일 경로와 읽기 권한을 확인하세요.");
    std::unique_ptr<juce::AudioFormatReader> reader(format->createReaderFor(input.release(), true));
    require(reader != nullptr, "오디오 codec을 읽을 수 없습니다. 손상된 파일, 지원하지 않는 codec 또는 DRM 보호 파일일 수 있습니다.");
    return reader;
}
juce::String AudioImport::hashFile(const juce::File& file, AudioImportControl& control, Stage stage)
{
    auto stream = file.createInputStream();
    require(stream && stream->openedOk(), "원본 hash 검증을 위해 파일을 열 수 없습니다.");
    std::unique_ptr<AVSHA, decltype(&av_free)> sha(av_sha_alloc(), av_free);
    require(sha != nullptr && av_sha_init(sha.get(), 256) == 0, "SHA-256 초기화 실패");
    std::vector<std::uint8_t> block(256 * 1024); Sample count = 0;
    for (;;)
    {
        control.checkpoint(stage, control.progress.load());
        const int n = stream->read(block.data(), static_cast<int>(block.size()));
        if (n <= 0) break;
        count += n; av_sha_update(sha.get(), block.data(), n);
    }
    require(stream->getStatus().wasOk() && count == file.getSize(), "원본 hash 검사 도중 파일 읽기에 실패했습니다.");
    std::uint8_t bytes[32]; av_sha_final(sha.get(), bytes);
    return juce::String::toHexString(bytes, 32, 0);
}
juce::Result AudioImport::inspect(const juce::File& file, AudioImportControl& control, ImportedAudioInfo& result)
{
    result = {};
    try
    {
        control.checkpoint(Stage::verifying, .05);
        auto reader = openReader(file);
        ImportedAudioInfo info;
        require(reader->numChannels == 1 || reader->numChannels == 2,
                "완성 오디오는 mono/stereo(1~2채널)만 지원합니다. 자동 downmix는 지원하지 않습니다.");
        require(std::isfinite(reader->sampleRate) && reader->sampleRate >= 1 && reader->sampleRate <= 768000
                && reader->sampleRate == std::floor(reader->sampleRate), "지원하지 않는 오디오 샘플레이트입니다.");
        info.sampleRate = static_cast<std::uint32_t>(reader->sampleRate); info.channels = static_cast<int>(reader->numChannels);
        info.bitsPerSample = file.hasFileExtension("wav;wave") ? static_cast<int>(reader->bitsPerSample) : 0;
        info.reader = reader->getFormatName(); info.readerReportedSamples = reader->lengthInSamples;
        auditDecode(file, control, info);
        const bool nativeMf = file.hasFileExtension("m4a;aac;mp4;m4b;wma");
        if (nativeMf) auditMediaFoundation(file, control, info);
        else info.readerDecodedSamples = info.rawDecodedSamples;
        if (file.hasFileExtension("wav;wave")) require(info.rawDecodedSamples == reader->lengthInSamples, "WAV의 선언 길이와 실제 디코드 길이가 다릅니다. 파일이 잘렸을 수 있습니다.");
        const auto count = nativeMf ? info.decodedSamples : info.rawDecodedSamples;
        // MP3 reader length is an estimate; the packet/decode audit is authoritative.
        if (!file.hasFileExtension("mp3")) require(count <= reader->lengthInSamples, "reader 길이보다 검증된 오디오가 깁니다.");
        reader->lengthInSamples = (std::max)(count, reader->lengthInSamples);
        juce::AudioBuffer<float> block(info.channels, 4096);
        for (Sample at = 0; at < count;)
        {
            control.checkpoint(Stage::verifying, .35 + .55 * static_cast<double>(at) / static_cast<double>(count));
            const int n = static_cast<int>((std::min)(Sample(4096), count - at));
            require(reader->read(&block, 0, n, at, true, info.channels == 2), "오디오 재열기/전체 디코드 검증에 실패했습니다.");
            for (int ch = 0; ch < info.channels; ++ch) for (int s = 0; s < n; ++s)
                require(std::isfinite(block.getSample(ch, s)), "오디오에 유효하지 않은 PCM 샘플이 있습니다.");
            at += n;
        }
        reader.reset();
        auto reopened = openReader(file);
        require(reopened->sampleRate == info.sampleRate && reopened->numChannels == static_cast<unsigned>(info.channels), "오디오 재열기 포맷 검증에 실패했습니다.");
        info.contentHash = hashFile(file, control);
        result = std::move(info); return juce::Result::ok();
    }
    catch (const std::exception& e) { return juce::Result::fail(juce::String::fromUTF8(e.what())); }
}
PreparedAudioImport::~PreparedAudioImport()
{
    // Only a directory created and owned by this attempt, never a caller-supplied recursive target.
    if (!committed && ownedDirectory != juce::File() && ownedDirectory.isAChildOf(request.projectDirectory.getChildFile("media/imports")))
        ownedDirectory.deleteRecursively();
}
juce::Result AudioImport::prepare(const AudioImportRequest& request, AudioImportControl& control,
                                std::unique_ptr<PreparedAudioImport>& output)
{
    output.reset();
    try
    {
        control.checkpoint(Stage::copying, 0);
        require(request.source.existsAsFile() && request.source.getSize() > 0, "불러올 오디오 파일이 없거나 비어 있습니다.");
        require(request.projectDirectory != juce::File() && isId(request.projectId) && request.projectFs > 0 && request.projectFs <= 768000 && request.playhead >= 0,
                "프로젝트 또는 플레이헤드가 올바르지 않습니다.");
        auto prepared = std::unique_ptr<PreparedAudioImport>(new PreparedAudioImport()); prepared->request = request;
        const auto imports = request.projectDirectory.getChildFile("media/imports");
        checked(imports.createDirectory());
        const auto directory = imports.getChildFile(prepared->mediaAsset.assetId);
        require(!directory.exists(), "오디오 import 경로가 이미 존재합니다.");
        checked(directory.createDirectory()); prepared->ownedDirectory = directory;
        prepared->copiedFile = directory.getChildFile(request.source.getFileName());
        // Deny concurrent writes/deletion of the external source while copying and comparing hashes.
        const HANDLE sourceLock = CreateFileW(request.source.getFullPathName().toWideCharPointer(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(sourceLock != INVALID_HANDLE_VALUE, "원본 파일을 읽기 전용으로 잠글 수 없습니다. 다른 프로그램의 쓰기가 끝난 뒤 다시 시도하세요.");
        struct Close { HANDLE h; ~Close() { CloseHandle(h); } } lock{sourceLock};
        auto source = request.source.createInputStream(); require(source && source->openedOk(), "원본 파일 읽기 실패");
        const auto size = request.source.getSize();
        DurableFile copy(request.copyFaults); checked(copy.open(prepared->copiedFile, DurableFile::OpenMode::createNew));
        std::vector<std::uint8_t> block(256 * 1024); Sample copied = 0;
        while (copied < size)
        {
            control.checkpoint(Stage::copying, static_cast<double>(copied) / static_cast<double>(size));
            const int n = source->read(block.data(), static_cast<int>(block.size()));
            require(n > 0, "오디오 복사 도중 원본 읽기에 실패했습니다.");
            checked(copy.write(block.data(), static_cast<size_t>(n))); copied += n;
        }
        require(source->getStatus().wasOk() && copied == size, "오디오 복사 길이 검증 실패");
        checked(copy.flushData()); checked(copy.close()); source.reset();
        checked(inspect(prepared->copiedFile, control, prepared->sourceInfo));
        require(prepared->sourceInfo.contentHash == hashFile(request.source, control), "복사한 오디오의 SHA-256이 원본과 다릅니다.");
        auto& asset = prepared->mediaAsset; const auto& info = prepared->sourceInfo;
        asset.kind = AssetKind::importAudio; asset.relativePath = prepared->copiedFile.getRelativePathFrom(request.projectDirectory).replaceCharacter('\\', '/');
        asset.originalFormat.codec = info.codec; asset.originalFormat.sampleRate = info.sampleRate;
        asset.originalFormat.channels = info.channels; asset.originalFormat.bitsPerSample = info.bitsPerSample;
        asset.contentIdentity = info.contentHash; asset.logicalLength = rescaleRound(info.decodedSamples, request.projectFs, info.sampleRate);
        asset.sourceUnitsNumerator = info.sampleRate; asset.sourceUnitsDenominator = request.projectFs;
        require(asset.logicalLength > 0 && request.playhead <= (std::numeric_limits<Sample>::max)() - asset.logicalLength, "클립 길이 또는 배치 위치가 범위를 벗어납니다.");
        asset.availableRanges = {{0, asset.logicalLength}};
        auto& track = prepared->importedTrack; track.kind = TrackKind::importAudio; track.name = request.source.getFileNameWithoutExtension();
        auto& clip = prepared->importedClip; clip.assetId = asset.assetId; clip.trackId = track.trackId;
        clip.lengthSamples = asset.logicalLength; clip.timelineStartSample = request.playhead;
        track.clips.edit().push_back(clip);
        checked(gocue::SafeFileWrite::writeTextVerified(directory.getChildFile(".import-info.json"), juce::JSON::toString(info.toVar()),
            [](const juce::String& value) { try { ImportedAudioInfo::fromVar(juce::JSON::parse(value)); return juce::Result::ok(); }
                catch (const std::exception& e) { return juce::Result::fail(e.what()); } }));
        control.checkpoint(Stage::ready, 1); output = std::move(prepared); return juce::Result::ok();
    }
    catch (const std::exception& e)
    {
        control.stage.store(control.cancelled.load() ? Stage::cancelled : Stage::failed);
        return juce::Result::fail(juce::String::fromUTF8(e.what()));
    }
}
ImportedAudioInfo AudioImport::loadInfo(const juce::File& projectDirectory, const MediaAsset& asset)
{
    require(asset.kind == AssetKind::importAudio && isProjectRelativePath(asset.relativePath)
            && asset.relativePath.startsWith("media/imports/"), "import 원본 경로가 올바르지 않습니다.");
    auto info = ImportedAudioInfo::fromVar(juce::JSON::parse(projectDirectory.getChildFile(asset.relativePath).getSiblingFile(".import-info.json")));
    require(info.contentHash == asset.contentIdentity && info.sampleRate == asset.originalFormat.sampleRate
        && info.channels == asset.originalFormat.channels && info.codec == asset.originalFormat.codec
        && asset.logicalLength == rescaleRound(info.decodedSamples, asset.sourceUnitsDenominator, asset.sourceUnitsNumerator),
        "원본과 import 메타데이터가 다릅니다.");
    return info;
}
juce::Result commitImportedAudio(RecorderDocument& document, PreparedAudioImport& prepared, AudioImportControl& control)
{
    if (control.cancelled.load()) return juce::Result::fail("오디오 불러오기를 취소했습니다.");
    if (prepared.committed || document.getProject().projectId != prepared.request.projectId
        || document.getProject().Fs != prepared.request.projectFs
        || (document.getFile() != juce::File() && document.getFile().getParentDirectory() != prepared.request.projectDirectory))
        return juce::Result::fail("불러오기 중 프로젝트가 바뀌었거나 이미 등록한 오디오입니다.");
    const auto result = document.performEdit("오디오 파일 불러오기", [&](EditState& edit)
    {
        // Verified against round 08 RecorderDocument::performEdit: its working object is RecorderProject.
        auto& next = static_cast<RecorderProject&>(edit);
        auto registry = std::make_shared<MediaRegistry>(*next.media);
        registry->assets.push_back(prepared.mediaAsset); next.media = std::move(registry);
        next.tracks.push_back(prepared.importedTrack);
    });
    if (result.wasOk()) prepared.committed = true;
    return result;
}
}
