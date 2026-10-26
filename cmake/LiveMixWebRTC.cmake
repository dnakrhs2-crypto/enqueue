include_guard(GLOBAL)

# Keep dependency options scoped here, including policies reset by old subprojects.
function(livemix_add_webrtc)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_SHARED_DEPS_LIBS OFF)
    set(ENABLE_PROGRAMS OFF)
    set(ENABLE_TESTING OFF)
    set(USE_STATIC_MBEDTLS_LIBRARY ON)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF)
    set(MSVC_STATIC_RUNTIME ON)
    set(GEN_FILES OFF)
    # The release already contains generated sources; its optional Python config
    # warning check needs scripts/ which is intentionally not in the vendored set.
    set(CMAKE_DISABLE_FIND_PACKAGE_Python3 TRUE)
    set(Python3_Interpreter_FOUND FALSE)
    add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/mbedtls"
                     "${PROJECT_BINARY_DIR}/third_party/mbedtls" EXCLUDE_FROM_ALL)
    # libdatachannel negotiates the baseline DTLS use_srtp extension even with
    # NO_MEDIA. This enables only negotiation/key export, not SRTP media handling.
    # Keep ssl.h's layout identical in the library and every consumer.
    foreach(tls_target mbedtls mbedx509 mbedcrypto)
        target_compile_definitions(${tls_target} PUBLIC MBEDTLS_SSL_DTLS_SRTP)
    endforeach()

    add_library(MbedTLS::MbedTLS INTERFACE IMPORTED GLOBAL)
    set_target_properties(MbedTLS::MbedTLS PROPERTIES
        INTERFACE_LINK_LIBRARIES "mbedtls;mbedx509;mbedcrypto"
        INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/third_party/mbedtls/include")

    set(NO_MEDIA ON)
    set(NO_WEBSOCKET ON)
    set(NO_EXAMPLES ON)
    set(NO_TESTS ON)
    set(USE_MBEDTLS ON)
    set(USE_GNUTLS OFF)
    set(USE_NICE OFF)
    set(PREFER_SYSTEM_LIB OFF)
    set(USE_SYSTEM_JUICE OFF)
    set(USE_SYSTEM_USRSCTP OFF)
    set(USE_SYSTEM_PLOG OFF)
    set(sctp_build_shared_lib OFF)
    set(sctp_build_programs OFF)
    set(sctp_werror OFF)
    add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/libdatachannel"
                     "${PROJECT_BINARY_DIR}/third_party/libdatachannel" EXCLUDE_FROM_ALL)
    # libdatachannel also declares a duplicate 'datachannel' target. Only its
    # explicitly static target (and juice-static) is linked into LiveMix.
    set_target_properties(datachannel juice PROPERTIES EXCLUDE_FROM_ALL TRUE)
    foreach(dependency mbedtls mbedx509 mbedcrypto everest p256m
                       datachannel datachannel-static juice juice-static usrsctp)
        if(TARGET ${dependency})
            set_property(TARGET ${dependency} PROPERTY MSVC_RUNTIME_LIBRARY
                         "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    endforeach()
endfunction()
livemix_add_webrtc()

target_sources(LiveMixCore INTERFACE "${PROJECT_SOURCE_DIR}/livemix/src/WebGuestTransport.cpp")
target_link_libraries(LiveMixCore INTERFACE datachannel-static)

juce_add_console_app(LiveMixWebGuestProbe PRODUCT_NAME "LiveMixWebGuestProbe" VERSION ${LIVEMIX_VERSION})
gocue_add_app_version(LiveMixWebGuestProbe)
target_sources(LiveMixWebGuestProbe PRIVATE "${PROJECT_SOURCE_DIR}/livemix/tools/WebGuestProbe.cpp")
target_compile_definitions(LiveMixWebGuestProbe PRIVATE
    JUCE_APPLICATION_NAME_STRING="LiveMixWebGuestProbe"
    GOCUE_HAS_WINSPARKLE=0)
target_link_libraries(LiveMixWebGuestProbe PRIVATE LiveMixCore winmm)
set_target_properties(LiveMixWebGuestProbe PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" EXCLUDE_FROM_ALL TRUE)
