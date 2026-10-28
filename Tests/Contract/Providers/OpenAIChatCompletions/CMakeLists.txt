cmake_minimum_required(VERSION 3.24)

# This directory is consumable both from the root project (add_subdirectory) and
# as a standalone contract project while the provider integration task lands.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(OpenAIChatCompletionsProviderContract LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.."
                       ABSOLUTE)

if(NOT TARGET EmulativeDialogBoostJson)
  if(EXISTS "${PROJECT_ROOT}/CMake/BoostJsonDependency.cmake")
    include("${PROJECT_ROOT}/CMake/BoostJsonDependency.cmake")
  else()
    message(FATAL_ERROR
      "EmulativeDialogBoostJson is unavailable. ED-0143 must add "
      "CMake/BoostJsonDependency.cmake before provider adapter tests can build.")
  endif()
endif()

set(PROVIDER_SOURCE
    "${PROJECT_ROOT}/Source/Providers/OpenAIChatCompletions/ChatCompletionsProvider.cpp")

add_library(OpenAIChatCompletionsProvider STATIC "${PROVIDER_SOURCE}")
target_include_directories(OpenAIChatCompletionsProvider
                           PUBLIC "${PROJECT_ROOT}")
target_compile_features(OpenAIChatCompletionsProvider PUBLIC cxx_std_20)
target_compile_options(OpenAIChatCompletionsProvider PRIVATE
                       -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(OpenAIChatCompletionsProvider
                      PUBLIC EmulativeDialogBoostJson)

add_executable(ChatCompletionsProviderTests
               "${CMAKE_CURRENT_LIST_DIR}/ChatCompletionsProviderTests.cpp")
target_link_libraries(ChatCompletionsProviderTests
                      PRIVATE OpenAIChatCompletionsProvider)
target_compile_features(ChatCompletionsProviderTests PRIVATE cxx_std_20)
target_compile_options(ChatCompletionsProviderTests PRIVATE
                       -Wall -Wextra -Wpedantic -Werror)

include(CTest)
add_test(NAME Provider.ChatCompletions COMMAND ChatCompletionsProviderTests)

if(DEFINED GCOV_EXECUTABLE AND NOT GCOV_EXECUTABLE STREQUAL ""
   AND DEFINED AGENT_RUNTIME_REPORT_DIR
   AND IS_ABSOLUTE "${AGENT_RUNTIME_REPORT_DIR}")
  set(PROVIDER_REPORT_DIR
      "${AGENT_RUNTIME_REPORT_DIR}/OpenAIChatCompletions")
  file(MAKE_DIRECTORY "${PROVIDER_REPORT_DIR}")
  add_custom_target(OpenAIChatCompletionsCoverageReport
    COMMAND "${GCOV_EXECUTABLE}" --json-format --branch-counts
            -o "$<TARGET_OBJECTS:OpenAIChatCompletionsProvider>"
            "${PROVIDER_SOURCE}"
    WORKING_DIRECTORY "${PROVIDER_REPORT_DIR}"
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Generating ChatCompletionsProvider.cpp gcov JSON v2 report")
  add_dependencies(OpenAIChatCompletionsCoverageReport
                   ChatCompletionsProviderTests)
endif()
