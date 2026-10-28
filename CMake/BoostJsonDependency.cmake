include_guard(GLOBAL)

if(NOT DEFINED EmulativeDialogBoostSourceDir OR
   NOT DEFINED EmulativeDialogBoostCommit)
  message(FATAL_ERROR
    "CMake/BoostAsioDependency.cmake must be included before "
    "CMake/BoostJsonDependency.cmake")
endif()

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
set(MissingSubmodules \"\")
foreach(Submodule IN LISTS RequiredSubmodules)
  if(NOT IS_DIRECTORY \"${EmulativeDialogBoostSourceDir}/\${Submodule}/include\")
    list(APPEND MissingSubmodules \"\${Submodule}\")
  endif()
endforeach()
if(MissingSubmodules)
  execute_process(
    COMMAND git -C \"${EmulativeDialogBoostSourceDir}\" submodule update
            --init -- \${MissingSubmodules}
    RESULT_VARIABLE UpdateResult
    ERROR_VARIABLE UpdateError)
  if(NOT UpdateResult EQUAL 0)
    message(FATAL_ERROR
      \"Failed to initialize pinned Boost.JSON submodules: \${UpdateError}\")
  endif()
endif()
foreach(Submodule IN LISTS RequiredSubmodules)
  if(NOT IS_DIRECTORY \"${EmulativeDialogBoostSourceDir}/\${Submodule}/include\")
    message(FATAL_ERROR \"Pinned Boost submodule is incomplete: \${Submodule}\")
  endif()
endforeach()
if(NOT EXISTS \"${EmulativeDialogBoostSourceDir}/libs/json/include/boost/json.hpp\" OR
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
if(TARGET EmulativeDialogBoostAsioSource)
  add_dependencies(EmulativeDialogBoostJsonVerify
                   EmulativeDialogBoostAsioSource)
endif()

# Surface superproject drift during configure when the cache already exists.
# Missing submodules are populated by the build-stage verify target so a cold
# cache can configure without network access.
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

add_library(EmulativeDialogBoostJson INTERFACE)
target_include_directories(
  EmulativeDialogBoostJson
  INTERFACE
    ${EmulativeDialogBoostJsonIncludeDirectories}
)
target_compile_features(EmulativeDialogBoostJson INTERFACE cxx_std_20)

# Boost.JSON uses process-wide synchronization; on MinGW that would otherwise
# add libwinpthread-1.dll to every consumer. Link the toolchain pthread support
# statically so the only runtime DLLs stay the existing libgcc/libstdc++ pair.
if(WIN32 AND MINGW)
  target_link_libraries(
    EmulativeDialogBoostJson
    INTERFACE
      "-Wl,-Bstatic"
      winpthread
      "-Wl,-Bdynamic")
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
