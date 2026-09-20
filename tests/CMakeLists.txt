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
