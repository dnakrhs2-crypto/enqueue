set(RECORDER_FFMPEG_ROOT "C:/Users/claude/SDKs/ffmpeg-lgpl/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1"
    CACHE PATH "Frozen BtbN n8.1 win64 LGPL shared SDK (headers, import libraries and DLLs)")
set(_recorder_ffmpeg_version "n8.1.2-51-g7ba069f4f1-20260908")
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "Skipping Recorder: requires MSVC x64.")
    return()
endif()
if(NOT IS_DIRECTORY "${RECORDER_FFMPEG_ROOT}")
    message(WARNING "Skipping Recorder: SDK directory missing: ${RECORDER_FFMPEG_ROOT}")
    return()
endif()
execute_process(COMMAND "${RECORDER_FFMPEG_ROOT}/bin/ffmpeg.exe" -version
    RESULT_VARIABLE _rec_ff_result OUTPUT_VARIABLE _rec_ff_version ERROR_VARIABLE _rec_ff_error TIMEOUT 15)
string(REGEX MATCH "^ffmpeg version ([^ \r\n]+) " _rec_ff_match "${_rec_ff_version}")
if(NOT _rec_ff_result EQUAL 0 OR NOT CMAKE_MATCH_1 STREQUAL _recorder_ffmpeg_version)
    message(WARNING "Skipping Recorder: requires FFmpeg ${_recorder_ffmpeg_version}: ${_rec_ff_result}; ${_rec_ff_version}; ${_rec_ff_error}")
    return()
endif()
if(NOT _rec_ff_version MATCHES "--enable-shared" OR NOT _rec_ff_version MATCHES "--disable-static"
   OR _rec_ff_version MATCHES "--enable-gpl" OR _rec_ff_version MATCHES "--enable-nonfree")
    message(WARNING "Skipping Recorder: requires the frozen LGPL shared FFmpeg build.")
    return()
endif()
# Use child processes for reads/hashes: file(READ/SHA256) errors in this configure
# would poison the parent even after return(). Missing/unreadable SDKs are optional.
execute_process(COMMAND "${CMAKE_COMMAND}" -E cat "${RECORDER_FFMPEG_ROOT}/include/libavcodec/version_major.h"
    RESULT_VARIABLE _rec_read_result OUTPUT_VARIABLE _rec_codec_header ERROR_VARIABLE _rec_read_error)
if(NOT _rec_read_result STREQUAL "0" OR NOT _rec_codec_header MATCHES "#define[ \t]+LIBAVCODEC_VERSION_MAJOR[ \t]+62([\r\n]|$)")
    message(WARNING "Skipping Recorder: requires readable avcodec 62 headers. ${_rec_read_error}")
    return()
endif()
# Hash checks bind the runtime binaries to the design's fixed SDK, not the folder's 'latest' name.
foreach(_rec_lock IN ITEMS
    "avcodec-62.dll|B5D79DDAF7186D8F07BD33D14EC0ADF1E6DC54B576A86D7E900A2638D0B44D7F"
    "avformat-62.dll|454F6E59EE61316BD8F522753E1C4B49042A4320EAD7B21A27B318C58E35E07B"
    "avutil-60.dll|4BC98F2E69FDDFE8685CCE641B7399D58BB2B1F71D4D9C88055DBE71B42E11A0")
    string(REPLACE "|" ";" _rec_lock "${_rec_lock}")
    list(GET _rec_lock 0 _rec_dll)
    list(GET _rec_lock 1 _rec_expected_hash)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sha256sum "${RECORDER_FFMPEG_ROOT}/bin/${_rec_dll}"
        RESULT_VARIABLE _rec_hash_result OUTPUT_VARIABLE _rec_hash_line ERROR_VARIABLE _rec_hash_error)
    string(REGEX MATCH "^[0-9a-fA-F]+" _rec_hash "${_rec_hash_line}")
    string(TOUPPER "${_rec_hash}" _rec_hash)
    if(NOT _rec_hash_result STREQUAL "0" OR NOT _rec_hash STREQUAL _rec_expected_hash)
        message(WARNING "Skipping Recorder: ${_rec_dll} missing/unreadable or SHA-256 differs from the frozen SDK. ${_rec_hash_error}")
        return()
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
            message(WARNING "Skipping Recorder: SDK file missing: ${_rec_file}")
            return()
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
    src/video/GpuColourConverter.cpp src/diagnostics/CaptureTelemetry.cpp src/diagnostics/MfJpegRangeProbe.cpp
    src/record/VideoCfrScheduler.cpp src/record/NvencEncoder.cpp src/record/Mp4TakeWriter.cpp
    src/record/ReferenceMixWriter.cpp src/record/EncodePipeline.cpp)
