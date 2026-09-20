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
    MixEngineTests.cpp
    WebDavBackupTests.cpp
    MixDocumentTests.cpp
    ChannelCardTests.cpp
    MuteGroupsTests.cpp
    ControlProtocolTests.cpp
    ControlDispatcherTests.cpp
    ControlStateTests.cpp
    ControlServerTests.cpp
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
    BackupManagerTests.cpp
    PluginChainTests.cpp
    RealVst3Tests.cpp)

target_compile_definitions(EnqueueTests PRIVATE
    JUCE_APPLICATION_NAME_STRING="EnqueueTests"
    GOCUE_HAS_WINSPARKLE=0
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(EnqueueTests PRIVATE GoCueCore LiveMixUI)

set_property(TARGET EnqueueTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_test(NAME EnqueueUnitTests COMMAND EnqueueTests)
add_test(NAME EnqueueMidiLoadTests COMMAND EnqueueTests --midi-load)
set_tests_properties(EnqueueMidiLoadTests PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS "midi;load")
