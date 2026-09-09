#pragma once
#include "diagnostics/MfJpegRangeProbe.h"

// Measured 2026-09-09, Windows 10.0.26200, FFmpeg n8.1.2-51-g7ba069f4f1-20260908.
// Direct MJPEG Decoder MFT {CB17E772-E1CC-4633-8450-5617AF577905}, 1920x1080 and
// 640x480, YUVJ420P input -> NV12. Both sizes gave these exact 16x16 means.
// Output attributes: matrix=0 (unknown), range=1 (0..255). No physical camera.
inline const std::vector<gocue::recorder::YuvValue> mfJpegFull601Fixture{
    {0,128,128}, {16,128,128}, {128,128,128}, {235,128,128}, {255,128,128},
    {97,102,205}, {141,77,64}, {68,205,116}, {158,154,51}, {112,179,192}, {187,51,140}
};
