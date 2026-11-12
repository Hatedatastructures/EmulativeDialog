cmake_minimum_required(VERSION 3.24)

# Compose the three provider adapter contract projects. A standalone configure
# supplies the pinned dependency targets here; a root configure already owns
# those targets and continues directly to the adapter subdirectories.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EmulativeDialogProviderContracts LANGUAGES CXX)
  include(CTest)
  get_filename_component(EMULATIVE_DIALOG_ROOT
      "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
  include("${EMULATIVE_DIALOG_ROOT}/CMake/DependencyPins.cmake")
  include("${EMULATIVE_DIALOG_ROOT}/CMake/BoostAsioDependency.cmake")
  include("${EMULATIVE_DIALOG_ROOT}/CMake/BoostJsonDependency.cmake")
endif()

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/OpenAIResponses"
                 "${CMAKE_CURRENT_BINARY_DIR}/OpenAIResponses")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/OpenAIChatCompletions"
                 "${CMAKE_CURRENT_BINARY_DIR}/OpenAIChatCompletions")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/AnthropicMessages"
                 "${CMAKE_CURRENT_BINARY_DIR}/AnthropicMessages")