target_include_directories(RecorderCore PUBLIC src)
target_link_libraries(RecorderCore PUBLIC RecorderCommon RecorderFFmpeg_avcodec RecorderFFmpeg_avformat RecorderFFmpeg_avutil RecorderFFmpeg_swscale RecorderFFmpeg_swresample
    PRIVATE mf mfplat mfreadwrite mfuuid ole32 propsys d3d11 dxgi d3dcompiler user32 winmm)
target_compile_definitions(RecorderCore PUBLIC RECORDER_FFMPEG_VERSION="${_recorder_ffmpeg_version}")
add_executable(RecorderProbe tools/RecorderProbe.cpp)
add_executable(RecorderTests tests/TestMain.cpp tests/CaptureContractTests.cpp tests/CaptureReviewTests.cpp tests/CfrSchedulerTests.cpp tests/EncoderContractTests.cpp)
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
add_test(NAME RecorderCaptureReview COMMAND RecorderTests --suite capture-review)
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

# BEGIN Recorder round 08: independent model and GUI (append-only integration block).
add_library(RecorderModel STATIC
    src/app/ProductIdentity.cpp src/app/RecorderSettings.cpp
    src/app/RecorderDocument.cpp src/app/EditHistory.cpp
    src/model/RecorderModel.cpp src/model/RecorderSerializer.cpp)
target_include_directories(RecorderModel PUBLIC src)
target_link_libraries(RecorderModel PUBLIC RecorderCommon)

# ProductIdentity is the single source for runtime names and generated version/icon resources.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/app/ProductIdentity.h" _rec_identity_lines
    REGEX "^#define RECORDER_(DISPLAY_NAME|INTERNAL_ID|VERSION|COMPANY) ")
foreach(_rec_identity_line IN LISTS _rec_identity_lines)
    string(REGEX MATCH "^#define (RECORDER_[A-Z_]+) \"([^\"]+)\"" _rec_identity_match "${_rec_identity_line}")
    set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/app/ProductIdentity.h")
juce_add_gui_app(Recorder
    PRODUCT_NAME "${RECORDER_DISPLAY_NAME}"
    COMPANY_NAME "${RECORDER_COMPANY}"
    BUNDLE_ID "${RECORDER_INTERNAL_ID}"
    VERSION "${RECORDER_VERSION}"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon_512.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon_512.png"
    NEEDS_WEB_BROWSER FALSE NEEDS_CURL FALSE)
get_target_property(_rec_generated_dir Recorder JUCE_GENERATED_SOURCES_DIRECTORY)
set(_rec_rc_file "${_rec_generated_dir}/Recorder_resources.rc")
if(EXISTS "${_rec_rc_file}")
    file(READ "${_rec_rc_file}" _rec_rc_text)
    string(REPLACE "." "," _rec_version_commas "${RECORDER_VERSION}")
    if(NOT _rec_rc_text MATCHES "FILEVERSION[ \t]+${_rec_version_commas},0")
        file(REMOVE "${_rec_rc_file}")
    endif()
