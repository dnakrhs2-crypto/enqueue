cmake_minimum_required(VERSION 3.22)

project(Enqueue VERSION 0.9.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Enqueue targets Windows only (ASIO / WASAPI output).")
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/model/SafeFileWrite.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/SettingsMigration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/RegionLoopSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/ReadAheadSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/CuePlayer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/HighQualityResampler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/AudioEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PluginChain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PluginHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/MediaFoundationAudioFormat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/StretchSource.cpp)

target_include_directories(GoCueCore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-stretch/include   # time-stretch (MIT, vendored headers)
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-linear/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/r8brain)   # sample-rate conversion (MIT, vendored headers)

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
    juce::juce_cryptography
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    mfplat mfreadwrite mfuuid propsys ole32 delayimp)   # Media Foundation AAC / M4A / WMA reader (delay-loaded: optional on Windows N)

# mfplat / mfreadwrite are missing on Windows N editions without the Media Feature Pack: delay-load them so the app
# still starts (MediaFoundationAudioFormat::isAvailable() probes the DLLs before touching them)
target_link_options(GoCueCore INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/DELAYLOAD:mfplat.dll> $<$<CXX_COMPILER_ID:MSVC>:/DELAYLOAD:mfreadwrite.dll>)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
juce_add_gui_app(Enqueue
    PRODUCT_NAME "Enqueue"
    COMPANY_NAME "Gomtwigim"
    VERSION ${PROJECT_VERSION}
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon_512.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon_512.png"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

# JUCE regenerates GoCue_resources.rc (VERSIONINFO) only when the icon changes, so on an
# incremental build a version bump would ship with the old file version. Drop a stale one here.
get_target_property(_gocue_generated_dir Enqueue JUCE_GENERATED_SOURCES_DIRECTORY)
set(_gocue_rc_file "${_gocue_generated_dir}/Enqueue_resources.rc")
if(EXISTS "${_gocue_rc_file}")
    file(READ "${_gocue_rc_file}" _gocue_rc_text)
    string(REPLACE "." "," _gocue_version_commas "${PROJECT_VERSION}")
    if(NOT _gocue_rc_text MATCHES "FILEVERSION[ \t]+${_gocue_version_commas},0")
        file(REMOVE "${_gocue_rc_file}")
        message(STATUS "Enqueue: version resource was stale -> regenerating for ${PROJECT_VERSION}")
    endif()
endif()

target_sources(Enqueue PRIVATE
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
    src/ui/SplitDivider.cpp
    src/ui/ManualWindow.cpp
    src/ui/CueCartView.cpp
    src/ui/PastePropertiesDialog.cpp
    src/ui/LevelMatrixComponent.cpp
    src/ui/PatchEditorDialog.cpp
    src/ui/CurveEditor.cpp
    src/ui/PluginWindows.cpp
    src/ui/PluginChainComponent.cpp
    src/ui/PluginDialogs.cpp)

target_compile_definitions(Enqueue PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Enqueue,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Enqueue,JUCE_VERSION>"
    GOCUE_HAS_WINSPARKLE=${GOCUE_HAS_WINSPARKLE}
    GOCUE_APPCAST_URL="${GOCUE_APPCAST_URL}"
    GOCUE_EDDSA_PUBLIC_KEY="${GOCUE_EDDSA_PUBLIC_KEY}")

target_link_libraries(Enqueue PRIVATE GoCueCore)

# Static VC runtime: the installed app must not depend on the VC++ redistributable.
set_property(TARGET Enqueue PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(GOCUE_HAS_WINSPARKLE)
    target_link_libraries(Enqueue PRIVATE gocue::winsparkle)
    add_custom_command(TARGET Enqueue POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOCUE_WINSPARKLE_DLL}" "$<TARGET_FILE_DIR:Enqueue>"
        COMMENT "Copying WinSparkle.dll next to Enqueue.exe")
endif()

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT Enqueue)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
# LiveMix (the live mic VST3 host) shares the core: its model and engine are a second interface library
add_library(LiveMixCore INTERFACE)
target_sources(LiveMixCore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/livemix/src/MixModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/livemix/src/MixEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/livemix/src/MixDocument.cpp)
target_include_directories(LiveMixCore INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/livemix/src)
target_link_libraries(LiveMixCore INTERFACE GoCueCore)
target_sources(LiveMixCore INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/livemix/src/WebDavBackup.cpp)

set(LIVEMIX_VERSION "0.1.5")   # LiveMix version (the Enqueue version is the project version); not a cache variable: a bump must reach existing build trees
set(LIVEMIX_APPCAST_URL "" CACHE STRING "LiveMix WinSparkle appcast URL (empty disables update checks)")

