#pragma once
#include "ExportJob.h"

namespace gocue::recorder
{
class WavExportWriter
{
public:
    struct Header
    {
        std::vector<std::uint8_t> bytes;
        std::uint64_t sampleCount = 0, dataBytes = 0, fileBytes = 0;
        std::uint32_t sampleRate = 0;
        unsigned channels = 1;
        bool rf64 = false;
    };
    // Known common length chooses RIFF/RF64 before any payload write. The public
    // header seam validates virtual >4GiB lengths without pretending to write PCM.
    static Header makeHeader(std::uint32_t Fs, std::uint64_t samples, unsigned channels = 1);
    static Header readHeader(const void*, std::size_t);
    static Header inspect(const juce::File&); // also verifies physical file length
    WavExportWriter(juce::File finalFile, std::uint32_t Fs, Sample samples, FileIoFaultAdapter* = nullptr, unsigned channels = 1);
    ~WavExportWriter();
    WavExportWriter(const WavExportWriter&) = delete;
    void append(const float* interleaved, unsigned frames); // layout.channels samples per frame
    void appendStereo(const float* left, const float* right, unsigned frames);
    void finish(); // exact count, durable flush, close, structural verification
    void publish(); // standalone no-replace .partial -> final; finish required
    void cancel();
    const juce::File& partialFile() const { return partial; }
    const Header& header() const { return layout; }
    Sample writtenSamples() const { return written; }
private:
    juce::File finalFile, partial;
    Header layout;
    DurableFile file;
    Sample written = 0;
    bool owns = false, finished = false, published = false;
};
}
