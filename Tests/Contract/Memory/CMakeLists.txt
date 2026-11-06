cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EmulativeDialogConversationMemoryContract LANGUAGES C CXX)
  include(CTest)
  get_filename_component(_EmulativeDialogRepoRoot
      "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
  include("${_EmulativeDialogRepoRoot}/CMake/DependencyPins.cmake")
  include("${_EmulativeDialogRepoRoot}/CMake/BoostAsioDependency.cmake")
  add_subdirectory(
      "${_EmulativeDialogRepoRoot}/Tests/Contract/Storage"
      "${CMAKE_CURRENT_BINARY_DIR}/Storage")
endif()

get_filename_component(EMULATIVE_DIALOG_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

add_library(CoreMemoryRecallTestSupport OBJECT
    "${EMULATIVE_DIALOG_ROOT}/Source/Core/CoreChatService.cpp"
    "${EMULATIVE_DIALOG_ROOT}/Source/Core/IdentityPolicy.cpp"
    "${EMULATIVE_DIALOG_ROOT}/Source/Core/OperationRecord.cpp"
    "${EMULATIVE_DIALOG_ROOT}/Source/Core/BusinessActionRegistry.cpp"
    "${EMULATIVE_DIALOG_ROOT}/Source/Agent/OutputBudgetController.cpp")
target_include_directories(CoreMemoryRecallTestSupport PRIVATE
    "${EMULATIVE_DIALOG_ROOT}")
target_compile_features(CoreMemoryRecallTestSupport PRIVATE cxx_std_20)
target_compile_options(CoreMemoryRecallTestSupport PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(CoreMemoryRecallTestSupport PRIVATE
    EmulativeDialogBoostAsio)
add_dependencies(CoreMemoryRecallTestSupport EmulativeDialogBoostAsioVerify)

add_library(ConversationMemoryObject OBJECT
    "${EMULATIVE_DIALOG_ROOT}/Source/Memory/ConversationMemory.cpp")
target_include_directories(ConversationMemoryObject PRIVATE
    "${EMULATIVE_DIALOG_ROOT}")
target_compile_features(ConversationMemoryObject PRIVATE cxx_std_20)
target_compile_options(ConversationMemoryObject PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

add_executable(ConversationMemoryTests
    "${CMAKE_CURRENT_LIST_DIR}/ConversationMemoryTests.cpp")
target_include_directories(ConversationMemoryTests PRIVATE
    "${EMULATIVE_DIALOG_ROOT}")
target_compile_features(ConversationMemoryTests PRIVATE cxx_std_20)
target_compile_options(ConversationMemoryTests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(ConversationMemoryTests PRIVATE
    ConversationMemoryObject CoreMemoryRecallTestSupport)
set_target_properties(ConversationMemoryTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

add_executable(SqliteMemoryStorageAdapterTests
    "${EMULATIVE_DIALOG_ROOT}/Source/Memory/SqliteMemoryStorageAdapter.cpp"
    "${EMULATIVE_DIALOG_ROOT}/Source/Storage/SqliteStore.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/SqliteMemoryStorageAdapterTests.cpp")
target_include_directories(SqliteMemoryStorageAdapterTests PRIVATE
    "${EMULATIVE_DIALOG_ROOT}")
target_compile_features(SqliteMemoryStorageAdapterTests PRIVATE cxx_std_20)
target_compile_options(SqliteMemoryStorageAdapterTests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(SqliteMemoryStorageAdapterTests PRIVATE
    EmulativeDialogSQLite ConversationMemoryObject CoreMemoryRecallTestSupport)
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

add_executable(CorePolicyAndActionContractTests
    "${EMULATIVE_DIALOG_ROOT}/Tests/Contract/Core/CorePolicyAndActionContractTests.cpp")
target_include_directories(CorePolicyAndActionContractTests PRIVATE
    "${EMULATIVE_DIALOG_ROOT}")
target_compile_features(CorePolicyAndActionContractTests PRIVATE cxx_std_20)
target_compile_options(CorePolicyAndActionContractTests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(CorePolicyAndActionContractTests PRIVATE
    CoreMemoryRecallTestSupport)
if(BUILD_TESTING)
  add_test(NAME Core.PolicyAndAction COMMAND CorePolicyAndActionContractTests)
endif()
