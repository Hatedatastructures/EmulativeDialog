cmake_minimum_required(VERSION 3.24)

project(EmulativeDialogAnthropicMessagesProviderContract LANGUAGES CXX)

enable_testing()

if(NOT TARGET EmulativeDialogBoostJson)
  message(FATAL_ERROR
      "EmulativeDialogBoostJson target is provided by ED-0143; "
      "build this provider contract after that dependency task merges.")
endif()

get_filename_component(EMULATIVE_DIALOG_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

add_executable(AnthropicMessagesProviderTests
    "${EMULATIVE_DIALOG_ROOT}/Source/Providers/AnthropicMessages/AnthropicMessagesProvider.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/AnthropicMessagesProviderTests.cpp")
target_include_directories(AnthropicMessagesProviderTests PRIVATE
    "${EMULATIVE_DIALOG_ROOT}")
target_compile_features(AnthropicMessagesProviderTests PRIVATE cxx_std_20)
target_compile_options(AnthropicMessagesProviderTests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(AnthropicMessagesProviderTests PRIVATE
    EmulativeDialogBoostJson)

add_test(NAME Provider.AnthropicMessages
    COMMAND AnthropicMessagesProviderTests)