endif()
target_sources(Recorder PRIVATE src/Main.cpp src/ui/RecorderLookAndFeel.cpp src/ui/MainComponent.cpp)
target_sources(Recorder PRIVATE src/support/CrashHandler.cpp)
target_link_libraries(Recorder PRIVATE RecorderModel juce::juce_gui_basics dbghelp)
target_compile_definitions(Recorder PRIVATE
    JUCE_CATCH_UNHANDLED_EXCEPTIONS=1
    JUCE_APPLICATION_NAME_STRING="${RECORDER_DISPLAY_NAME}"
    JUCE_APPLICATION_VERSION_STRING="${RECORDER_VERSION}")
target_compile_definitions(RecorderTests PRIVATE JUCE_CATCH_UNHANDLED_EXCEPTIONS=1)
foreach(_rec_r08_target IN ITEMS RecorderModel Recorder)
    set_property(TARGET ${_rec_r08_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(${_rec_r08_target} PRIVATE /utf-8 /W4 /permissive- /EHsc)
endforeach()

# The round-01 runner only dispatches capture-contract. Keep that file untouched and
# rename just its entry point so the new test file can dispatch both suites.
# Suite routing: tests/TestMain.cpp registry (no per-round main renames).
target_sources(RecorderTests PRIVATE tests/RecorderProjectTests.cpp)
target_link_libraries(RecorderTests PRIVATE RecorderModel)
add_test(NAME RecorderProjectRoundtrip COMMAND RecorderTests --suite project-roundtrip)
# END Recorder round 08.

# Rounds 09+10: append-only integration, single shared test main and suite registry.
target_sources(RecorderCore PRIVATE
    src/audio/RecorderAudioEngine.cpp src/playback/PlaybackBlockQueue.cpp
    src/sync/LinearClockMapper.cpp src/record/TakeController.cpp)
target_link_libraries(RecorderCore PUBLIC RecorderModel)
target_sources(RecorderTests PRIVATE tests/RecorderAudioTests.cpp tests/TakeControllerTests.cpp)
add_test(NAME RecorderAudio COMMAND RecorderTests --suite recorder-audio)
add_test(NAME RecorderTakeController COMMAND RecorderTests --suite take-lifecycle)
# Round 07 / spike 4B. Append-only target integration; one shared test main.
target_sources(RecorderCore PRIVATE src/storage/RecoverySupport.cpp src/storage/Mp4RecoveryIndex.cpp src/storage/RecoveryScanner.cpp)
target_link_libraries(RecorderCore PUBLIC RecorderModel PRIVATE bcrypt)
target_sources(RecorderTests PRIVATE tests/RecoveryTests.cpp tests/LargeFileTests.cpp tools/CrashFixtures.cpp)
target_include_directories(RecorderTests PRIVATE tools)
add_test(NAME RecorderRecovery COMMAND RecorderTests --suite recovery-idempotence)
add_test(NAME RecorderLargeFile COMMAND RecorderTests --suite large-files)
add_executable(RecorderCrashHarness tools/CrashHarness.cpp tools/CrashFixtures.cpp)
target_link_libraries(RecorderCrashHarness PRIVATE RecorderCore)
set_property(TARGET RecorderCrashHarness PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(RecorderCrashHarness PRIVATE /utf-8 /W4 /permissive- /EHsc)
set_target_properties(RecorderCrashHarness PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/RecorderCrashHarness_artefacts/$<CONFIG>")
add_custom_command(TARGET RecorderCrashHarness POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${_rec_dlls} "$<TARGET_FILE_DIR:RecorderCrashHarness>" VERBATIM)

# BEGIN Recorder round 04: clocks / calibration / standalone probe (append-only).
target_sources(RecorderCore PRIVATE
    src/sync/ClockMapper.cpp src/sync/CameraClockMapper.cpp src/sync/CalibrationProfile.cpp)
target_sources(RecorderTests PRIVATE tests/ClockMapperTests.cpp)
add_test(NAME RecorderClockMapping COMMAND RecorderTests --suite clock-mapping)
add_executable(RecorderClockProbe tools/ClockProbe.cpp)
target_link_libraries(RecorderClockProbe PRIVATE RecorderCore)
set_property(TARGET RecorderClockProbe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(RecorderClockProbe PRIVATE /utf-8 /W4 /permissive- /EHsc)
set_target_properties(RecorderClockProbe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/RecorderClockProbe_artefacts/$<CONFIG>")
add_custom_command(TARGET RecorderClockProbe POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${_rec_dlls} "$<TARGET_FILE_DIR:RecorderClockProbe>" VERBATIM)
# END Recorder round 04.
# Round 13: pure timeline edits, immutable render plans, and snapshot history contracts.
target_sources(RecorderModel PRIVATE src/model/ClipEdits.cpp src/model/RenderPlanCompiler.cpp)
target_sources(RecorderTests PRIVATE
    tests/ClipEditTests.cpp tests/LinkEditTests.cpp tests/EditHistoryTests.cpp tests/EditPropertyTests.cpp)
add_test(NAME RecorderCutLinkHistory COMMAND RecorderTests --suite cut-link-history)
add_test(NAME RecorderEditProperty COMMAND RecorderTests --suite edit-property --seed 909 --iterations 1000)
# Round 16 / spike 6A: independent audio import and derived PCM (append-only).
add_library(RecorderImport STATIC
    src/media/AudioImport.cpp src/playback/ImportedAudioCache.cpp
    ../src/audio/MediaFoundationAudioFormat.cpp ../src/audio/HighQualityResampler.cpp)
target_include_directories(RecorderImport PRIVATE ../third_party/r8brain)
target_link_libraries(RecorderImport PUBLIC RecorderModel RecorderCore PRIVATE juce::juce_audio_formats)
target_compile_definitions(RecorderImport PUBLIC JUCE_USE_MP3AUDIOFORMAT=1)
set_property(TARGET RecorderImport PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(RecorderImport PRIVATE /utf-8 /W4 /permissive- /EHsc)
target_sources(Recorder PRIVATE src/ui/AudioImportPanel.cpp)
target_link_libraries(Recorder PRIVATE RecorderImport)
target_sources(RecorderTests PRIVATE tests/AudioImportTests.cpp tests/ImportedClipTests.cpp)
target_link_libraries(RecorderTests PRIVATE RecorderImport)
target_link_libraries(RecorderProbe PRIVATE RecorderImport)
target_compile_definitions(RecorderProbe PRIVATE RECORDER_IMPORT_FFMPEG_EXE="${RECORDER_FFMPEG_ROOT}/bin/ffmpeg.exe")
target_compile_definitions(RecorderTests PRIVATE RECORDER_IMPORT_FFMPEG_EXE="${RECORDER_FFMPEG_ROOT}/bin/ffmpeg.exe")
add_test(NAME RecorderAudioImport COMMAND RecorderTests --suite audio-import)
add_custom_command(TARGET Recorder POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${_rec_dlls} "$<TARGET_FILE_DIR:Recorder>" VERBATIM)
# Round 11: finalized-file playback only (append-only integration).
target_sources(RecorderCore PRIVATE
    src/media/MediaIndex.cpp src/playback/VideoPlaybackEngine.cpp
    src/playback/TimelineTransport.cpp src/playback/TimelineAudioRenderer.cpp
    src/playback/AsioPlaybackOutput.cpp)
target_link_libraries(RecorderCore PUBLIC RecorderModel)
target_sources(RecorderProbe PRIVATE tools/PlaybackProbe.cpp tools/AlignmentProbe.cpp)
target_sources(RecorderTests PRIVATE tests/PlaybackTests.cpp)
add_test(NAME RecorderPlaybackEngine COMMAND RecorderTests --suite playback-engine)

# Rounds 17+18: append-only integration. One ASIO device, independent UI panels.
target_sources(RecorderModel PRIVATE src/model/TakeStackEdits.cpp)
target_sources(RecorderImport PRIVATE src/record/DubbingController.cpp)
target_sources(Recorder PRIVATE src/ui/DubbingPanel.cpp src/ui/TakeListPanel.cpp)
target_sources(RecorderProbe PRIVATE tools/DubbingProbe.cpp)
target_sources(RecorderTests PRIVATE tests/DubbingPlacementTests.cpp tests/DubbingFailureTests.cpp tests/TakeStackTests.cpp)
add_test(NAME RecorderDubbingPlacement COMMAND RecorderTests --suite dubbing-clock-placement)
add_test(NAME RecorderDubbingFailures COMMAND RecorderTests --suite dubbing-failures)
add_test(NAME RecorderTakeStackEdits COMMAND RecorderTests --suite take-stack-edits)

# Round 12: app/device/UI wiring. Append only; keep the shared test entry point.
target_sources(RecorderCore PRIVATE src/media/PeakCache.cpp src/media/ThumbnailCache.cpp
    src/app/RecorderSession.cpp src/ui/UiState.cpp)
target_sources(Recorder PRIVATE src/ui/RecordView.cpp src/ui/TimelineView.cpp
    src/ui/RecorderTransportBar.cpp src/ui/AudioSettingsPanel.cpp src/ui/CameraSettingsPanel.cpp
    src/ui/ProjectDialogs.cpp src/ui/DemoAutomation.cpp)
target_link_libraries(Recorder PRIVATE RecorderCore juce::juce_gui_extra)
target_sources(RecorderProbe PRIVATE tools/DemoProbe.cpp)
target_sources(RecorderTests PRIVATE tests/UiWiringTests.cpp src/ui/AudioSettingsPanel.cpp)
add_test(NAME RecorderUiWiring COMMAND RecorderTests --suite ui-wiring)
# Round 05 / spike 3: diagnostic dual-camera load, append-only integration.
add_library(RecorderDualFixtures STATIC tools/FramePatternSource.cpp tools/DualAudioLoad.cpp)
target_include_directories(RecorderDualFixtures PUBLIC tools)
target_link_libraries(RecorderDualFixtures PUBLIC RecorderCore)
add_executable(RecorderDualProbe tools/DualCaptureProbe.cpp)
target_link_libraries(RecorderDualProbe PRIVATE RecorderDualFixtures)
foreach(_rec_r05_target IN ITEMS RecorderDualFixtures RecorderDualProbe)
    set_property(TARGET ${_rec_r05_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(${_rec_r05_target} PRIVATE /utf-8 /W4 /permissive- /EHsc)
endforeach()
set_target_properties(RecorderDualProbe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/RecorderDualProbe_artefacts/$<CONFIG>")
add_custom_command(TARGET RecorderDualProbe POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${_rec_dlls} "$<TARGET_FILE_DIR:RecorderDualProbe>" VERBATIM)
target_sources(RecorderTests PRIVATE tests/QueueIsolationTests.cpp)
target_link_libraries(RecorderTests PRIVATE RecorderDualFixtures)
add_test(NAME RecorderQueueIsolation COMMAND RecorderTests --suite queue-isolation)
# Round 14: preserve round-13 declarations/ABI, compile its completed implementation
# once from playback. No edits to the shared model or another round's source list.
set_source_files_properties(src/model/RenderPlanCompiler.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(RecorderModel PRIVATE src/playback/RenderPlanCompiler.cpp src/audio/MicroFade.cpp src/model/MarkerExport.cpp)
# The existing RecorderImport target owns juce_audio_formats. Recorder/Probe/Tests
# already link it; do not compile a second copy with different MP3 definitions.
target_sources(RecorderTests PRIVATE tests/AudioRenderFixtures.cpp tests/AudioCutRenderTests.cpp tests/PlaybackQueueTests.cpp)
add_test(NAME RecorderAudioCutRender COMMAND RecorderTests --suite audio-cut-render)
add_test(NAME RecorderAudioPrefetchUnderrun COMMAND RecorderTests --suite audio-prefetch-underrun)

# Round 15: shared headless interaction logic; GUI and probe use the same commands.
add_library(RecorderTimelineLogic STATIC src/ui/TimelineView.logic.cpp src/ui/TimelineView.automation.cpp)
target_link_libraries(RecorderTimelineLogic PUBLIC RecorderModel)
set_property(TARGET RecorderTimelineLogic PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(RecorderTimelineLogic PRIVATE /utf-8 /W4 /permissive- /EHsc)
target_sources(Recorder PRIVATE src/ui/ClipInspector.cpp src/ui/TrackHeader.cpp src/ui/MarkerPanel.cpp src/ui/MarkerColourSwatches.cpp src/ui/TimelineView.scenario.cpp)
target_link_libraries(Recorder PRIVATE RecorderTimelineLogic)
target_sources(RecorderProbe PRIVATE tools/UiProbe.cpp)
target_link_libraries(RecorderProbe PRIVATE RecorderTimelineLogic)
target_sources(RecorderTests PRIVATE tests/RippleTests.cpp tests/ReorderTests.cpp tests/MarkerTests.cpp)
target_link_libraries(RecorderTests PRIVATE RecorderTimelineLogic)
add_test(NAME RecorderRippleReorderMarkers COMMAND RecorderTests --suite ripple-reorder-markers)

# Rounds 14+15 merge regression: exercise suite lifetimes in one process as well.
add_test(NAME RecorderAllSuites COMMAND RecorderTests)

# Round 19: edit durability and recovery. One test main; append-only integration.
target_sources(RecorderCore PRIVATE src/storage/EditJournal.cpp)
target_sources(RecorderTests PRIVATE tests/EditJournalTests.cpp tests/RetakeRecoveryTests.cpp)
add_test(NAME RecorderEditJournalReplay COMMAND RecorderTests --suite edit-journal-replay)
add_dependencies(RecorderProbe RecorderCrashHarness)

# Round 24: product camera slots; append-only integration and the shared runner.
target_sources(RecorderTests PRIVATE tests/CameraSlotTests.cpp)
add_test(NAME RecorderCameraSlots COMMAND RecorderTests --suite camera-slots)

# Rounds 32+33: append-only dependency audit, updater and packaging description.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_rec_audit "${CMAKE_CURRENT_SOURCE_DIR}/../tools/recorder/audit_ffmpeg.py")
set(_rec_lock_file "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg.lock.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_rec_lock_file}" "${_rec_audit}")
set(_rec_copy_args "")
foreach(_rec_copy IN LISTS _rec_dlls)
    list(APPEND _rec_copy_args --copy-dll "${_rec_copy}")
endforeach()
execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${_rec_audit}"
    --sdk "${RECORDER_FFMPEG_ROOT}" --expected-version "${_recorder_ffmpeg_version}"
    --lock "${_rec_lock_file}" --report "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-audit.json" ${_rec_copy_args}
    RESULT_VARIABLE _rec_audit_result OUTPUT_VARIABLE _rec_audit_output ERROR_VARIABLE _rec_audit_error TIMEOUT 60)
if(NOT _rec_audit_result STREQUAL "0")
    message(FATAL_ERROR "Recorder DLL copy/SDK lock audit failed: ${_rec_audit_output} ${_rec_audit_error}")
endif()
set(RECORDER_UPDATE_PUBLIC_KEY "" CACHE STRING "Recorder EdDSA public key; independent of Enqueue/LiveMix")
target_sources(Recorder PRIVATE src/app/RecorderUpdater.cpp)
target_compile_definitions(Recorder PRIVATE RECORDER_HAS_WINSPARKLE=${GOCUE_HAS_WINSPARKLE}
    RECORDER_UPDATE_PUBLIC_KEY="${RECORDER_UPDATE_PUBLIC_KEY}")
# Product name may contain the provisional Korean label; executable identity stays stable.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/app/ProductIdentity.h" _rec_package_stem REGEX "^#define RECORDER_PACKAGE_STEM ")
string(REGEX REPLACE "^#define RECORDER_PACKAGE_STEM \"([^\"]+)\"$" "\\1" _rec_package_stem "${_rec_package_stem}")
set_target_properties(Recorder PROPERTIES OUTPUT_NAME "${_rec_package_stem}")
if(GOCUE_HAS_WINSPARKLE)
    target_link_libraries(Recorder PRIVATE gocue::winsparkle)
    add_custom_command(TARGET Recorder POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${GOCUE_WINSPARKLE_DLL}" "$<TARGET_FILE_DIR:Recorder>" VERBATIM)
endif()
# The script consumes generator-resolved paths. Future targets/tests are built via ALL/CTest.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/recorder-package-$<CONFIG>.json" CONTENT
    "{\n  \"schema_version\": 1,\n  \"exe\": \"$<TARGET_FILE:Recorder>\",\n  \"ffmpeg_root\": \"${RECORDER_FFMPEG_ROOT}\",\n  \"winsparkle_dir\": \"${WINSPARKLE_DIR}\",\n  \"winsparkle_dll\": \"${GOCUE_WINSPARKLE_DLL}\",\n  \"public_key\": \"${RECORDER_UPDATE_PUBLIC_KEY}\"\n}\n")

# Rounds 20+21: immutable export jobs, offline materials and one final MP4.
# Append-only integration; shared TestMain.cpp owns every suite entry point.
add_library(RecorderExport STATIC
    src/export/ExportJob.cpp src/export/TimelineExporter.cpp
    src/export/WavExportWriter.cpp src/export/FinalVideoExporter.cpp)
target_link_libraries(RecorderExport PUBLIC RecorderImport)
set_property(TARGET RecorderExport PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(RecorderExport PRIVATE /utf-8 /W4 /permissive- /EHsc)
target_sources(RecorderTests PRIVATE tests/ExportRangeTests.cpp tests/WavExportTests.cpp tests/FinalExportTests.cpp)
target_link_libraries(RecorderTests PRIVATE RecorderExport)
target_sources(RecorderProbe PRIVATE tools/ExportProbe.cpp tests/AudioRenderFixtures.cpp)
target_link_libraries(RecorderProbe PRIVATE RecorderExport RecorderDualFixtures)
target_link_libraries(Recorder PRIVATE RecorderExport)
add_test(NAME RecorderExportAudioRange COMMAND RecorderTests --suite export-audio-range)
add_test(NAME RecorderWavExport COMMAND RecorderTests --suite wav-export)
add_test(NAME RecorderFinalExportSources COMMAND RecorderTests --suite final-export-sources)
# Rounds 25+27: append-only registration, single shared test runner.
target_sources(RecorderTests PRIVATE tests/MixedFpsTests.cpp tests/DualTakeIntegrationTests.cpp)
add_test(NAME RecorderMixedFps COMMAND RecorderTests --suite mixed-fps)
add_test(NAME RecorderDualTakeIntegration COMMAND RecorderTests --suite partial-camera-take)
# Reuse the standalone dual harness for RecorderProbe dual-load/integration.
target_link_libraries(RecorderDualProbe PRIVATE RecorderImport)
add_dependencies(RecorderProbe RecorderDualProbe)

# Pick up round-20/21's target regardless of append order after the merge.
# Missing exporters remain an explicit UNAVAILABLE probe stage.
function(_recorder_r27_link_export)
    if(TARGET RecorderExport)
        target_link_libraries(RecorderDualProbe PRIVATE RecorderExport)
        target_compile_definitions(RecorderDualProbe PRIVATE RECORDER_R27_EXPORT_AVAILABLE=1)
    endif()
endfunction()
cmake_language(DEFER CALL _recorder_r27_link_export)
# Round 26 / spike 5B: dual playback, bounded caches and visible timeline queries.
target_sources(RecorderTimelineLogic PRIVATE src/ui/TimelineView.scale.cpp)
target_sources(RecorderTests PRIVATE tests/DualPlaybackTests.cpp tests/SeekGenerationTests.cpp)
add_test(NAME RecorderDualPlayback COMMAND RecorderTests --suite dual-playback)
add_test(NAME RecorderSeekGeneration COMMAND RecorderTests --suite seek-generation)
# Headless component layout/paint uses the unchanged product widgets without HWNDs.
target_sources(RecorderTests PRIVATE src/ui/TimelineView.cpp src/ui/RecorderTransportBar.cpp
    src/ui/TrackHeader.cpp src/ui/ClipInspector.cpp src/ui/MarkerPanel.cpp src/ui/MarkerColourSwatches.cpp src/ui/RecordView.cpp)
target_link_libraries(RecorderTests PRIVATE juce::juce_gui_extra)
target_sources(Recorder PRIVATE src/ui/ShortcutSettingsPanel.cpp)
target_sources(Recorder PRIVATE src/ui/TimelineUxScenario.cpp)
target_sources(RecorderTests PRIVATE src/ui/ShortcutSettingsPanel.cpp)
target_sources(RecorderTests PRIVATE tests/TimelineUxTests.cpp)
add_test(NAME RecorderTimelineUx COMMAND RecorderTests --suite timeline-ux)
# Round 28: lifecycle/fault boundaries. Single suite registry; append only.
target_sources(RecorderCore PRIVATE src/app/RecorderLifecycle.cpp src/capture/PreviewRecovery.cpp)
target_link_libraries(Recorder PRIVATE PowrProf)
target_sources(RecorderTests PRIVATE tests/LifecycleTests.cpp)
target_sources(RecorderProbe PRIVATE tools/LifecycleProbe.cpp tools/CrashFixtures.cpp)
add_test(NAME RecorderLifecycle COMMAND RecorderTests --suite lifecycle)

# Rounds 22+23: sequential material publication, export lifecycle and Korean UI.
# Append only; the shared test runner remains the sole main().
target_sources(RecorderExport PRIVATE src/export/MaterialExporter.cpp src/export/ExportController.cpp)
target_sources(Recorder PRIVATE src/ui/ExportDialog.cpp)
target_sources(RecorderTests PRIVATE tests/MaterialExportTests.cpp tests/ExportLifecycleTests.cpp)
add_test(NAME RecorderMaterialsAlignment COMMAND RecorderTests --suite materials-alignment)
add_test(NAME RecorderExportLifecycle COMMAND RecorderTests --suite export-lifecycle)

# Round 31: shared, device-free hardening fixtures; one registered test runner.
add_library(RecorderHardeningFixtures STATIC tests/HardeningChecks.cpp
    tests/EditPropertyChecks.cpp tests/DubbingEpochChecks.cpp)
target_link_libraries(RecorderHardeningFixtures PUBLIC RecorderExport RecorderTimelineLogic)
set_property(TARGET RecorderHardeningFixtures PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(RecorderHardeningFixtures PRIVATE /utf-8 /W4 /permissive- /EHsc)
target_sources(RecorderTests PRIVATE tests/HardeningTests.cpp)
target_sources(RecorderProbe PRIVATE tools/HardeningProbe.cpp)
target_link_libraries(RecorderTests PRIVATE RecorderHardeningFixtures)
target_link_libraries(RecorderProbe PRIVATE RecorderHardeningFixtures)
add_test(NAME RecorderHardening COMMAND RecorderTests --suite hardening)

# Source discontinuity recovery: same shared runner, no hardware required.
target_sources(RecorderTests PRIVATE tests/SourceReanchorTests.cpp)
add_test(NAME RecorderSourceReanchor COMMAND RecorderTests --suite source-reanchor)

# Cut-edit stability: shared suite registry, real GUI interaction regressions.
target_sources(RecorderTests PRIVATE tests/CutEditStabilityTests.cpp)
add_test(NAME RecorderCutEditStability COMMAND RecorderTests --suite cut-edit-stability)
target_sources(Recorder PRIVATE tools/CutEditStress.cpp)
