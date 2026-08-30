juce_add_console_app(GoCueTests PRODUCT_NAME "GoCueTests")

target_sources(GoCueTests PRIVATE
    TestMain.cpp
    FadeEnvelopeTests.cpp
    CueListTests.cpp
    ProjectSerializerTests.cpp
    AudioEngineTests.cpp)

target_compile_definitions(GoCueTests PRIVATE
    JUCE_APPLICATION_NAME_STRING="GoCueTests"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(GoCueTests PRIVATE GoCueCore)

add_test(NAME GoCueUnitTests COMMAND GoCueTests)
