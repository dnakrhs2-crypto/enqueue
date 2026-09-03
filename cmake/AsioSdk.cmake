# Locates the Steinberg ASIO SDK.
#
# The SDK licence does not allow redistribution, so it is never vendored into
# this repository. Download it from https://www.steinberg.net/asiosdk, extract
# it anywhere, and point ASIO_SDK_DIR at the folder that contains
# common/iasiodrv.h (e.g. C:/SDKs/ASIOSDK). The environment variable
# ASIO_SDK_DIR is honoured as a fallback (handy for CI).
#
# Outputs (in the including scope):
#   GOCUE_HAS_ASIO          1 when the SDK was found, else 0
#   GOCUE_ASIO_INCLUDE_DIR  include directory to add when GOCUE_HAS_ASIO is 1

set(ASIO_SDK_DIR "" CACHE PATH "Steinberg ASIO SDK root (the folder containing common/iasiodrv.h)")

if(NOT ASIO_SDK_DIR AND DEFINED ENV{ASIO_SDK_DIR})
    set(ASIO_SDK_DIR "$ENV{ASIO_SDK_DIR}")
endif()

set(GOCUE_HAS_ASIO 0)
set(GOCUE_ASIO_INCLUDE_DIR "")

if(ASIO_SDK_DIR)
    file(TO_CMAKE_PATH "${ASIO_SDK_DIR}" _gocue_asio_dir)
    if(EXISTS "${_gocue_asio_dir}/common/iasiodrv.h")
        set(GOCUE_HAS_ASIO 1)
        set(GOCUE_ASIO_INCLUDE_DIR "${_gocue_asio_dir}/common")
        message(STATUS "Enqueue: ASIO SDK found at ${_gocue_asio_dir} -> JUCE_ASIO=1")
    else()
        message(WARNING "Enqueue: ASIO_SDK_DIR='${_gocue_asio_dir}' does not contain common/iasiodrv.h -> building WITHOUT ASIO (WASAPI only)")
    endif()
else()
    message(WARNING "Enqueue: ASIO_SDK_DIR is not set -> building WITHOUT ASIO (WASAPI only). See README.md.")
endif()
