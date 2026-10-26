# P1 CODE-001 PascalCase 路径与标识符静态审计

## 审计结论

审计固定基线 d151c5b3287718964d376ad550953e4d34890a08 的完整 Git 跟踪树：81 个文件路径及其 40 个隐含目录，共 121 个路径项。路径检查发现 0 项；C++ 命名检查覆盖全部 16 个 Git 跟踪 C++ 文件，发现 0 个 PascalCase / 宏命名问题；CMake 项目名和 4 个目标均为 PascalCase。

这是针对 CODE-001 的静态命名证据。它不验证产品行为，也不表示 lint 已接入 CI 或 P1 ExitGate 已签署。Workflow/EnforcementMatrix.yaml 中 CODE-001 仍为 Planned，MAN-003 仍为 Pending；Workflow/CurrentState.md 也记载 P1 ExitGate Pending。该证据不更新计划或矩阵状态。

审计对象是 EnforcementMatrix 的 RuleId CODE-001。P1 计划没有 TaskId=CODE-001 的独立 TaskSpec，也没有适用于跨目录路径/标识符审计的单一 ModuleId；因此使用任务派发中锁定的基线和范围，不推导或修改其他任务字段。仓库根不存在 .codegraph/，按指引未调用 CodeGraph。

## 规则、范围与例外

规则依据 Standards/CppEngineeringStandard.md 的“大驼峰定义”“必需名称例外”和“静态检查”章节：

- 项目自有目录名、文件基名、namespace 片段、类型、函数/方法、枚举值、成员/局部变量/参数/模板参数、常量/constexpr 变量及 CMake target 使用 PascalCase。
- C/C++ 宏按 UPPER_SNAKE_CASE 检查。
- 路径后缀扩展名（如 .cpp、.yaml、.json）不作为基名字符参与检查。
- 精确工具名例外包括 AGENTS.md、CMakeLists.txt 和 CMakePresets.json。基线中实际命中这三项；其余已列入标准但基线中不存在的固定名称不视为实际例外。
- 第三方库、Windows SDK、标准库和外部 API 的名称保留上游拼写。它们不属于本仓库跟踪路径或项目自有声明；C++ 检查的头文件过滤限定在本审计 worktree。

范围覆盖完整基线树，而不只覆盖标准路径检查器规定的 Source、Tests、Standards、Architecture 和 Workflow：同时检查根级文件、CMake、Goals、Research、Tools。Git 元数据和基线中不存在的生成文件/空目录不属于 Git 树分母；审计 worktree 在运行前无未跟踪或忽略文件。

## 工具、命令与结果

| 用途 | 工具版本 | 结果 |
|---|---|---|
| 固定审计树和文件清单 | Git 2.53.0.windows.1 | git ls-tree -r --name-only，退出码 0；81 个文件 |
| 路径命名 | PowerShell 7.6.9 | 下方脚本，退出码 0；121 个路径项、0 个发现 |
| C++ 标识符命名 | C:/Users/C1373/AppData/Local/Programs/Python/Python312/Scripts/clang-tidy.exe，LLVM 22.1.0；MinGW-w64 target x86_64-w64-windows-gnu；libstdc++ headers 16.1.0 | 下方脚本，退出码 0；16/16 文件解析，命名发现 0 |
| CMake 项目/目标定位 | ripgrep 15.1.0 | 下方命令，退出码 0；项目名和 4 个 target 均符合 |
| 依赖解析输入 | yaml-cpp commit 56e3bb550c91fd7005566f19c079cb7a503223cf | 与基线 CMake/DependencyPins.cmake 固定提交一致；只作为第三方 include 输入，未修改 |

实际可执行文件与版本核对（PowerShell 7.6.9）：

- Get-Command clang-tidy -All 的首个 PATH 命中为 C:/Users/C1373/AppData/Local/Programs/Python/Python312/Scripts/clang-tidy.exe；PowerShell 的裸命令 clang-tidy 使用该路径。核验命令 & 'C:/Users/C1373/AppData/Local/Programs/Python/Python312/Scripts/clang-tidy.exe' --version 输出 LLVM 22.1.0，退出码 0。
- 第二个 PATH 命中为 C:/msys64/ucrt64/bin/clang-tidy.exe；核验命令 & 'C:/msys64/ucrt64/bin/clang-tidy.exe' --version 输出 LLVM 22.1.4，退出码 0。此可执行文件没有用于本次审计。

