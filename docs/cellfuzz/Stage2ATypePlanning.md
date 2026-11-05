# 阶段 2A：cellinstrument 入口、预检与类型规划

本阶段执行 [实施计划 §2A](InstrumentImplementationPlan.md#2a-入口预检与类型处理)，依据 [设计 §4](InstrumentDesign.md#4-首版观测规则)。验收对象是保存的 selection 中普通 cell 的全部输出、mux 全部选择端口和 register Q；规划前后 RTLIL 与分析批次不变。

后续状态：2B 已完成完整 manifest，3A 已开放普通旁路物化；`-plan-only` 现输出 `stage: observation-planning`，省略该选项输出 `status: materialized`。本页保留 2A 交付时的类型报告说明和验收记录。当前 schema 与确定性导出 ID 见 [Stage2BManifest.md](Stage2BManifest.md)，物化见 [Stage3AMaterialization.md](Stage3AMaterialization.md)，以下“当前/将”描述均指 2A 阶段。

## 边界与接口

- 新入口 `cellinstrument -plan-only -manifest <file>`。正式物化尚未交付，省略 `-plan-only` 明确失败。
- 接收设计约定的 `-mem-access on|off`（默认 on）、`-mem-content off|query`（默认 off）、`-chunk-width <正整数>`（默认 256）、`-prefix <ASCII标识符>`（默认 cf）；不接收选择参数或命令尾部 selection。
- 当前产物明确标为 `stage: type-planning`、`status: planned`，只描述配置、分析身份、观测源和寄存器静态语义。它是 2A 的可审查类型计划；2B 将扩充完整图、确定性 transport 布局及正式 manifest 契约。本阶段不声称提供可逆位段或新增接口。
- 恰好一个目标 top，且为完整模块选择；无 process、未收集 memory、模块实例、blackbox/whitebox top 或已有 `cellfuzz_instrumented` 标记。先恢复有效分析快照，不重新评分。
- 整个目标模块检查可识别的内建类型和正式端口方向，缺失/未知端口、显式 FSM、隐式全局时钟 FF 和不能确认导出模型的类型报错，即使没有选中它也不能把其存在隐藏为可导出设计。
- 组合逻辑使用 Yosys 内建 evaluable 类型及显式的 buffer barrier / tristate 类型。普通组合类型具有仓库 simlib/simcells 模型；这不保证 `write_verilog` 会将每种类型内联为行为表达式，也不调用会改写设计的后端作探测。
- 默认 `flatten` 生成的无端口 `$scopeinfo` 只保存来源元数据，Verilog 后端会忽略；它可以保留在原图登记中，不生成观测项。
- 被选 memory 若启用业务观测或 query，则报告相应能力尚未交付；两项关闭时登记关闭原因，不为 memory 生成普通组合输出项。不改变默认值来绕过阶段限制。

共享模型位于 `passes/cmds/cellfuzz/monitor_plan.h/.cc`；入口位于 `passes/cmds/cellinstrument.cc`，仍由 cellfilter 组件编译一次并声明 `PROVIDES cellinstrument`。遵循已有 tab 缩进、PascalCase 类型和 snake_case 成员规范，例如：

```cpp
RestoredAnalysis analysis = SnapshotStore::restore(design, top->name);
MonitorPlan plan = create_monitor_plan(analysis, options);
```

计划只在本次命令内使用；源 `SigSpec` 引用当前设计，修改或切换设计前必须丢弃。观测项分别保存 graph cell/port ID、role 和原始有序源绑定。原 module port 复用原接口，不生成旁路项；内部源与 module port 别名时仍有独立身份。

Register 由 `FfData` 解析，使用 `FfInitVals` 读取 Q 别名上的初值；保存时钟、极性、控制优先级、latch/细粒度标志和 reset 值。当前控制和 AD 进入静态记录，D 仍在原图源绑定中；只有 Q 生成运行观测项。常量 D 的普通 latch 会由 FfData 归一化为异步 reset 语义，latch 身份仍依据正式原类型保留。mux 按设计列出的类型显式分派，门级 MUX4/8/16 包括 S/T/U/V，不以端口名称推断类型。

本阶段产物通过同目录临时文件发布，检查 write/flush/close 和 rename；失败时移除临时文件，保留已有普通文件。`artifact_io.h/.cc` 在当前阶段提供实际被入口调用的共享函数，后续正式 manifest 可复用，不建立空接口。拒绝目录或设备文件目标；不提供 stdout 模式。

例如无 memory 的设计可以执行：

```text
read_verilog tests/cellfuzz/graph.v
hierarchy -top graph
proc; memory_collect; flatten
cellfilter -topk 1
cellinstrument -plan-only -manifest type-plan.json
```

类型报告中 `items` 的 `cell_id` / `port_id` 引用已恢复图，`source` 按 bit 0 起编码 `[原始wire名, 逻辑offset]` 或常量 State 数值；`registers` 保存静态控制与初值，`unobserved_cells` 解释未选择、未入图和 memory 关闭。这里不形成分析结果文件输入。原图仍从 SnapshotStore 取得，2B 将把完整图与静态项一起导出。

## 实施与测试点

1. 独立 C++ 消费端先验证多输出、全部 mux 类型、FF/latch 的源与语义，接口缺失时应失败；再添加共享模型和规划实现。
2. CLI 回归覆盖默认/显式参数、top/选择/预处理/类型错误、缺失与过期快照；运行计划前后逐字节比较 RTLIL 和 scratchpad 批次。
3. 用真实 RTL 和程序化内建 cell 夹具覆盖正负沿、同步/异步 reset、enable 优先级、异步 load、SR、门级 FF、初值别名、宽向量、拼接/重复 bit/X/Z 和空选择。
4. 错误诊断与产物失败路径、重复计划不消耗快照；保留阶段 1A/1B/1C 和原 cellfilter 回归，完成故障注入与代码审查。

构建和复现命令（仓库根目录）：

```sh
cmake --build build -j 4
make -C tests cellfuzz/Makefile various/Makefile
make -C tests/cellfuzz all summary
make -C tests/various cellfilter.sh
git diff --check
```

不增加第三方依赖，不改变选择算法、不修改功能 RTLIL、不生成成功插装标记。测试沿用动态测试插件、Python 3 和 Bash；正式 manifest/布局、旁路物化、memory 观测与查询按后续阶段交付。

## 验收记录

| 验收点 | 自动验证 |
| --- | --- |
| 全部已知输出 | `$alu` X/Y/CO、`$fa` X/Y、`$lcu` CO 逐项检查；65 bit 输出完整保留；未入图的 dead cell 无观测 |
| mux 选择源 | 全部 10 种设计约定 mux 类型，门级 S/T/U/V 无遗漏；pmux 6 bit 选择源包含别名、重复 bit 和 X/Z，精确比较位序 |
| register Q 与语义 | 10 个手工情形精确检查初值别名、极性、同步 reset/enable 优先级、异步 load、SR、常量 D latch；正式表中全部 143 种非全局时钟 FF/latch 产生且只产生 Q 项 |
| 行为导出 | 143 种 FF/latch 的 `write_verilog` 结果重新 `read_verilog`，通过 `hierarchy -check; proc; check -assert` |
| 选择与不变性 | saved top-k、排除 register、自定义空集合；CLI/API 前后 RTLIL 和完整批次逐字节比较，重复计划产物相同 |
| 预检 | 未设置/多个 top、部分/空选择、box、已有标记、process、未收集 memory、残留层次、未知类型/方向、缺失输出、显式 FSM、全局时钟粗/细粒度 FF、错误控制位宽、缺失/过期快照均拒绝；默认 flatten 的 scopeinfo 合法 |
| memory 阶段边界 | on/off、off/query、on/query 明确失败；off/off 保留关闭原因；未选 memory 不生成项；零读端口的只写 memory 可以关闭观测后规划 |
| 参数与失败 | 必填参数、非法值、越界整数、选择参数和尾部 selection 均诊断；I/O/参数失败后的原快照仍可消费 |
| 产物发布 | 不存在目录、设备目标拒绝；通过 RLIMIT_FSIZE=512 与忽略 SIGXFSZ 触发实际写/flush 失败，同进程确认旧文件及快照保留、临时文件清理 |

新增测试先在 `monitor_plan.h` 缺失时确认失败。只写 memory 的空 RD_DATA 和默认 flatten 生成的 scopeinfo 都经失败用例确认后修复，未删除或放宽原负例。

审查时临时把“所有输出”条件改为“仅 Y 输出”，回归在 `$alu` 的 X/Y/CO 精确集合断言处失败，随后恢复源码并重建。独立只读审查覆盖正式方向表、FF/初值语义、全模块预检及产物错误路径，包括 scopeinfo 修复，未发现需修复问题。

本阶段完整 CMake 构建通过，无编译告警或错误；`make -C tests/cellfuzz all summary` 为 3 passed / 0 failed（shared_graph、snapshot、type_plan）；既有 `tests/various/cellfilter.sh` 为 PASS；`git diff --check` 通过。这些证据只验收 2A；正式位段布局和完整静态图由 2B 验收。
