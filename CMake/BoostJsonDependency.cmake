include_guard(GLOBAL)

if(NOT DEFINED EmulativeDialogBoostSourceDir OR
   NOT DEFINED EmulativeDialogBoostCommit)
  message(FATAL_ERROR
    "CMake/BoostAsioDependency.cmake must be included before "
    "CMake/BoostJsonDependency.cmake")
endif()

option(
  EMULATIVE_DIALOG_OFFLINE
  "Require pinned dependency sources to be present without fetching"
  OFF)

# These direct gitlinks are populated by ExternalProject in
# CMake/BoostAsioDependency.cmake. This module only verifies the checkout and
# never mutates it or starts a network operation.
set(EmulativeDialogBoostJsonRequiredSubmodules
    libs/json
    libs/container
    libs/describe
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

if(EMULATIVE_DIALOG_OFFLINE)
  if(NOT EXISTS "${EmulativeDialogBoostSourceDir}/.git")
    message(FATAL_ERROR
      "EMULATIVE_DIALOG_OFFLINE requires a pre-populated pinned Boost checkout at "
      "${EmulativeDialogBoostSourceDir}.")
  endif()
  foreach(Submodule IN LISTS EmulativeDialogBoostJsonRequiredSubmodules)
    if(NOT IS_DIRECTORY
       "${EmulativeDialogBoostSourceDir}/${Submodule}/include")
      message(FATAL_ERROR
        "EMULATIVE_DIALOG_OFFLINE requires pinned gitlink ${Submodule} "
        "to be populated before configure.")
    endif()
  endforeach()
endif()

set(EmulativeDialogBoostJsonVerifyScript
    "${CMAKE_BINARY_DIR}/EmulativeDialogBoostJsonVerify.cmake")
file(WRITE "${EmulativeDialogBoostJsonVerifyScript}" "
if(DEFINED EmulativeDialogBoostJsonVerifySourceDir)
  set(VerifySourceDir \"\${EmulativeDialogBoostJsonVerifySourceDir}\")
else()
  set(VerifySourceDir \"${EmulativeDialogBoostSourceDir}\")
endif()
set(RequiredSubmodules \"${EmulativeDialogBoostJsonRequiredSubmodules}\")
foreach(Submodule IN LISTS RequiredSubmodules)
  if(NOT IS_DIRECTORY \"\${VerifySourceDir}/\${Submodule}/include\")
    message(FATAL_ERROR
      \"Pinned Boost submodule is incomplete: \${Submodule}\")
  endif()
endforeach()
execute_process(
  COMMAND git -C \"\${VerifySourceDir}\" rev-parse HEAD
  OUTPUT_VARIABLE ActualCommit
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE CommitResult
  ERROR_VARIABLE CommitError)
if(NOT CommitResult EQUAL 0 OR
   NOT \"\${ActualCommit}\" STREQUAL \"${EmulativeDialogBoostCommit}\")
  message(FATAL_ERROR
    \"Boost checkout resolved to '\${ActualCommit}', expected '${EmulativeDialogBoostCommit}': \${CommitError}\")
endif()
if(NOT EXISTS \"\${VerifySourceDir}/libs/json/include/boost/json.hpp\" OR
   NOT EXISTS \"\${VerifySourceDir}/libs/config/include/boost/version.hpp\" OR
   NOT EXISTS \"\${VerifySourceDir}/LICENSE_1_0.txt\")
  message(FATAL_ERROR
    \"Pinned Boost.JSON source tree is incomplete: \${VerifySourceDir}\")
endif()
message(STATUS \"Verified pinned Boost.JSON checkout ${EmulativeDialogBoostCommit}\")
")

add_custom_target(
  EmulativeDialogBoostJsonVerify
  COMMAND "${CMAKE_COMMAND}" -P "${EmulativeDialogBoostJsonVerifyScript}"
  COMMENT "Read-only verification of pinned Boost.JSON checkout and gitlinks"
  VERBATIM)
if(TARGET EmulativeDialogBoostAsioSource)
  add_dependencies(EmulativeDialogBoostJsonVerify
                   EmulativeDialogBoostAsioSource)
endif()

# Surface superproject drift during configure only when the checkout exists.
# A missing tree is expected on a cold cache and is populated by ExternalProject.
if(EXISTS "${EmulativeDialogBoostSourceDir}/.git")
  execute_process(
    COMMAND git -C "${EmulativeDialogBoostSourceDir}" rev-parse HEAD
    OUTPUT_VARIABLE EmulativeDialogBoostJsonActualCommit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE EmulativeDialogBoostJsonCommitResult
    ERROR_VARIABLE EmulativeDialogBoostJsonCommitError)
  if(NOT EmulativeDialogBoostJsonCommitResult EQUAL 0 OR
     NOT "${EmulativeDialogBoostJsonActualCommit}" STREQUAL
         "${EmulativeDialogBoostCommit}")
    message(FATAL_ERROR
      "Boost checkout resolved to '${EmulativeDialogBoostJsonActualCommit}', "
      "expected '${EmulativeDialogBoostCommit}': "
      "${EmulativeDialogBoostJsonCommitError}")
  endif()
endif()

set(EmulativeDialogBoostJsonIncludeDirectories)
foreach(Submodule IN LISTS EmulativeDialogBoostJsonRequiredSubmodules)
  list(APPEND EmulativeDialogBoostJsonIncludeDirectories
       "${EmulativeDialogBoostSourceDir}/${Submodule}/include")
endforeach()

add_library(
  EmulativeDialogBoostJson STATIC
  "${CMAKE_CURRENT_LIST_DIR}/BoostJsonImplementation.cpp")
target_include_directories(
  EmulativeDialogBoostJson
  PUBLIC
    ${EmulativeDialogBoostJsonIncludeDirectories})
target_compile_features(EmulativeDialogBoostJson PUBLIC cxx_std_20)
add_dependencies(EmulativeDialogBoostJson EmulativeDialogBoostJsonVerify)
set_target_properties(
  EmulativeDialogBoostJson
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

# Keep the static archive free of platform linker state and pthread dependencies.
# Final executable consumers opt into the balanced WinHTTP/MinGW link options.
set(EmulativeDialogBoostJsonWinPthreadLinkOption "")
set(EmulativeDialogBoostJsonNoPthreadLinkOption "")
if(WIN32 AND MINGW)
  set(EmulativeDialogBoostJsonNoPthreadLinkOption "-no-pthread")
  set(EmulativeDialogBoostJsonWinPthreadLinkOption
      "-Wl,--push-state,-Bstatic,-lwinpthread,-Bdynamic,--pop-state")
endif()

get_target_property(
  EmulativeDialogBoostJsonInterfaceLinkLibraries
  EmulativeDialogBoostJson
  INTERFACE_LINK_LIBRARIES)
if(EmulativeDialogBoostJsonInterfaceLinkLibraries AND
   EmulativeDialogBoostJsonInterfaceLinkLibraries MATCHES
       "winpthread|-Wl,-B(static|dynamic)")
  message(FATAL_ERROR
    "EmulativeDialogBoostJson must not propagate winpthread or linker modes: "
    "${EmulativeDialogBoostJsonInterfaceLinkLibraries}")
endif()
get_target_property(
  EmulativeDialogBoostJsonInterfaceLinkOptions
  EmulativeDialogBoostJson
  INTERFACE_LINK_OPTIONS)
if(EmulativeDialogBoostJsonInterfaceLinkOptions AND
   EmulativeDialogBoostJsonInterfaceLinkOptions MATCHES
       "winpthread|-Wl,-B(static|dynamic)")
  message(FATAL_ERROR
    "EmulativeDialogBoostJson must not propagate winpthread or linker modes: "
    "${EmulativeDialogBoostJsonInterfaceLinkOptions}")
endif()

get_target_property(
  EmulativeDialogBoostJsonIncludeDirs
  EmulativeDialogBoostJson
  INTERFACE_INCLUDE_DIRECTORIES)
foreach(IncludeDir IN LISTS EmulativeDialogBoostJsonIncludeDirs)
  string(REPLACE "\\" "/" NormalizedIncludeDir "${IncludeDir}")
  string(TOLOWER "${NormalizedIncludeDir}" LowerIncludeDir)
  if(LowerIncludeDir MATCHES "^[a-z]:/msys64/")
    message(FATAL_ERROR
      "Boost.JSON resolved from the system MSYS2 tree: ${IncludeDir}")
  endif()
endforeach()