# The online backup's server account is built into the app from a JSON file that lives outside the repository
# ({"url": "https://server:port", "share": "/backups", "user": ..., "password": ...}); without it the build has
# no online backup. The password goes in as hex so it is not a plain string in the binary.
set(LIVEMIX_BACKUP_ACCOUNT_FILE "C:/Users/claude/SDKs/gocue_release/livemix_backup.json" CACHE FILEPATH "JSON with the backup server account built into LiveMix (not in the repository)")
set(_livemix_backup_defs "")
if(EXISTS "${LIVEMIX_BACKUP_ACCOUNT_FILE}")
    file(READ "${LIVEMIX_BACKUP_ACCOUNT_FILE}" _livemix_backup_json)
    string(JSON _livemix_backup_url GET "${_livemix_backup_json}" url)
    string(JSON _livemix_backup_share GET "${_livemix_backup_json}" share)
    string(JSON _livemix_backup_user GET "${_livemix_backup_json}" user)
    string(JSON _livemix_backup_password GET "${_livemix_backup_json}" password)
    string(HEX "${_livemix_backup_password}" _livemix_backup_password_hex)
    set(_livemix_backup_defs
        LIVEMIX_BACKUP_URL="${_livemix_backup_url}"
        LIVEMIX_BACKUP_SHARE="${_livemix_backup_share}"
        LIVEMIX_BACKUP_USER="${_livemix_backup_user}"
        LIVEMIX_BACKUP_PASSWORD_HEX="${_livemix_backup_password_hex}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LIVEMIX_BACKUP_ACCOUNT_FILE}")
    message(STATUS "LiveMix: backup server account from ${LIVEMIX_BACKUP_ACCOUNT_FILE} (${_livemix_backup_url}${_livemix_backup_share}, ${_livemix_backup_user})")
else()
    message(STATUS "LiveMix: no backup server account file (${LIVEMIX_BACKUP_ACCOUNT_FILE}) - the online backup is off in this build")
endif()

juce_add_gui_app(LiveMix
    PRODUCT_NAME "LiveMix"
    COMPANY_NAME "Gomtwigim"
    VERSION ${LIVEMIX_VERSION}
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/livemix/assets/icon_512.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/livemix/assets/icon_512.png"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

get_target_property(_livemix_generated_dir LiveMix JUCE_GENERATED_SOURCES_DIRECTORY)
set(_livemix_rc_file "${_livemix_generated_dir}/LiveMix_resources.rc")
if(EXISTS "${_livemix_rc_file}")
    file(READ "${_livemix_rc_file}" _livemix_rc_text)
    string(REPLACE "." "," _livemix_version_commas "${LIVEMIX_VERSION}")
    if(NOT _livemix_rc_text MATCHES "FILEVERSION[ \t]+${_livemix_version_commas},0")
        message(STATUS "LiveMix: version resource was stale -> regenerating for ${LIVEMIX_VERSION}")
        file(REMOVE "${_livemix_rc_file}")
    endif()
endif()

target_sources(LiveMix PRIVATE
    livemix/src/Main.cpp
    livemix/src/LiveMixSettings.cpp
    livemix/src/ui/MainComponent.cpp
    livemix/src/ui/TopBar.cpp
    livemix/src/ui/ChannelCard.cpp
    livemix/src/ui/MasterCard.cpp
    livemix/src/ui/ChainDrawer.cpp
    livemix/src/ui/FxDrawer.cpp
    livemix/src/ui/SettingsDialog.cpp
    livemix/src/ui/BackupDialog.cpp
    src/app/Updater.cpp
    src/ui/PluginWindows.cpp)

target_include_directories(LiveMix PRIVATE livemix/src livemix/src/ui)
target_compile_definitions(LiveMix PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:LiveMix,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:LiveMix,JUCE_VERSION>"
    GOCUE_HAS_WINSPARKLE=${GOCUE_HAS_WINSPARKLE}
    GOCUE_APPCAST_URL="${LIVEMIX_APPCAST_URL}"
    GOCUE_EDDSA_PUBLIC_KEY="${GOCUE_EDDSA_PUBLIC_KEY}"
    ${_livemix_backup_defs})
target_link_libraries(LiveMix PRIVATE LiveMixCore)
set_property(TARGET LiveMix PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(GOCUE_HAS_WINSPARKLE)
    target_link_libraries(LiveMix PRIVATE gocue::winsparkle)
    add_custom_command(TARGET LiveMix POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOCUE_WINSPARKLE_DLL}" "$<TARGET_FILE_DIR:LiveMix>"
        COMMENT "Copying WinSparkle.dll next to LiveMix.exe")
endif()

add_subdirectory(tests)
