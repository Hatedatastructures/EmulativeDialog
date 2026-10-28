include(FetchContent)

set(EmulativeDialogBoostVersion "1.92.0")
set(EmulativeDialogBoostCommit "afdfa32505af73e3d208144b3f623f0096cb62b6")
FetchContent_Declare(
  Boost
  GIT_REPOSITORY https://github.com/boostorg/boost.git
  GIT_TAG "${EmulativeDialogBoostCommit}"
  GIT_SHALLOW FALSE
  GIT_SUBMODULES_RECURSE TRUE
)

set(EmulativeDialogYamlCppVersion "0.9.0")
set(EmulativeDialogYamlCppCommit "56e3bb550c91fd7005566f19c079cb7a503223cf")
FetchContent_Declare(
  YamlCpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG "${EmulativeDialogYamlCppCommit}"
  GIT_SHALLOW FALSE
)

# SQLite is pinned to an immutable official amalgamation archive. The archive
# hash is verified by FetchContent before extraction; ED-0122 consumers build
# the vendored `sqlite3.c` into the only approved SQLite target and never link
# a system `sqlite3` library.
set(EmulativeDialogSqliteVersion "3.50.1")
set(EmulativeDialogSqliteAmalgamationUrl
    "https://www.sqlite.org/2025/sqlite-amalgamation-3500100.zip")
set(EmulativeDialogSqliteAmalgamationSha256
    "41716B44AC8777188C4C3F1F370F01C9CB9E3B6428EB5C981D086C35DE2D9D3F")
FetchContent_Declare(
  SQLite
  URL "${EmulativeDialogSqliteAmalgamationUrl}"
  URL_HASH "SHA256=${EmulativeDialogSqliteAmalgamationSha256}"
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)

# The build probe does not populate or link these dependencies. Their pinned
# declarations are available for consumers to opt in when their contracts land.
