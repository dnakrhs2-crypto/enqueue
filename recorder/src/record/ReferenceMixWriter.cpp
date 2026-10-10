#include "ReferenceMixWriter.h"
extern "C"
{
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
}
#include <algorithm>
#include <array>
#include <cmath>

namespace gocue::recorder
{
struct ReferenceMixWriter::State
{
    CodecPtr codec;
    FramePtr frame = ffFrame(), converted = ffFrame();
    PacketPtr packet = ffPacket();
    SwrContext* swr = nullptr;
    AVAudioFifo* fifo = nullptr;
    std::array<float, 2048> source{};
    std::int64_t inputSamples = 0, convertedSamples = 0, encodedSamples = 0, requested = 0;
    std::int64_t firstPts = AV_NOPTS_VALUE, lastEnd = AV_NOPTS_VALUE;
    std::uint64_t packets = 0;
    bool finished = false;
    bool synthetic = false;
    unsigned inputRate = 44100;
    ~State() { swr_free(&swr); av_audio_fifo_free(fifo); }
    void receive(const PacketSink& sink)
    {
        for (;;)
        {
            const int result = avcodec_receive_packet(codec.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            ffCheckLocalized(result, "참조 AAC 패킷을 받을 수 없습니다.");
            if (firstPts == AV_NOPTS_VALUE) firstPts = packet->pts;
            if (packet->pts == AV_NOPTS_VALUE || packet->pts != packet->dts || (packets && packet->pts < lastEnd))
                throw std::runtime_error("AAC 재생 시각과 디코딩 시각이 맞지 않습니다.");
            lastEnd = packet->pts + packet->duration; ++packets;
            sink(*packet); av_packet_unref(packet.get());
        }
    }
    void encodeAvailable(const PacketSink& sink, bool tail)
    {
        while ((av_audio_fifo_size(fifo) >= codec->frame_size && encodedSamples + codec->frame_size <= requested)
            || (tail && av_audio_fifo_size(fifo)))
        {
            const int count = std::min(codec->frame_size, av_audio_fifo_size(fifo));
            ffCheckLocalized(av_frame_make_writable(frame.get()), "AAC 프레임에 쓸 수 없습니다.");
            frame->nb_samples = count; frame->pts = encodedSamples;
            if (av_audio_fifo_read(fifo, reinterpret_cast<void**>(frame->data), count) != count) throw std::runtime_error("AAC용 PCM 대기열을 읽을 수 없습니다.");
            ffCheckLocalized(avcodec_send_frame(codec.get(), frame.get()), "참조 PCM을 AAC로 인코딩할 수 없습니다.");
            encodedSamples += count; receive(sink);
        }
    }
    int resample(int count, const PacketSink& sink)
    {
        const uint8_t* input[] = {reinterpret_cast<const uint8_t*>(source.data())};
        const int output = swr_convert(swr, converted->data, 2048, count ? input : nullptr, count);
        ffCheckLocalized(output, "참조 오디오를 44100Hz에서 48000Hz로 변환할 수 없습니다.");
        if (av_audio_fifo_space(fifo) < output) throw std::runtime_error("참조 PCM 대기열 용량을 넘었습니다.");
        if (av_audio_fifo_write(fifo, reinterpret_cast<void**>(converted->data), output) != output) throw std::runtime_error("AAC용 PCM 대기열에 쓸 수 없습니다.");
        convertedSamples += output; encodeAvailable(sink, false); return output;
    }
};
ReferenceMixWriter::ReferenceMixWriter() : ReferenceMixWriter(44100) { state->synthetic = true; }
ReferenceMixWriter::ReferenceMixWriter(unsigned inputRate) : state(std::make_unique<State>())
{
    auto& s = *state;
    if (inputRate < 8000 || inputRate > 768000) throw std::invalid_argument("참조 오디오 입력 샘플레이트가 8000~768000Hz 범위를 벗어났습니다.");
    s.inputRate = inputRate;
    const auto* encoder = avcodec_find_encoder_by_name("aac");
    if (!encoder) throw std::runtime_error("현재 FFmpeg에 내장 AAC 인코더가 없습니다.");
    s.codec.reset(avcodec_alloc_context3(encoder)); if (!s.codec) throw std::bad_alloc();
    auto& c = *s.codec;
    c.sample_rate = 48000; c.time_base = {1, 48000}; c.sample_fmt = AV_SAMPLE_FMT_FLTP;
    c.ch_layout = AV_CHANNEL_LAYOUT_STEREO; c.bit_rate = 192000; c.profile = AV_PROFILE_AAC_LOW;
    c.flags |= AV_CODEC_FLAG_GLOBAL_HEADER; c.thread_count = 1;
    ffCheckLocalized(avcodec_open2(&c, encoder, nullptr), "내장 AAC LC 인코더를 열 수 없습니다.");
    if (c.frame_size != 1024 || !(encoder->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME)) throw std::runtime_error("고정 AAC 프레임 구성이 예상과 다릅니다.");
    for (auto* f : {s.frame.get(), s.converted.get()})
    {
        f->format = c.sample_fmt; f->sample_rate = c.sample_rate; f->nb_samples = f == s.frame.get() ? c.frame_size : 2048;
        ffCheckLocalized(av_channel_layout_copy(&f->ch_layout, &c.ch_layout), "스테레오 채널 구성을 복사할 수 없습니다.");
        ffCheckLocalized(av_frame_get_buffer(f, 0), "AAC용 PCM 프레임을 만들 수 없습니다.");
    }
    AVChannelLayout inputLayout = AV_CHANNEL_LAYOUT_STEREO;
    ffCheckLocalized(swr_alloc_set_opts2(&s.swr, &c.ch_layout, c.sample_fmt, c.sample_rate, &inputLayout, AV_SAMPLE_FMT_FLT, int(inputRate), 0, nullptr), "참조 오디오 샘플레이트 변환을 준비할 수 없습니다.");
    ffCheckLocalized(swr_init(s.swr), "참조 오디오 샘플레이트 변환을 시작할 수 없습니다.");
    s.fifo = av_audio_fifo_alloc(c.sample_fmt, 2, 4096); if (!s.fifo) throw std::bad_alloc();
}
ReferenceMixWriter::~ReferenceMixWriter() = default;
const AVCodecContext& ReferenceMixWriter::context() const { return *state->codec; }
void ReferenceMixWriter::advance(std::int64_t target, const PacketSink& sink)
{
    auto& s = *state;
    if (!s.synthetic) throw std::logic_error("Use append for real reference PCM");
    if (s.finished || target < s.requested || target > 48000LL * 86400 * 7) throw std::invalid_argument("Reference duration must increase within 7 days");
    s.requested = target;
    const auto inputEnd = av_rescale_rnd(target, 44100, 48000, AV_ROUND_UP);
    constexpr double tau = 6.2831853071795864769;
    while (s.inputSamples < inputEnd)
    {
        const int count = static_cast<int>(std::min<std::int64_t>(1024, inputEnd - s.inputSamples));
        for (int i = 0; i < count; ++i)
        {
            const auto n = s.inputSamples + i;
            const double t = static_cast<double>(n) / 44100;
            const double common = 0.12 * std::sin(tau * 1000 * t);
            s.source[static_cast<size_t>(i) * 2] = static_cast<float>(common + 0.04 * std::sin(tau * 440 * t));
            s.source[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(common + 0.04 * std::sin(tau * 660 * t));
        }
        s.inputSamples += count; s.resample(count, sink);
    }
}
void ReferenceMixWriter::finish(std::int64_t target, const PacketSink& sink)
{
    if (state->synthetic) advance(target, sink);
    else if (target != state->requested || state->finished) throw std::logic_error("참조 오디오 입력 길이가 맞지 않습니다.");
    auto& s = *state;
    while (s.resample(0, sink) > 0) {}
    // The source end was rounded up by at most one source sample. Retain exactly
    // the requested presentation samples, never pad the logical duration to AAC.
    const auto excess = s.convertedSamples - target;
    if (excess < 0 || excess > 2 || s.encodedSamples > target) throw std::runtime_error("참조 오디오의 샘플레이트 변환 후 길이가 맞지 않습니다.");
    const int validTail = static_cast<int>(target - s.encodedSamples);
    if (validTail)
    {
        ffCheckLocalized(av_frame_make_writable(s.frame.get()), "AAC 끝부분에 쓸 수 없습니다.");
        if (av_audio_fifo_read(s.fifo, reinterpret_cast<void**>(s.frame->data), validTail) != validTail) throw std::runtime_error("AAC 끝부분을 읽을 수 없습니다.");
        s.frame->nb_samples = validTail; s.frame->pts = s.encodedSamples;
        ffCheckLocalized(avcodec_send_frame(s.codec.get(), s.frame.get()), "참조 AAC의 마지막 부분 프레임을 인코딩할 수 없습니다.");
        s.encodedSamples += validTail; s.receive(sink);
    }
    ffCheckLocalized(avcodec_send_frame(s.codec.get(), nullptr), "참조 AAC 인코딩을 마무리할 수 없습니다."); s.receive(sink);
    if (s.firstPts != -s.codec->initial_padding || s.lastEnd != target) throw std::runtime_error("AAC 앞부분 보정이나 재생 길이가 맞지 않습니다.");
    s.finished = true;
}
void ReferenceMixWriter::append(const float* pcm, unsigned frames, const PacketSink& sink)
{
    auto& s = *state;
    if (s.synthetic || s.finished || !pcm || !frames) throw std::invalid_argument("추가할 참조 PCM 데이터가 올바르지 않습니다.");
    for (std::size_t i = 0; i < std::size_t(frames) * 2; ++i)
        if (!std::isfinite(pcm[i])) throw std::invalid_argument("참조 PCM에 유효하지 않은 값이 있습니다.");
    unsigned consumed = 0;
    const unsigned block = std::min(1024u, std::max(1u, s.inputRate * 1024u / 48000u));
    while (consumed < frames)
    {
        const auto count = std::min(block, frames - consumed);
        std::copy(pcm + consumed * 2, pcm + (consumed + count) * 2, s.source.begin());
        s.inputSamples += count;
        s.requested = av_rescale_rnd(s.inputSamples, 48000, s.inputRate, AV_ROUND_NEAR_INF);
        s.resample(int(count), sink); consumed += count;
    }
}
void ReferenceMixWriter::finishInput(const PacketSink& sink) { finish(state->requested, sink); }
std::int64_t ReferenceMixWriter::padding(std::int64_t valid, int size, int initial)
{
    if (valid < 0 || size <= 0 || initial < 0) throw std::invalid_argument("AAC 앞부분 보정 계산이 올바르지 않습니다.");
    return (size - ((valid % size + initial % size) % size)) % size;
}
juce::var ReferenceMixWriter::toJson() const
{
    const auto& s = *state; auto v = jsonObject();
    jsonSet(v, "source", s.synthetic ? "Synthetic 44100 Hz float stereo fixture" : "Actual take PCM at interface Fs; fixed armed-microphone mean duplicated L/R (zero microphones = silence)");
    jsonSet(v, "inputSampleRate", int(s.inputRate));
    jsonSet(v, "codec", "aac"); jsonSet(v, "profile", "LC"); jsonSet(v, "bitRate", 192000);
    jsonSet(v, "sampleRate", 48000); jsonSet(v, "channels", 2); jsonSet(v, "inputSamples", jsonInt(s.inputSamples));
    jsonSet(v, "resampledSamples", jsonInt(s.convertedSamples)); jsonSet(v, "presentationSamples", jsonInt(s.encodedSamples));
    jsonSet(v, "initialPadding", s.codec->initial_padding); jsonSet(v, "trailingPadding", jsonInt(padding(s.encodedSamples, s.codec->frame_size, s.codec->initial_padding)));
    jsonSet(v, "firstPacketPts", juce::var(static_cast<juce::int64>(s.firstPts))); jsonSet(v, "lastPacketEnd", juce::var(static_cast<juce::int64>(s.lastEnd)));
    jsonSet(v, "packets", jsonInt(s.packets)); jsonSet(v, "finished", s.finished);
    jsonSet(v, "primingPolicy", "Preserve negative encoder PTS, initial_padding and final packet duration; MP4 use_editlist=1 and avoid_negative_ts=disabled. Presentation zero verified by reopening final MP4; crashed empty_moov gapless playback is not certified.");
    return v;
}
}
