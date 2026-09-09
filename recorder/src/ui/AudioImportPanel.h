#pragma once
#include "playback/ImportedAudioCache.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::recorder
{
// Standalone dubbing component. Round 12 supplies project folder/playhead/recording state
// and consumes onImported to refresh the timeline and its waveform cache.
class AudioImportPanel final : public juce::Component, private juce::Timer
{
public:
    explicit AudioImportPanel(RecorderDocument&);
    ~AudioImportPanel() override;
    void setImportContext(juce::File projectDirectory, Sample playhead = 0);
    void setRecordingActive(bool);
    void importFile(const juce::File&); // also available to drag/drop and host commands
    void cancelImport();
    bool isImporting() const { return worker != nullptr; }
    void resized() override;
    std::function<void(const MediaAsset&, const CachedImportedAudio&)> onImported;
private:
    void chooseFile();
    void timerCallback() override;
    RecorderDocument& document;
    juce::File directory;
    Sample playhead = 0;
    bool recording = false;
    double progress = 0;
    juce::TextButton importButton{juce::String::fromUTF8("오디오 파일 불러오기")};
    juce::TextButton cancelButton{juce::String::fromUTF8("취소")};
    juce::Label status;
    juce::ProgressBar progressBar{progress};
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<ImportedAudioCache::Worker> worker;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioImportPanel)
};
}
