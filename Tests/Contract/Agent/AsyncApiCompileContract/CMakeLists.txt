cmake_minimum_required(VERSION 3.24)

project(AsyncApiCompileContract LANGUAGES CXX)

include(CTest)

get_filename_component(
  PROJECT_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../.."
  ABSOLUTE
)

include("${PROJECT_ROOT}/CMake/BoostAsioDependency.cmake")

add_executable(
  AsyncApiCompileContractTests
  AsyncApiCompileContract.cpp
)
target_include_directories(
  AsyncApiCompileContractTests
  PRIVATE
    "${PROJECT_ROOT}"
)
target_link_libraries(
  AsyncApiCompileContractTests
  PRIVATE
    EmulativeDialogBoostAsio
)
target_compile_features(
  AsyncApiCompileContractTests
  PRIVATE
    cxx_std_20
)
target_compile_options(
  AsyncApiCompileContractTests
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)
set_target_properties(
  AsyncApiCompileContractTests
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
add_dependencies(
  AsyncApiCompileContractTests
  EmulativeDialogBoostAsioVerify
)

if(BUILD_TESTING)
  add_test(
    NAME Agent.AsyncApiCompileContract
    COMMAND AsyncApiCompileContractTests
  )
endif()