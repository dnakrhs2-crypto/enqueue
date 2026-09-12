#include "CrashFixtures.h"
#include "storage/StorageEncoding.h"
#include <array>
#include <chrono>
#include <cstring>
#include <thread>

namespace gocue::recorder::crashFixture
{
using namespace recovery;
juce::File directory(const char* label)
{
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("RecorderR07-" + juce::String(label) + "-" + juce::Uuid().toString());
    check(root.createDirectory()); return root;
}
std::int32_t pcm(std::uint64_t sample, unsigned mic)
{
    // Stateless coordinate oracle; every recovered sample can be checked across chunks/gaps.
    const auto bits = std::uint32_t((sample * 104729u + (mic + 1) * 13007u) & 0xffffffu);
    return bits & 0x800000u ? std::int32_t(bits) - 16777216 : std::int32_t(bits);
}
WavTrackWriter::Config wavConfig(const juce::File& root, unsigned mics)
{
    WavTrackWriter::Config c; c.projectDirectory = root; c.mics = mics; c.framesPerBlock = 1600;
    for (unsigned i = 0; i < mics; ++i) c.devices.push_back({"r07-synthetic", "Mic " + juce::String(i + 1), int(i + 1), int(i), int(i)});
    return c;
}
void push(WavTrackWriter& writer, std::uint64_t first, std::uint32_t frames, unsigned mics)
{
    std::vector<std::int32_t> data(std::size_t(frames) * mics);
    for (std::uint32_t f = 0; f < frames; ++f) for (unsigned m = 0; m < mics; ++m) data[std::size_t(f) * mics + m] = pcm(first + f, m);
    require(writer.tryPush(data.data(), frames, first), "Synthetic WAV queue rejected block: " + writer.status().getErrorMessage());
}
Camera::Camera(const juce::File& file, FileIoFaultAdapter* fault, recovery::Hook hook)
{
    const auto* encoder = avcodec_find_encoder_by_name("libopenh264"); require(encoder != nullptr, "Pinned SDK libopenh264 required for CPU synthetic harness");
    codec.reset(avcodec_alloc_context3(encoder)); require(codec != nullptr, "Allocate synthetic H264");
    codec->width = 1920; codec->height = 1080; codec->pix_fmt = AV_PIX_FMT_YUV420P; codec->time_base = {1, 30}; codec->framerate = {30, 1};
    codec->bit_rate = 1000000; codec->gop_size = 30; codec->max_b_frames = 0; codec->thread_count = 1; codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    codec->color_range = AVCOL_RANGE_MPEG; codec->colorspace = AVCOL_SPC_BT709; codec->color_primaries = AVCOL_PRI_BT709; codec->color_trc = AVCOL_TRC_BT709;
    ffCheck(avcodec_open2(codec.get(), encoder, nullptr), "Open synthetic CPU H264");
    picture->format = codec->pix_fmt; picture->width = codec->width; picture->height = codec->height;
    ffCheck(av_frame_get_buffer(picture.get(), 32), "Allocate synthetic picture");
    mux = std::make_unique<Mp4TakeWriter>(file, *codec, audio.context(), fault, std::move(hook));
}
void Camera::receive()
{
    auto packet = ffPacket();
    for (;;)
    {
        const auto code = avcodec_receive_packet(codec.get(), packet.get()); if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) break;
        ffCheck(code, "Synthetic H264 packet"); if (packet->duration == 0) packet->duration = 1; mux->video(*packet); av_packet_unref(packet.get());
    }
}
void Camera::frame()
{
    ffCheck(av_frame_make_writable(picture.get()), "Synthetic picture writable");
    for (int y = 0; y < 1080; ++y) std::memset(picture->data[0] + y * picture->linesize[0], int(32 + count % 160), 1920);
    for (int y = 0; y < 540; ++y) for (int plane = 1; plane < 3; ++plane) std::memset(picture->data[plane] + y * picture->linesize[plane], 128, 960);
    picture->pts = count; picture->duration = 1; picture->pict_type = count % 30 == 0 ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
    audio.advance((count + 1) * 1600, [&](const auto& p) { mux->audio(p); }); ffCheck(avcodec_send_frame(codec.get(), picture.get()), "Encode synthetic frame"); receive(); ++count;
}
void Camera::finish()
{
    ffCheck(avcodec_send_frame(codec.get(), nullptr), "Drain synthetic H264"); receive();
    audio.finish(samples(), [&](const auto& p) { mux->audio(p); }); mux->finalize();
}
RecorderProject baseline(const juce::File& root)
{
    RecorderProject project; RecoveryScanner::writeCheckpoint(root.getChildFile("project.recorder"), project);
    const auto takeId = juce::Uuid(); const auto cameraId = juce::Uuid();
    const auto cameraPath = "media/takes/" + takeId.toDashedString() + "/cam1.mp4";
    Camera camera(root.getChildFile(cameraPath)); for (int i = 0; i < 30; ++i) camera.frame(); camera.finish();
    MediaAsset asset; asset.assetId = cameraId.toString(); asset.relativePath = cameraPath; asset.logicalLength = 48000; asset.availableRanges = {{0, 48000}};
    asset.contentIdentity = sha256(root.getChildFile(cameraPath)); asset.originalFormat.codec = "h264"; asset.originalFormat.width = 1920; asset.originalFormat.height = 1080;
    asset.sourceUnitsNumerator = 30; asset.sourceUnitsDenominator = 48000;
    Take take; take.takeId = takeId.toString(); take.number = 6; take.createdAt = "2026-09-09T00:00:00Z"; take.name = "baseline-complete";
    take.cam1AssetId = asset.assetId; take.logicalLength = 48000; take.state = TakeState::complete;
    RecorderDocument doc; CheckpointInfo info; check(doc.adopt(project, root.getChildFile("project.recorder"), info)); check(doc.placeTake(take, {asset}));
    project = doc.getProject(); RecoveryScanner::writeTakeManifest(root, project, take.takeId); RecoveryScanner::writeCheckpoint(root.getChildFile("project.recorder"), project);
    // One acknowledged edit exists only in the edit journal (after checkpoint).
    TicketSink sink; doc.setJournalSink(&sink);
    check(doc.performEdit("durable marker", [](EditState& edit) { Marker marker; marker.sample = 24000; marker.name = "saved-before-crash"; edit.markers.push_back(marker); }));
    RecoveryScanner::appendEdit(root.getChildFile("journal/edits-000001.log"), sink.ticket, doc.getProject());
    return doc.getProject();
}
std::map<juce::String, juce::String> hashes(const juce::File& root, bool originalsOnly)
{
    std::map<juce::String, juce::String> result;
    for (const auto& f : root.findChildFiles(juce::File::findFiles, true))
    {
        const auto path = f.getRelativePathFrom(root).replaceCharacter('\\', '/');
        if (path.endsWith(".lock") || (originalsOnly && path.startsWith("recovery/"))) continue;
        result.emplace(path, sha256(f));
    }
    return result;
}
bool verifyPcm(const juce::File& root, const MediaAsset& a, unsigned mic)
{
    for (const auto& chunk : a.chunks)
    {
        juce::FileInputStream in(root.getChildFile(chunk.relativePath)); if (in.failedToOpen()) return false;
        std::array<std::uint8_t, 44> h{}; if (in.read(h.data(), 44) != 44 || storageEncoding::get<std::uint32_t>(h.data() + 40) != std::uint64_t(chunk.sourceRange.length) * 3) return false;
        std::array<std::uint8_t, 3 * 4096> bytes{};
        for (Sample read = 0; read < chunk.sourceRange.length;)
        {
            const int n = int(std::min<Sample>(4096, chunk.sourceRange.length - read)); if (in.read(bytes.data(), n * 3) != n * 3) return false;
            for (int i = 0; i < n; ++i)
            {
                const auto* p = bytes.data() + i * 3; const auto bits = std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16);
                if (bits != (std::uint32_t(pcm(std::uint64_t(chunk.sourceRange.start + read + i), mic)) & 0xffffffu)) return false;
            }
            read += n;
        }
    }
    return true;
}
juce::var largeFaultChecks(const juce::File& root)
{
    struct Fault : FileIoFaultAdapter
    {
        bool full = false, crossed = false; std::uint64_t maximum = 0;
        std::uint64_t observedOffset(std::uint64_t p) const override { return p + (1ull << 32) - 16; }
        juce::Result beforeIo(FileIoOperation op, const juce::File&, std::uint64_t offset, std::size_t bytes) override
        {
            maximum = std::max(maximum, offset); if (offset < (1ull << 32) && bytes > (1ull << 32) - offset) crossed = true;
            return full && op == FileIoOperation::append ? juce::Result::fail("Injected ERROR_DISK_FULL (Win32 112)") : juce::Result::ok();
        }
    } fault;
    check(root.createDirectory()); DurableFile file(&fault); check(file.open(root.getChildFile("virtual-boundary.bin"), DurableFile::OpenMode::createNew));
    std::array<std::uint8_t, 32> bytes{}; check(file.write(bytes.data(), bytes.size())); check(file.flushData());
    require(fault.crossed && fault.maximum >= (1ull << 32) && file.writtenBytes() == 32, "Virtual 4GiB offset narrowed or real file extended");
    fault.full = true; const auto failed = file.write(bytes.data(), bytes.size()); require(failed.failed() && failed.getErrorMessage().contains("112") && file.durableBytes() == 32, "Disk-full error/watermark propagation");
    require(file.close().failed() && root.getChildFile("virtual-boundary.bin").getSize() == 32, "Disk-full changed real file");
    JournalFilePosition pos; pos.validBytes = (1ull << 32) + 44; pos.validSamples = (1ull << 32) / 3;
    require(RecoveryScanner::wavSamples((1ull << 32) + 43, pos) == ((1ull << 32) - 1) / 3, "64-bit recovery byte arithmetic");
    auto report = object(); set(report, "status", "PASS"); set(report, "adapter", "Virtual observation offsets; native file remains 32 bytes. Injected ERROR_DISK_FULL, no disk filling.");
    set(report, "maximumVirtualOffset", integer(std::int64_t(fault.maximum))); set(report, "actualBytes", 32); set(report, "diskFull", failed.getErrorMessage()); return report;
}
}
