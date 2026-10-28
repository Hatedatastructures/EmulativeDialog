cmake_minimum_required(VERSION 3.24)

project(EmulativeDialog LANGUAGES C CXX)

include(CTest)
include(CMake/DependencyPins.cmake)
include(CMake/BoostAsioDependency.cmake)

set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(YamlCpp)

add_executable(BuildProbe CMake/BuildProbe.cpp)
target_compile_features(BuildProbe PRIVATE cxx_std_20)
set_target_properties(
  BuildProbe
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

add_executable(BoostAsioCoroutineProbe CMake/BoostAsioCoroutineProbe.cpp)
target_link_libraries(BoostAsioCoroutineProbe PRIVATE EmulativeDialogBoostAsio)
target_compile_features(BoostAsioCoroutineProbe PRIVATE cxx_std_20)
add_dependencies(BoostAsioCoroutineProbe EmulativeDialogBoostAsioVerify)
set_target_properties(
  BoostAsioCoroutineProbe
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(BUILD_TESTING)
  add_test(NAME BuildProbe.Cxx20 COMMAND $<TARGET_FILE:BuildProbe>)
  add_test(
    NAME BuildProbe.BoostAsioCxx20
    COMMAND $<TARGET_FILE:BoostAsioCoroutineProbe>
  )
  add_subdirectory(Tests/Contract/Agent)
  add_subdirectory(Tests/Contract/Storage)
endif()

add_library(WorkflowControllerCore STATIC
  Tools/WorkflowController/WorkflowController.cpp
  Tools/WorkflowController/WindowsProcessRunner.cpp
)
target_include_directories(WorkflowControllerCore PUBLIC Tools/WorkflowController)
target_compile_features(WorkflowControllerCore PUBLIC cxx_std_20)
target_link_libraries(WorkflowControllerCore PUBLIC yaml-cpp::yaml-cpp)
set_target_properties(
  WorkflowControllerCore
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

add_executable(WorkflowController Tools/WorkflowController/Main.cpp)
target_link_libraries(WorkflowController PRIVATE WorkflowControllerCore)
target_compile_features(WorkflowController PRIVATE cxx_std_20)

add_executable(WorkflowControllerContractTests
  Tests/Contract/WorkflowController/WorkflowControllerContractTests.cpp
  Tests/Contract/WorkflowController/WorkflowControllerTestWorkspace.hpp
)
target_link_libraries(WorkflowControllerContractTests PRIVATE WorkflowControllerCore)
target_compile_features(WorkflowControllerContractTests PRIVATE cxx_std_20)
add_test(
  NAME WorkflowController.Contract
  COMMAND $<TARGET_FILE:WorkflowControllerContractTests>
)

if(BUILD_TESTING)
  set(WORKFLOW_CONTROLLER_COVERAGE_REPORT_DIR
      "${AGENT_RUNTIME_REPORT_DIR}/WorkflowController")
  set(WORKFLOW_CONTROLLER_COVERAGE_REPORT_FILE
      "${WORKFLOW_CONTROLLER_COVERAGE_REPORT_DIR}/WorkflowControllerContractTests.cpp.gcov.json.gz")
  set(WORKFLOW_CONTROLLER_GCOV_RAW_REPORT_FILE
      "${WORKFLOW_CONTROLLER_COVERAGE_REPORT_DIR}/WorkflowControllerContractTests.gcov.json.gz")
  file(MAKE_DIRECTORY "${WORKFLOW_CONTROLLER_COVERAGE_REPORT_DIR}")
  add_custom_target(WorkflowControllerTestWorkspaceCoverageReport
    BYPRODUCTS "${WORKFLOW_CONTROLLER_COVERAGE_REPORT_FILE}"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${WORKFLOW_CONTROLLER_GCOV_RAW_REPORT_FILE}"
    COMMAND "${GCOV_EXECUTABLE}" --json-format --branch-counts
            -o "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/WorkflowControllerContractTests.dir/Tests/Contract/WorkflowController/WorkflowControllerContractTests.cpp.obj"
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Contract/WorkflowController/WorkflowControllerContractTests.cpp"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${WORKFLOW_CONTROLLER_COVERAGE_REPORT_FILE}"
    COMMAND "${CMAKE_COMMAND}" -E rename "${WORKFLOW_CONTROLLER_GCOV_RAW_REPORT_FILE}"
            "${WORKFLOW_CONTROLLER_COVERAGE_REPORT_FILE}"
    WORKING_DIRECTORY "${WORKFLOW_CONTROLLER_COVERAGE_REPORT_DIR}"
    VERBATIM
    COMMENT "Generating WorkflowController test-workspace gcov JSON v2 coverage")
  add_dependencies(WorkflowControllerTestWorkspaceCoverageReport
                   WorkflowControllerContractTests)
endif()
