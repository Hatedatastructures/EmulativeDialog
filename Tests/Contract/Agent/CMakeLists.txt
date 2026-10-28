cmake_minimum_required(VERSION 3.20)
project(AgentRuntimeContract LANGUAGES CXX)

include(CTest)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED GCOV_EXECUTABLE OR GCOV_EXECUTABLE STREQUAL "")
  message(FATAL_ERROR "GCOV_EXECUTABLE must be set to the pinned gcov executable")
endif()
if(NOT IS_ABSOLUTE "${AGENT_RUNTIME_REPORT_DIR}")
  message(FATAL_ERROR "AGENT_RUNTIME_REPORT_DIR must be an absolute task build path")
endif()

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(AGENT_RUNTIME_SOURCE "${PROJECT_ROOT}/Source/Agent/AgentRuntime.cpp")
set(AGENT_RUNTIME_CONTRACT_TEST "${CMAKE_CURRENT_LIST_DIR}/AgentRuntimeContractTests.cpp")
set(AGENT_RUNTIME_VERTICAL_TEST "${CMAKE_CURRENT_LIST_DIR}/AgentRuntimeVerticalSliceTests.cpp")
set(CORE_SOURCES
    "${PROJECT_ROOT}/Source/Core/IdentityPolicy.cpp"
    "${PROJECT_ROOT}/Source/Core/OperationRecord.cpp"
    "${PROJECT_ROOT}/Source/Core/CoreChatService.cpp"
    "${PROJECT_ROOT}/Tests/Contract/Providers/Contracts/FakeProvider.cpp")

file(MAKE_DIRECTORY "${AGENT_RUNTIME_REPORT_DIR}")

# A single instrumented object is shared by both test executables so gcov
# reports one AgentRuntime.cpp record with counters from both CTest runs.
add_library(AgentRuntimeObject OBJECT "${AGENT_RUNTIME_SOURCE}")
target_include_directories(AgentRuntimeObject PUBLIC "${PROJECT_ROOT}")
target_compile_options(AgentRuntimeObject PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

add_executable(AgentRuntimeContractTests
    ${CORE_SOURCES}
    "${AGENT_RUNTIME_CONTRACT_TEST}")
target_include_directories(AgentRuntimeContractTests PRIVATE "${PROJECT_ROOT}")
target_compile_options(AgentRuntimeContractTests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(AgentRuntimeContractTests PRIVATE AgentRuntimeObject)

add_executable(AgentRuntimeVerticalSliceTests
    ${CORE_SOURCES}
    "${PROJECT_ROOT}/Tests/Contract/Channels/Contracts/MockChannelAdapter.cpp"
    "${AGENT_RUNTIME_VERTICAL_TEST}")
target_include_directories(AgentRuntimeVerticalSliceTests PRIVATE "${PROJECT_ROOT}")
target_compile_options(AgentRuntimeVerticalSliceTests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(AgentRuntimeVerticalSliceTests PRIVATE AgentRuntimeObject)

add_test(NAME AgentRuntime.Contract COMMAND AgentRuntimeContractTests)
add_test(NAME AgentRuntime.VerticalSlice COMMAND AgentRuntimeVerticalSliceTests)

set(INSTRUCTION_POLICY_SOURCE
    "${PROJECT_ROOT}/Source/Core/InstructionPolicy.cpp")
set(INSTRUCTION_ASSEMBLER_SOURCE
    "${PROJECT_ROOT}/Source/Agent/InstructionAssembler.cpp")

add_executable(InstructionAssemblerTests
    "${INSTRUCTION_POLICY_SOURCE}"
    "${INSTRUCTION_ASSEMBLER_SOURCE}"
    "${CMAKE_CURRENT_LIST_DIR}/InstructionAssemblerTests.cpp")
target_include_directories(InstructionAssemblerTests PRIVATE "${PROJECT_ROOT}")
target_compile_options(InstructionAssemblerTests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

add_test(NAME Agent.InstructionAssembler COMMAND InstructionAssemblerTests)

add_custom_target(AgentRuntimeCoverageReport
    COMMAND "${GCOV_EXECUTABLE}" --json-format --branch-counts
            -o "$<TARGET_OBJECTS:AgentRuntimeObject>" "${AGENT_RUNTIME_SOURCE}"
    WORKING_DIRECTORY "${AGENT_RUNTIME_REPORT_DIR}"
    COMMAND_EXPAND_LISTS
    VERBATIM
    COMMENT "Generating the AgentRuntime.cpp gcov JSON v2 report")
add_dependencies(AgentRuntimeCoverageReport
    AgentRuntimeContractTests
    AgentRuntimeVerticalSliceTests)
