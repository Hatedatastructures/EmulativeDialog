cmake_minimum_required(VERSION 3.24)

project(EmulativeDialog LANGUAGES C CXX)

include(CTest)
include(CMake/DependencyPins.cmake)
include(CMake/BoostAsioDependency.cmake)
include(CMake/BoostJsonDependency.cmake)

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

add_executable(BoostJsonProbe CMake/BoostJsonProbe.cpp)
target_link_libraries(BoostJsonProbe PRIVATE EmulativeDialogBoostJson)
target_compile_features(BoostJsonProbe PRIVATE cxx_std_20)
add_dependencies(BoostJsonProbe EmulativeDialogBoostJsonVerify)
if(WIN32 AND MINGW)
  target_link_options(
    BoostJsonProbe
    PRIVATE
      "${EmulativeDialogBoostJsonNoPthreadLinkOption}")
  target_link_libraries(
    BoostJsonProbe
    PRIVATE
      "${EmulativeDialogBoostJsonWinPthreadLinkOption}")
endif()
set_target_properties(
  BoostJsonProbe
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
  add_test(
    NAME BuildProbe.BoostJsonCxx20
    COMMAND $<TARGET_FILE:BoostJsonProbe>
  )
  set(EmulativeDialogBoostJsonMissingSubmoduleSource
      "${CMAKE_CURRENT_BINARY_DIR}/BoostJsonMissingSubmoduleFixture")
  foreach(Submodule IN LISTS EmulativeDialogBoostJsonRequiredSubmodules)
    if(Submodule STREQUAL "libs/json")
      file(MAKE_DIRECTORY
           "${EmulativeDialogBoostJsonMissingSubmoduleSource}/libs/json")
    else()
      file(MAKE_DIRECTORY
           "${EmulativeDialogBoostJsonMissingSubmoduleSource}/${Submodule}/include")
    endif()
  endforeach()
  set(EmulativeDialogBoostJsonMissingSubmoduleTestScript
      "${CMAKE_CURRENT_BINARY_DIR}/VerifyMissingBoostJsonSubmodule.cmake")
  file(WRITE "${EmulativeDialogBoostJsonMissingSubmoduleTestScript}" "
execute_process(
  COMMAND \"${CMAKE_COMMAND}\"
          \"-DEmulativeDialogBoostJsonVerifySourceDir=${EmulativeDialogBoostJsonMissingSubmoduleSource}\"
          -P \"${EmulativeDialogBoostJsonVerifyScript}\"
  RESULT_VARIABLE VerifyResult
  OUTPUT_VARIABLE VerifyOutput
  ERROR_VARIABLE VerifyError)
if(VerifyResult EQUAL 0)
  message(FATAL_ERROR \"Boost.JSON verification accepted an empty JSON gitlink\")
endif()
if(NOT VerifyError MATCHES \"Pinned Boost submodule is incomplete: libs/json\")
  message(FATAL_ERROR \"Unexpected missing-gitlink failure: \${VerifyError}\")
endif()
message(STATUS \"Missing pinned Boost.JSON gitlink fails closed without network access\")
")
  add_test(
    NAME Dependency.BoostJsonMissingSubmoduleFails
    COMMAND "${CMAKE_COMMAND}" -P
            "${EmulativeDialogBoostJsonMissingSubmoduleTestScript}")
  if(WIN32 AND MINGW)
    set(EmulativeDialogBoostJsonConsumerLinkScript
        "${CMAKE_CURRENT_BINARY_DIR}/VerifyBoostJsonConsumerLink.cmake")
    file(WRITE "${EmulativeDialogBoostJsonConsumerLinkScript}" "
set(LinkFile \"${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/BoostJsonProbe.dir/link.txt\")
if(NOT EXISTS \"\${LinkFile}\")
  message(FATAL_ERROR \"BoostJsonProbe linker command is missing: \${LinkFile}\")
endif()
file(READ \"\${LinkFile}\" LinkCommand)
set(LinkLibrariesFile
    \"${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/BoostJsonProbe.dir/linkLibs.rsp\")
if(NOT EXISTS \"\${LinkLibrariesFile}\")
  message(FATAL_ERROR
    \"BoostJsonProbe linker response file is missing: \${LinkLibrariesFile}\")
endif()
file(READ \"\${LinkLibrariesFile}\" LinkLibraries)
string(APPEND LinkCommand \"\\n\${LinkLibraries}\")
string(FIND \"\${LinkCommand}\" \"libEmulativeDialogBoostJson.a\"
  BoostJsonLibraryPosition)
if(BoostJsonLibraryPosition EQUAL -1)
  message(FATAL_ERROR
    \"BoostJsonProbe link line does not reference the static implementation library: \${LinkCommand}\")
endif()
string(FIND \"\${LinkCommand}\"
  \"${EmulativeDialogBoostJsonWinPthreadLinkOption}\"
  WinPthreadOptionPosition)
if(WinPthreadOptionPosition EQUAL -1)
  message(FATAL_ERROR
    \"BoostJsonProbe does not statically link winpthread: \${LinkCommand}\")
endif()
if(WinPthreadOptionPosition LESS BoostJsonLibraryPosition)
  message(FATAL_ERROR
    \"BoostJsonProbe places static winpthread before the unresolved symbols in EmulativeDialogBoostJson: \${LinkCommand}\")
endif()
string(FIND \"\${LinkCommand}\"
  \"${EmulativeDialogBoostJsonNoPthreadLinkOption}\"
  NoPthreadOptionPosition)
if(NoPthreadOptionPosition EQUAL -1)
  message(FATAL_ERROR
    \"BoostJsonProbe allows GCC to append dynamic pthread implicitly: \${LinkCommand}\")
endif()
message(STATUS \"Verified BoostJsonProbe consumer link options\")
")
    add_test(
      NAME Dependency.BoostJsonWinPthreadLink
      COMMAND "${CMAKE_COMMAND}" -P
              "${EmulativeDialogBoostJsonConsumerLinkScript}")
  endif()
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
