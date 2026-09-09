#pragma once
#include "AsioTapAbi.h"
#include "app/RecorderSettings.h"
#include "playback/PlaybackBlockQueue.h"
#include "record/ReferenceMixWriter.h"
#include "record/WavTrackWriter.h"
#include "sync/IClockMapper.h"
#include <array>
#include <memory>

namespace gocue::recorder
{
class RecorderAudioEngine
{
public:
    enum class Error { none, rawOverflow, pcmOverflow, writeFailed, invalidNative, clockDiscontinuity,
                       sampleRateChanged, asioReset, missedStart, missedStop, cancelled };
    struct DeviceInfo
    {
        juce::String name;
        unsigned sampleRate = 0, bufferFrames = 0;
        int inputLatency = 0, outputLatency = 0, physicalInputs = 0, physicalOutputs = 0;
        std::vector<int> activeToPhysical;
        bool synthetic = false;
    };
    struct TakeConfig
    {
        juce::File projectDirectory;
        juce::Uuid takeId;
        std::int64_t placementSample = 0;
        std::vector<JournalFileDescription> additionalFiles;
        std::vector<juce::String> microphoneAssetIds; // armed logical order; generated if empty
        PacketSink referencePackets; // AAC worker only; empty = no reference stream
        FileIoFaultAdapter* faults = nullptr;
    };
    explicit RecorderAudioEngine(std::shared_ptr<IClockMapper> = {});
    ~RecorderAudioEngine();
    RecorderAudioEngine(const RecorderAudioEngine&) = delete;
    static juce::StringArray deviceNames(); // control thread, ASIO only
    juce::Result openDevice(const juce::String& name, unsigned requestedFs, int requestedBuffer = 0);
    // Hardware-free adapter uses the identical native/transport/output path.
    juce::Result openSynthetic(unsigned Fs, unsigned block, int physicalInputs = 8, int physicalOutputs = 2);
    juce::Result closeDevice();
    DeviceInfo deviceInfo() const;
    // Entries are logical microphones 0..7; -1 = unselected. Unique physical IDs.
    juce::Result setInputMap(const std::array<int, 8>&);
    juce::Result setOutputMap(OutputMapping);
    juce::Result arm(unsigned mic, bool);
    std::vector<unsigned> armedMicrophones() const; // logical 1..8
    std::vector<JournalDeviceMapping> microphoneMapping() const;
    void setInputMonitoring(bool enabled, std::uint8_t selected = 0xff) noexcept;
    void setListeningState(std::uint8_t muteMask, std::uint8_t soloMask) noexcept;
    // Caller keeps queue alive through detach. Detach waits for in-flight output
    // callbacks on the control thread; the callback never deletes an object.
    void setPlaybackQueue(PlaybackBlockQueue*);
    juce::Result prepare(TakeConfig); // preparation worker/control; allocates, opens WAV + journal
    juce::Result startAt(std::int64_t sample); // queued; worker flushes TakeStarted before adoption
    juce::Result stopAt(std::int64_t sample);  // exact exclusive sample; callback adopts
    void abort(Error) noexcept; // failure flag; last callback-confirmed range is preserved
    // After Nstop, caller places metadata first. These are blocking worker APIs.
    juce::Result finishCapture(const juce::Uuid& placementEditId);
    juce::Result finishJournal(bool complete);
    const AVCodecContext* referenceContext() const; // copy codec parameters before startAt
    void pollDeviceEvents(); // control; detects reset even if no next buffer arrives
    bool clockReady() const;
    ClockMapping clockMapping() const;
    std::int64_t currentSample() const noexcept; // last callback-exclusive boundary
    std::int64_t startSample() const noexcept;   // -1 until adopted
    bool startCommitted() const noexcept;       // durable TakeStarted, eligible for callback adoption
    std::int64_t stopSample() const noexcept;    // -1 until adopted
    std::int64_t acceptedEnd() const noexcept;
    Error error() const noexcept;
    bool referenceFailed() const noexcept;
    std::array<float, 8> peaks() const noexcept;
    juce::var telemetry() const; // after finishCapture
    // Synthetic driver/test adapter only. Real ASIO invokes the native hook before
    // JUCE's float conversion and uses its matching output callback afterwards.
    void processBlock(const BlockStamp&, const NativeInputView*, unsigned count,
                      const float* const* inputs, float* const* outputs, unsigned outputCount) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
