cmake_minimum_required(VERSION 3.20)
project(EndpointGrantAuthorityContractTests LANGUAGES CXX)

include(CTest)
find_package(Threads REQUIRED)

if(NOT WIN32)
  message(FATAL_ERROR "EndpointGrantAuthority requires the native Windows CNG and Winsock APIs")
endif()

get_filename_component(EMULATIVE_DIALOG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_executable(EndpointGrantAuthorityTests
  "${EMULATIVE_DIALOG_ROOT}/Source/Core/EndpointGrantAuthority.cpp"
  EndpointGrantAuthorityTests.cpp
)
target_include_directories(EndpointGrantAuthorityTests PRIVATE "${EMULATIVE_DIALOG_ROOT}")
target_compile_features(EndpointGrantAuthorityTests PRIVATE cxx_std_20)
target_compile_options(EndpointGrantAuthorityTests PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)
target_compile_definitions(EndpointGrantAuthorityTests PRIVATE
  EMULATIVE_DIALOG_ENABLE_TEST_SEAMS=1
)
target_link_libraries(EndpointGrantAuthorityTests PRIVATE bcrypt ws2_32 Threads::Threads)

add_test(NAME Core.EndpointGrantAuthority COMMAND EndpointGrantAuthorityTests)
