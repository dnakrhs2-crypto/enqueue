# R8 owns this optional LiveMix 0.17 test registration fragment.
target_sources(EnqueueTests PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/LiveMixRecordingEngineTests.cpp"
    "${PROJECT_SOURCE_DIR}/tests/LiveMixRecordingSettingsTests.cpp")
