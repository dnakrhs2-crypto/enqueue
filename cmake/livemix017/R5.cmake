# R5 recording engine and headless, non-distributed file probe.
add_library(LiveMixRecording INTERFACE)
target_sources(LiveMixRecording INTERFACE
    "${PROJECT_SOURCE_DIR}/livemix/src/recording/Recorder.cpp"
    "${PROJECT_SOURCE_DIR}/livemix/src/recording/RecordingQueue.cpp"
    "${PROJECT_SOURCE_DIR}/livemix/src/recording/RecordingRecovery.cpp"
    "${PROJECT_SOURCE_DIR}/livemix/src/recording/WavSink.cpp"
    "${PROJECT_SOURCE_DIR}/livemix/src/recording/MfMp3Sink.cpp")
target_include_directories(LiveMixRecording INTERFACE "${PROJECT_SOURCE_DIR}/livemix/src")
target_compile_features(LiveMixRecording INTERFACE c_std_17 cxx_std_17)
target_compile_options(LiveMixRecording INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(LiveMixRecording INTERFACE juce::juce_core juce::juce_audio_formats
    mfplat mfuuid wmcodecdspuuid ole32 delayimp)
target_link_options(LiveMixRecording INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/DELAYLOAD:mfplat.dll>)
target_link_libraries(LiveMixCore INTERFACE LiveMixRecording)

juce_add_console_app(LiveMixRecorderProbe PRODUCT_NAME "LiveMixRecorderProbe" VERSION ${LIVEMIX_VERSION})
gocue_add_app_version(LiveMixRecorderProbe)
target_sources(LiveMixRecorderProbe PRIVATE
    "${PROJECT_SOURCE_DIR}/livemix/tools/RecorderProbe.cpp"
    "${PROJECT_SOURCE_DIR}/livemix/shared/lm_asrc.c")
target_include_directories(LiveMixRecorderProbe PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(LiveMixRecorderProbe PRIVATE LiveMixRecording juce::juce_recommended_config_flags)
target_compile_definitions(LiveMixRecorderProbe PRIVATE
    JUCE_APPLICATION_NAME_STRING="LiveMixRecorderProbe"
    JUCE_APPLICATION_VERSION_STRING="${LIVEMIX_VERSION}"
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1)
set_property(TARGET LiveMixRecorderProbe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(LiveMixRecorderProbe PROPERTIES EXCLUDE_FROM_ALL TRUE)
