# Offline WinHTTP transport contract target.
#
# The root CMakeLists.txt is outside this task's frozen WriteSet, so this file
# is a self-contained project for the task validation command. Integration into
# the root build is a reported follow-up gap.
cmake_minimum_required(VERSION 3.24)
project(EmulativeDialogWinHttpTransportContract LANGUAGES CXX)

include(CTest)

set(EMULATIVE_DIALOG_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../..")

add_library(EmulativeDialogWinHttpTransport STATIC
  "${EMULATIVE_DIALOG_ROOT}/Source/Platform/Network/WinHttpTransport.cpp")
target_include_directories(EmulativeDialogWinHttpTransport
  PUBLIC "${EMULATIVE_DIALOG_ROOT}")
target_compile_features(EmulativeDialogWinHttpTransport PUBLIC cxx_std_20)
set_target_properties(EmulativeDialogWinHttpTransport
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

# Compile out the system binding for the offline contract target. Production
# consumers must not define this; they link winhttp and use the real API.
target_compile_definitions(EmulativeDialogWinHttpTransport
  PRIVATE EMULATIVE_DIALOG_WINHTTP_OFFLINE_TESTS)

if(WIN32)
  target_link_libraries(EmulativeDialogWinHttpTransport PRIVATE winhttp)
endif()

add_executable(HttpTransportPortTests HttpTransportPortTests.cpp)
target_include_directories(HttpTransportPortTests
  PRIVATE "${EMULATIVE_DIALOG_ROOT}")
target_compile_features(HttpTransportPortTests PRIVATE cxx_std_20)
target_compile_options(HttpTransportPortTests
  PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(HttpTransportPortTests
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

add_executable(WinHttpTransportTests WinHttpTransportTests.cpp)
target_link_libraries(WinHttpTransportTests
  PRIVATE EmulativeDialogWinHttpTransport)
target_compile_features(WinHttpTransportTests PRIVATE cxx_std_20)
set_target_properties(WinHttpTransportTests
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

if(BUILD_TESTING)
  add_test(
    NAME Platform.Network.HttpTransportPort
    COMMAND $<TARGET_FILE:HttpTransportPortTests>)
  add_test(
    NAME Platform.Network.WinHttpTransport
    COMMAND $<TARGET_FILE:WinHttpTransportTests>)
endif()
