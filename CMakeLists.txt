cmake_minimum_required(VERSION 3.24)

project(EmulativeDialog LANGUAGES CXX)

include(CTest)
include(CMake/DependencyPins.cmake)

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

if(BUILD_TESTING)
  add_test(NAME BuildProbe.Cxx20 COMMAND $<TARGET_FILE:BuildProbe>)
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
)
target_link_libraries(WorkflowControllerContractTests PRIVATE WorkflowControllerCore)
target_compile_features(WorkflowControllerContractTests PRIVATE cxx_std_20)
add_test(
  NAME WorkflowController.Contract
  COMMAND $<TARGET_FILE:WorkflowControllerContractTests>
)
