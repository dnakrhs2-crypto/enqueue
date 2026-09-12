#pragma once
#include "playback/ImportedAudioCache.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::recorder
{
// Shared import command for the main window and dubbing. All document/UI callbacks
// run on the owner thread; the worker owns copying, validation and cache generation.
class AudioImportPanel final : public juce::Component, private juce::Timer
{
public:
    explicit AudioImportPanel(RecorderDocument&);
    ~AudioImportPanel() override;
    void setImportContext(juce::File projectDirectory, Sample playhead = 0);
    void setRecordingActive(bool);
    void chooseFile();
    void importFile(const juce::File&); // also available to drag/drop and host commands
    void cancelImport();
    void shutdown(); // cancel and join before the document/project can go away
    bool isImporting() const { return worker != nullptr; }
    bool isBusy() const { return worker != nullptr || chooser != nullptr; }
    void setCompact(bool value) { compact = value; resized(); }
    void resized() override;
    std::function<void(const MediaAsset&, const CachedImportedAudio&)> onImported;
    std::function<void(const juce::String&)> onStatusChanged;
    std::function<void()> onBusyChanged;
private:
    friend struct ImportUiTestAccess;
    void report(const juce::String&);
    void timerCallback() override;
    RecorderDocument& document;
    juce::File directory;
    Sample playhead = 0;
    bool recording = false, compact = false;
    std::function<juce::File()> chooseFileForTesting; // replace only the native picker in headless UI tests
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
