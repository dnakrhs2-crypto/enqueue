#include "ThumbnailCache.h"
#include "record/Ffmpeg.h"
#include "support/Platform.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace gocue::recorder
{
ThumbnailCache::ThumbnailCache() : worker([this] { run(); }) {}
ThumbnailCache::~ThumbnailCache()
{
    { const std::lock_guard<std::mutex> lock(mutex); stopping = true; queue.clear(); }
    wake.notify_all(); worker.join();
}
bool ThumbnailCache::enqueue(const juce::String& key, Job job)
{
    const std::lock_guard<std::mutex> lock(mutex);
    if (stopping || keys.count(key) || queue.size() >= maximumPending) return false;
    keys.insert(key); queue.push_back({key, std::move(job)}); wake.notify_one(); return true;
}
void ThumbnailCache::setRecording(bool on) { { const std::lock_guard<std::mutex> lock(mutex); recording = on; } wake.notify_all(); }
bool ThumbnailCache::mayRun() const { const std::lock_guard<std::mutex> lock(mutex); return !recording && !stopping; }
std::size_t ThumbnailCache::pending() const { const std::lock_guard<std::mutex> lock(mutex); return queue.size(); }
void ThumbnailCache::run()
{
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    for (;;)
    {
        Request r;
        { std::unique_lock<std::mutex> lock(mutex); wake.wait(lock, [&] { return stopping || (!recording && !queue.empty()); });
          if (stopping) break; r = std::move(queue.front()); queue.pop_front(); }
        try
        {
            r.job([this]
            {
                // Preserve the unfinished strip across a new recording. Returning
                // early would leave its dedup key published with only partial data.
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return stopping || !recording; }); return stopping;
            });
        }
        catch (...) { /* Rebuildable cache failure never fails an original. */ }
        { const std::lock_guard<std::mutex> lock(mutex); keys.erase(r.key); }
    }
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}
std::vector<ThumbnailFrame> ThumbnailCache::decode(const juce::File& file, unsigned Fs, const std::function<bool()>& yield)
{
    if (file.getFileName().contains(".recording.")) return {};
    AVFormatContext* input = nullptr;
    ffCheck(avformat_open_input(&input, file.getFullPathName().toRawUTF8(), nullptr, nullptr), "Open thumbnail");
    const std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)> owner(input, [](auto* p) { avformat_close_input(&p); });
    ffCheck(avformat_find_stream_info(input, nullptr), "Read thumbnail streams");
    const auto stream = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0); ffCheck(stream, "Find thumbnail video");
    const auto* codec = avcodec_find_decoder(input->streams[stream]->codecpar->codec_id);
    const std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)> ctx(avcodec_alloc_context3(codec), [](auto* p) { avcodec_free_context(&p); });
    ffCheck(avcodec_parameters_to_context(ctx.get(), input->streams[stream]->codecpar), "Thumbnail parameters"); ctx->thread_count = 1;
    ffCheck(avcodec_open2(ctx.get(), codec, nullptr), "Thumbnail decoder");
    auto packet = ffPacket(); auto frame = ffFrame(); std::vector<ThumbnailFrame> result;
    const auto* st = input->streams[stream]; const auto duration = st->duration > 0 ? st->duration : 0;
    for (int i = 0; i < 12 && !yield(); ++i)
    {
        const auto target = (st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time) + duration * i / 12;
        if (av_seek_frame(input, stream, target, AVSEEK_FLAG_BACKWARD) < 0) break; avcodec_flush_buffers(ctx.get());
        bool found = false;
        while (!yield() && !found && av_read_frame(input, packet.get()) >= 0)
        {
            if (packet->stream_index == stream && avcodec_send_packet(ctx.get(), packet.get()) >= 0)
                while (avcodec_receive_frame(ctx.get(), frame.get()) == 0)
                {
                    if (frame->best_effort_timestamp < target) continue;
                    ThumbnailFrame thumb; thumb.sample = av_rescale_q(frame->best_effort_timestamp - (st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time), st->time_base, AVRational{1, int(Fs)});
                    thumb.rgb.resize(std::size_t(thumb.width * thumb.height * 3));
                    const std::unique_ptr<SwsContext, void(*)(SwsContext*)> scaler(sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format), thumb.width, thumb.height, AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr), sws_freeContext);
                    if (!scaler) throw std::runtime_error("Thumbnail scaler unavailable");
                    std::uint8_t* pixels[] = {thumb.rgb.data()}; int strides[] = {thumb.width * 3};
                    sws_scale(scaler.get(), frame->data, frame->linesize, 0, frame->height, pixels, strides);
                    result.push_back(std::move(thumb)); found = true; break;
                }
            av_packet_unref(packet.get());
        }
        if (!found) break;
    }
    return result;
}
}
