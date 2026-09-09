set(RECORDER_FFMPEG_ROOT "C:/Users/claude/SDKs/ffmpeg-lgpl/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1"
    CACHE PATH "Frozen BtbN n8.1 win64 LGPL shared SDK (headers, import libraries and DLLs)")
set(_recorder_ffmpeg_version "n8.1.2-51-g7ba069f4f1-20260908")
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Recorder requires MSVC x64.")
endif()
execute_process(COMMAND "${RECORDER_FFMPEG_ROOT}/bin/ffmpeg.exe" -version
    RESULT_VARIABLE _rec_ff_result OUTPUT_VARIABLE _rec_ff_version ERROR_VARIABLE _rec_ff_error)
string(REGEX MATCH "^ffmpeg version ([^ \r\n]+) " _rec_ff_match "${_rec_ff_version}")
if(NOT _rec_ff_result EQUAL 0 OR NOT CMAKE_MATCH_1 STREQUAL _recorder_ffmpeg_version)
    message(FATAL_ERROR "Recorder requires FFmpeg ${_recorder_ffmpeg_version}: ${_rec_ff_result}; ${_rec_ff_version}; ${_rec_ff_error}")
endif()
if(NOT _rec_ff_version MATCHES "--enable-shared" OR NOT _rec_ff_version MATCHES "--disable-static"
   OR _rec_ff_version MATCHES "--enable-gpl" OR _rec_ff_version MATCHES "--enable-nonfree")
    message(FATAL_ERROR "Recorder requires the frozen LGPL shared FFmpeg build.")
endif()
file(READ "${RECORDER_FFMPEG_ROOT}/include/libavcodec/version_major.h" _rec_codec_header)
if(NOT _rec_codec_header MATCHES "#define[ \t]+LIBAVCODEC_VERSION_MAJOR[ \t]+62([\r\n]|$)")
    message(FATAL_ERROR "Recorder requires avcodec 62 headers.")
endif()
# Hash checks bind the runtime binaries to the design's fixed SDK, not the folder's 'latest' name.
foreach(_rec_lock IN ITEMS
    "avcodec-62.dll|B5D79DDAF7186D8F07BD33D14EC0ADF1E6DC54B576A86D7E900A2638D0B44D7F"
    "avformat-62.dll|454F6E59EE61316BD8F522753E1C4B49042A4320EAD7B21A27B318C58E35E07B"
    "avutil-60.dll|4BC98F2E69FDDFE8685CCE641B7399D58BB2B1F71D4D9C88055DBE71B42E11A0")
    string(REPLACE "|" ";" _rec_lock "${_rec_lock}")
    list(GET _rec_lock 0 _rec_dll)
    list(GET _rec_lock 1 _rec_expected_hash)
    file(SHA256 "${RECORDER_FFMPEG_ROOT}/bin/${_rec_dll}" _rec_hash)
    string(TOUPPER "${_rec_hash}" _rec_hash)
    if(NOT _rec_hash STREQUAL _rec_expected_hash)
        message(FATAL_ERROR "Recorder: ${_rec_dll} SHA-256 differs from the frozen SDK.")
    endif()
endforeach()
foreach(_rec_lib IN ITEMS avcodec avformat avutil swscale swresample)
    if(_rec_lib STREQUAL "avcodec" OR _rec_lib STREQUAL "avformat")
        set(_rec_abi 62)
    elseif(_rec_lib STREQUAL "avutil")
        set(_rec_abi 60)
    elseif(_rec_lib STREQUAL "swscale")
        set(_rec_abi 9)
    else()
        set(_rec_abi 6)
    endif()
    foreach(_rec_file IN ITEMS "lib/${_rec_lib}.lib" "bin/${_rec_lib}-${_rec_abi}.dll")
        if(NOT EXISTS "${RECORDER_FFMPEG_ROOT}/${_rec_file}")
            message(FATAL_ERROR "Recorder SDK file missing: ${_rec_file}")
        endif()
    endforeach()
    add_library(RecorderFFmpeg_${_rec_lib} SHARED IMPORTED)
    set_target_properties(RecorderFFmpeg_${_rec_lib} PROPERTIES
        IMPORTED_IMPLIB "${RECORDER_FFMPEG_ROOT}/lib/${_rec_lib}.lib"
        IMPORTED_LOCATION "${RECORDER_FFMPEG_ROOT}/bin/${_rec_lib}-${_rec_abi}.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${RECORDER_FFMPEG_ROOT}/include")
endforeach()

