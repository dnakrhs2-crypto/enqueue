# Pure JS tests do not need a browser, native devices, network or dependencies.
find_program(LIVEMIX_R6_NODE node)
if(LIVEMIX_R6_NODE)
    add_test(NAME LiveMixWebGuestJsTests
        COMMAND "${LIVEMIX_R6_NODE}" "${CMAKE_CURRENT_LIST_DIR}/r6/unit.mjs")
    set_tests_properties(LiveMixWebGuestJsTests PROPERTIES LABELS "livemix017;r6" TIMEOUT 60)
endif()
# HTTP/browser runners are explicit: they start local servers and the browser
# runner must preserve its one-attempt sandbox failure evidence.