### 路径检查

检查器以基线 Git tree 为分母，加入每个文件的所有目录前缀，并按区分大小写方式核验每个路径项。基名结构规则为 ^[A-Z][a-z0-9]*(?:[A-Z][a-z0-9]*)*$：首字母大写、无分隔符，并以大写边界表达后续 PascalCase 词段。检查器剥离最后一个文件扩展名；只对标准明确列出的三个实际固定文件名做精确豁免。

在 worktree 根目录的 PowerShell 7.6.9 中运行：

    $Repo = 'H:\Qbot\EmulativeDialog.p1-code-audit'
    $Base = 'd151c5b3287718964d376ad550953e4d34890a08'
    $Files = @(git -C $Repo ls-tree -r --name-only $Base)
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $AllPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    foreach ($Path in $Files) {
        [void]$AllPaths.Add($Path)
        $Parts = $Path -split '/'
        for ($i = 1; $i -lt $Parts.Count; $i++) {
            [void]$AllPaths.Add(($Parts[0..($i - 1)] -join '/'))
        }
    }
    $Exceptions = @('AGENTS.md', 'CMakeLists.txt', 'CMakePresets.json')
    $Rule = '^[A-Z][a-z0-9]*(?:[A-Z][a-z0-9]*)*$'
    $Violations = [System.Collections.Generic.List[object]]::new()
    foreach ($Entry in $AllPaths) {
        $Leaf = ($Entry -split '/')[-1]
        $IsFile = $Files -ccontains $Entry
        if ($IsFile -and $Exceptions -ccontains $Leaf) { continue }
        $Stem = $Leaf
        if ($IsFile -and $Leaf.Contains('.')) {
            $Stem = $Leaf.Substring(0, $Leaf.LastIndexOf('.'))
        }
        if ($Stem -notmatch $Rule) {
            $Violations.Add([pscustomobject]@{ Path = $Entry; Stem = $Stem })
        }
    }
    "TRACKED_FILES=$($Files.Count)"
    "DIRECTORIES=$($AllPaths.Count - $Files.Count)"
    "PATH_ENTRIES=$($AllPaths.Count)"
    "VIOLATIONS=$($Violations.Count)"
    if ($Violations.Count -gt 0) { $Violations | Format-Table -AutoSize; exit 1 }
    exit 0

结果：TRACKED_FILES=81, DIRECTORIES=40, PATH_ENTRIES=121, VIOLATIONS=0，进程退出码 0。精确实际例外为 AGENTS.md、CMakeLists.txt、CMakePresets.json。

### C++ 标识符检查

