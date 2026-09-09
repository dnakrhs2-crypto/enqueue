#include "FramePatternSource.h"
#include "support/BoundedSpscQueue.h"
#include "support/ThreadPriority.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace gocue::recorder::probe
{
namespace
{
std::uint8_t checksum(std::uint64_t bits)
{
    std::uint8_t crc = 0;
    for (unsigned byte = 1; byte < 8; ++byte)
    {
        crc ^= static_cast<std::uint8_t>(bits >> (byte * 8));
        for (int bit = 0; bit < 8; ++bit) crc = static_cast<std::uint8_t>((crc << 1) ^ ((crc & 128) ? 7 : 0));
    }
    return crc;
}
void rectangle(VideoSurface& s, unsigned x, unsigned y, unsigned w, unsigned h, std::uint8_t luma)
{
    for (unsigned row = y; row < std::min(s.height, y + h); ++row)
        std::memset(s.y() + static_cast<std::size_t>(row) * s.width + x, luma, std::min(w, s.width - x));
}
struct InputClose { void operator()(AVFormatContext* p) const { avformat_close_input(&p); } };
}
void paintPattern(VideoSurface& s, PatternId id)
{
    if (s.width < 320 || s.height < 192 || !id.frame || id.camera < 1 || id.camera > 2)
        throw std::invalid_argument("Pattern requires >=320x192 and camera 1/2, positive uint32 frame");
    const std::uint8_t bars[]{32, 64, 96, 128, 160, 192, 224, 112};
    for (unsigned y = 0; y < s.height; ++y)
        for (unsigned b = 0; b < 8; ++b)
        {
            const auto left = b * s.width / 8, right = (b + 1) * s.width / 8;
            std::memset(s.y() + static_cast<std::size_t>(y) * s.width + left, bars[b], right - left);
        }
    for (std::size_t i = 0; i < s.nv12.size() / 3; i += 2)
    { s.uv()[i] = static_cast<std::uint8_t>(80 + id.camera * 24); s.uv()[i + 1] = static_cast<std::uint8_t>(176 - id.camera * 16); }
    const auto stripe = static_cast<unsigned>((static_cast<std::uint64_t>(id.frame) * 13) % (s.width - 16));
    rectangle(s, stripe, s.height / 2, 16, s.height / 2, 235);
    rectangle(s, 16, 16, 288, 160, 16);
    // Keep barcode and digits chroma-neutral even through colour normalisation.
    for (unsigned y = 8; y < 88; ++y) std::memset(s.uv() + static_cast<std::size_t>(y) * s.width + 16, 128, 288);
    auto bits = (0xd5c3ULL << 48) | (static_cast<std::uint64_t>(id.camera) << 40) | (static_cast<std::uint64_t>(id.frame) << 8);
    bits |= checksum(bits);
    for (unsigned b = 0; b < 64; ++b) rectangle(s, 32 + (b % 16) * 16, 32 + (b / 16) * 16, 16, 16, (bits >> b) & 1 ? 235 : 16);
    // Human-readable decimal frame number, seven-segment digits.
    const unsigned digits[]{0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f};
    const auto number = std::to_string(id.frame);
    for (std::size_t i = 0; i < number.size(); ++i)
    {
        const unsigned x = 32 + static_cast<unsigned>(i) * 24, y = 112, mask = digits[number[i] - '0'];
        const unsigned segments[][4]{{4,0,12,4},{16,4,4,12},{16,20,4,12},{4,32,12,4},{0,20,4,12},{0,4,4,12},{4,16,12,4}};
        for (unsigned bit = 0; bit < 7; ++bit) if (mask & (1u << bit)) rectangle(s, x + segments[bit][0], y + segments[bit][1], segments[bit][2], segments[bit][3], 235);
    }
}
std::optional<PatternId> readPattern(const std::uint8_t* y, int stride, int width, int height)
{
    if (!y || width < 320 || height < 192 || stride < width) return {};
    std::uint64_t bits = 0;
    for (unsigned b = 0; b < 64; ++b)
    {
        unsigned sum = 0;
        for (unsigned row = 4; row < 12; ++row) for (unsigned col = 4; col < 12; ++col)
            sum += y[static_cast<std::size_t>(32 + (b / 16) * 16 + row) * stride + 32 + (b % 16) * 16 + col];
        const auto mean = sum / 64;
        if (mean > 80 && mean < 176) return {};
        if (mean >= 176) bits |= 1ULL << b;
    }
    if ((bits >> 48) != 0xd5c3 || static_cast<std::uint8_t>(bits) != checksum(bits)) return {};
    PatternId id{static_cast<std::uint32_t>(bits >> 8), static_cast<unsigned>((bits >> 40) & 255)};
    return id.frame && id.camera >= 1 && id.camera <= 2 ? std::optional<PatternId>(id) : std::nullopt;
}
FramePatternOracle::FramePatternOracle(unsigned c, Rational n, Rational p, unsigned seconds) : camera(c), native(n), project(p)
{
    if (c < 1 || c > 2 || !seconds) throw std::invalid_argument("Invalid oracle configuration");
    nativeCount = VideoCfrScheduler::frameCount(static_cast<std::int64_t>(seconds) * 10000000, n);
    outputCount = VideoCfrScheduler::frameCount(static_cast<std::int64_t>(seconds) * 10000000, p);
    if (nativeCount > 216000 || outputCount > 216000) throw std::invalid_argument("Pixel oracle is bounded to one hour at 60fps");
    requiredIds.resize(static_cast<std::size_t>(nativeCount + 1)); observedIds.resize(requiredIds.size());
    for (std::uint64_t i = 0; i < outputCount; ++i) requiredIds[expectedSource(i)] = true;
}
std::uint64_t FramePatternOracle::expectedSource(std::uint64_t index) const
{ return std::min(nativeCount, static_cast<std::uint64_t>(VideoCfrScheduler::nearestNativeIndex(static_cast<std::int64_t>(index), native, project)) + 1); }
void FramePatternOracle::observe(std::optional<PatternId> id, std::optional<std::int64_t> pts)
{
    const auto expected = expectedSource(decoded);
    if (!id || id->camera != camera || id->frame != expected) ++badPositions;
    const bool bad = !id || id->camera != camera || id->frame != expected || (pts && *pts != static_cast<std::int64_t>(decoded));
    if (!id) ++invalid;
    else
    {
        if (id->camera != camera) ++wrongCamera;
        else if (id->frame <= nativeCount) observedIds[id->frame] = true;
        if (id->frame != expected) ++mismatches;
        if (previous == id->frame) ++repeated;
        if (previous > id->frame) ++regressions;
        previous = id->frame;
    }
    if (pts && *pts != static_cast<std::int64_t>(decoded)) ++ptsErrors;
    if (bad && firstErrors.size() < 32)
    {
        auto e = jsonObject(); jsonSet(e, "outputIndex", decoded); jsonSet(e, "expectedSourceId", expected);
        jsonSet(e, "decodedSourceId", id ? juce::var(static_cast<juce::int64>(id->frame)) : juce::var());
        jsonSet(e, "decodedCamera", id ? id->camera : 0); firstErrors.add(e);
    }
    ++decoded;
}
juce::var FramePatternOracle::finish() const
{
    auto v = jsonObject(); const auto missing = outputCount > decoded ? outputCount - decoded : 0;
    std::uint64_t loss = 0;
    for (std::size_t id = 1; id < requiredIds.size(); ++id) if (requiredIds[id] && !observedIds[id]) ++loss;
    const bool pass = decoded == outputCount && !loss && !badPositions && !ptsErrors && !regressions;
    jsonSet(v, "result", pass ? "PASS" : "FAIL"); jsonSet(v, "captureLoss", loss);
    jsonSet(v, "expectedOutputFrames", outputCount); jsonSet(v, "decodedFrames", decoded); jsonSet(v, "missingOutputFrames", missing);
    jsonSet(v, "badOutputPositions", badPositions);
    jsonSet(v, "invalidPixelIds", invalid); jsonSet(v, "wrongCamera", wrongCamera); jsonSet(v, "unexpectedSourceAtCfrGrid", mismatches);
    jsonSet(v, "ptsErrors", ptsErrors); jsonSet(v, "regressions", regressions); jsonSet(v, "observedRepeats", repeated);
    jsonSet(v, "intentionalNativeOmissions", nativeCount > outputCount ? nativeCount - outputCount : 0);
    jsonSet(v, "firstErrors", firstErrors);
    jsonSet(v, "definition", "Full decoded MP4 pixels vs independent nominal native/project rational grid (older at ties, tail clamped). captureLoss counts distinct required native IDs absent from the decoded output, not sensor-only loss. badOutputPositions separately counts invalid/wrong-camera/off-grid pixels; PTS errors also fail. Intentional CFR repeats/omissions are allowed. Bounded offline ID bitsets, no duration-dependent live queue.");
    jsonSet(v, "coverage", native.value() > project.value() ? "CFR-selected native frames only; omitted native frames cannot be certified from this MP4" : "All native IDs expected in the MP4, including first and last; synthetic source only");
    return v;
}
juce::var inspectPatternMp4(const juce::File& file, unsigned camera, Rational native, Rational project, unsigned seconds)
{
    ScopedRecorderPriority priority(RecorderThreadRole::background);
    AVFormatContext* raw = nullptr;
    ffCheck(avformat_open_input(&raw, file.getFullPathName().toRawUTF8(), nullptr, nullptr), "Open pixel-oracle MP4");
    std::unique_ptr<AVFormatContext, InputClose> input(raw);
    ffCheck(avformat_find_stream_info(raw, nullptr), "Find pixel-oracle streams");
    const int stream = av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0); ffCheck(stream, "Find oracle video");
    const auto* codec = avcodec_find_decoder(raw->streams[stream]->codecpar->codec_id);
    if (!codec) throw std::runtime_error("Oracle decoder unavailable");
    CodecPtr decoder(avcodec_alloc_context3(codec)); if (!decoder) throw std::bad_alloc();
    ffCheck(avcodec_parameters_to_context(decoder.get(), raw->streams[stream]->codecpar), "Copy oracle codec parameters");
    decoder->thread_count = 1; decoder->err_recognition = AV_EF_EXPLODE;
    ffCheck(avcodec_open2(decoder.get(), codec, nullptr), "Open software pixel decoder");
    auto packet = ffPacket(); auto frame = ffFrame(); FramePatternOracle oracle(camera, native, project, seconds);
    const auto receive = [&](bool draining)
    {
        for (;;)
        {
            const int result = avcodec_receive_frame(decoder.get(), frame.get());
            if (result == AVERROR_EOF) return;
            if (result == AVERROR(EAGAIN)) { if (draining) throw std::runtime_error("Oracle flush did not reach EOF"); return; }
            ffCheck(result, "Decode pixel oracle");
            if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P && frame->format != AV_PIX_FMT_NV12)
                throw std::runtime_error("Oracle requires decoded 8-bit YUV");
            if (frame->best_effort_timestamp == AV_NOPTS_VALUE) throw std::runtime_error("Oracle frame has no PTS");
            const auto pts = av_rescale_q(frame->best_effort_timestamp, raw->streams[stream]->time_base, AVRational{static_cast<int>(project.denominator), static_cast<int>(project.numerator)});
            oracle.observe(readPattern(frame->data[0], frame->linesize[0], frame->width, frame->height), pts); av_frame_unref(frame.get());
        }
    };
    for (;;)
    {
        const int result = av_read_frame(raw, packet.get()); if (result == AVERROR_EOF) break; ffCheck(result, "Read oracle packet");
        if (packet->stream_index == stream)
        {
            int sent = avcodec_send_packet(decoder.get(), packet.get());
            if (sent == AVERROR(EAGAIN)) { receive(false); sent = avcodec_send_packet(decoder.get(), packet.get()); }
            ffCheck(sent, "Submit oracle packet"); receive(false);
        }
        av_packet_unref(packet.get());
    }
    ffCheck(avcodec_send_packet(decoder.get(), nullptr), "Flush oracle decoder"); receive(true);
    auto result = oracle.finish(); jsonSet(result, "file", file.getFullPathName()); jsonSet(result, "priorityError", priority.error); return result;
}

