#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "WorkflowController.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace EmulativeDialog::WorkflowController {
namespace {

class ScopedHandle final {
 public:
  explicit ScopedHandle(HANDLE Handle = INVALID_HANDLE_VALUE) : Handle(Handle) {}
  ~ScopedHandle() {
    if (Handle != INVALID_HANDLE_VALUE && Handle != nullptr) {
      CloseHandle(Handle);
    }
  }
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  HANDLE Get() const { return Handle; }
  void Close() {
    if (Handle != INVALID_HANDLE_VALUE && Handle != nullptr) {
      CloseHandle(Handle);
      Handle = INVALID_HANDLE_VALUE;
    }
  }
  HANDLE Release() {
    const HANDLE Value = Handle;
    Handle = INVALID_HANDLE_VALUE;
    return Value;
  }

 private:
  HANDLE Handle;
};

std::wstring Utf8ToWide(const std::string& Value) {
  if (Value.empty()) {
    return {};
  }
  const int Required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            Value.data(),
                                            static_cast<int>(Value.size()),
                                            nullptr, 0);
  if (Required <= 0) {
    throw WorkflowError("Process argument is not valid UTF-8");
  }
  std::wstring Result(static_cast<std::size_t>(Required), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
                          static_cast<int>(Value.size()), Result.data(), Required) != Required) {
    throw WorkflowError("Unable to convert process argument to UTF-16");
  }
  return Result;
}

std::wstring QuoteWindowsArgument(const std::wstring& Argument) {
  std::wstring Quoted;
  Quoted.push_back(L'"');
  std::size_t Backslashes = 0;
  for (const wchar_t Character : Argument) {
    if (Character == L'\\') {
      ++Backslashes;
      continue;
    }
    if (Character == L'"') {
      Quoted.append(Backslashes * 2 + 1, L'\\');
      Quoted.push_back(L'"');
      Backslashes = 0;
      continue;
    }
    Quoted.append(Backslashes, L'\\');
    Backslashes = 0;
    Quoted.push_back(Character);
  }
  Quoted.append(Backslashes * 2, L'\\');
  Quoted.push_back(L'"');
  return Quoted;
}

std::filesystem::path ResolveExecutable(const std::string& Name) {
  const std::wstring WideName = Utf8ToWide(Name);
  std::vector<wchar_t> Buffer(32768, L'\0');
  const DWORD Length = SearchPathW(nullptr, WideName.c_str(), L".exe",
                                   static_cast<DWORD>(Buffer.size()), Buffer.data(), nullptr);
  if (Length == 0 || Length >= Buffer.size()) {
    throw WorkflowError("Validation executable is not available on PATH: " + Name);
  }
  return std::filesystem::path(std::wstring(Buffer.data(), Length));
}

std::filesystem::path CreateOutputPath() {
  static std::atomic<unsigned long long> Sequence{0};
  const auto Tick = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::string Name = "EmulativeDialog-WorkflowController-" +
      std::to_string(GetCurrentProcessId()) + "-" + std::to_string(Tick) + "-" +
      std::to_string(++Sequence) + ".log";
  return std::filesystem::temp_directory_path() / Name;
}

std::string ReadBoundedOutput(const std::filesystem::path& Path) {
  std::ifstream Input(Path, std::ios::binary);
  if (!Input) {
    return {};
  }
  constexpr std::size_t MaximumOutputBytes = 4U * 1024U * 1024U;
  std::string Output;
  Output.resize(MaximumOutputBytes);
  Input.read(Output.data(), static_cast<std::streamsize>(Output.size()));
  Output.resize(static_cast<std::size_t>(Input.gcount()));
  return Output;
}

std::string WindowsError(const std::string& Operation) {
  return Operation + ": " +
         std::system_category().message(static_cast<int>(GetLastError()));
}

}  // namespace

