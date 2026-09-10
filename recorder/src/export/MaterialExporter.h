#pragma once
#include "FinalVideoExporter.h"

namespace gocue::recorder
{
struct MaterialExportOptions
{
    bool includeImports = false;
    // The app may supply its selected completed source. Older projects have no
    // persisted default-source field: a dub with several imports must choose.
    std::optional<AudioSourceMask> referenceAudio;
};
struct MaterialOutput
{
    juce::String name;
    std::optional<TrackKind> camera;
    AudioSourceMask audio;
    int sourceChannel = 0; // -1 = interleaved stereo slot
    unsigned channels = 1;
};
class MaterialExporter
{
public:
    static std::vector<TrackKind> cameras(const RecorderProject&);
    static AudioSourceMask referenceAudio(const ExportJob&, const MaterialExportOptions&);
    static std::vector<MaterialOutput> outputs(const ExportJob&, const MaterialExportOptions&);
    // CPU tests may provide a real software mux/verify adapter. The default is
    // always the production D3D11VA/NVENC exporter, with no codec fallback.
    using CameraRenderer = std::function<juce::var(const ExportJob&, const FinalExportSelection&, ExportControl&, FileIoFaultAdapter*)>;
    static juce::var run(const ExportJob&, const MaterialExportOptions&, ExportControl&,
                         FileIoFaultAdapter* = nullptr, const CameraRenderer& = {});
};
}
