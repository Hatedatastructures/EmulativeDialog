cmake_minimum_required(VERSION 3.24)

project(AsyncRuntimeVerticalSlice LANGUAGES CXX)

include(CTest)
find_package(Threads REQUIRED)

if(NOT DEFINED EMULATIVE_DIALOG_OFFLINE OR NOT EMULATIVE_DIALOG_OFFLINE)
  message(FATAL_ERROR "EMULATIVE_DIALOG_OFFLINE must be ON for this target")
endif()
if(NOT DEFINED FETCHCONTENT_UPDATES_DISCONNECTED OR
   NOT FETCHCONTENT_UPDATES_DISCONNECTED)
  message(FATAL_ERROR
    "FETCHCONTENT_UPDATES_DISCONNECTED must be ON for this target")
endif()
if(NOT IS_ABSOLUTE "${ASYNC_RUNTIME_REPORT_DIR}")
  message(FATAL_ERROR
    "ASYNC_RUNTIME_REPORT_DIR must be an absolute task build path")
endif()
file(MAKE_DIRECTORY "${ASYNC_RUNTIME_REPORT_DIR}")

get_filename_component(
  PROJECT_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../.."
  ABSOLUTE
)

include("${PROJECT_ROOT}/CMake/BoostAsioDependency.cmake")

add_executable(
  AsyncRuntimeVerticalSliceTests
  "${PROJECT_ROOT}/Source/Agent/AsyncAgentRuntime.cpp"
  "${PROJECT_ROOT}/Source/Agent/OutputBudgetController.cpp"
  "${PROJECT_ROOT}/Source/Core/CoreChatService.cpp"
  "${PROJECT_ROOT}/Source/Core/IdentityPolicy.cpp"
  "${PROJECT_ROOT}/Source/Core/OperationRecord.cpp"
  "${PROJECT_ROOT}/Tests/Contract/Channels/Contracts/MockChannelAdapter.cpp"
  AsyncRuntimeVerticalSliceTests.cpp
)
target_include_directories(
  AsyncRuntimeVerticalSliceTests
  PRIVATE
    "${PROJECT_ROOT}"
)
target_link_libraries(
  AsyncRuntimeVerticalSliceTests
  PRIVATE
    EmulativeDialogBoostAsio
    Threads::Threads
)
target_compile_features(
  AsyncRuntimeVerticalSliceTests
  PRIVATE
    cxx_std_20
)
target_compile_options(
  AsyncRuntimeVerticalSliceTests
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)
set_target_properties(
  AsyncRuntimeVerticalSliceTests
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
add_dependencies(
  AsyncRuntimeVerticalSliceTests
  EmulativeDialogBoostAsioVerify
)

if(BUILD_TESTING)
  add_test(
    NAME Agent.AsyncRuntimeVerticalSlice
    COMMAND AsyncRuntimeVerticalSliceTests
  )
  set_tests_properties(
    Agent.AsyncRuntimeVerticalSlice
    PROPERTIES TIMEOUT 30
  )
endif()
