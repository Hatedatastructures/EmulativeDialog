#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace EmulativeDialog::WorkflowController::TestSupport {

class WorkflowControllerTestWorkspace final {
 public:
  WorkflowControllerTestWorkspace(const std::filesystem::path& BuildDirectory,
                                  std::string FixtureName)
      : BuildDirectory(std::filesystem::absolute(BuildDirectory).lexically_normal()),
        TempRoot(this->BuildDirectory / "T"),
        FixtureName(std::move(FixtureName)) {
    if (!IsValidFixtureName(this->FixtureName)) {
      throw std::invalid_argument("Invalid WorkflowController fixture name");
    }

    std::error_code Error;
    if (!std::filesystem::is_directory(this->BuildDirectory, Error) || Error) {
      throw std::runtime_error("WorkflowController build directory does not exist");
    }
    if (IsReparsePoint(TempRoot)) {
      throw std::runtime_error("WorkflowController T root must not be a reparse point");
    }
    if (!std::filesystem::exists(TempRoot, Error)) {
      if (Error || !std::filesystem::create_directory(TempRoot, Error) || Error) {
        throw std::runtime_error("Unable to create WorkflowController T root");
      }
    }
    if (!std::filesystem::is_directory(TempRoot, Error) || Error ||
        IsReparsePoint(TempRoot)) {
      throw std::runtime_error("WorkflowController T root is not an owned directory");
    }

    FixturePrefix = "WorkflowControllerTestWorkspace-" + this->FixtureName + "-";
    for (unsigned Attempt = 0; Attempt < 16; ++Attempt) {
      Path = TempRoot / (FixturePrefix + UniqueSuffix());
      Error.clear();
      if (std::filesystem::create_directory(Path, Error)) {
        break;
      }
      if (Error && Error != std::errc::file_exists) {
        throw std::runtime_error("Unable to create WorkflowController fixture workspace");
      }
      Path.clear();
    }
    if (Path.empty()) {
      throw std::runtime_error("Unable to allocate a unique WorkflowController workspace");
    }

    Marker = Path / "OwnedByWorkflowControllerTestWorkspace.txt";
    MarkerText = "WorkflowControllerTestWorkspace:" + Path.filename().string();
    std::ofstream MarkerOutput(Marker, std::ios::binary | std::ios::trunc);
    MarkerOutput << MarkerText;
    MarkerOutput.close();
    if (!MarkerOutput) {
      std::error_code CleanupError;
      std::filesystem::remove(Path, CleanupError);
      Path.clear();
      TryRemoveEmptyTempRoot(this->BuildDirectory, TempRoot);
      throw std::runtime_error("Unable to create WorkflowController workspace marker");
    }
  }

  WorkflowControllerTestWorkspace(const WorkflowControllerTestWorkspace&) = delete;
  WorkflowControllerTestWorkspace& operator=(const WorkflowControllerTestWorkspace&) = delete;
  WorkflowControllerTestWorkspace(WorkflowControllerTestWorkspace&&) = delete;
  WorkflowControllerTestWorkspace& operator=(WorkflowControllerTestWorkspace&&) = delete;

  ~WorkflowControllerTestWorkspace() noexcept { (void)Cleanup(); }

  const std::filesystem::path& GetPath() const noexcept { return Path; }
  const std::filesystem::path& GetMarkerPath() const noexcept { return Marker; }
  const std::string& GetExpectedMarkerText() const noexcept { return MarkerText; }
  const std::filesystem::path& GetTempRoot() const noexcept { return TempRoot; }
  const std::string& GetFixturePrefix() const noexcept { return FixturePrefix; }

  bool Cleanup() noexcept {
    if (Cleaned) {
      return true;
    }
    if (Path.empty() || !CleanupOwnedWorkspace(BuildDirectory, Path, FixturePrefix,
                                               MarkerText)) {
      return false;
    }
    Cleaned = true;
    return true;
  }

  static bool CleanupOwnedWorkspace(const std::filesystem::path& BuildDirectory,
                                    const std::filesystem::path& CandidatePath,
                                    std::string_view FixturePrefix,
                                    std::string_view ExpectedMarkerText) noexcept {
    try {
      const std::filesystem::path AbsoluteBuildDirectory =
          std::filesystem::absolute(BuildDirectory).lexically_normal();
      const std::filesystem::path ExpectedTempRoot = AbsoluteBuildDirectory / "T";
      const std::filesystem::path AbsoluteCandidate =
          std::filesystem::absolute(CandidatePath).lexically_normal();
      if (ExpectedTempRoot.parent_path() != AbsoluteBuildDirectory ||
          AbsoluteCandidate.parent_path() != ExpectedTempRoot ||
          !AbsoluteCandidate.filename().string().starts_with(FixturePrefix) ||
          IsReparsePoint(ExpectedTempRoot) || IsReparsePoint(AbsoluteCandidate)) {
        return false;
      }

      std::error_code Error;
      if (!std::filesystem::is_directory(ExpectedTempRoot, Error) || Error ||
          !std::filesystem::is_directory(AbsoluteCandidate, Error) || Error) {
        return false;
      }

      const std::filesystem::path MarkerPath =
          AbsoluteCandidate / "OwnedByWorkflowControllerTestWorkspace.txt";
      if (IsReparsePoint(MarkerPath) ||
          !std::filesystem::is_regular_file(MarkerPath, Error) || Error ||
          ReadMarker(MarkerPath) != ExpectedMarkerText) {
        return false;
      }

      if (!RemoveOwnedContents(AbsoluteCandidate, MarkerPath) ||
          IsReparsePoint(MarkerPath) || ReadMarker(MarkerPath) != ExpectedMarkerText ||
          !ClearReadOnlyAttribute(MarkerPath)) {
        return false;
      }
      Error.clear();
      if (!std::filesystem::remove(MarkerPath, Error) || Error ||
          !ClearReadOnlyAttribute(AbsoluteCandidate)) {
        return false;
      }
      Error.clear();
      if (!std::filesystem::remove(AbsoluteCandidate, Error) || Error) {
        return false;
      }
      TryRemoveEmptyTempRoot(AbsoluteBuildDirectory, ExpectedTempRoot);
      return true;
    } catch (...) {
      return false;
    }
  }

