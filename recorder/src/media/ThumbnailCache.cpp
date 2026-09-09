#include "ThumbnailCache.h"
#include "record/Ffmpeg.h"
#include "support/Platform.h"
#include <algorithm>
extern "C" {
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace gocue::recorder
{
ThumbnailCache::ThumbnailCache(Decoder decode) : decoder(std::move(decode)), worker([this] { run(); }) {}
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
bool ThumbnailCache::request(const juce::String& key, const juce::File& file, unsigned Fs, std::int64_t sample, bool priority)
{
    if (key.isEmpty() || !Fs || sample < 0 || file.getFileName().containsIgnoreCase(".recording.")) return false;
    const auto jobKey = key + "@" + juce::String(sample);
    const std::lock_guard<std::mutex> lock(mutex);
    if (stopping || cache.count(jobKey) || keys.count(jobKey)) return false;
    if (queue.size() >= maximumPending)
    {
        if (!priority) return false;
        keys.erase(queue.back().key); queue.pop_back();
    }
    const auto gen = generation;
    Job job = [this, key, jobKey, file, Fs, sample, gen](const auto& checkpoint)
    {
        const auto cancelled = [&]
        {
            if (checkpoint()) return true;
            const std::lock_guard<std::mutex> guard(mutex); return gen != generation;
        };
        const auto frames = decoder ? decoder(file, Fs, {sample}, cancelled) : decodePoints(file, Fs, {sample}, cancelled);
        const std::lock_guard<std::mutex> guard(mutex);
        if (gen != generation || stopping) { ++counters.stale; return; }
        for (const auto& frame : frames)
        {
            if (frame.width <= 0 || frame.height <= 0 || frame.width > 640 || frame.height > 360
                || frame.rgb.size() != std::size_t(frame.width) * frame.height * 3 || frame.rgb.size() > maximumBytes) continue;
            while (!cache.empty() && counters.bytes + frame.rgb.size() > maximumBytes)
            {
                const auto oldest = std::min_element(cache.begin(), cache.end(), [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
                counters.bytes -= oldest->second.frame->rgb.size(); cache.erase(oldest); ++counters.evictions;
            }
            auto value = std::make_shared<const ThumbnailFrame>(frame);
            cache.emplace(jobKey, Cached{key, sample, std::move(value), ++access}); counters.bytes += frame.rgb.size();
            counters.frames = cache.size(); counters.peakBytes = (std::max)(counters.peakBytes, counters.bytes); break;
        }
    };
    keys.insert(jobKey);
    if (priority) queue.push_front({jobKey, std::move(job)}); else queue.push_back({jobKey, std::move(job)});
    wake.notify_one(); return true;
}
std::shared_ptr<const ThumbnailFrame> ThumbnailCache::nearest(const juce::String& key, std::int64_t sample)
{
    const std::lock_guard<std::mutex> lock(mutex);
    Cached* best = nullptr;
    for (auto& item : cache) if (item.second.sourceKey == key
        && (!best || std::abs(item.second.requested - sample) < std::abs(best->requested - sample))) best = &item.second;
    if (!best) { ++counters.misses; return {}; }
    ++counters.hits; best->used = ++access; return best->frame;
}
void ThumbnailCache::invalidate()
{
    const std::lock_guard<std::mutex> lock(mutex); ++generation;
    for (const auto& r : queue) keys.erase(r.key);
    queue.clear(); cache.clear(); counters.bytes = counters.frames = 0;
    wake.notify_all();
}
ThumbnailCache::Stats ThumbnailCache::stats() const
{ const std::lock_guard<std::mutex> lock(mutex); auto s = counters; s.pending = queue.size(); return s; }
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
{ return decodePoints(file, Fs, {}, yield); }
std::vector<ThumbnailFrame> ThumbnailCache::decodePoints(const juce::File& file, unsigned Fs,
    const std::vector<std::int64_t>& samples, const std::function<bool()>& yield)
{
    if (!Fs || Fs > 768000 || file.getFileName().containsIgnoreCase(".recording.") || yield()) return {};
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
    const auto count = samples.empty() ? std::size_t{12} : (std::min)(samples.size(), maximumPending);
    for (std::size_t i = 0; i < count && !yield(); ++i)
    {
        const auto target = (st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time)
            + (samples.empty() ? duration * std::int64_t(i) / 12 : av_rescale_q(samples[i], AVRational{1, int(Fs)}, st->time_base));
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
