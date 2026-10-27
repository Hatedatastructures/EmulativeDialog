include_guard(GLOBAL)

include(ExternalProject)

set(EmulativeDialogBoostVersion "1.92.0")
set(EmulativeDialogBoostCommit "afdfa32505af73e3d208144b3f623f0096cb62b6")

if(NOT DEFINED FETCHCONTENT_BASE_DIR)
  message(FATAL_ERROR
    "FETCHCONTENT_BASE_DIR must name the task dependency cache")
endif()

set(EmulativeDialogBoostRequiredSubmodules
    libs/asio
    libs/align
    libs/assert
    libs/config
    libs/core
    libs/mp11
    libs/predef
    libs/static_assert
    libs/system
    libs/throw_exception
    libs/type_traits)

set(EmulativeDialogBoostCheckoutDir
    "${FETCHCONTENT_BASE_DIR}/EmulativeDialogBoostAsioSource")
set(EmulativeDialogBoostSourceDir "${EmulativeDialogBoostCheckoutDir}")

if(NOT EXISTS "${EmulativeDialogBoostSourceDir}/.git")
  ExternalProject_Add(
    EmulativeDialogBoostAsioSource
    PREFIX "${FETCHCONTENT_BASE_DIR}/EmulativeDialogBoostAsioSource-populate"
    GIT_REPOSITORY https://github.com/boostorg/boost.git
    GIT_TAG "${EmulativeDialogBoostCommit}"
    GIT_SHALLOW FALSE
    GIT_SUBMODULES ${EmulativeDialogBoostRequiredSubmodules}
    GIT_SUBMODULES_RECURSE FALSE
    UPDATE_DISCONNECTED TRUE
    SOURCE_DIR "${EmulativeDialogBoostSourceDir}"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    TEST_COMMAND "")
endif()

execute_process(
  COMMAND git -C "${EmulativeDialogBoostSourceDir}" rev-parse HEAD
  OUTPUT_VARIABLE EmulativeDialogBoostActualCommit
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE EmulativeDialogBoostCommitResult
  ERROR_VARIABLE EmulativeDialogBoostCommitError)
if(NOT EmulativeDialogBoostCommitResult EQUAL 0 OR
   NOT "${EmulativeDialogBoostActualCommit}" STREQUAL
       "${EmulativeDialogBoostCommit}")
  message(FATAL_ERROR
    "Boost checkout resolved to '${EmulativeDialogBoostActualCommit}', "
    "expected '${EmulativeDialogBoostCommit}': "
    "${EmulativeDialogBoostCommitError}")
endif()

set(EmulativeDialogBoostIncludeDirectories)
foreach(Submodule IN LISTS EmulativeDialogBoostRequiredSubmodules)
  set(SubmoduleIncludeDir
      "${EmulativeDialogBoostSourceDir}/${Submodule}/include")
  if(NOT IS_DIRECTORY "${SubmoduleIncludeDir}")
    message(FATAL_ERROR
      "Pinned Boost submodule is incomplete: ${Submodule}")
  endif()
  list(APPEND EmulativeDialogBoostIncludeDirectories
       "${SubmoduleIncludeDir}")
endforeach()

if(NOT EXISTS
       "${EmulativeDialogBoostSourceDir}/libs/asio/include/boost/asio.hpp" OR
   NOT EXISTS
       "${EmulativeDialogBoostSourceDir}/libs/config/include/boost/version.hpp" OR
   NOT EXISTS "${EmulativeDialogBoostSourceDir}/LICENSE_1_0.txt")
  message(FATAL_ERROR
    "Pinned Boost source tree is incomplete: "
    "${EmulativeDialogBoostSourceDir}")
endif()

add_library(EmulativeDialogBoostAsio INTERFACE)
target_include_directories(
  EmulativeDialogBoostAsio
  INTERFACE
    ${EmulativeDialogBoostIncludeDirectories}
)
target_compile_features(EmulativeDialogBoostAsio INTERFACE cxx_std_20)
if(WIN32)
  target_link_libraries(EmulativeDialogBoostAsio INTERFACE ws2_32)
endif()

get_target_property(
  EmulativeDialogBoostAsioIncludeDirs
  EmulativeDialogBoostAsio
  INTERFACE_INCLUDE_DIRECTORIES)
foreach(IncludeDir IN LISTS EmulativeDialogBoostAsioIncludeDirs)
  string(REPLACE "\\" "/" NormalizedIncludeDir "${IncludeDir}")
  string(TOLOWER "${NormalizedIncludeDir}" LowerIncludeDir)
  if(LowerIncludeDir MATCHES "^[a-z]:/msys64/")
    message(FATAL_ERROR
      "Boost.Asio resolved from the system MSYS2 tree: ${IncludeDir}")
  endif()
endforeach()