  static bool TryRemoveEmptyTempRoot(const std::filesystem::path& BuildDirectory,
                                     const std::filesystem::path& TempRoot) noexcept {
    try {
      const std::filesystem::path AbsoluteBuildDirectory =
          std::filesystem::absolute(BuildDirectory).lexically_normal();
      const std::filesystem::path ExpectedTempRoot = AbsoluteBuildDirectory / "T";
      const std::filesystem::path AbsoluteTempRoot =
          std::filesystem::absolute(TempRoot).lexically_normal();
      if (AbsoluteTempRoot != ExpectedTempRoot ||
          AbsoluteTempRoot.parent_path() != AbsoluteBuildDirectory ||
          IsReparsePoint(AbsoluteTempRoot)) {
        return false;
      }

      std::error_code Error;
      if (!std::filesystem::is_directory(AbsoluteTempRoot, Error) || Error ||
          !std::filesystem::is_empty(AbsoluteTempRoot, Error) || Error) {
        return false;
      }
      return std::filesystem::remove(AbsoluteTempRoot, Error) && !Error;
    } catch (...) {
      return false;
    }
  }

 private:
  static bool IsValidFixtureName(std::string_view Value) noexcept {
    return !Value.empty() && std::all_of(Value.begin(), Value.end(), [](char Character) {
      return (Character >= 'A' && Character <= 'Z') ||
             (Character >= 'a' && Character <= 'z') ||
             (Character >= '0' && Character <= '9') || Character == '-' ||
             Character == '_';
    });
  }

  static std::string UniqueSuffix() {
    static std::atomic_uint64_t Sequence{0};
    const std::uint64_t Value = Sequence.fetch_add(1, std::memory_order_relaxed);
    return std::to_string(GetCurrentProcessId()) + "-" +
           std::to_string(GetTickCount64()) + "-" + std::to_string(Value);
  }

  static bool IsReparsePoint(const std::filesystem::path& Path) noexcept {
    const DWORD Attributes = GetFileAttributesW(Path.c_str());
    return Attributes != INVALID_FILE_ATTRIBUTES &&
           (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  }

  static bool ClearReadOnlyAttribute(const std::filesystem::path& Path) noexcept {
    const DWORD Attributes = GetFileAttributesW(Path.c_str());
    if (Attributes == INVALID_FILE_ATTRIBUTES ||
        (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return false;
    }
    if ((Attributes & FILE_ATTRIBUTE_READONLY) == 0) {
      return true;
    }
    return SetFileAttributesW(Path.c_str(), Attributes & ~FILE_ATTRIBUTE_READONLY) != 0;
  }

  static bool RemoveOwnedContents(const std::filesystem::path& Directory,
                                  const std::filesystem::path& ProtectedMarker) {
    if (IsReparsePoint(Directory)) {
      return false;
    }

    std::vector<std::filesystem::path> Children;
    std::error_code Error;
    for (std::filesystem::directory_iterator Iterator(Directory, Error), End;
         !Error && Iterator != End; Iterator.increment(Error)) {
      Children.push_back(Iterator->path());
    }
    if (Error) {
      return false;
    }

    for (const std::filesystem::path& Child : Children) {
      if (Child == ProtectedMarker) {
        continue;
      }
      const DWORD Attributes = GetFileAttributesW(Child.c_str());
      if (Attributes == INVALID_FILE_ATTRIBUTES) {
        return false;
      }
      if ((Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        const BOOL Removed = (Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                                 ? RemoveDirectoryW(Child.c_str())
                                 : DeleteFileW(Child.c_str());
        if (Removed == 0) {
          return false;
        }
        continue;
      }

      if ((Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        if (!RemoveOwnedContents(Child, ProtectedMarker) ||
            !ClearReadOnlyAttribute(Child)) {
          return false;
        }
        Error.clear();
        if (!std::filesystem::remove(Child, Error) || Error) {
          return false;
        }
      } else {
        if (!ClearReadOnlyAttribute(Child)) {
          return false;
        }
        Error.clear();
        if (!std::filesystem::remove(Child, Error) || Error) {
          return false;
        }
      }
    }
    return true;
  }

  static std::string ReadMarker(const std::filesystem::path& MarkerPath) {
    std::ifstream Input(MarkerPath, std::ios::binary);
    if (!Input) {
      return {};
    }
    return std::string(std::istreambuf_iterator<char>(Input),
                        std::istreambuf_iterator<char>());
  }

  std::filesystem::path BuildDirectory;
  std::filesystem::path TempRoot;
  std::string FixtureName;
  std::string FixturePrefix;
  std::filesystem::path Path;
  std::filesystem::path Marker;
  std::string MarkerText;
  bool Cleaned = false;
};

}  // namespace EmulativeDialog::WorkflowController::TestSupport
