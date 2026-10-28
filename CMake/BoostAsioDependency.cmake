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

set(EmulativeDialogBoostIncludeDirectories)
foreach(Submodule IN LISTS EmulativeDialogBoostRequiredSubmodules)
  set(SubmoduleIncludeDir
      "${EmulativeDialogBoostSourceDir}/${Submodule}/include")
  list(APPEND EmulativeDialogBoostIncludeDirectories
       "${SubmoduleIncludeDir}")
endforeach()

# Configure only registers the pinned source and consumers. The checkout is
# populated by the ExternalProject at build time, so configure must not assume
# that the tree already exists. Verification runs as a build-stage target
# after population; this keeps a clean cache able to configure.
set(EmulativeDialogBoostVerifyScript
    "${CMAKE_BINARY_DIR}/EmulativeDialogBoostAsioVerify.cmake")
file(WRITE "${EmulativeDialogBoostVerifyScript}" "
execute_process(
  COMMAND git -C \"${EmulativeDialogBoostSourceDir}\" rev-parse HEAD
  OUTPUT_VARIABLE ActualCommit
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE CommitResult
  ERROR_VARIABLE CommitError)
if(NOT CommitResult EQUAL 0 OR
   NOT \"\${ActualCommit}\" STREQUAL \"${EmulativeDialogBoostCommit}\")
  message(FATAL_ERROR
    \"Boost checkout resolved to '\${ActualCommit}', expected '${EmulativeDialogBoostCommit}': \${CommitError}\")
endif()
set(RequiredSubmodules \"${EmulativeDialogBoostRequiredSubmodules}\")
foreach(Submodule IN LISTS RequiredSubmodules)
  if(NOT IS_DIRECTORY \"${EmulativeDialogBoostSourceDir}/\${Submodule}/include\")
    message(FATAL_ERROR \"Pinned Boost submodule is incomplete: \${Submodule}\")
  endif()
endforeach()
if(NOT EXISTS \"${EmulativeDialogBoostSourceDir}/libs/asio/include/boost/asio.hpp\" OR
   NOT EXISTS \"${EmulativeDialogBoostSourceDir}/libs/config/include/boost/version.hpp\" OR
   NOT EXISTS \"${EmulativeDialogBoostSourceDir}/LICENSE_1_0.txt\")
  message(FATAL_ERROR
    \"Pinned Boost source tree is incomplete: ${EmulativeDialogBoostSourceDir}\")
endif()
message(STATUS \"Verified pinned Boost.Asio checkout ${EmulativeDialogBoostCommit}\")
")

add_custom_target(
  EmulativeDialogBoostAsioVerify
  COMMAND "${CMAKE_COMMAND}" -P "${EmulativeDialogBoostVerifyScript}"
  COMMENT "Verifying pinned Boost.Asio checkout and submodules"
  VERBATIM)
if(TARGET EmulativeDialogBoostAsioSource)
  add_dependencies(EmulativeDialogBoostAsioVerify
                   EmulativeDialogBoostAsioSource)
endif()

# When the checkout is already present at configure time, surface drift early.
# A missing checkout is expected on a clean cache and is verified at build.
if(EXISTS "${EmulativeDialogBoostSourceDir}/.git")
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
