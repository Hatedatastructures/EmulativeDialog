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

# The build probe does not populate or link these dependencies. Their pinned
# declarations are available for consumers to opt in when their contracts land.