struct FramePatternSource::State
{
    Config config;
    CameraMode mode;
    VideoSurfacePool& preview;
    std::shared_ptr<CaptureTelemetry> telemetry;
    std::function<void(const VideoSurface&)> sink;
    VideoSurface scratch;
    CodecPtr jpeg;
    FramePtr jpegFrame = ffFrame(); PacketPtr jpegPacket = ffPacket();
    struct Slot { std::vector<std::uint8_t> bytes; FrameStamp stamp; };
    std::array<Slot, 4> slots; // producer + two pending + one decode owner
    std::array<std::atomic<bool>, 4> owned{};
    BoundedSpscQueue<int, 2> queue;
    std::thread producer, worker;
    std::atomic<bool> stopping{false}, producerDone{false}, workerDone{false};
    std::atomic<bool> degraded{false};
    std::uint64_t skipped = 0, generated = 0, workerIdGaps = 0, lastWorkerId = 0;
    std::string producerError, workerError;
    DWORD producerPriorityError = 0;
    State(Config c, VideoSurfacePool& p, std::shared_ptr<CaptureTelemetry> t, std::function<void(const VideoSurface&)> s)
        : config(c), preview(p), telemetry(std::move(t)), sink(std::move(s))
    {
        if (c.camera < 1 || c.camera > 2 || (c.fps != 30 && c.fps != 60) || !c.seconds || c.seconds > 3600
            || (c.subtype != CaptureSubtype::nv12 && c.subtype != CaptureSubtype::mjpeg)) throw std::invalid_argument("Invalid synthetic camera configuration");
        mode.width = 1920; mode.height = 1080; mode.fps = {c.fps, 1}; mode.subtype = c.subtype; mode.stride = 1920;
        mode.interlace = MFVideoInterlace_Progressive;
        mode.colour = {MFVideoTransferMatrix_BT709, MFNominalRange_16_235, MFVideoPrimaries_BT709, MFVideoTransFunc_709};
        scratch.prepare(mode.width, mode.height);
        for (auto& slot : slots) slot.bytes.reserve(scratch.nv12.size() * 2);
        if (c.subtype == CaptureSubtype::mjpeg)
        {
            const auto* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG); if (!codec) throw std::runtime_error("MJPEG pattern encoder unavailable");
            jpeg.reset(avcodec_alloc_context3(codec)); if (!jpeg) throw std::bad_alloc();
            jpeg->width = 1920; jpeg->height = 1080; jpeg->pix_fmt = AV_PIX_FMT_YUVJ420P;
            jpeg->time_base = {1, static_cast<int>(c.fps)}; jpeg->thread_count = 1; jpeg->flags |= AV_CODEC_FLAG_QSCALE;
            jpeg->global_quality = 3 * FF_QP2LAMBDA; jpeg->color_range = AVCOL_RANGE_JPEG; jpeg->colorspace = AVCOL_SPC_BT709;
            ffCheck(avcodec_open2(jpeg.get(), codec, nullptr), "Open synthetic MJPEG encoder");
            jpegFrame->width = 1920; jpegFrame->height = 1080; jpegFrame->format = jpeg->pix_fmt;
            jpegFrame->quality = jpeg->global_quality;
            ffCheck(av_frame_get_buffer(jpegFrame.get(), 32), "Allocate synthetic JPEG surface");
            mode.colour.range = MFNominalRange_0_255;
        }
    }
    void make(std::uint32_t number, std::vector<std::uint8_t>& bytes)
    {
        paintPattern(scratch, {number, config.camera});
        if (!jpeg) { bytes.assign(scratch.nv12.begin(), scratch.nv12.end()); return; }
        ffCheck(av_frame_make_writable(jpegFrame.get()), "Prepare source JPEG frame");
        // Source image is generated in full-range JPEG code values. Colour
        // normalisation still runs through the production decoder afterwards.
        for (unsigned y = 0; y < 1080; ++y) std::memcpy(jpegFrame->data[0] + y * jpegFrame->linesize[0], scratch.y() + y * 1920, 1920);
        for (unsigned y = 0; y < 540; ++y) for (unsigned x = 0; x < 960; ++x)
        {
            jpegFrame->data[1][y * jpegFrame->linesize[1] + x] = scratch.uv()[y * 1920 + x * 2];
            jpegFrame->data[2][y * jpegFrame->linesize[2] + x] = scratch.uv()[y * 1920 + x * 2 + 1];
        }
        jpegFrame->pts = number - 1; ffCheck(avcodec_send_frame(jpeg.get(), jpegFrame.get()), "Generate source JPEG");
        ffCheck(avcodec_receive_packet(jpeg.get(), jpegPacket.get()), "Receive source JPEG");
        if (static_cast<std::size_t>(jpegPacket->size) > scratch.nv12.size() * 2) throw std::runtime_error("Synthetic JPEG exceeded prepared sample cap");
        bytes.assign(jpegPacket->data, jpegPacket->data + jpegPacket->size); av_packet_unref(jpegPacket.get());
    }
    void produce(std::shared_future<void> start, const std::atomic<std::int64_t>& origin)
    {
        ScopedRecorderPriority priority(RecorderThreadRole::capturePreview); producerPriorityError = priority.error;
        try
        {
            while (!stopping && start.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {}
            const auto zero = origin.load(), frequency = qpcFrequency();
            const std::uint64_t count = static_cast<std::uint64_t>(config.seconds) * config.fps;
            for (std::uint64_t index = 0; index < count && !stopping; ++index)
            {
                const auto due = zero + static_cast<std::int64_t>(index) * frequency / config.fps;
                while (!stopping && qpcNow() < due) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (stopping) break;
                const auto current = static_cast<std::uint64_t>(std::max<std::int64_t>(0, qpcNow() - zero)) * config.fps / frequency;
                if (current > index) { const auto next = std::min(current, count); skipped += next - index; degraded = true; index = next; if (index >= count) break; }
                int slot = -1;
                for (int i = 0; i < 4; ++i) { bool free = false; if (owned[i].compare_exchange_strong(free, true)) { slot = i; break; } }
                if (slot < 0) { telemetry->loss(LossReason::captureDecodeOverflow); continue; }
                auto& item = slots[slot]; make(static_cast<std::uint32_t>(index + 1), item.bytes); ++generated;
                auto& stamp = item.stamp; stamp = {}; stamp.frame = index + 1; stamp.generation = config.generation;
                stamp.pts100ns = VideoCfrScheduler::gridTime(static_cast<std::int64_t>(index), mode.fps);
                const auto originalQpc = zero + static_cast<std::int64_t>(index) * frequency / config.fps;
                stamp.deviceTimestamp100ns = static_cast<std::uint64_t>(originalQpc / frequency * 10000000 + originalQpc % frequency * 10000000 / frequency);
                stamp.hasDeviceTimestamp = stamp.deviceTimestampValid = true; stamp.callback = stamp.enqueued = qpcNow();
                ++telemetry->callbacks; ++telemetry->samples;
                std::int64_t unset = 0; telemetry->firstCallbackQpc.compare_exchange_strong(unset, stamp.callback);
                const auto queued = queue.producerSize() + 1;
                if (!queue.push(slot)) { owned[slot] = false; telemetry->loss(LossReason::captureDecodeOverflow); }
                else telemetry->queueHighWater.store(std::max<std::uint64_t>(telemetry->queueHighWater.load(), queued));
            }
        }
        catch (const std::exception& e) { producerError = e.what(); degraded = true; telemetry->loss(LossReason::sourceError); }
        producerDone = true;
    }
    void decode(std::promise<void> ready)
    {
        ScopedRecorderPriority priority(RecorderThreadRole::capturePreview); telemetry->capturePriorityError = priority.error;
        bool announced = false;
        try
        {
            CaptureFrameDecoder decoder(mode, 1, config.colourDevice); CaptureDecodeRecovery recovery;
            // Prepare colour conversion before measurement using the same path.
            std::vector<std::uint8_t> warm; make(1, warm); FrameStamp warmStamp;
            decoder.decodeBytes(warm.data(), warm.size(), scratch, warmStamp); decoder.flush();
            ready.set_value(); announced = true;
            for (;;)
            {
                int slot = -1;
                if (!queue.pop(slot)) { if (producerDone) break; std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
                auto& input = slots[slot]; auto stamp = input.stamp; stamp.worker = qpcNow();
                if (telemetry->ms(stamp.worker - stamp.enqueued) > 2 * mode.fps.periodMs())
                { owned[slot] = false; telemetry->loss(LossReason::lateQueueDiscard); continue; }
                const auto target = preview.acquireWrite();
                if (target == VideoSurfacePool::none) { owned[slot] = false; telemetry->loss(LossReason::surfacePoolExhausted); continue; }
                bool ok = false;
                try { ok = recovery.frame(decoder, *telemetry, [&] { decoder.decodeBytes(input.bytes.data(), input.bytes.size(), preview.surface(target), stamp); }); }
                catch (...) { owned[slot] = false; preview.release(target); throw; }
                owned[slot] = false;
                if (!ok) { preview.release(target); continue; }
                auto& output = preview.surface(target); output.stamp = stamp;
                workerIdGaps += stamp.frame - lastWorkerId - 1; lastWorkerId = stamp.frame;
                telemetry->recordWorker(stamp); ++telemetry->decoded;
                if (output.colourAssumed) ++telemetry->colourAssumptions;
                try { if (sink) sink(output); } catch (...) { preview.release(target); throw; }
                if (preview.publish(target)) telemetry->loss(LossReason::previewMailboxOverwrite);
                telemetry->latestReadyFrame = stamp.frame;
            }
        }
        catch (const std::exception& e) { workerError = e.what(); degraded = true; stopping = true; if (!announced) ready.set_exception(std::current_exception()); }
        workerDone = true;
    }
};
FramePatternSource::FramePatternSource(Config c, VideoSurfacePool& p, std::shared_ptr<CaptureTelemetry> t, std::function<void(const VideoSurface&)> s)
    : state(std::make_unique<State>(c, p, std::move(t), std::move(s))) {}
FramePatternSource::~FramePatternSource() { stop(); }
void FramePatternSource::start(std::shared_future<void> gate, const std::atomic<std::int64_t>& origin)
{
    if (!gate.valid() || state->worker.joinable()) throw std::logic_error("Synthetic source needs one start gate");
    std::promise<void> ready; auto prepared = ready.get_future();
    state->worker = std::thread(&State::decode, state.get(), std::move(ready));
    try { prepared.get(); state->producer = std::thread(&State::produce, state.get(), gate, std::cref(origin)); }
    catch (...) { state->producerDone = true; throw; }
}
void FramePatternSource::stop()
{
    state->stopping = true;
    if (state->producer.joinable()) state->producer.join(); else state->producerDone = true;
    if (state->worker.joinable()) state->worker.join();
}
bool FramePatternSource::finished() const noexcept { return state->workerDone.load(); }
bool FramePatternSource::failed() const noexcept { return state->degraded.load(); }
juce::var FramePatternSource::toJson() const
{
    auto v = jsonObject(); jsonSet(v, "sourceKind", "synthetic"); jsonSet(v, "pipelineId", state->telemetry->pipelineId);
    jsonSet(v, "mode", CameraCatalog::modeJson(state->mode)); jsonSet(v, "generatedFrames", state->generated);
    jsonSet(v, "sourceGenerationSkipped", state->skipped); jsonSet(v, "decodedInputIdGaps", state->workerIdGaps);
    jsonSet(v, "producerError", state->producerError); jsonSet(v, "workerError", state->workerError);
    jsonSet(v, "producerPriorityError", state->producerPriorityError);
    jsonSet(v, "definition", "Absolute QPC pacing; nominal source PTS/device QPC retained, actual callback QPC after pattern generation. Two pending samples, separate producer/decode slots. Missed deadlines skip IDs, never accelerate catch-up. FFmpeg CPU JPEG generation cost belongs to the synthetic load, not a camera."); return v;
}
CameraMode FramePatternSource::mode() const { return state->mode; }
void FramePatternSource::makePacket(std::uint32_t number, std::vector<std::uint8_t>& bytes) { state->make(number, bytes); }
}
