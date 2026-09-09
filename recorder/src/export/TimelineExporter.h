#pragma once
#include "ExportJob.h"

namespace gocue::recorder
{
// Worker-only bounded PCM. No seek/start/end transport ramp; every call evaluates
// the original absolute timeline's shared microfade envelopes.
class ExportAudioRenderer
{
public:
    ExportAudioRenderer(const ExportJob&, std::vector<AudioSourceBinding>, AudioSourceMask);
    void render(Sample outputOffset, unsigned frames, float* left, float* right) const;
private:
    const ExportJob& job;
    TimelineAudioRenderer renderer;
};
class TimelineExporter
{
public:
    static std::vector<AudioSourceBinding> openSources(const ExportJob&, const AudioSourceMask&, ExportControl&);
    // Stereo optional imports are delivered as two mono PCM24 files, preserving
    // both channels without an implicit downmix. Mic materials are always mono.
    static juce::var audioMaterials(const ExportJob&, ExportControl&, bool includeImports = false, FileIoFaultAdapter* = nullptr);
    static juce::Array<juce::var> assetIds(const ExportJob&, const AudioSourceMask&);
};
}
