#pragma once
#include "RecorderModel.h"

namespace gocue::recorder
{
// YouTube chapter lines, sorted by sample then name; UTF-8 encoding is the caller's job.
juce::String markerExportText(const RecorderProject&);
}
