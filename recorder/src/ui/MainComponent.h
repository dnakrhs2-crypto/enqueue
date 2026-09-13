#pragma once
#include "RecordView.h"
#include "TimelineView.h"
#include "AudioSettingsPanel.h"
#include "CameraSettingsPanel.h"
#include "AudioImportPanel.h"
#include "app/RecorderSession.h"
#include "app/RecorderPowerMonitor.h"
#include <optional>

namespace gocue::recorder
{
class ExportDialog;
class MainComponent : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer, private juce::KeyListener, private juce::FocusChangeListener
{
public:
    MainComponent(RecorderDocument&, RecorderSettings&, TakeController::VideoFactory = {});
    ~MainComponent() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray&, int, int) override;
    void showError(const juce::String&);
    void showUnhandledException(const juce::File& report);
    bool routeShortcut(const juce::KeyPress&, juce::Component* origin);
    void showFault(RecorderFault fault) { showError(recorderFaultText(fault)); }
    void openProject(const juce::File&);
    void initialiseProject(const juce::File& explicitPath, bool promptIfMissing, bool connectDevices = true);
    void requestClose(std::function<void()>);
    void createProject(const juce::String&, const juce::File&, unsigned fps);
    // timelineMode (diagnostic): "" = record tab; "gap" = from the second take, record in the timeline tab 5 s past the end;
    // "overwrite" = 3 s inside the last take (its audio is heard while recording, the overlap is replaced).
    void startDemo(int iterations, const juce::File& devices, int asioDevice, const juce::File& report, const juce::String& timelineMode = {});
    std::shared_ptr<RecorderLifecycle> lifecycleState() const { return session.lifecycleState(); }
    void updateShutdownRequested();
    void updateShutdownBlocked();
    static juce::String updateBlockedText(std::uint32_t lifecycleFlags, bool captureBusy); // names what has to finish first
    void checkForUpdates();
    void connectDevicesFromSettings() { if (!demo && devicesEnabled) session.configure(settings.get()); }
private:
    friend struct StabilityTestAccess;
    friend struct ShortcutExceptionTestAccess;
    friend struct ImportUiTestAccess;
    struct FileResult
    {
        juce::Result result = juce::Result::ok(); bool opening = false, recovered = false;
        juce::File file; RecorderProject loaded; CheckpointInfo info; RecorderDocument::Snapshot written;
    };
    struct Demo
    {
        enum class Step { opening, configuring, ready, recording, waitingPlayback, showingPlayback, finished } step = Step::opening;
        int iterations = 20, iteration = 0, asioIndex = -1, returnCode = 1;
        juce::String timelineMode; bool timelineArmed = false; Sample expectedPlacement = -1; Id previousCam1Asset;
        juce::File devices, report, folder;
        std::int64_t phaseQpc = qpcNow(), stopQpc = 0, clipQpc = 0;
        juce::Array<juce::var> rows;
        std::future<juce::Result> writing;
    };
    void timerCallback() override;
    bool keyPressed(const juce::KeyPress&, juce::Component*) override;
    bool keyStateChanged(bool, juce::Component*) override;
    void globalFocusChanged(juce::Component*) override;
    void refresh(); void setTimeline(bool); void recordClicked(); void stopClicked(); void latestClicked();
    void promptMarker(); void dismissMarkerPrompt();
    void projectMenu(); void newProjectDialog(); void chooseOpen(); void saveProject(); void saveTo(const juce::File&);
    void beforeSwitch(std::function<void()>); void showSettings(); void persistSettings(); void continueClose();
    void demoTick(); void finishDemo(const juce::String&, const juce::String&);
    void publishLifecycle();
    void retryFinalization();
    bool importBusy() const { return importStarting || audioImporter.isBusy(); }
    bool canImportAudio() const;
    void importAudio(const juce::File& = {});
    bool ownsShortcutOrigin(const juce::Component*) const;
    bool startFileWork(FileResult, std::function<FileResult()>);
    FileResult collectFileWork();
    void settingsFailed(const juce::String&);
    RecorderDocument& document;
    RecorderSettings& settings;
    RecordView recordView;
    TimelineView timelineView;
    RecorderSession session; // joins native host users before RecordView destruction
    AudioImportPanel audioImporter;
    std::unique_ptr<ExportDialog> exportDialog;
    std::unique_ptr<juce::DocumentWindow> settingsWindow, projectWindow;
    juce::Component::SafePointer<juce::AlertWindow> markerWindow;
    Id markerProject;
    AudioSettingsPanel* audioPanel = nullptr;
    CameraSettingsPanel* cameraPanel = nullptr;
    juce::Label* settingsError = nullptr;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<Demo> demo;
    std::future<FileResult> fileWork;
    FileResult pendingFile; // preserve checkpoint/open metadata if a future throws
    std::function<void(const char*)> beforeWorkerStart; // owner-thread failure injection
    std::future<juce::Result> settingsWork;
    std::future<juce::int64> spaceWork;
    std::function<void()> afterSave, closeAction;
    bool timeline = false, refreshPending = true, settingsPending = false;
    bool devicesEnabled = true, promptAfterStartupOpen = false;
    std::optional<UserSettings> pendingConfigure; // a settings edit made while the previous one is still applying
    juce::Time launchedAt = juce::Time::getCurrentTime(), lastQuietCheck;
    juce::int64 remainingBytes = -1;
    std::uint64_t spaceGeneration = 0;
    std::uint32_t lastUi = 0, lastSpace = 0;
    juce::KeyPress heldShortcut;
    juce::Component::SafePointer<juce::Component> shortcutFocus;
    std::int64_t lastStopButtonQpc = 0;
    juce::String banner;
    juce::String exceptionBanner; // persists across device/status refreshes
    juce::TextButton aboutButton, updateButton, retryButton;
    std::array<juce::Label, 4> footerLabels;
    std::array<int, 4> footerWidths{}; bool footerWidthsStale = true, footerLive = false; // measured/repainted only when a footer string or the recording state changes
    RecorderPowerMonitor powerMonitor;
    bool closeCommitRequested = false;
    bool importStarting = false;
    juce::String importPlacement;
    juce::TooltipWindow tooltips {this};
};
}
