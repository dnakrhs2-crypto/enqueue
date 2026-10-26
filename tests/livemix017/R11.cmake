# Browser-free preference/copy checks; the explicit Python runner owns browser QA.
find_program(LIVEMIX_R11_NODE node)
if(LIVEMIX_R11_NODE)
    add_test(NAME LiveMixWebJoinUiTests
        COMMAND "${LIVEMIX_R11_NODE}" "${CMAKE_CURRENT_LIST_DIR}/r11/unit.mjs")
    set_tests_properties(LiveMixWebJoinUiTests PROPERTIES LABELS "livemix017;r11" TIMEOUT 30)
endif()
