# Included by recorder/CMakeLists.txt to keep the parallel capture round's wiring untouched.
target_sources(RecorderCore PRIVATE ${CMAKE_CURRENT_LIST_DIR}/AsioTimingBridge.cpp ${CMAKE_CURRENT_LIST_DIR}/RawAudioTap.cpp ${CMAKE_CURRENT_LIST_DIR}/NativePcmConverter.cpp)
add_library(RecorderAudioDevices STATIC)
target_link_libraries(RecorderAudioDevices PRIVATE juce::juce_audio_devices)
target_include_directories(RecorderAudioDevices PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
target_compile_definitions(RecorderAudioDevices PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=1
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_ASIO=${GOCUE_HAS_ASIO} JUCE_ASIO_USE_EXTERNAL_SDK=1
    JUCE_WASAPI=1 JUCE_DIRECTSOUND=0 JUCE_ASIO_DEBUGGING=0 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
if(GOCUE_HAS_ASIO)
    target_include_directories(RecorderAudioDevices PRIVATE "${GOCUE_ASIO_INCLUDE_DIR}")
endif()
# Missing patch must be visible, while device-free converter/statistics tests and
# other products remain buildable. Never silently substitute float input for raw.
file(READ "${juce_SOURCE_DIR}/modules/juce_audio_devices/native/juce_ASIO_windows.cpp" _rec_asio_source)
set(_rec_asio_tap 0)
if(GOCUE_HAS_ASIO AND _rec_asio_source MATCHES "recorderReadEvents")
    set(_rec_asio_tap 1)
else()
    message(WARNING "Recorder ASIO native capture unavailable: apply tools/juce-patches/0002-recorder-asio-timing-tap.patch to the shared JUCE clone.")
endif()
target_compile_definitions(RecorderAudioDevices PRIVATE JUCE_RECORDER_ASIO_TAP=${_rec_asio_tap})
target_compile_definitions(RecorderCore PUBLIC RECORDER_ASIO_TAP_AVAILABLE=${_rec_asio_tap})
set_property(TARGET RecorderAudioDevices PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(RecorderAudioDevices PRIVATE /utf-8 /W4 /permissive- /EHsc)
target_sources(RecorderProbe PRIVATE "${CMAKE_CURRENT_LIST_DIR}/AsioProbe.cpp")
target_link_libraries(RecorderCore PUBLIC RecorderAudioDevices)
# Suite routing: tests/TestMain.cpp registry (no per-round main renames).
add_test(NAME RecorderAsioNativePcm COMMAND RecorderTests --suite asio-native-pcm)
