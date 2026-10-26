target_sources(LiveMixUI INTERFACE "${PROJECT_SOURCE_DIR}/livemix/src/ui/RecordingWindow.cpp")
target_link_libraries(LiveMixUI INTERFACE EnqueueQrCode)
