# Offline OpenAI Responses provider contract target.
#
# The root CMakeLists.txt is outside this task's frozen WriteSet, so this file
# is a self-contained project for task validation. Root registration is a
# follow-up integration item once the ED-0143 Boost.JSON target is merged.
cmake_minimum_required(VERSION 3.24)
project(EmulativeDialogOpenAIResponsesContract LANGUAGES CXX)

include(CTest)

set(EMULATIVE_DIALOG_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../..")

add_library(EmulativeDialogOpenAIResponses STATIC
  "${EMULATIVE_DIALOG_ROOT}/Source/Providers/OpenAIResponses/OpenAIResponsesProvider.cpp")
target_include_directories(EmulativeDialogOpenAIResponses
  PUBLIC "${EMULATIVE_DIALOG_ROOT}")
target_compile_features(EmulativeDialogOpenAIResponses PUBLIC cxx_std_20)
set_target_properties(EmulativeDialogOpenAIResponses
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

if(NOT TARGET EmulativeDialogBoostJson)
  if(NOT DEFINED EMULATIVE_DIALOG_BOOST_JSON_INCLUDE_DIR OR
     NOT DEFINED EMULATIVE_DIALOG_BOOST_JSON_LIBRARY)
    message(FATAL_ERROR
      "EmulativeDialogBoostJson (ED-0143) is not available. For diagnostic "
      "standalone verification pass "
      "-DEMULATIVE_DIALOG_BOOST_JSON_INCLUDE_DIR and "
      "-DEMULATIVE_DIALOG_BOOST_JSON_LIBRARY.")
  endif()
  add_library(EmulativeDialogBoostJson STATIC IMPORTED GLOBAL)
  set_target_properties(EmulativeDialogBoostJson
    PROPERTIES
      IMPORTED_LOCATION "${EMULATIVE_DIALOG_BOOST_JSON_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES
        "${EMULATIVE_DIALOG_BOOST_JSON_INCLUDE_DIR}")
  message(WARNING
    "Using a diagnostic imported Boost.JSON for standalone verification; "
    "the final root build links ED-0143 EmulativeDialogBoostJson.")
endif()
target_link_libraries(EmulativeDialogOpenAIResponses
  PUBLIC EmulativeDialogBoostJson)

add_executable(OpenAIResponsesProviderTests
  OpenAIResponsesProviderTests.cpp)
target_link_libraries(OpenAIResponsesProviderTests
  PRIVATE EmulativeDialogOpenAIResponses)
target_compile_features(OpenAIResponsesProviderTests PRIVATE cxx_std_20)
target_compile_definitions(OpenAIResponsesProviderTests
  PRIVATE
    "EMULATIVE_DIALOG_RESPONSES_FIXTURE_DIR=\"${EMULATIVE_DIALOG_ROOT}/Tests/Contract/Providers/OpenAIResponses/Fixtures\"")
set_target_properties(OpenAIResponsesProviderTests
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

if(BUILD_TESTING)
  add_test(
    NAME Provider.OpenAIResponses
    COMMAND $<TARGET_FILE:OpenAIResponsesProviderTests>)
endif()
