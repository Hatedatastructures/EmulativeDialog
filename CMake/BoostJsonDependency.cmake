include_guard(GLOBAL)

if(NOT DEFINED EmulativeDialogBoostSourceDir OR
   NOT DEFINED EmulativeDialogBoostCommit)
  message(FATAL_ERROR
    "CMake/BoostAsioDependency.cmake must be included before "
    "CMake/BoostJsonDependency.cmake")
endif()

include(ExternalProject)

option(
  EMULATIVE_DIALOG_OFFLINE
  "Require pinned dependency sources to be present without fetching"
  OFF)

# Boost.JSON is consumed from the same pinned Boost 1.92.0 superproject
# checkout as Boost.Asio. The shared submodules already required by Asio are
# listed again so this target is self-verifying when used on its own.
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

# Population is a separate buildstage operation. The verify target below is
# deliberately read-only so validation never changes a pinned checkout.
set(EmulativeDialogBoostJsonPopulateScript
    "${CMAKE_BINARY_DIR}/EmulativeDialogBoostJsonPopulate.cmake")
file(WRITE "${EmulativeDialogBoostJsonPopulateScript}" "
set(SourceDir \"${EmulativeDialogBoostSourceDir}\")
set(RequiredSubmodules \"${EmulativeDialogBoostJsonRequiredSubmodules}\")
set(MissingSubmodules \"\")
foreach(Submodule IN LISTS RequiredSubmodules)
  if(NOT IS_DIRECTORY \"\${SourceDir}/\${Submodule}/include\")
    list(APPEND MissingSubmodules \"\${Submodule}\")
  endif()
endforeach()
if(NOT MissingSubmodules)
  message(STATUS \"Boost.JSON submodules are already populated\")
  return()
endif()
if(\"${EMULATIVE_DIALOG_OFFLINE}\")
  string(REPLACE \";\" \", \" MissingText \"\${MissingSubmodules}\")
  message(FATAL_ERROR
    \"EMULATIVE_DIALOG_OFFLINE requires a complete Boost.JSON source tree; missing submodules: \${MissingText}\")
endif()
execute_process(
  COMMAND git -C \"\${SourceDir}\" submodule update --init --
          \${MissingSubmodules}
  RESULT_VARIABLE UpdateResult
  ERROR_VARIABLE UpdateError)
if(NOT UpdateResult EQUAL 0)
  message(FATAL_ERROR
    \"Failed to populate pinned Boost.JSON submodules: \${UpdateError}\")
endif()
foreach(Submodule IN LISTS MissingSubmodules)
  if(NOT IS_DIRECTORY \"\${SourceDir}/\${Submodule}/include\")
    message(FATAL_ERROR
      \"Boost.JSON submodule remains incomplete after population: \${Submodule}\")
  endif()
endforeach()
message(STATUS \"Populated pinned Boost.JSON submodules\")
")

# A cold cache registers EmulativeDialogBoostAsioSource through ED-0131. Add
# the JSON-specific population to that ExternalProject before configure runs.
if(TARGET EmulativeDialogBoostAsioSource)
  ExternalProject_Add_Step(
    EmulativeDialogBoostAsioSource
    emulative_dialog_boost_json_populate
    COMMAND "${CMAKE_COMMAND}" -P
            "${EmulativeDialogBoostJsonPopulateScript}"
    COMMENT "Populating pinned Boost.JSON submodules"
    DEPENDEES download
    DEPENDERS configure
    VERBATIM)
  set(EmulativeDialogBoostJsonPrerequisiteTarget
      EmulativeDialogBoostAsioSource)
else()
  # A cache prepared by ED-0131 has no ExternalProject target because its
  # checkout already exists. Populate it lazily before any JSON compilation.
  add_custom_target(
    EmulativeDialogBoostJsonPopulate
    COMMAND "${CMAKE_COMMAND}" -P
            "${EmulativeDialogBoostJsonPopulateScript}"
    COMMENT "Checking and populating pinned Boost.JSON submodules"
    VERBATIM)
  set(EmulativeDialogBoostJsonPrerequisiteTarget
      EmulativeDialogBoostJsonPopulate)
endif()

if(EMULATIVE_DIALOG_OFFLINE AND
   NOT EXISTS "${EmulativeDialogBoostSourceDir}/.git")
  message(FATAL_ERROR
    "EMULATIVE_DIALOG_OFFLINE requires an existing pinned Boost checkout at "
    "${EmulativeDialogBoostSourceDir}; pre-populate "
    "FETCHCONTENT_BASE_DIR/EmulativeDialogBoostAsioSource before configuring.")
endif()

set(EmulativeDialogBoostJsonVerifyScript
    "${CMAKE_BINARY_DIR}/EmulativeDialogBoostJsonVerify.cmake")
file(WRITE "${EmulativeDialogBoostJsonVerifyScript}" "
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
set(RequiredSubmodules \"${EmulativeDialogBoostJsonRequiredSubmodules}\")
foreach(Submodule IN LISTS RequiredSubmodules)
  if(NOT IS_DIRECTORY \"${EmulativeDialogBoostSourceDir}/\${Submodule}/include\")
    message(FATAL_ERROR
      \"Pinned Boost submodule is incomplete: \${Submodule}; run the Boost.JSON populate target\")
  endif()
endforeach()
if(NOT EXISTS \"${EmulativeDialogBoostSourceDir}/libs/json/include/boost/json.hpp\" OR
   NOT EXISTS \"${EmulativeDialogBoostSourceDir}/libs/config/include/boost/version.hpp\" OR
   NOT EXISTS \"${EmulativeDialogBoostSourceDir}/LICENSE_1_0.txt\")
  message(FATAL_ERROR
    \"Pinned Boost.JSON source tree is incomplete: ${EmulativeDialogBoostSourceDir}\")
endif()
message(STATUS \"Verified pinned Boost.JSON checkout ${EmulativeDialogBoostCommit}\")
")

add_custom_target(
  EmulativeDialogBoostJsonVerify
  COMMAND "${CMAKE_COMMAND}" -P "${EmulativeDialogBoostJsonVerifyScript}"
  COMMENT "Verifying pinned Boost.JSON checkout and submodules"
  VERBATIM)
add_dependencies(EmulativeDialogBoostJsonVerify
                 ${EmulativeDialogBoostJsonPrerequisiteTarget})

# Surface superproject drift during configure when the cache already exists.
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
    ${EmulativeDialogBoostJsonIncludeDirectories}
)
target_compile_features(EmulativeDialogBoostJson PUBLIC cxx_std_20)
add_dependencies(EmulativeDialogBoostJson
                 ${EmulativeDialogBoostJsonPrerequisiteTarget})
set_target_properties(
  EmulativeDialogBoostJson
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# A STATIC library cannot carry a private library dependency: CMake exposes it
# to consumers as $<LINK_ONLY:...>. Keep the static archive neutral and expose
# one balanced final-link option for executable/test consumers instead.
set(EmulativeDialogBoostJsonWinPthreadLinkOption "")
set(EmulativeDialogBoostJsonNoPthreadLinkOption "")
if(WIN32 AND MINGW)
  # GCC otherwise appends its own dynamic -lpthread after the balanced option.
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
