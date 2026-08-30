# Locates the WinSparkle binary package (https://github.com/vslavik/winsparkle/releases).
#
#   -DWINSPARKLE_DIR=C:/path/to/WinSparkle-0.9.4
#
# The folder must contain include/winsparkle.h and x64/Release/WinSparkle.{dll,lib}.
# The environment variable WINSPARKLE_DIR is honoured as a fallback.
#
# Outputs (in the including scope):
#   GOCUE_HAS_WINSPARKLE   1 when found, else 0
#   GOCUE_WINSPARKLE_DLL   full path of WinSparkle.dll (to copy next to the exe)
#   gocue::winsparkle      imported shared library target

set(WINSPARKLE_DIR "" CACHE PATH "WinSparkle binary package root (contains include/winsparkle.h)")

if(NOT WINSPARKLE_DIR AND DEFINED ENV{WINSPARKLE_DIR})
    set(WINSPARKLE_DIR "$ENV{WINSPARKLE_DIR}")
endif()

set(GOCUE_HAS_WINSPARKLE 0)
set(GOCUE_WINSPARKLE_DLL "")

if(WINSPARKLE_DIR)
    file(TO_CMAKE_PATH "${WINSPARKLE_DIR}" _gocue_ws)
    set(_gocue_ws_header "${_gocue_ws}/include/winsparkle.h")
    set(_gocue_ws_lib "${_gocue_ws}/x64/Release/WinSparkle.lib")
    set(_gocue_ws_dll "${_gocue_ws}/x64/Release/WinSparkle.dll")

    if(EXISTS "${_gocue_ws_header}" AND EXISTS "${_gocue_ws_lib}" AND EXISTS "${_gocue_ws_dll}")
        if(NOT TARGET gocue::winsparkle)
            add_library(gocue::winsparkle SHARED IMPORTED)
            set_target_properties(gocue::winsparkle PROPERTIES
                IMPORTED_LOCATION "${_gocue_ws_dll}"
                IMPORTED_IMPLIB "${_gocue_ws_lib}"
                INTERFACE_INCLUDE_DIRECTORIES "${_gocue_ws}/include")
        endif()

        set(GOCUE_HAS_WINSPARKLE 1)
        set(GOCUE_WINSPARKLE_DLL "${_gocue_ws_dll}")
        message(STATUS "GoCue: WinSparkle found at ${_gocue_ws}")
    else()
        message(WARNING "GoCue: WINSPARKLE_DIR='${_gocue_ws}' is missing include/winsparkle.h or x64/Release/WinSparkle.{lib,dll} -> building WITHOUT auto-update")
    endif()
else()
    message(STATUS "GoCue: WINSPARKLE_DIR not set -> building WITHOUT auto-update")
endif()
