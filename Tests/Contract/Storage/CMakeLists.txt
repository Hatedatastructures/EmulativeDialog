cmake_minimum_required(VERSION 3.24)

# Standalone consumers (`cmake -S Tests/Contract/Storage`) and the root
# `add_subdirectory` integration must expose the same contract: one static
# `EmulativeDialogSQLite` target plus an always-present `SQLiteBuildProbe`
# generated only under the task build directory.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EmulativeDialogStorageContract LANGUAGES C CXX)
  include(CTest)
  include("${CMAKE_CURRENT_LIST_DIR}/../../../CMake/DependencyPins.cmake")
endif()

if(NOT TARGET EmulativeDialogSQLite)
  FetchContent_MakeAvailable(SQLite)

  add_library(EmulativeDialogSQLite STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
  target_include_directories(EmulativeDialogSQLite
      PUBLIC "${sqlite_SOURCE_DIR}")
  target_compile_definitions(EmulativeDialogSQLite PRIVATE
      SQLITE_THREADSAFE=1
      SQLITE_OMIT_LOAD_EXTENSION=1
      SQLITE_DQS=0
      SQLITE_DEFAULT_MEMSTATUS=0)
  set_target_properties(EmulativeDialogSQLite PROPERTIES
      C_STANDARD 11
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF
      POSITION_INDEPENDENT_CODE ON)
  if(WIN32)
    target_link_libraries(EmulativeDialogSQLite PUBLIC ws2_32)
  else()
    target_link_libraries(EmulativeDialogSQLite PUBLIC ${CMAKE_DL_LIBS})
  endif()
endif()

# The probe translation unit is generated under the build tree so the
# repository never carries a linked-in SQLite source or probe stub.
set(_EmulativeDialogSqliteProbeSource
    "${CMAKE_BINARY_DIR}/SQLiteBuildProbe.cpp")
file(WRITE "${_EmulativeDialogSqliteProbeSource}"
"#include <sqlite3.h>

int main() {
  return sqlite3_libversion_number() > 0 ? 0 : 1;
}
")
add_executable(SQLiteBuildProbe "${_EmulativeDialogSqliteProbeSource}")
target_compile_features(SQLiteBuildProbe PRIVATE cxx_std_20)
set_target_properties(SQLiteBuildProbe PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
target_link_libraries(SQLiteBuildProbe PRIVATE EmulativeDialogSQLite)

if(BUILD_TESTING)
  add_test(NAME SQLiteBuildProbe COMMAND SQLiteBuildProbe)
endif()

# ED-0123 provides the storage implementation and its tests later. Both
# frozen paths must exist before the consumer target is visible.
get_filename_component(_EmulativeDialogRepoRoot
    "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(_EmulativeDialogSqliteStoreSource
    "${_EmulativeDialogRepoRoot}/Source/Storage/SqliteStore.cpp")
set(_EmulativeDialogSqliteStoreTests
    "${CMAKE_CURRENT_LIST_DIR}/SqliteStoreTests.cpp")
if(EXISTS "${_EmulativeDialogSqliteStoreSource}" AND
   EXISTS "${_EmulativeDialogSqliteStoreTests}")
  add_executable(SqliteStoreTests
      "${_EmulativeDialogSqliteStoreSource}"
      "${_EmulativeDialogSqliteStoreTests}")
  target_include_directories(SqliteStoreTests PRIVATE
      "${_EmulativeDialogRepoRoot}")
  target_compile_features(SqliteStoreTests PRIVATE cxx_std_20)
  target_link_libraries(SqliteStoreTests PRIVATE EmulativeDialogSQLite)
  if(BUILD_TESTING)
    add_test(NAME SqliteStoreTests COMMAND SqliteStoreTests)
    set_tests_properties(SqliteStoreTests PROPERTIES
        ENVIRONMENT
          "EMULATIVEDIALOG_TEST_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/Data")
  endif()
endif()
