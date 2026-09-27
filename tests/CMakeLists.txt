juce_add_console_app(EnqueueTests PRODUCT_NAME "EnqueueTests")

target_sources(EnqueueTests PRIVATE
    TestMain.cpp
    ShortcutTests.cpp
    ShortcutRouterTests.cpp
    ShortcutUiTests.cpp
    MidiModelTests.cpp
    MidiRoutingTests.cpp
    MidiGoTests.cpp
    MidiDeviceTests.cpp
    MidiUiTests.cpp
    MidiMainComponentTests.cpp
    AuditFixUndoTests.cpp
    AuditFix0923EngineTests.cpp
    AuditFix0923CaptureFocusTests.cpp
    AuditFix0923UiTests.cpp
    AuditFix0923FinalTests.cpp
    ../src/ui/MainComponent.cpp
    ../src/ui/TransportBar.cpp
    ../src/ui/CueInspector.cpp
    ../src/ui/WaveformView.cpp
    ../src/ui/TimeLoopsPanel.cpp
    ../src/ui/AudioSettingsDialog.cpp
    ../src/ui/WorkspaceSettingsDialog.cpp
    ../src/ui/FooterBar.cpp
    ../src/ui/ActiveCuesPanel.cpp
    ../src/ui/ContainerTabs.cpp
    ../src/ui/SplitDivider.cpp
    ../src/ui/ManualWindow.cpp
    ../src/ui/YouTubeWindow.cpp
    ../src/ui/CueCartView.cpp
    ../src/ui/PastePropertiesDialog.cpp
    ../src/ui/PatchEditorDialog.cpp
    ../src/ui/CurveEditor.cpp
    ../src/ui/PluginChainComponent.cpp
    ../src/ui/PluginManager.cpp
    ../src/ui/PluginDialogs.cpp
    ../src/ui/KeyCapture.cpp
    ../src/ui/ShortcutSettingsTab.cpp
    ../src/ui/MidiInputSettingsPanel.cpp
    ../src/ui/CueMidiPanel.cpp
    ../src/app/AppSettings.cpp
    ../src/ui/CueTable.cpp
    ../src/ui/LevelMatrixComponent.cpp
    FadeEnvelopeTests.cpp
    EnvelopeTests.cpp
    ProjectHistoryTests.cpp
    ProjectDocumentTests.cpp
    SplitLayoutTests.cpp
    CueOrderTests.cpp
    DeviceProbeTests.cpp
    OutputLimitTests.cpp
    SettingsMigrationTests.cpp
    ProjectFileExtensionTests.cpp
    LiveLoopTests.cpp
    WmaDecodeTests.cpp
    PanicGateTests.cpp
    HotkeyLoadTests.cpp
    SafeSaveTests.cpp
    MixSessionTests.cpp
    LiveMixAsrcTests.cpp
    LiveMixMonitorTests.cpp
    MixEngineTests.cpp
    WebDavBackupTests.cpp
    MixDocumentTests.cpp
    ChannelCardTests.cpp
    MuteGroupsTests.cpp
    ControlProtocolTests.cpp
    ControlDispatcherTests.cpp
    ControlStateTests.cpp
    ControlServerTests.cpp
    AuditL1Tests.cpp
    AuditFix0923LiveMixTests.cpp
    LiveMixPluginTransitionTests.cpp
    PluginPresetTests.cpp
    PluginSearchTests.cpp
    LoudnessMeterTests.cpp
    LoudnessAverageTests.cpp
    UiScaleTests.cpp
    ReopenLastProjectTests.cpp
    CueIconsTests.cpp
    WaitProgressTests.cpp
    FadeOutSettingTests.cpp
    OutputDiagnosticsTests.cpp
    ResamplerTests.cpp
    CueListTests.cpp
    YouTubeDownloaderTests.cpp
    CueNumberingTests.cpp
    CuePropertyPasteTests.cpp
    LevelMatrixTests.cpp
    AudioPatchTests.cpp
    MediaFoundationTests.cpp
    FadeCurveTests.cpp
    FadeRunnerTests.cpp
    SchedulerTests.cpp
    ProjectSerializerTests.cpp
    AudioEngineTests.cpp
    RegionPlaybackTests.cpp
    CueControllerTests.cpp
    AuditFixCueTests.cpp
    AuditFix0923CueTests.cpp
    BackupManagerTests.cpp
    PluginChainTests.cpp
    RealVst3Tests.cpp)

target_compile_definitions(EnqueueTests PRIVATE
    JUCE_APPLICATION_NAME_STRING="EnqueueTests"
    GOCUE_HAS_WINSPARKLE=0
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(EnqueueTests PRIVATE GoCueCore LiveMixUI)

# Compile the same C17 implementation with allocator hooks, including in Release.
# No global allocator overrides: unrelated JUCE and UI tests retain their allocator.
set(_asrc_probe "${CMAKE_CURRENT_BINARY_DIR}/lm_asrc_allocation_probe.c")
file(GENERATE OUTPUT "${_asrc_probe}" CONTENT "
#include <stdlib.h>
#include \"lm_obs_protocol.h\"
void* lm_test_malloc(size_t);
void* lm_test_calloc(size_t, size_t);
void* lm_test_realloc(void*, size_t);
void lm_test_free(void*);
#define malloc lm_test_malloc
#define calloc lm_test_calloc
#define realloc lm_test_realloc
#define free lm_test_free
#define lm_asrc_size lm_test_asrc_size
#define lm_asrc_init lm_test_asrc_init
#define lm_asrc_reset lm_test_asrc_reset
#define lm_asrc_set_correction_ppm lm_test_asrc_set_correction_ppm
#define lm_asrc_process lm_test_asrc_process
#define lm_asrc_latency_input_frames lm_test_asrc_latency_input_frames
#define lm_drift_init lm_test_drift_init
#define lm_drift_update lm_test_drift_update
#include \"${PROJECT_SOURCE_DIR}/livemix/shared/lm_asrc.c\"
")
target_sources(EnqueueTests PRIVATE "${_asrc_probe}")

set_property(TARGET EnqueueTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_test(NAME EnqueueUnitTests COMMAND EnqueueTests)
add_test(NAME EnqueueMidiLoadTests COMMAND EnqueueTests --midi-load)
set_tests_properties(EnqueueMidiLoadTests PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS "midi;load")
