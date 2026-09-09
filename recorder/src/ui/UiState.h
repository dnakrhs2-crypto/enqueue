#pragma once
#include "app/RecorderDocument.h"
#include "app/RecorderSettings.h"
#include "audio/RecorderAudioEngine.h"
#include "record/TakeController.h"
#include "capture/CameraCatalog.h"

namespace gocue::recorder
{
struct RecorderUiState
{
    bool live = false, structureLocked = false, canRecord = false, canStop = false, canTransport = false;
    bool camera2Off = true, timebaseFixed = false;
    unsigned armedMicrophones = 0;
    juce::String warning, takeStatus;
};
RecorderUiState mapUiState(const RecorderProject&, const UserSettings&, TakeController::State,
                          bool documentLocked, bool preparingDevices, bool asioReady, bool camera1Ready);
juce::Result validateAudioSettings(const UserSettings&, const RecorderAudioEngine::DeviceInfo&, const RecorderProject&);
juce::Result validateCameraSettings(const UserSettings&, const std::vector<CameraDevice>&);
juce::String formatRecorderTime(Sample, unsigned Fs);
}