ProcessResult WindowsProcessRunner::Run(
    const std::vector<std::string>& Arguments,
    const std::filesystem::path& WorkingDirectory) {
  if (Arguments.empty()) {
    throw WorkflowError("Process invocation requires an executable");
  }
  if (!WorkingDirectory.is_absolute() || !std::filesystem::is_directory(WorkingDirectory)) {
    throw WorkflowError("Process working directory must be an existing absolute directory");
  }

  const std::filesystem::path Executable = ResolveExecutable(Arguments.front());
  std::wstring CommandLine = QuoteWindowsArgument(Executable.wstring());
  for (std::size_t Index = 1; Index < Arguments.size(); ++Index) {
    CommandLine.push_back(L' ');
    CommandLine += QuoteWindowsArgument(Utf8ToWide(Arguments[Index]));
  }
  std::vector<wchar_t> MutableCommandLine(CommandLine.begin(), CommandLine.end());
  MutableCommandLine.push_back(L'\0');

  const std::filesystem::path OutputPath = CreateOutputPath();
  SECURITY_ATTRIBUTES Inheritable{};
  Inheritable.nLength = sizeof(Inheritable);
  Inheritable.bInheritHandle = TRUE;
  ScopedHandle Output(CreateFileW(OutputPath.c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ, &Inheritable, CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY, nullptr));
  if (Output.Get() == INVALID_HANDLE_VALUE) {
    throw WorkflowError(WindowsError("Unable to create validation output capture"));
  }
  ScopedHandle Input(CreateFileW(L"NUL", GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &Inheritable, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
  if (Input.Get() == INVALID_HANDLE_VALUE) {
    const std::string Error = WindowsError("Unable to open NUL for validation stdin");
    Output.Close();
    DeleteFileW(OutputPath.c_str());
    throw WorkflowError(Error);
  }

  STARTUPINFOW Startup{};
  Startup.cb = sizeof(Startup);
  Startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  Startup.wShowWindow = SW_HIDE;
  Startup.hStdInput = Input.Get();
  Startup.hStdOutput = Output.Get();
  Startup.hStdError = Output.Get();
  PROCESS_INFORMATION Process{};
  const std::wstring WorkingDirectoryText = WorkingDirectory.wstring();
  if (!CreateProcessW(Executable.c_str(), MutableCommandLine.data(), nullptr, nullptr,
                      TRUE, CREATE_NO_WINDOW, nullptr, WorkingDirectoryText.c_str(),
                      &Startup, &Process)) {
    const std::string Error = WindowsError("Unable to start frozen validation command");
    Input.Close();
    Output.Close();
    DeleteFileW(OutputPath.c_str());
    throw WorkflowError(Error);
  }
  ScopedHandle ProcessHandle(Process.hProcess);
  ScopedHandle ThreadHandle(Process.hThread);
  Input.Close();
  Output.Close();

  constexpr DWORD TimeoutMilliseconds = 10U * 60U * 1000U;
  const DWORD WaitResult = WaitForSingleObject(ProcessHandle.Get(), TimeoutMilliseconds);
  if (WaitResult == WAIT_TIMEOUT) {
    TerminateProcess(ProcessHandle.Get(), ERROR_TIMEOUT);
    WaitForSingleObject(ProcessHandle.Get(), INFINITE);
    const std::string OutputText = ReadBoundedOutput(OutputPath);
    DeleteFileW(OutputPath.c_str());
    throw WorkflowError("Frozen validation timed out after ten minutes: " +
                        RedactSensitiveText(OutputText));
  }
  if (WaitResult != WAIT_OBJECT_0) {
    const std::string Message = WindowsError("Unable to wait for validation process");
    DeleteFileW(OutputPath.c_str());
    throw WorkflowError(Message);
  }
  DWORD ExitCode = ERROR_GEN_FAILURE;
  if (!GetExitCodeProcess(ProcessHandle.Get(), &ExitCode)) {
    const std::string Message = WindowsError("Unable to read validation exit code");
    DeleteFileW(OutputPath.c_str());
    throw WorkflowError(Message);
  }
  std::string CapturedOutput = ReadBoundedOutput(OutputPath);
  DeleteFileW(OutputPath.c_str());
  CapturedOutput = RedactSensitiveText(CapturedOutput);
  return {ExitCode, std::move(CapturedOutput)};
}

}  // namespace EmulativeDialog::WorkflowController
