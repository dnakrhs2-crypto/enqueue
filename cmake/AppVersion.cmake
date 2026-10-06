# Keep version changes local to one translation unit per executable. configure_file
# preserves the source timestamp when the target's version has not changed.
function(gocue_add_app_version target)
    get_target_property(APP_VERSION ${target} JUCE_VERSION)
    set(_app_version_source "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}/AppVersion.cpp")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AppVersion.cpp.in" "${_app_version_source}" @ONLY)
    target_sources(${target} PRIVATE "${_app_version_source}")
endfunction()
