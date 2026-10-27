# ED-0124/R8 WorkflowController.Contract Windows path failure

Captured: 2026-10-27 14:55 Asia/Shanghai

Task worktree: `H:/Qbot/EmulativeDialog.ED-0124-R8`

Central build root: `H:/Qbot/EmulativeDialog.integration-port-contract.build`

## Frozen validation result

The R8 configure and build commands exited 0. `AgentRuntime.VerticalSlice` passed. The focused Controller test failed from the task build junction:

```text
Working directory: H:/Qbot/EmulativeDialog.ED-0124-R8
Command: I:/cmake/bin/ctest.exe --preset windows-agent-slice -R "^WorkflowController\.Contract$" --output-on-failure
Exit code: 8
CTest output: WorkflowControllerContractFailure: Pinned context CLI did not load the immutable TaskSpec ReadSet
```

The unmodified `LastTest.log` is at `H:/Qbot/EmulativeDialog.integration-port-contract.build/ED-0124-R8/windows-agent-slice/Testing/Temporary/LastTest.log`.

## Path-length reproduction

A temporary diagnostic copy of the contract test was built under the central `.build` root. Launching it through the same long task junction reproduced the underlying child-process error:

```powershell
$env:PATH = 'I:/cmake/bin;C:/msys64/ucrt64/bin;' + $env:PATH
Set-Location H:/Qbot/EmulativeDialog.ED-0124-R8.build/windows-agent-slice
.\WorkflowControllerContractDiagnostic-20261027-1435.exe
```

```text
DEBUG_CONTEXT_EXIT=1
WorkflowControllerError: Git operation failed (exit 128): fatal: failed to stat 'a50d7c571ce7b2e72917ab0b139dcbaf23ef94e2:Workflow/Events/Bootstrap/P1.jsonl': Filename too long
WorkflowControllerContractFailure: Pinned context CLI did not load the immutable TaskSpec ReadSet
```

This isolates the failure to the deeply nested temporary Git fixture path used by the Controller contract test. The diagnostic copy and its binaries are under `H:/Qbot/EmulativeDialog.integration-port-contract.build/Diagnostics` and `.../D/R8`; no diagnostic file was added to the source worktree.

For a causal check, a second temporary copy changed only the fixture root expression to `std::filesystem::absolute(BuildDirectory) / "T"` and was launched through the same task junction:

```powershell
C:/msys64/ucrt64/bin/g++.exe -DYAML_CPP_STATIC_DEFINE @CMakeFiles/WorkflowControllerContractTests.dir/includes_CXX.rsp -g -std=c++20 -c H:/Qbot/EmulativeDialog.integration-port-contract.build/D/R8/WorkflowControllerContractTests-PathSafeDiagnostic.cpp -o H:/Qbot/EmulativeDialog.integration-port-contract.build/D/R8/WorkflowControllerContractTests-PathSafeDiagnostic.obj
C:/msys64/ucrt64/bin/g++.exe -g -Wl,--whole-archive H:/Qbot/EmulativeDialog.integration-port-contract.build/D/R8/WorkflowControllerContractTests-PathSafeDiagnostic.obj -Wl,--no-whole-archive -o H:/Qbot/EmulativeDialog.integration-port-contract.build/ED-0124-R8/windows-agent-slice/WorkflowControllerContractPathSafeDiagnostic.exe -Wl,--major-image-version,0,--minor-image-version,0 @CMakeFiles/WorkflowControllerContractTests.dir/linkLibs.rsp
$env:PATH = 'I:/cmake/bin;C:/msys64/ucrt64/bin;' + $env:PATH
Set-Location H:/Qbot/EmulativeDialog.ED-0124-R8.build/windows-agent-slice
.\WorkflowControllerContractPathSafeDiagnostic.exe
```

```text
WorkflowController AC-1..AC-6 contract fixtures passed
PATHSAFE_DIAGNOSTIC_EXIT=0
```

The shorter root made the complete test pass. Inspection then found generated fixture directories containing read-only Git object files; the existing test cleanup did not remove every fixture directory. The planned implementation must use the exact `BuildDirectory / "T"` parent, keep the marker guard, clear read-only attributes only inside the exact owned directory, skip reparse points, and remove the shared temp root only when empty.

## Scope and side effects

- No source file in the task worktree was changed during diagnosis; R8 still has only its two uncommitted CMake changes.
- All temporary binaries, copied sources, and fixture repositories were created under the centralized `.build` root.
- The failing test used synthetic local Git repositories only; no provider, QQ account, external service, credential, or live user data was involved.
- The test-source fix is outside `RootBuildAndDependencies` WriteRoots, so CR-ED-0124-009 is an explicit cross-module `IntegrationTask` with both module IDs and both module shards in ReadSet. Its four paths are exactly two BuildRelease roots and two WorkflowTools test roots. R9 adds the `WorkflowControllerTestWorkspaceCoverageReport` target; validation invokes it through the allowlisted CMake build command, and the target runs the pinned gcov tool internally. The JSON v2 report is selected by the `WorkflowControllerTestWorkspace.hpp` file record, and the evidence records its executed-line count, executable-line count, and calculated percentage.
