cmake_minimum_required(VERSION 3.22)

project(GoCue VERSION 0.1.0 LANGUAGES C CXX)

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
# Shared core: cue model + audio engine. An INTERFACE target so the sources
# (and the JUCE modules) are compiled into both the app and the test runner.
# ---------------------------------------------------------------------------
add_library(GoCueCore INTERFACE)

target_sources(GoCueCore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model/Cue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model/CueList.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model/ProjectSerializer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/CuePlayer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/AudioEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PluginChain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PluginHost.cpp)

target_include_directories(GoCueCore INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

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
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
juce_add_gui_app(GoCue
    PRODUCT_NAME "GoCue"
    COMPANY_NAME "GoCue"
    VERSION ${PROJECT_VERSION}
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(GoCue PRIVATE
    src/Main.cpp
    src/app/AppSettings.cpp
    src/app/ProjectDocument.cpp
    src/ui/MainComponent.cpp
    src/ui/TransportBar.cpp
    src/ui/CueTable.cpp
    src/ui/CueInspector.cpp
    src/ui/AudioSettingsDialog.cpp
    src/ui/PluginWindows.cpp
    src/ui/PluginChainComponent.cpp
    src/ui/PluginDialogs.cpp)

target_compile_definitions(GoCue PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:GoCue,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:GoCue,JUCE_VERSION>")

target_link_libraries(GoCue PRIVATE GoCueCore)

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT GoCue)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
