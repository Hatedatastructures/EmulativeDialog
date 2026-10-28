cmake_minimum_required(VERSION 3.24)

project(EmulativeDialogConversationMemoryContract LANGUAGES CXX)

include(CTest)

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

if(BUILD_TESTING)
  add_test(NAME ConversationMemoryTests COMMAND ConversationMemoryTests)
  set_tests_properties(ConversationMemoryTests PROPERTIES
      WORKING_DIRECTORY "${EMULATIVE_DIALOG_ROOT}")
endif()
