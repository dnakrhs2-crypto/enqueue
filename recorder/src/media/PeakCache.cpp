#include "PeakCache.h"
#include "model/SafeFileWrite.h"
#include <algorithm>
#include <cmath>

namespace gocue::recorder
{
PeakCache::PeakCache(unsigned Fs, unsigned channels, unsigned width)
{
    if (!Fs || !channels || channels > 16) throw std::invalid_argument("Invalid peak dimensions");
    state.sampleRate = Fs; state.channels = channels; state.samplesPerBin = width ? width : std::max(1u, Fs / 100);
    state.bins.reserve(maximumBins);
}
void PeakCache::append(const std::int32_t* pcm, unsigned frames, std::uint64_t first)
{
    const std::lock_guard<std::mutex> lock(mutex);
    if (!pcm || state.complete || first != state.samples) throw std::invalid_argument("Noncontiguous peak input");
    for (unsigned i = 0; i < frames; ++i)
    {
        if (state.samples % state.samplesPerBin == 0)
        {
            if (state.bins.size() == maximumBins)
            {
                for (std::size_t b = 0; b < maximumBins / 2; ++b)
                    for (unsigned c = 0; c < state.channels; ++c)
                        state.bins[b][c] = {std::min(state.bins[b * 2][c].minimum, state.bins[b * 2 + 1][c].minimum),
                                            std::max(state.bins[b * 2][c].maximum, state.bins[b * 2 + 1][c].maximum)};
                state.bins.resize(maximumBins / 2); state.samplesPerBin *= 2;
            }
            state.bins.push_back({});
            for (unsigned c = 0; c < state.channels; ++c)
            { const auto value = float(pcm[std::size_t(i) * state.channels + c]) / 8388608.0f; state.bins.back()[c] = {value, value}; }
        }
        for (unsigned c = 0; c < state.channels; ++c)
        {
            const auto value = float(pcm[std::size_t(i) * state.channels + c]) / 8388608.0f;
            auto& b = state.bins.back()[c]; b.minimum = std::min(b.minimum, value); b.maximum = std::max(b.maximum, value);
        }
        ++state.samples;
    }
}
void PeakCache::finish() { const std::lock_guard<std::mutex> lock(mutex); state.complete = true; }
PeakSnapshot PeakCache::snapshot() const { const std::lock_guard<std::mutex> lock(mutex); return state; }
juce::Result PeakCache::write(const juce::File& file, const PeakSnapshot& s)
{
    auto* o = new juce::DynamicObject(); juce::var root(o);
    o->setProperty("version", 1); o->setProperty("rate", int(s.sampleRate)); o->setProperty("channels", int(s.channels));
    o->setProperty("width", juce::int64(s.samplesPerBin)); o->setProperty("samples", juce::int64(s.samples)); o->setProperty("complete", s.complete);
    juce::Array<juce::var> bins;
    for (const auto& b : s.bins) { juce::Array<juce::var> v; for (unsigned c = 0; c < s.channels; ++c) { v.add(b[c].minimum); v.add(b[c].maximum); } bins.add(v); }
    o->setProperty("bins", bins); return gocue::SafeFileWrite::writeTextVerified(file, juce::JSON::toString(root, true));
}
PeakSnapshot PeakCache::read(const juce::File& file)
{
    if (file.getSize() > 16 * 1024 * 1024) throw std::invalid_argument("Peak cache too large");
    const auto v = juce::JSON::parse(file); PeakSnapshot s;
    s.sampleRate = unsigned(int(v["rate"])); s.channels = unsigned(int(v["channels"]));
    s.samplesPerBin = std::uint64_t(juce::int64(v["width"])); s.samples = std::uint64_t(juce::int64(v["samples"])); s.complete = bool(v["complete"]);
    auto* bins = v["bins"].getArray();
    if (int(v["version"]) != 1 || !s.sampleRate || s.sampleRate > 768000 || !s.channels || s.channels > 16 || !s.samplesPerBin
        || s.samplesPerBin > (1ull << 40) || s.samples > (1ull << 52) || !bins || bins->size() > int(maximumBins)
        || std::uint64_t(bins->size()) != (s.samples + s.samplesPerBin - 1) / s.samplesPerBin) throw std::invalid_argument("Invalid peak cache");
    for (const auto& values : *bins)
    {
        auto* row = values.getArray(); if (!row || row->size() != int(s.channels * 2)) throw std::invalid_argument("Invalid peak row");
        std::array<PeakBin, 16> b{};
        for (unsigned c = 0; c < s.channels; ++c)
        {
            b[c] = {float((*row)[int(c * 2)]), float((*row)[int(c * 2 + 1)])};
            if (!std::isfinite(b[c].minimum) || !std::isfinite(b[c].maximum) || b[c].minimum < -1 || b[c].maximum > 1 || b[c].minimum > b[c].maximum)
                throw std::invalid_argument("Invalid peak extrema");
        }
        s.bins.push_back(b);
    }
    return s;
}
}
