# Only Enqueue and its tests link this target. GoCueCore also belongs to LiveMix.
add_library(EnqueueCivetWeb STATIC "${PROJECT_SOURCE_DIR}/third_party/civetweb/src/civetweb.c")
target_include_directories(EnqueueCivetWeb PUBLIC "${PROJECT_SOURCE_DIR}/third_party/civetweb/include")
target_compile_definitions(EnqueueCivetWeb PRIVATE NO_SSL USE_WEBSOCKET NO_CGI NO_FILES NO_CACHING)
target_compile_options(EnqueueCivetWeb PRIVATE $<$<C_COMPILER_ID:MSVC>:/utf-8>)
set_property(TARGET EnqueueCivetWeb PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(EnqueueCivetWeb PUBLIC ws2_32)

add_library(EnqueueQrCode STATIC "${PROJECT_SOURCE_DIR}/third_party/qrcodegen/qrcodegen.cpp")
target_include_directories(EnqueueQrCode PUBLIC "${PROJECT_SOURCE_DIR}/third_party/qrcodegen")
set_property(TARGET EnqueueQrCode PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

file(GLOB _remote_icons CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/remote/web/icons/*.svg")
juce_add_binary_data(EnqueueRemoteWeb NAMESPACE RemoteWebData HEADER_NAME RemoteWebData.h SOURCES
    "${PROJECT_SOURCE_DIR}/src/remote/web/index.html"
    "${PROJECT_SOURCE_DIR}/src/remote/web/app.css"
    "${PROJECT_SOURCE_DIR}/src/remote/web/app.js"
    ${_remote_icons})
set_property(TARGET EnqueueRemoteWeb PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(EnqueueRemote INTERFACE)
target_sources(EnqueueRemote INTERFACE
    "${PROJECT_SOURCE_DIR}/src/remote/RemoteProtocol.cpp"
    "${PROJECT_SOURCE_DIR}/src/remote/RemoteServer.cpp"
    "${PROJECT_SOURCE_DIR}/src/remote/RemoteNetwork.cpp"
    "${PROJECT_SOURCE_DIR}/src/remote/RemoteSnapshot.cpp"
    "${PROJECT_SOURCE_DIR}/src/remote/RemoteService.cpp"
    "${PROJECT_SOURCE_DIR}/src/remote/RemoteWindow.cpp"
    "${PROJECT_SOURCE_DIR}/src/remote/MainComponentRemote.cpp")
target_link_libraries(EnqueueRemote INTERFACE EnqueueCivetWeb EnqueueQrCode EnqueueRemoteWeb iphlpapi bcrypt)
