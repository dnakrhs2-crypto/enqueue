cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
option(LIVEMIX_OBS_TESTS "Build the OBS-independent LiveMix receiver tests" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs advapi32)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/livemix-obs.h
    src/livemix-source.c
    src/livemix-filter.c
    src/receiver.h
    src/receiver.c
    ../livemix/shared/lm_asrc.c
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src ../livemix/shared)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(LIVEMIX_OBS_TESTS)
  enable_testing()
  add_executable(livemix-obs-core-tests src/receiver.c ../livemix/shared/lm_asrc.c tests/core_tests.c)
  target_include_directories(livemix-obs-core-tests PRIVATE src ../livemix/shared)
  target_link_libraries(livemix-obs-core-tests PRIVATE advapi32)
  add_test(NAME livemix-obs-core-tests COMMAND livemix-obs-core-tests)
endif()
