#pragma once
// Keep the round-13 document/consumer ABI. Its declarations live at the original
// include path; the sole implementation is now playback/RenderPlanCompiler.cpp.
#include "model/RenderPlanCompiler.h"

namespace gocue::recorder
{
// Immutable data-only source table, separate from worker-owned file readers.
// Asset chunks remain one logical source; spans never expose storage boundaries.
struct AudioRenderPlan
{
    std::shared_ptr<const CompiledRenderPlan> timeline;
    std::vector<MediaAsset> sources; // only assets referenced by active audio clips
};
std::shared_ptr<const AudioRenderPlan> compileAudioRenderPlan(const RecorderProject&);
}