使用 clang-tidy 的 readability-identifier-naming。DefaultCase=CamelCase 覆盖未单独配置的 C++ 声明类别；clang-tidy 将此模式称为 CamelCase，其形式符合本项目的 PascalCase。MacroDefinitionCase=UPPER_CASE 覆盖宏规则。所有此检查的诊断被设为错误；检查文档见 [clang-tidy readability-identifier-naming](https://clang.llvm.org/extra/clang-tidy/checks/readability/identifier-naming.html)。

命令从固定基线的 Git tree 选择全部 C++ 扩展名文件，直接传入全部 16 个文件；为 WorkflowControllerContractTests.cpp 加入 CMake 已配置的 include path。头文件过滤限定为当前审计 worktree；MSYS2 标准头和精确锁定的 yaml-cpp 源码仅用于解析。

    $Repo = (Get-Location).Path.Replace('\', '/')
    $Base = 'd151c5b3287718964d376ad550953e4d34890a08'
    $CppFiles = @(git ls-tree -r --name-only $Base |
        Where-Object { $_ -match '\.(h|hh|hpp|hxx|c|cc|cpp|cxx)$' } |
        Sort-Object)
    $Common = @(
        '--target=x86_64-w64-windows-gnu', '-stdlib=libstdc++', '-std=c++20',
        '-x', 'c++', '-I', $Repo, '-I', "$Repo/Tools/WorkflowController",
        '-isystem', 'H:/Qbot/EmulativeDialog.integration.build/windows-mingw/_deps/yamlcpp-src/include',
        '-isystem', 'C:/msys64/ucrt64/include/c++/16.1.0',
        '-isystem', 'C:/msys64/ucrt64/include/c++/16.1.0/x86_64-w64-mingw32',
        '-isystem', 'C:/msys64/ucrt64/include/c++/16.1.0/backward',
        '-isystem', 'C:/msys64/ucrt64/include',
        '-isystem', 'C:/msys64/ucrt64/include-fixed'
    )
    $Config = "{CheckOptions: [{key: readability-identifier-naming.DefaultCase, value: CamelCase}, {key: readability-identifier-naming.MacroDefinitionCase, value: UPPER_CASE}] }"
    $FailedFiles = [System.Collections.Generic.List[string]]::new()
    foreach ($File in $CppFiles) {
        clang-tidy --checks='-*,readability-identifier-naming' --warnings-as-errors='readability-identifier-naming' --header-filter='^H:[\\/]+Qbot[\\/]+EmulativeDialog\.p1-code-audit[\\/]+' --config=$Config $File -- $Common
        $ExitCode = $LASTEXITCODE
        if ($ExitCode -ne 0) { $FailedFiles.Add("$File (exit $ExitCode)") }
    }
    "CPP_FILE_COUNT=$($CppFiles.Count)"
    "CLANG_TIDY_FINDING_COUNT=$($FailedFiles.Count)"
    if ($FailedFiles.Count -gt 0) { $FailedFiles; exit 1 }
    exit 0

结果：CPP_FILE_COUNT=16, CLANG_TIDY_FINDING_COUNT=0，进程退出码 0。最终调用没有命名诊断。命令仅启用了 readability-identifier-naming；其他 clang-tidy 检查未运行，因此没有其他检查的诊断结果。

C++ 子分母（16）：

- CMake/BuildProbe.cpp
- Source/Channels/Contracts/ChannelAdapterPort.h
- Source/Providers/Contracts/ModelProviderPort.h
- Tests/Contract/Channels/Contracts/ChannelAdapterPortContractTests.cpp
- Tests/Contract/Channels/Contracts/MockChannelAdapter.cpp
- Tests/Contract/Channels/Contracts/MockChannelAdapter.h
- Tests/Contract/Channels/Contracts/MockChannelAdapterContractTests.cpp
- Tests/Contract/Providers/Contracts/FakeProvider.cpp
- Tests/Contract/Providers/Contracts/FakeProvider.h
- Tests/Contract/Providers/Contracts/FakeProviderContractTests.cpp
- Tests/Contract/Providers/Contracts/ModelProviderPortContractTests.cpp
- Tests/Contract/WorkflowController/WorkflowControllerContractTests.cpp
- Tools/WorkflowController/Main.cpp
- Tools/WorkflowController/WindowsProcessRunner.cpp
- Tools/WorkflowController/WorkflowController.cpp
- Tools/WorkflowController/WorkflowController.h

第一次全文件调用漏掉 Tools/WorkflowController include path，在 Tests/Contract/WorkflowController/WorkflowControllerContractTests.cpp 因找不到 WorkflowController.h 退出 1。上面的最终命令加入 CMakeLists.txt 为 WorkflowControllerCore 配置的包含目录后，16 个文件全部成功解析。该错误是命令参数遗漏，不是源码发现。

### CMake 名称检查

    rg -n -A 3 '^\s*(project|add_executable|add_library|add_custom_target)\s*\(' CMakeLists.txt CMake

退出码 0。定位结果：

- CMakeLists.txt:3 — project EmulativeDialog
- CMakeLists.txt:14 — executable BuildProbe
- CMakeLists.txt:28 — library WorkflowControllerCore
- CMakeLists.txt:43 — executable WorkflowController
- CMakeLists.txt:47 — executable WorkflowControllerContractTests

五个名称均为 PascalCase；没有 add_custom_target 声明。CTest 名称不属于 CMake target 名称规则。

## 发现与定位

| 类别 | 发现数 | 定位 |
|---|---:|---|
| 基线路径 / 目录名 | 0 | 无违规路径 |
| C++ 声明标识符 | 0 | 16 个 C++ 文件均无命名诊断 |
| C++ 宏 | 0 | 16 个 C++ 文件均无宏命名诊断 |
| CMake project / target | 0 | CMakeLists.txt 的上述五个名称均符合 |

没有发现需要代码修复的命名偏差。未运行构建或测试；未修改源代码、P1 计划、任务计划、EnforcementMatrix 或其他既有文件。

## 文件分母清单

以下 81 个路径逐项构成 BaseCommit 的文件分母；40 个目录由这些路径的前缀完整导出。

    AGENTS.md
    Architecture/Contracts/ChannelAdapterContract.md
    Architecture/Contracts/CoreAgentContract.md
    Architecture/Contracts/MemoryContract.md
    Architecture/Contracts/ModelProviderContract.md
    Architecture/Decisions/Adr0001CppRuntime.md
    Architecture/Decisions/Adr0002CodexAgentCppRewrite.md
    Architecture/ModuleMap.yaml
    Architecture/Modules/BuildRelease.yaml
    Architecture/Modules/Channels.yaml
    Architecture/Modules/HostCoreAgent.yaml
    Architecture/Modules/MemoryStorage.yaml
    Architecture/Modules/Platform.yaml
    Architecture/Modules/Providers.yaml
    Architecture/Modules/Tui.yaml
    Architecture/Modules/WorkflowTools.yaml
    Architecture/SystemArchitecture.md
    Architecture/TuiApi.md
    CMake/BuildProbe.cpp
    CMake/DependencyPins.cmake
    CMakeLists.txt
    CMakePresets.json
    Goals/ProductGoals.md
    ProjectOverview.md
    Research/Platforms/PlatformCapabilityMatrix.md
    Research/TuiReferenceMatrix.yaml
    Source/Channels/Contracts/ChannelAdapterPort.h
    Source/Providers/Contracts/ModelProviderPort.h
    Standards/CppEngineeringStandard.md
    Standards/DependencyPolicy.md
    Standards/SecurityAndPrivacy.md
    Tests/Contract/Channels/Contracts/ChannelAdapterPortContractTests.cpp
    Tests/Contract/Channels/Contracts/MockChannelAdapter.cpp
    Tests/Contract/Channels/Contracts/MockChannelAdapter.h
    Tests/Contract/Channels/Contracts/MockChannelAdapterContractTests.cpp
    Tests/Contract/Context/Fixtures/ContextBudgetCases.json
    Tests/Contract/Memory/Fixtures/FixtureIndex.json
    Tests/Contract/Memory/Fixtures/ForgetRecoveryCases.json
    Tests/Contract/Memory/Fixtures/ScopeConsentCases.json
    Tests/Contract/Providers/AnthropicMessages/Fixtures/CanonicalRequest.json
    Tests/Contract/Providers/AnthropicMessages/Fixtures/ErrorCases.json
    Tests/Contract/Providers/AnthropicMessages/Fixtures/FixtureIndex.json
    Tests/Contract/Providers/AnthropicMessages/Fixtures/StreamCases.json
    Tests/Contract/Providers/Contracts/FakeProvider.cpp
    Tests/Contract/Providers/Contracts/FakeProvider.h
    Tests/Contract/Providers/Contracts/FakeProviderContractTests.cpp
    Tests/Contract/Providers/Contracts/ModelProviderPortContractTests.cpp
    Tests/Contract/Providers/OpenAIChatCompletions/Fixtures/CanonicalRequest.json
    Tests/Contract/Providers/OpenAIChatCompletions/Fixtures/ErrorCases.json
    Tests/Contract/Providers/OpenAIChatCompletions/Fixtures/FixtureIndex.json
    Tests/Contract/Providers/OpenAIChatCompletions/Fixtures/StreamCases.json
    Tests/Contract/Providers/OpenAIResponses/Fixtures/CanonicalRequest.json
    Tests/Contract/Providers/OpenAIResponses/Fixtures/ErrorCases.json
    Tests/Contract/Providers/OpenAIResponses/Fixtures/FixtureIndex.json
    Tests/Contract/Providers/OpenAIResponses/Fixtures/StreamCases.json
    Tests/Contract/WorkflowController/WorkflowControllerContractTests.cpp
    Tools/WorkflowController/Main.cpp
    Tools/WorkflowController/WindowsProcessRunner.cpp
    Tools/WorkflowController/WorkflowController.cpp
    Tools/WorkflowController/WorkflowController.h
    Workflow/AgentExecutionStandard.md
    Workflow/AgentPool.yaml
    Workflow/ChangeRequestTemplate.md
    Workflow/CodexCapabilityMap.yaml
    Workflow/CodexCapabilityMap/AgentCore.yaml
    Workflow/Communications/Bootstrap/P1.jsonl
    Workflow/CurrentState.md
    Workflow/EnforcementMatrix.yaml
    Workflow/Events/Bootstrap/P0.jsonl
    Workflow/Events/Bootstrap/P1.jsonl
    Workflow/MultiAgentCollaborationStandard.md
    Workflow/OpenDecisions.md
    Workflow/Plans/P0.yaml
    Workflow/Plans/P1.yaml
    Workflow/Plans/P2.yaml
    Workflow/Plans/P3.yaml
    Workflow/Roadmap.md
    Workflow/TaskIndex.yaml
    Workflow/TaskLifecycle.md
    Workflow/TaskReportTemplate.md
    Workflow/TaskTemplate.yaml

## 变更记录

- BaseCommit: d151c5b3287718964d376ad550953e4d34890a08
- CR-ED-0013-008 的 Submitted EventId 99141cee-01a0-4e93-b151-4f870bb22acb 与 Approved EventId 7fb1dcac-49b6-475e-bbe2-ada1bf9af27f 记录在集成提交 68e83fa16f5fddd2c9c75baee1d64bff04111a93 的 Workflow/Events/Bootstrap/P1.jsonl；ApprovalTiming 更正 EventId 7ad92d16-3496-46c1-96b5-9a902b69d997 记录在集成提交 7b3ccd7a4df1d5f3eec144130cbfa76cede2b21e。CR 仅批准证据交付路径，P1 计划未修改。
- 原始路径偏差：第一次对默认目录执行的 apply_patch 是未提交草稿；草稿文件已撤回，没有进入任何 commit。H:\Qbot\EmulativeDialog\Workflow\Evidence 留下空目录，按 Coordinator 指示保留供只读复核；撤回后未再写入原始 worktree。
- 正确路径时间线：Coordinator 先在本线程消息中批准 PascalCase 路径 Workflow/Evidence/P1Code001.md，之后 Agent 才在 Task/P1-CODE-001-Audit 隔离 worktree 写入正确路径。该路径的首个 branch commit 是 4ed8910a0e82ac8f85c6768411bb92fe9e48ddc4，提交时间为 2026-10-27 02:39:37+08:00。
- Submitted/Approved Event 的 OccurredAt 均为 2026-10-27 02:40:26+08:00，即首个正确路径提交之后 49 秒；Correction Event 7ad92d16-3496-46c1-96b5-9a902b69d997 澄清 OccurredAt 表示写入 EventStore 的登记时间，不是审批决定时间。RequestedBeforeArtifactWrite 与 ApprovalPrecededArtifactWrite 依据协调者消息先于正确路径写入的顺序；精确的会话消息时钟未保存，因此不能由这些 Event 时间推断决策时刻。
- 后续提交 fde056d1325b0bcec6e529654170ada1e0111f38 与 1a7bf6cdffef79a94c95bcd1bcbde5297252d2ea 仅修正审计报告措辞；首个正确路径 artifact commit 仍为 4ed8910a0e82ac8f85c6768411bb92fe9e48ddc4。
- 本报告只覆盖上述固定基线的静态名称/路径规则；不代替独立 Reviewer 或 Coordinator 对 P1 ExitGate 的决定。
