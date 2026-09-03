juce_add_console_app(GoCueTests PRODUCT_NAME "GoCueTests")

target_sources(GoCueTests PRIVATE
    TestMain.cpp
    FadeEnvelopeTests.cpp
    EnvelopeTests.cpp
    ProjectHistoryTests.cpp
    ProjectDocumentTests.cpp
    SplitLayoutTests.cpp
    CueOrderTests.cpp
    DeviceProbeTests.cpp
    OutputLimitTests.cpp
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

target_compile_definitions(GoCueTests PRIVATE
    JUCE_APPLICATION_NAME_STRING="GoCueTests"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(GoCueTests PRIVATE GoCueCore)

set_property(TARGET GoCueTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_test(NAME GoCueUnitTests COMMAND GoCueTests)
