cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EmulativeDialogConversationMemoryContract LANGUAGES C CXX)
  include(CTest)
  get_filename_component(_EmulativeDialogRepoRoot
      "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
  include("${_EmulativeDialogRepoRoot}/CMake/DependencyPins.cmake")
  add_subdirectory(
      "${_EmulativeDialogRepoRoot}/Tests/Contract/Storage"
      "${CMAKE_CURRENT_BINARY_DIR}/Storage")
endif()

get_filename_component(EMULATIVE_DIALOG_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

add_executable(ConversationMemoryTests
    "${EMULATIVE_DIALOG_ROOT}/Source/Memory/ConversationMemory.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ConversationMemoryTests.cpp")
target_include_directories(ConversationMemoryTests PRIVATE
    "${EMULATIVE_DIALOG_ROOT}")
target_compile_features(ConversationMemoryTests PRIVATE cxx_std_20)
target_compile_options(ConversationMemoryTests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
set_target_properties(ConversationMemoryTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

add_executable(SqliteMemoryStorageAdapterTests
    "${EMULATIVE_DIALOG_ROOT}/Source/Memory/ConversationMemory.cpp"
    "${EMULATIVE_DIALOG_ROOT}/Source/Memory/SqliteMemoryStorageAdapter.cpp"
    "${EMULATIVE_DIALOG_ROOT}/Source/Storage/SqliteStore.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/SqliteMemoryStorageAdapterTests.cpp")
target_include_directories(SqliteMemoryStorageAdapterTests PRIVATE
    "${EMULATIVE_DIALOG_ROOT}")
target_compile_features(SqliteMemoryStorageAdapterTests PRIVATE cxx_std_20)
target_compile_options(SqliteMemoryStorageAdapterTests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(SqliteMemoryStorageAdapterTests PRIVATE
    EmulativeDialogSQLite)
set_target_properties(SqliteMemoryStorageAdapterTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

if(BUILD_TESTING)
  add_test(NAME ConversationMemoryTests COMMAND ConversationMemoryTests)
  set_tests_properties(ConversationMemoryTests PROPERTIES
      WORKING_DIRECTORY "${EMULATIVE_DIALOG_ROOT}")

  add_test(NAME SqliteMemoryStorageAdapterTests
      COMMAND SqliteMemoryStorageAdapterTests)
  set_tests_properties(SqliteMemoryStorageAdapterTests PROPERTIES
      WORKING_DIRECTORY "${EMULATIVE_DIALOG_ROOT}"
      ENVIRONMENT
        "EMULATIVEDIALOG_TEST_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/Data")
endif()
