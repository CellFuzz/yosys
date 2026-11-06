# 阶段 1B/1C：会话快照、发布与恢复绑定

本阶段完成 [InstrumentImplementationPlan.md](InstrumentImplementationPlan.md) 的 1B/1C，即分阶段提交表中的第 2 项。依据 [InstrumentDesign.md §3](InstrumentDesign.md#3-会话内分析结果交接)，目标是让后续消费端在同一会话取得 `cellfilter` 的明确结果，无需 DOT；设计内容不匹配时拒绝使用旧结果。观测规划、`cellinstrument`、manifest 和 RTLIL 插装仍属于后续交付。

## 代码组织与接口

| 路径 | 职责 |
| --- | --- |
| `passes/cmds/cellfuzz/analysis_snapshot.h/.cc` | 值快照、内部编码、完整批次存储、选择恢复与源绑定检查 |
| `passes/cmds/cellfuzz/structural_digest.cc` | 不修改设计的规范化结构摘要，包含相关模块定义 |
| `passes/cmds/cellfuzz/cellgraph.h/.cc` | 共用语义类型查询和保留连接索引，构图与恢复共用 |
| `passes/cmds/cellfilter.cc` | 调用开始失效、无条件选择、暂存各模块结果、最后统一发布 |
| `tests/cellfuzz/snapshot.cc/.py/.sh` | 独立测试消费端、编码/绑定用例和真实 CLI 会话回归 |

共享 API 位于 `Yosys::CellFuzz`，沿用 tab 缩进、PascalCase 类型名和 snake_case 成员名。新增源码仍只编入 `cellfilter` 组件一次；复用仓库已有 json11 和 SHA-1 实现，没有新增第三方依赖或 C++ 测试框架。

后续消费端优先使用一次完成读取、校验和绑定的入口：

```cpp
#include "passes/cmds/cellfuzz/analysis_snapshot.h"

using namespace Yosys::CellFuzz;

RestoredAnalysis analysis = SnapshotStore::restore(design, top->name);
const CellGraph &graph = *analysis.graph;
const CellGraphSelection &selection = analysis.selection;
const CellGraphView &view = analysis.view;
```

`SnapshotStore::load()` 只解码值，适合检查快照元信息；单独调用它不证明设计仍匹配。消费这些值前必须调用 `snapshot.restore(design)`，或直接采用上面的组合入口。缺失、版本不支持、记录损坏、过期或绑定失败均通过 `log_cmd_error` 报错，要求重新运行 `cellfilter`，不自动重新评分。

`RestoredAnalysis` 中的图和信号是本次消费调用的 live 绑定，设计修改或切换后必须丢弃。`AnalysisSnapshot` 本身只有字符串、配置、整数集合和 JSON 值，可按值保留；不含 `Cell*`、`Wire*`、`SigSpec`、`SigMap` 或回调函数。

## 内部值记录与身份

当前 `Design::scratchpad["cellfuzz.analysis"]` 存放版本 2 的 JSON 批次：外层为 `version` 和 `modules`。每个模块记录包含：

| 字段 | 内容 |
| --- | --- |
| `version` / `module` | 内部版本和未反转义的模块原名 |
| `build_options` | register、memory、FSM 实际生效角色 |
| `strategy` | 策略 ID、版本和实际参数；内置为 `all` 或 `region-topk`，记录 `top_k` |
| `structure_digest` | 当前模块及相关定义的规范化结构摘要 |
| `analysis_id` | 去除本字段后，对完整规范化快照计算的内容摘要 |
| `graph` | 原图 cell 顺序、语义类型/角色/保留原因，全部 port 和 raw/canonical 绑定，原图保留连接 |
| `selection` | 明确 cell/module port 集合，以及 region 成员、关键 cell 和上下游边界说明 |

图的原始 ID 由记录数组位置保留，恢复不依赖当前 RTLIL 字典迭代顺序。cell 和 wire 通过原始 RTLIL 名称查找；名称直接由 JSON 字符串编码，不使用分隔符拼接或显示名称作为身份。wire bit 表示为 `[原始名称, RTLIL逻辑偏移]`，常量 bit 保存其 `RTLIL::State` 数值；bit 数组按 bit 0 起保存有序引用，重复 bit、X/Z、切片和拼接不丢失。声明 offset、方向等由结构摘要覆盖，不能把逻辑偏移误作 HDL 下标。

类型、参数和来源属性在结构摘要确认一致后，从当前对象拷贝到恢复的 `GraphCell` / `GraphModuleInfo`。这样避免在会话记录中再次保存相同参数与来源，同时保证记录对应分析时的值。恢复保留的是原图连接；折叠 view 再从保存的明确集合构造，不以 view 替换原图。

自定义 region 策略调用 `AnalysisSnapshot::capture(graph, selection, strategy)` 时显式提供自己的策略 ID、正版本和参数。快照保存已经选定的集合和说明，不编码或恢复回调。即使策略返回空集或非固定大小集合，恢复也不调用它或回退到 top-k。

该 JSON 是内部会话编码，不提供选择文件输入或分析文件导入。编码/摘要不承诺跨源码或跨 Yosys 版本兼容。SHA-1 复用已有工具，用于内容识别和意外变化检测，不是对用户可编辑 scratchpad 的身份认证；即使记录重新计算了内容 ID，源名称、字段类型和 bit 范围仍需校验。

## 结构摘要范围和规范化

`structural_digest(module)` 读取当前模块，并递归包含 cell 类型所引用的模块定义；使用已访问集合处理共享依赖和环状引用，未引用模块不影响摘要。规范化过程不调用 `module->sort()`，不修改原 RTLIL。

- 模块：原名、属性、可用参数名及参数默认值、原 module port 顺序。
- wire：原名、位宽、声明 offset、upto/signed、port ID/方向及属性，包含初始化。
- cell：原名、类型、全部参数、属性及完整有序端口信号。
- 模块连续连接：完整左右 SigSpec；无序连接记录整体排序，每侧 bit 顺序和重复 bit 保留。
- RTLIL memory 定义：原名、位宽、offset、深度、属性；collected memory 的参数和业务连接由 cell 记录覆盖。
- process：来源属性、case/switch 比较和信号、动作、sync 类型/信号及 memory 写动作/优先级。存在优先级语义的列表保持原序。

按名称建立的 JSON object 对无序字典规范排序；常量使用 flags 和完整 bit 字符串，字符串长度保留位宽，X/Z/其他内部状态不转成宿主整数。临时选择栈、scratchpad、输出路径和分析时间戳不作为摘要输入。设计自身的来源/初始化等属性仍视为内容。

## 发布与恢复流程

`cellfilter` 的执行顺序：

```text
清除旧批次（早于参数解析）
  → 解析参数并执行原有前置检查
  → 对每个目标模块构图、选择并暂存值快照
  → 可选地构造 view、输出 DOT
  → 检查请求的 DOT 写入、flush 和 close 结果
  → 单次 scratchpad 写入发布完整模块批次
```

任何参数、模块处理或 DOT I/O 失败都不发布新结果，旧批次也不可再消费。目标模块为空时发布空批次；新调用只分析部分模块时，以该批次整体替换旧批次，不保留未再分析模块的旧结果。是否输出 DOT、输出到哪个路径都不影响同一参数下的快照内容。构图与选择不增加功能 wire/cell/port 或插装标记。

恢复先检查内容 ID、模块是否存在和结构摘要，再分配新的 live 图，按保存的名称解析当前 cell/wire。校验类型、角色、保留原因、端口方向/宽度和 raw 连接；每个 wire bit 的偏移必须在当前位宽内。canonical 绑定既保留原值，又通过当前 SigMap 验证与 raw 绑定逐位等价，因此允许无序连接记录重排后的等价代表变化。

恢复随后检查保留连接和明确集合的 ID 范围、成员合法性及 region 说明，用共用 `index_connections()` 建立邻接，再执行 `create_view(selection)`。这里不执行 `build()` 的路径筛选，也不执行 `select_objects()`，原图 ID、保留对象和选择说明均来自快照。

## Design 操作的边界

会话存储关联当前 `Design` 的 scratchpad，没有进程全局的 Design 指针表。当前 `design` 命令实现不保存/复制 scratchpad 中的分析批次，也不会因模块 clone 而传递旧 live 指针：

| 操作后状态 | 本阶段行为 |
| --- | --- |
| 只读 `stat` / `ls`、更换临时选择、无序记录排序 | 摘要不变，仍可恢复 |
| reset/push 后模块不存在 | 拒绝并要求重新分析 |
| load/pop 得到结构不同的模块 | 摘要不匹配，拒绝 |
| load/pop 得到完全匹配的同名模块 | 当前仍存在的批次可以按名称绑定到新对象 |
| save 旧设计后，又运行新的 cellfilter，再 load 旧设计 | 不恢复旧分析批次；仍只检查最近发布批次是否匹配当前模块 |

表中描述的是当前实现的受测行为，不增加 `design -save/-load/-push/-pop` 的分析结果持久化契约。跨 Design 的测试复制值记录，是为验证无旧指针依赖，不是新增文件导入能力。

## 测试点和复现

测试消费端仅链接共享头文件和自身代码，运行时使用 Yosys 中的生产实现。验证既检查编码往返，也实际恢复图、源 bit 和 view，不以 JSON 字段存在作为验收依据。

| 验收点 | 自动检查 |
| --- | --- |
| 无 DOT 交接 | CLI 无配额、K=1/2、`-nodes none -topk 1` 均发布；开/关 DOT 的完整快照相等 |
| 不修改功能 RTLIL | 发布及消费前后 `write_rtlil` 字节相等；保留阶段 1A 的 DOT 和 RTLIL 回归 |
| 批次替换和查找 | 两模块分别读回；重跑更换 K 改变集合；仅分析一个模块或空目标后旧模块结果不可读 |
| 失败不留结果 | 同一进程捕获非法参数、文件打开失败、`/dev/full` 写失败；第二个模块失败时也没有部分发布 |
| 特殊名与位引用 | 转义引号、括号、反斜杠名称，来源字符串、超过 64 bit 的参数，raw/canonical 别名、重复 bit、X/Z |
| 确定性恢复 | 销毁原 Design，在 cell 创建顺序不同的新 Design 恢复；重新捕获的编码和原始 DOT 精确相等 |
| 不重选 | 自定义策略带独立 ID/版本，分别返回空集和超过 K=1 的集合；销毁原 Design/回调后恢复，集合与编码仍准确，不回退到内置 K |
| 过期拒绝 | 初始化、位宽/声明/方向、参数、类型、连接位序/重复位/常量、模块连接、删除或重命名对象改变均拒绝 |
| 依赖定义 | 未引用模块不影响摘要，相关递归定义和 process 动作变化被检出，环状定义遍历终止 |
| 缺失/损坏 | 缺失模块或批次、未知版本、损坏内容 ID、错误 JSON，以及自哈希但 bit 越界的记录均拒绝 |
| Design 操作 | reset/save/load/push/pop/push-copy 的缺失或精确重新绑定行为符合上表；分析后 load/pop 切回不同结构时拒绝旧结果 |
| 空图 | 无 cell/port 的模块可编码、发布和恢复 |
| 状态类型 | `stateful.v` 的 register、collected memory 和显式 FSM，启用/排除状态类型后均实际恢复，RTLIL 保持不变 |

开发先确认共享快照接口缺失时测试失败，并确认接入发布前，无 DOT 的真实消费端报 `analysis is missing`。失败调用检查在同一 Yosys 进程完成，避免进程结束掩盖旧批次残留。

从仓库根目录复现：

```sh
cmake --build build -j 4
make -C tests cellfuzz/Makefile various/Makefile
make -C tests/cellfuzz all summary
make -C tests/various cellfilter.sh
git diff --check
```

独立运行新增回归：

```sh
YOSYS="$PWD/build/yosys" YOSYS_CONFIG="$PWD/build/yosys-config" bash tests/cellfuzz/snapshot.sh
```

测试依赖沿用阶段 1A：启用动态插件的 Yosys、同一构建的 `yosys-config`、C++ 编译器、Python 3、Bash；写失败用例使用 Linux 的 `/dev/full`。临时插件和比较产物自动清理。负例诊断被测试捕获并检查，测试包装器的结果必须结合 `.result` 和 `summary` 判断。

## 本阶段验收记录

- 现有 CMake 配置的完整构建通过，构建日志没有编译告警或错误。
- `make -C tests/cellfuzz all summary`：`shared_graph.sh` 和 `snapshot.sh` 均为 PASS，summary 为 2 passed / 0 failed。前者保留阶段 1A 的 DOT 基线与 RTLIL 检查；后者执行本阶段的接口消费、发布、恢复与拒绝用例。
- `make -C tests/various cellfilter.sh`：PASS，原有 register/memory/FSM node 配置和排除状态屏障保持兼容。
- 故障注入：临时反转结构摘要相等条件，正常恢复失败；临时绕过该条件，过期负例在 `expect_error` 断言处失败；临时移除 `cellfilter` 开始处的失效调用，同进程失败调用用例在旧批次残留断言处失败。三种注入均被检测，生产源码已与备份逐字节核对恢复，随后完整构建和上述回归通过。
- 代码审查覆盖正确性、可读性、模块边界、输入校验和遍历成本；独立只读审查未发现需要修复的问题。内部 JSON 使用现有库，不增加外部导入入口或认证承诺。
- `git diff --check` 通过。

这些记录验收分阶段提交表第 2 项（实施计划 1B/1C）。后续观测规划、manifest、RTLIL 插装和 Verilator 侧工作仍按原计划交付。

## 2026-11-06 摘要引擎修正

版本 2 使用命令期 `StructuralDigestContext`，每个可达模块只生成一次局部记录，再按 SCC 的有序成员及外部依赖合成摘要。cellfilter 的所有目标共享同一上下文；恢复时重新针对当前 Design 计算，不复用分析时缓存。版本 1 批次明确拒绝并提示重跑 cellfilter；SelectionStrategy 版本与 manifest schema 不变，不透明 structure_digest/analysis_id 值发生变化。

`snapshot.cc` 固定链、菱形、共享实例、自环、双模块循环、循环叶子、孤立节点、创建顺序、clone/save/load、child 参数/连接/init/process 优先顺序和无关修改的行为。4096 个根的局部序列化计数精确等于 4096，深链使用显式栈。墙钟及 RSS 是补充证据，命令和样本见 [修复验收记录](ReviewFixResults.md)；以上不改变原批次完整发布与 cellfilter 只读 RTLIL 的边界。
