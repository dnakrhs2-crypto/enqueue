#pragma once
#include "VideoSurfacePool.h"
#include <memory>

namespace gocue::recorder
{
enum class ColourDevice { hardware, warpForTests };
// Dedicated decode-worker D3D11 context, never shares the present context.
// CPU NV12 upload -> compute matrix/range conversion -> CPU NV12 readback.
// The default MF 709-limited/assumed path skips this stage entirely.
class GpuColourConverter
{
public:
    GpuColourConverter(unsigned width, unsigned height, ColourDevice);
    ~GpuColourConverter();
    void convert(VideoSurface&, bool matrix601, bool fullRange);
private:
    struct State;
    std::unique_ptr<State> state;
};
}