# Reuse only JSON/file support. No GoCueCore, audio engine or PluginHost dependency.
add_library(RecorderCommon STATIC ../src/model/SafeFileWrite.cpp)
target_link_libraries(RecorderCommon PRIVATE juce::juce_core)
get_target_property(_rec_juce_includes juce::juce_core INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(RecorderCommon PUBLIC ../src ${_rec_juce_includes})
target_compile_definitions(RecorderCommon PUBLIC
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=1
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)

add_library(RecorderCore STATIC
    src/capture/CameraCatalog.cpp src/capture/MfCameraCapture.cpp
    src/video/CaptureFrameDecoder.cpp src/video/VideoSurfacePool.cpp src/video/PreviewPresenter.cpp
    src/video/GpuColourConverter.cpp src/diagnostics/CaptureTelemetry.cpp
    src/record/VideoCfrScheduler.cpp src/record/NvencEncoder.cpp src/record/Mp4TakeWriter.cpp
    src/record/ReferenceMixWriter.cpp src/record/EncodePipeline.cpp)
target_include_directories(RecorderCore PUBLIC src)
target_link_libraries(RecorderCore PUBLIC RecorderCommon RecorderFFmpeg_avcodec RecorderFFmpeg_avformat RecorderFFmpeg_avutil RecorderFFmpeg_swscale RecorderFFmpeg_swresample
    PRIVATE mf mfplat mfreadwrite mfuuid ole32 propsys d3d11 dxgi d3dcompiler user32 winmm)
target_compile_definitions(RecorderCore PUBLIC RECORDER_FFMPEG_VERSION="${_recorder_ffmpeg_version}")
add_executable(RecorderProbe tools/RecorderProbe.cpp)
add_executable(RecorderTests tests/TestMain.cpp tests/CaptureContractTests.cpp tests/CfrSchedulerTests.cpp tests/EncoderContractTests.cpp)
include(src/audio/RecorderAudio.cmake)
target_sources(RecorderTests PRIVATE tests/AsioStampTests.cpp tests/NativePcmTests.cpp)
foreach(_rec_target IN ITEMS RecorderCommon RecorderCore RecorderProbe RecorderTests)
    set_property(TARGET ${_rec_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(${_rec_target} PRIVATE /utf-8 /W4 /permissive- /EHsc)
endforeach()
# The SDK currently contains only FFmpeg DLLs. Copy the complete same-archive DLL closure;
# Includes avformat and swresample for the round 02 MP4/AAC writers. No static FFmpeg archive.
file(GLOB _rec_dlls CONFIGURE_DEPENDS "${RECORDER_FFMPEG_ROOT}/bin/*.dll")
foreach(_rec_exe IN ITEMS RecorderProbe RecorderTests)
    target_link_libraries(${_rec_exe} PRIVATE RecorderCore)
    set_target_properties(${_rec_exe} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${_rec_exe}_artefacts/$<CONFIG>")
    add_custom_command(TARGET ${_rec_exe} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${_rec_dlls} "$<TARGET_FILE_DIR:${_rec_exe}>" VERBATIM)
endforeach()
add_test(NAME RecorderCaptureContracts COMMAND RecorderTests --suite capture-contract)
add_test(NAME RecorderCfrScheduler COMMAND RecorderTests --suite cfr-scheduler)
add_test(NAME RecorderEncoderContracts COMMAND RecorderTests --suite encoder-contract)
message(STATUS "Recorder: pinned LGPL shared FFmpeg ${_recorder_ffmpeg_version}, avcodec ABI 62")

# Round 06 / spike 4A: WAV chunks and durable storage. Keep this block append-only.
target_sources(RecorderCore PRIVATE src/storage/DurableFile.cpp src/storage/RecordingJournal.cpp src/record/WavTrackWriter.cpp)
target_sources(RecorderTests PRIVATE tests/JournalTests.cpp tests/WavChunkTests.cpp)
# Suite routing: tests/TestMain.cpp registry (no per-round main renames).
add_test(NAME RecorderJournalDurable COMMAND RecorderTests --suite journal-durable)
add_executable(RecorderDurabilityProbe tools/DurabilityProbe.cpp)
target_link_libraries(RecorderDurabilityProbe PRIVATE RecorderCore)
set_property(TARGET RecorderDurabilityProbe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(RecorderDurabilityProbe PRIVATE /utf-8 /W4 /permissive- /EHsc)
set_target_properties(RecorderDurabilityProbe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/RecorderDurabilityProbe_artefacts/$<CONFIG>")
add_custom_command(TARGET RecorderDurabilityProbe POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${_rec_dlls} "$<TARGET_FILE_DIR:RecorderDurabilityProbe>" VERBATIM)
