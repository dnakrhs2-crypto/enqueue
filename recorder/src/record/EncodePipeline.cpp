#include "EncodePipeline.h"
#include "Mp4TakeWriter.h"
#include "ReferenceMixWriter.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>

namespace gocue::recorder
{
namespace
{
struct Trace
{
    std::int64_t pts = 0, mappedTime = 0;
    FrameStamp source;
    bool repeated = false;
};
struct VideoPackets
{
    struct Item { PacketPtr packet = ffPacket(); Trace trace; };
    std::array<Item, 180> items;
    const unsigned limit;
    const std::uint64_t byteLimit;
    std::atomic<std::uint64_t> read{0}, written{0}, bytes{0}, maxBytes{0}, maxCount{0};
    VideoPackets(unsigned fps, std::uint64_t maxRate) : limit(fps * 3), byteLimit(maxRate * 3 / 8) {}
    bool push(const AVPacket& packet, Trace trace)
    {
        const auto w = written.load(), r = read.load(std::memory_order_acquire);
        if (w - r >= limit || packet.size < 0 || bytes.load() + packet.size > byteLimit) return false;
        auto& item = items[w % limit];
        ffCheck(av_packet_ref(item.packet.get(), &packet), "Queue encoded packet reference"); item.trace = trace;
        const auto b = bytes.fetch_add(packet.size) + packet.size;
        maxBytes.store(std::max(maxBytes.load(), b)); maxCount.store(std::max(maxCount.load(), w - r + 1));
        written.store(w + 1, std::memory_order_release); return true;
    }
    Item* peek()
    {
        const auto r = read.load(); return r == written.load(std::memory_order_acquire) ? nullptr : &items[r % limit];
    }
    void release()
    {
        auto& item = items[read.load() % limit]; bytes.fetch_sub(item.packet->size); av_packet_unref(item.packet.get());
        read.fetch_add(1, std::memory_order_release);
    }
};
struct Event
{
    HANDLE value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Event() { if (!value) checkHr(HRESULT_FROM_WIN32(GetLastError()), "Create recording worker event"); }
    ~Event() { CloseHandle(value); }
    void signal() noexcept { SetEvent(value); }
    void wait() { if (WaitForSingleObject(value, 2) == WAIT_FAILED) checkHr(HRESULT_FROM_WIN32(GetLastError()), "Wait recording worker"); }
};
}
struct EncodePipeline::State
{
    NvencProfile profile;
    Rational native;
    unsigned seconds;
    juce::File output;
    std::shared_ptr<CaptureTelemetry> capture;
    std::unique_ptr<CameraTimeMapper> mapper;
    NvencFramePool surfaces;
    VideoPackets packets;
    VideoCfrScheduler scheduler;
    Event framesReady, packetsReady;
    std::thread encodeThread, muxThread;
    std::atomic<bool> stopping{false}, encodeDone{false}, muxFailed{false};
    std::atomic<std::uint64_t> surfaceLoss{0}, packetLoss{0};
    std::atomic<std::int64_t> originQpc{0}, end100ns{0};
    juce::var encoderReport, muxReport, audioReport, inspection;
    std::string encodeError, muxError;
    std::uint64_t traceCount = 0, traceHash = 14695981039346656037ULL;
    juce::File traceFile;
    State(NvencProfile p, Rational rate, unsigned duration, juce::File f, std::shared_ptr<CaptureTelemetry> t, std::unique_ptr<CameraTimeMapper> m)
        : profile(std::move(p)), native(rate), seconds(duration), output(std::move(f)), capture(std::move(t)),
          mapper(m ? std::move(m) : std::make_unique<MfPtsTimeMapper>(qpcFrequency())), surfaces(profile.cpuSurfaces()),
          packets(profile.fps, profile.maxRate()), scheduler(native, Rational{static_cast<unsigned>(profile.fps), 1}),
          traceFile(output.getSiblingFile(output.getFileNameWithoutExtension() + ".source-ids.csv"))
    {
        profile.validate(); if (!seconds || seconds > 86400 * 7) throw std::invalid_argument("Encode seconds must be 1..604800");
        end100ns.store(static_cast<std::int64_t>(seconds) * 10000000);
    }
    void mux(const AVCodecContext& codec, std::promise<void> prepared)
    {
        bool signalled = false;
        std::unique_ptr<Mp4TakeWriter> writer;
        std::unique_ptr<ReferenceMixWriter> reference;
        try
        {
            reference = std::make_unique<ReferenceMixWriter>();
            if (traceFile.exists()) throw std::runtime_error("Source-ID CSV already exists; use a fresh output directory");
            writer = std::make_unique<Mp4TakeWriter>(output, codec, reference->context());
            std::ofstream trace(std::filesystem::path(traceFile.getFullPathName().toWideCharPointer()), std::ios::binary);
            trace.exceptions(std::ios::badbit | std::ios::failbit);
            trace << "outputPts,sourceId,mfPts100ns,callbackQpc,mapped100ns,repeated\n";
            const PacketSink audioSink = [&](const AVPacket& p) { writer->audio(p); };
            prepared.set_value(); signalled = true;
            for (;;)
            {
                if (auto* item = packets.peek())
                {
                    const auto& t = item->trace;
                    reference->advance(std::min<std::int64_t>((t.pts + 1) * 48000 / profile.fps, end100ns.load() * 48000 / 10000000), audioSink);
                    writer->video(*item->packet);
                    trace << t.pts << ',' << t.source.frame << ',' << t.source.pts100ns << ',' << t.source.callback << ',' << t.mappedTime << ',' << (t.repeated ? 1 : 0) << '\n';
                    ++traceCount; traceHash = (traceHash ^ t.source.frame) * 1099511628211ULL;
                    packets.release();
                }
                else if (encodeDone.load(std::memory_order_acquire)) break;
                else packetsReady.wait();
            }
            trace.flush(); trace.close();
            if (encodeError.empty() && !surfaceLoss.load() && !packetLoss.load() && traceCount)
            {
                reference->finish(end100ns.load() * 48000 / 10000000, audioSink);
                writer->finalize(); inspection = Mp4TakeWriter::inspect(output);
                if (!static_cast<bool>(inspection["presentationStartsAtZero"])) throw std::runtime_error("Final MP4 presentation does not start at zero");
            }
        }
        catch (const std::exception& e)
        {
            muxError = e.what(); muxFailed.store(true);
            if (!signalled) prepared.set_exception(std::current_exception());
        }
        if (writer) muxReport = writer->toJson();
        if (reference) audioReport = reference->toJson();
    }
    void encode(std::promise<void> prepared)
    {
        bool signalled = false;
        std::unique_ptr<NvencEncoder> encoder;
        std::deque<Trace> pendingTraces;
        try
        {
            encoder = std::make_unique<NvencEncoder>(profile); encoder->open();
            std::promise<void> muxReady; auto future = muxReady.get_future();
            muxThread = std::thread(&State::mux, this, std::cref(encoder->context()), std::move(muxReady)); future.get();
            prepared.set_value(); signalled = true;
            const PacketSink sink = [&](const AVPacket& packet)
            {
                if (pendingTraces.empty() || pendingTraces.front().pts != packet.pts) throw std::runtime_error("Source ID/encoded PTS association failed");
                if (muxFailed.load()) throw std::runtime_error("Mux worker failed; preview remains independent");
                if (!packets.push(packet, pendingTraces.front())) { ++packetLoss; throw std::runtime_error("Three-second video packet queue overflow"); }
                pendingTraces.pop_front(); packetsReady.signal();
            };
            std::uint64_t seenCaptureLoss = 0, seenEncodeLoss = 0;
            bool stopApplied = false;
            const auto consumeFrames = [&]
            {
                int index = -1;
                while (surfaces.pop(index))
                {
                    const auto& stamp = surfaces.stamp(index);
                    try
                    {
                        const auto time = mapper->map(stamp);
                        if (!originQpc.load()) originQpc.store(stamp.callback);
                        scheduler.push({stamp.frame, time, index});
                    }
                    catch (...) { surfaces.release(index); throw; }
                }
                const auto knownCapture = capture->count(LossReason::captureDecodeOverflow) + capture->count(LossReason::lateQueueDiscard)
                    + capture->count(LossReason::decoderError) + capture->count(LossReason::surfacePoolExhausted);
                const auto knownEncode = surfaceLoss.load();
                scheduler.noteLoss(CfrReason::captureLoss, knownCapture - seenCaptureLoss); seenCaptureLoss = knownCapture;
                scheduler.noteLoss(CfrReason::encodeLoss, knownEncode - seenEncodeLoss); seenEncodeLoss = knownEncode;
            };
            for (;;)
            {
                if (muxFailed.load()) throw std::runtime_error("Mux worker failed");
                consumeFrames();
                const auto now = mapper->now(qpcNow());
                if (stopping.load() && !stopApplied)
                {
                    end100ns.store(std::min(end100ns.load(), now)); stopApplied = true;
                }
                const auto target = VideoCfrScheduler::frameCount(end100ns.load(), Rational{static_cast<unsigned>(profile.fps), 1});
                while (scheduler.nextPts() < target)
                {
                    // Incorporate arrivals during an encode backlog before deciding
                    // that the next grid lacks a valid input frame.
                    consumeFrames();
                    auto selection = scheduler.select(mapper->now(qpcNow()), stopApplied);
                    if (!selection) break;
                    for (size_t i = 0; i < selection->releasedCount; ++i) surfaces.release(selection->released[i]);
                    pendingTraces.push_back({selection->pts, selection->input.time100ns, surfaces.stamp(selection->input.slot), selection->repeated});
                    encoder->submit(surfaces.frame(selection->input.slot), selection->pts, sink);
                }
                if (scheduler.nextPts() >= target || (stopApplied && !scheduler.size())) break;
                framesReady.wait();
            }
            encoder->drain(sink);
        }
        catch (const std::exception& e)
        {
            encodeError = e.what(); if (!signalled) prepared.set_exception(std::current_exception());
        }
        if (encoder) encoderReport = encoder->toJson();
        size_t count = 0; const auto released = scheduler.releaseAll(count);
        for (size_t i = 0; i < count; ++i) surfaces.release(released[i]);
        encodeDone.store(true, std::memory_order_release); packetsReady.signal();
        // Codec parameters were copied at mux preparation, but keep its context
        // alive until the mux worker exits on both startup failure and normal stop.
        if (muxThread.joinable()) muxThread.join();
    }
};
EncodePipeline::EncodePipeline(NvencProfile p, Rational n, unsigned seconds, juce::File out, std::shared_ptr<CaptureTelemetry> t, std::unique_ptr<CameraTimeMapper> m)
    : state(std::make_unique<State>(std::move(p), n, seconds, std::move(out), std::move(t), std::move(m))) {}
EncodePipeline::~EncodePipeline() { stop(); }
void EncodePipeline::start()
{
    if (state->encodeThread.joinable()) throw std::logic_error("Encode pipeline is single-use");
    std::promise<void> ready; auto future = ready.get_future();
    state->encodeThread = std::thread(&State::encode, state.get(), std::move(ready)); future.get();
}
void EncodePipeline::offer(const VideoSurface& frame) noexcept
{
    if (state->stopping.load() || state->encodeDone.load()) return;
    if (!state->surfaces.copy(frame)) state->surfaceLoss.fetch_add(1);
    state->framesReady.signal();
}
void EncodePipeline::stop()
{
    state->stopping.store(true); state->framesReady.signal();
    if (state->encodeThread.joinable()) state->encodeThread.join();
    int slot = -1; while (state->surfaces.pop(slot)) state->surfaces.release(slot);
}
double EncodePipeline::secondsSinceOrigin() const noexcept
{
    const auto origin = state->originQpc.load(); return origin ? static_cast<double>(qpcNow() - origin) / qpcFrequency() : -1;
}
juce::var EncodePipeline::toJson() const
{
    const auto& s = *state; auto value = jsonObject();
    jsonSet(value, "encoder", s.encoderReport); jsonSet(value, "cfr", s.scheduler.counters().toJson());
    jsonSet(value, "mux", s.muxReport); jsonSet(value, "referenceAudio", s.audioReport); jsonSet(value, "finalInspection", s.inspection);
    jsonSet(value, "surfaceQueueHighWater", static_cast<int>(s.surfaces.highWater()));
    jsonSet(value, "surfaceQueueCapacity", s.surfaces.capacity()); jsonSet(value, "surfaceBudgetIncludingNvenc", s.profile.surfaceLimit());
    jsonSet(value, "surfaceQueueRemaining", static_cast<int>(s.surfaces.occupied()));
    jsonSet(value, "packetQueueCapacity", static_cast<int>(s.packets.limit)); jsonSet(value, "packetQueueByteLimit", jsonInt(s.packets.byteLimit));
    jsonSet(value, "packetQueueHighWater", jsonInt(s.packets.maxCount.load())); jsonSet(value, "packetQueueBytesHighWater", jsonInt(s.packets.maxBytes.load()));
    jsonSet(value, "encodeLoss", jsonInt(s.surfaceLoss.load() + s.packetLoss.load()));
    jsonSet(value, "surfaceOverflow", jsonInt(s.surfaceLoss.load())); jsonSet(value, "packetOverflow", jsonInt(s.packetLoss.load()));
    jsonSet(value, "encodeError", s.encodeError); jsonSet(value, "muxError", s.muxError);
    jsonSet(value, "sourceIdCsv", s.traceFile.getFullPathName()); jsonSet(value, "sourceIdRows", jsonInt(s.traceCount));
    jsonSet(value, "sourceIdFnv64", juce::String::toHexString(static_cast<juce::int64>(s.traceHash)));
    jsonSet(value, "clockMapping", "Initial MF PTS relative to first retained frame; first arrival QPC anchors deadlines only. Replaceable CameraTimeMapper; no ASIO, drift calibration or exposure-time claim.");
    jsonSet(value, "logicalDuration100ns", juce::var(static_cast<juce::int64>(s.end100ns.load())));
    jsonSet(value, "complete", s.encodeError.empty() && s.muxError.empty() && !s.surfaceLoss.load() && !s.packetLoss.load() && static_cast<bool>(s.muxReport["finalized"]));
    return value;
}
juce::var EncodePipeline::headroom(NvencProfile profile, unsigned seconds)
{
    profile.validate(); if (!seconds || seconds > 3600) throw std::invalid_argument("Headroom wall seconds must be 1..3600");
    NvencEncoder encoder(profile); encoder.open();
    NvencFramePool bank(profile.cpuSurfaces()); VideoSurface surface; surface.prepare(1920, 1080);
    std::array<int, 15> slots{};
    for (int phase = 0; phase < bank.capacity(); ++phase)
    {
        for (unsigned y = 0; y < surface.height; ++y)
            for (unsigned x = 0; x < surface.width; ++x)
                surface.y()[static_cast<size_t>(y) * surface.width + x] = static_cast<uint8_t>(16 + ((x / 8 + y / 8 + phase * 11) % 220));
        for (size_t i = 0; i < surface.nv12.size() / 3; i += 2)
        { surface.uv()[i] = static_cast<uint8_t>(48 + (phase * 13 + i / 128) % 160); surface.uv()[i + 1] = static_cast<uint8_t>(208 - (phase * 7 + i / 256) % 160); }
        if (!bank.copy(surface) || !bank.pop(slots[static_cast<size_t>(phase)])) throw std::runtime_error("Synthetic prepared bank failed");
    }
    std::uint64_t bytes = 0, frames = 0;
    const PacketSink sink = [&](const AVPacket& p) { bytes += p.size; };
    const auto start = qpcNow(), deadline = start + static_cast<std::int64_t>(seconds) * qpcFrequency();
    while (qpcNow() < deadline)
    {
        encoder.submit(bank.frame(slots[frames % bank.capacity()]), static_cast<std::int64_t>(frames), sink); ++frames;
    }
    encoder.drain(sink);
    const double elapsed = static_cast<double>(qpcNow() - start) / qpcFrequency();
    const auto fps = static_cast<double>(frames) / elapsed; auto value = encoder.toJson();
    jsonSet(value, "secondsMeasured", elapsed); jsonSet(value, "encodeOnlyFps", fps); jsonSet(value, "goalFps", 156.0);
    jsonSet(value, "goalMet", fps >= 156); jsonSet(value, "headroomRatioTo120Fps", fps / 120.0); jsonSet(value, "bytesDiscarded", jsonInt(bytes));
    jsonSet(value, "definition", "One NVENC session, prebuilt changing NV12 pattern bank, maximum-speed submit/receive and final drain. Wall-time seconds, no capture/preview/CFR/PCM/AAC/mux/disk. Content-specific throughput; two-session capture+preview+8ch approval remains separate.");
    return value;
}
}
