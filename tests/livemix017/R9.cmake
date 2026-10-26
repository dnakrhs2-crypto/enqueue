# R9 owns this optional LiveMix 0.17 test registration fragment.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME LiveMixR9Analysis COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/livemix017/r9/test_analysis.py")
    set_tests_properties(LiveMixR9Analysis PROPERTIES LABELS "LiveMix;R9" TIMEOUT 60)
endif()
