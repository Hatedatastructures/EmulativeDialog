include_guard(GLOBAL)

set(EmulativeDialogBoostVersion "1.92.0")
set(EmulativeDialogBoostCommit "afdfa32505af73e3d208144b3f623f0096cb62b6")

set(EmulativeDialogBoostArchive
    "boost-1.92.0-cmake.tar.xz")
set(EmulativeDialogBoostArchiveSha256
    "9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d")
if(NOT DEFINED FETCHCONTENT_BASE_DIR)
  message(FATAL_ERROR
    "FETCHCONTENT_BASE_DIR must name the task dependency cache")
endif()

set(EmulativeDialogBoostSourceDir
    "${FETCHCONTENT_BASE_DIR}/EmulativeDialogBoost-${EmulativeDialogBoostVersion}")
set(EmulativeDialogBoostArchivePath
    "${FETCHCONTENT_BASE_DIR}/Archives/${EmulativeDialogBoostArchive}")

if(NOT EXISTS "${EmulativeDialogBoostSourceDir}/boost/version.hpp")
  file(MAKE_DIRECTORY "${FETCHCONTENT_BASE_DIR}/Archives")
  if(NOT EXISTS "${EmulativeDialogBoostArchivePath}")
    file(
      DOWNLOAD
        "https://github.com/boostorg/boost/releases/download/boost-1.92.0/${EmulativeDialogBoostArchive}"
        "${EmulativeDialogBoostArchivePath}"
      EXPECTED_HASH "SHA256=${EmulativeDialogBoostArchiveSha256}"
      SHOW_PROGRESS
      STATUS BoostDownloadStatus)
    list(GET BoostDownloadStatus 0 BoostDownloadResult)
    if(NOT BoostDownloadResult EQUAL 0)
      list(GET BoostDownloadStatus 1 BoostDownloadMessage)
      message(FATAL_ERROR
        "Boost 1.92.0 download failed: ${BoostDownloadMessage}")
    endif()
  endif()

  file(SHA256 "${EmulativeDialogBoostArchivePath}" ActualBoostArchiveSha256)
  if(NOT "${ActualBoostArchiveSha256}" STREQUAL
     "${EmulativeDialogBoostArchiveSha256}")
    message(FATAL_ERROR
      "Boost archive SHA256 mismatch: ${ActualBoostArchiveSha256}")
  endif()

  file(REMOVE_RECURSE "${EmulativeDialogBoostSourceDir}")
  file(ARCHIVE_EXTRACT
       INPUT "${EmulativeDialogBoostArchivePath}"
       DESTINATION "${EmulativeDialogBoostSourceDir}")
endif()

set(EmulativeDialogBoostIncludeDir
    "${EmulativeDialogBoostSourceDir}")

if(NOT EXISTS "${EmulativeDialogBoostIncludeDir}/boost/asio.hpp" OR
   NOT EXISTS "${EmulativeDialogBoostIncludeDir}/boost/version.hpp" OR
   NOT EXISTS "${EmulativeDialogBoostIncludeDir}/LICENSE_1_0.txt")
  message(FATAL_ERROR
    "Pinned Boost archive is incomplete: "
    "${EmulativeDialogBoostIncludeDir}")
endif()

add_library(EmulativeDialogBoostAsio INTERFACE)
target_include_directories(
  EmulativeDialogBoostAsio
  INTERFACE
    "${EmulativeDialogBoostSourceDir}"
    "${EmulativeDialogBoostIncludeDir}"
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
