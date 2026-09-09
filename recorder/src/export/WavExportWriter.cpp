#include "WavExportWriter.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace gocue::recorder
{
namespace
{
void put(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t v, unsigned n)
{ for (unsigned i = 0; i < n; ++i) b.at(at + i) = static_cast<std::uint8_t>(v >> (i * 8)); }
std::uint64_t get(const std::uint8_t* b, std::size_t at, unsigned n)
{ std::uint64_t v = 0; for (unsigned i = 0; i < n; ++i) v |= std::uint64_t(b[at + i]) << (i * 8); return v; }
void tag(std::vector<std::uint8_t>& b, std::size_t at, const char* t) { std::memcpy(b.data() + at, t, 4); }
}
WavExportWriter::Header WavExportWriter::makeHeader(std::uint32_t Fs, std::uint64_t samples)
{
    exportRequire(Fs && Fs <= 768000 && samples <= (std::uint64_t((std::numeric_limits<Sample>::max)()) - 81) / 3, "WAV size/rate overflows");
    Header h; h.sampleCount = samples; h.sampleRate = Fs; h.dataBytes = samples * 3;
    h.rf64 = h.dataBytes + (h.dataBytes & 1) + 44 > 0xffffffffULL;
    h.bytes.resize(h.rf64 ? 80 : 44); h.fileBytes = h.bytes.size() + h.dataBytes + (h.dataBytes & 1);
    auto& b = h.bytes; tag(b, 0, h.rf64 ? "RF64" : "RIFF"); put(b, 4, h.rf64 ? 0xffffffffULL : h.fileBytes - 8, 4); tag(b, 8, "WAVE");
    std::size_t fmt = 12;
    if (h.rf64)
    {
        tag(b, 12, "ds64"); put(b, 16, 28, 4); put(b, 20, h.fileBytes - 8, 8);
        put(b, 28, h.dataBytes, 8); put(b, 36, samples, 8); put(b, 44, 0, 4); fmt = 48;
    }
    tag(b, fmt, "fmt "); put(b, fmt + 4, 16, 4); put(b, fmt + 8, 1, 2); put(b, fmt + 10, 1, 2);
    put(b, fmt + 12, Fs, 4); put(b, fmt + 16, std::uint64_t(Fs) * 3, 4); put(b, fmt + 20, 3, 2); put(b, fmt + 22, 24, 2);
    tag(b, fmt + 24, "data"); put(b, fmt + 28, h.rf64 ? 0xffffffffULL : h.dataBytes, 4); return h;
}
WavExportWriter::Header WavExportWriter::readHeader(const void* memory, std::size_t count)
{
    exportRequire(memory && count >= 44, "Truncated WAV header"); const auto* b = static_cast<const std::uint8_t*>(memory);
    const bool rf64 = std::memcmp(b, "RF64", 4) == 0;
    exportRequire((rf64 || std::memcmp(b, "RIFF", 4) == 0) && std::memcmp(b + 8, "WAVE", 4) == 0 && count >= (rf64 ? 80u : 44u), "Invalid WAV/RF64 header");
    const std::size_t fmt = rf64 ? 48 : 12;
    const auto data = get(b, rf64 ? 28 : 40, rf64 ? 8 : 4);
    exportRequire(data % 3 == 0, "Unaligned PCM24 payload");
    auto h = makeHeader(static_cast<std::uint32_t>(get(b, fmt + 12, 4)), data / 3);
    exportRequire(h.rf64 == rf64 && std::memcmp(b, h.bytes.data(), h.bytes.size()) == 0, "WAV/RF64 size/ds64/PCM format mismatch"); return h;
}
WavExportWriter::Header WavExportWriter::inspect(const juce::File& path)
{
    juce::FileInputStream stream(path); std::uint8_t bytes[80]{};
    exportRequire(stream.openedOk(), "Cannot reopen exported WAV"); const int read = stream.read(bytes, 80);
    auto h = readHeader(bytes, static_cast<std::size_t>((std::max)(read, 0)));
    exportRequire(path.getSize() >= 0 && static_cast<std::uint64_t>(path.getSize()) == h.fileBytes, "Exported WAV physical length mismatch");
    if (h.dataBytes & 1) { exportRequire(stream.setPosition(static_cast<juce::int64>(h.fileBytes - 1)) && stream.readByte() == 0, "Invalid WAV pad byte"); }
    return h;
}
WavExportWriter::WavExportWriter(juce::File target, std::uint32_t Fs, Sample samples, FileIoFaultAdapter* faults)
    : finalFile(std::move(target)), partial(finalFile.getSiblingFile(finalFile.getFileName() + ".partial")),
      layout(makeHeader(Fs, static_cast<std::uint64_t>(samples))), file(faults)
{
    exportRequire(samples >= 0 && finalFile.hasFileExtension("wav") && !finalFile.exists() && !partial.exists(), "WAV export output collision/invalid length");
    exportCheck(finalFile.getParentDirectory().createDirectory()); exportCheck(file.open(partial, DurableFile::OpenMode::createNew)); owns = true;
    try { exportCheck(file.write(layout.bytes.data(), layout.bytes.size())); }
    catch (...) { file.close(); partial.deleteFile(); throw; }
}
WavExportWriter::~WavExportWriter()
{ if (owns && !finished && !published) { file.close(); partial.deleteFile(); } }
void WavExportWriter::append(const float* mono, unsigned frames)
{
    exportRequire(!finished && !published && mono && std::uint64_t(frames) <= layout.sampleCount - std::uint64_t(written), "Invalid WAV append/too many samples");
    std::vector<std::uint8_t> pcm(std::size_t(frames) * 3);
    for (unsigned i = 0; i < frames; ++i)
    {
        exportRequire(std::isfinite(mono[i]), "Non-finite exported PCM");
        const auto q = static_cast<std::int32_t>(std::llround((std::max)(-1.0, (std::min)(double(mono[i]), 8388607.0 / 8388608.0)) * 8388608.0));
        const auto u = static_cast<std::uint32_t>(q);
        for (unsigned j = 0; j < 3; ++j) pcm[std::size_t(i) * 3 + j] = static_cast<std::uint8_t>(u >> (j * 8));
    }
    exportCheck(file.write(pcm.data(), pcm.size())); written += frames;
}
void WavExportWriter::finish()
{
    exportRequire(!finished && !published && std::uint64_t(written) == layout.sampleCount, "Incomplete/already finished WAV export");
    if (layout.dataBytes & 1) { const std::uint8_t zero = 0; exportCheck(file.write(&zero, 1)); }
    // Rewriting the final header also exercises the same durable header fault path
    // as recording. No logical length is inferred from partially written payload.
    exportCheck(file.writeAt(0, layout.bytes.data(), layout.bytes.size()));
    exportCheck(file.flushData()); exportCheck(file.close()); inspect(partial); finished = true;
}
void WavExportWriter::publish()
{ exportRequire(finished && !published, "WAV must be verified before publish"); exportRename(partial, finalFile); published = true; }
void WavExportWriter::cancel()
{
    exportCheck(file.close());
    if (owns && !published) exportRequire(!partial.exists() || partial.deleteFile(), "Cannot remove owned partial WAV");
    owns = false;
}
}
