cmake_minimum_required(VERSION 3.24)

project(EmulativeDialog LANGUAGES CXX)

include(CTest)
include(CMake/DependencyPins.cmake)

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
