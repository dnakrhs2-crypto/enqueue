cmake_minimum_required(VERSION 3.22)

project(GoCue VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "GoCue targets Windows only (ASIO / WASAPI output).")
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ---------------------------------------------------------------------------
# JUCE 8
#   Default: fetched from GitHub at configure time (tag 8.0.15).
#   Local checkout: -DFETCHCONTENT_SOURCE_DIR_JUCE=C:/path/to/JUCE
#   (see README.md / CMakeUserPresets.json)
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
# Steinberg ASIO SDK (optional, downloaded separately, never committed)
#   -DASIO_SDK_DIR=C:/path/to/ASIOSDK   ->  JUCE_ASIO=1
# ---------------------------------------------------------------------------
include(cmake/AsioSdk.cmake)

# ---------------------------------------------------------------------------
# WinSparkle auto-update (optional, downloaded separately)
#   -DWINSPARKLE_DIR=C:/path/to/WinSparkle-0.9.4
#   -DGOCUE_APPCAST_URL=https://github.com/<owner>/<repo>/releases/latest/download/appcast.xml
#   -DGOCUE_EDDSA_PUBLIC_KEY=<base64 public key from winsparkle-tool>
# ---------------------------------------------------------------------------
include(cmake/WinSparkle.cmake)
set(GOCUE_APPCAST_URL "" CACHE STRING "WinSparkle appcast URL (empty disables update checks)")
set(GOCUE_EDDSA_PUBLIC_KEY "" CACHE STRING "Base64 EdDSA public key that verifies signed updates")

# ---------------------------------------------------------------------------
# Shared core: cue model + audio engine. An INTERFACE target so the sources
# (and the JUCE modules) are compiled into both the app and the test runner.
# ---------------------------------------------------------------------------
add_library(GoCueCore INTERFACE)

target_sources(GoCueCore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model/Cue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model/CueList.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model/ProjectSerializer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model/CueNumbering.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model/CuePropertyPaste.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model/LevelMatrix.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model/AudioPatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model/FadeCurve.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/ProjectDocument.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/CueController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/Scheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/FadeRunner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/BackupManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/RegionLoopSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/ReadAheadSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/CuePlayer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/AudioEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PluginChain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PluginHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/MediaFoundationAudioFormat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/StretchSource.cpp)

target_include_directories(GoCueCore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-stretch/include   # time-stretch (MIT, vendored headers)
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-linear/include)

target_compile_definitions(GoCueCore INTERFACE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_ASIO=${GOCUE_HAS_ASIO}
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_WINDOWS_MEDIA_FORMAT=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)

if(GOCUE_HAS_ASIO)
    target_include_directories(GoCueCore INTERFACE ${GOCUE_ASIO_INCLUDE_DIR})
endif()

# Source files are UTF-8 (Korean UI strings).
target_compile_options(GoCueCore INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_link_libraries(GoCueCore INTERFACE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    mfplat mfreadwrite mfuuid propsys ole32)   # Media Foundation AAC / M4A reader

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
juce_add_gui_app(GoCue
    PRODUCT_NAME "GoCue"
    COMPANY_NAME "GoCue"
    VERSION ${PROJECT_VERSION}
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon_512.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon_512.png"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

# JUCE regenerates GoCue_resources.rc (VERSIONINFO) only when the icon changes, so on an
# incremental build a version bump would ship with the old file version. Drop a stale one here.
get_target_property(_gocue_generated_dir GoCue JUCE_GENERATED_SOURCES_DIRECTORY)
set(_gocue_rc_file "${_gocue_generated_dir}/GoCue_resources.rc")
if(EXISTS "${_gocue_rc_file}")
    file(READ "${_gocue_rc_file}" _gocue_rc_text)
    string(REPLACE "." "," _gocue_version_commas "${PROJECT_VERSION}")
    if(NOT _gocue_rc_text MATCHES "FILEVERSION[ \t]+${_gocue_version_commas},0")
        file(REMOVE "${_gocue_rc_file}")
        message(STATUS "GoCue: version resource was stale -> regenerating for ${PROJECT_VERSION}")
    endif()
endif()

target_sources(GoCue PRIVATE
    src/Main.cpp
    src/app/AppSettings.cpp
    src/app/Updater.cpp
    src/ui/MainComponent.cpp
    src/ui/TransportBar.cpp
    src/ui/CueTable.cpp
    src/ui/CueInspector.cpp
    src/ui/WaveformView.cpp
    src/ui/TimeLoopsPanel.cpp
    src/ui/AudioSettingsDialog.cpp
    src/ui/WorkspaceSettingsDialog.cpp
    src/ui/FooterBar.cpp
    src/ui/ActiveCuesPanel.cpp
    src/ui/ContainerTabs.cpp
    src/ui/CueCartView.cpp
    src/ui/PastePropertiesDialog.cpp
    src/ui/LevelMatrixComponent.cpp
    src/ui/PatchEditorDialog.cpp
    src/ui/CurveEditor.cpp
    src/ui/PluginWindows.cpp
    src/ui/PluginChainComponent.cpp
    src/ui/PluginDialogs.cpp)

target_compile_definitions(GoCue PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:GoCue,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:GoCue,JUCE_VERSION>"
    GOCUE_HAS_WINSPARKLE=${GOCUE_HAS_WINSPARKLE}
    GOCUE_APPCAST_URL="${GOCUE_APPCAST_URL}"
    GOCUE_EDDSA_PUBLIC_KEY="${GOCUE_EDDSA_PUBLIC_KEY}")

target_link_libraries(GoCue PRIVATE GoCueCore)

# Static VC runtime: the installed app must not depend on the VC++ redistributable.
set_property(TARGET GoCue PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(GOCUE_HAS_WINSPARKLE)
    target_link_libraries(GoCue PRIVATE gocue::winsparkle)
    add_custom_command(TARGET GoCue POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOCUE_WINSPARKLE_DLL}" "$<TARGET_FILE_DIR:GoCue>"
        COMMENT "Copying WinSparkle.dll next to GoCue.exe")
endif()

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT GoCue)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
