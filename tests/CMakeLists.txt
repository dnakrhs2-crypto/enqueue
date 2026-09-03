juce_add_console_app(EnqueueTests PRODUCT_NAME "EnqueueTests")

target_sources(EnqueueTests PRIVATE
    TestMain.cpp
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
    ResamplerTests.cpp
    CueListTests.cpp
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
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(EnqueueTests PRIVATE GoCueCore)

set_property(TARGET EnqueueTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_test(NAME EnqueueUnitTests COMMAND EnqueueTests)
