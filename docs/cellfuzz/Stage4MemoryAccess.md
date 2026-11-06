# 第四步：Memory 业务端口观测

本阶段落实已确认的 [实施计划第四步](InstrumentImplementationPlan.md#6-第四步memory-业务端口观测) 和 [设计 §5.1](InstrumentDesign.md#51-业务端口观测)。默认 `-mem-access on -mem-content off` 支持已选 collected memory；`-mem-content query` 在已选 memory 上继续预检拒绝。CLI、选择算法、普通输出/mux/register 规则及发布失败边界沿用前三步。

后续第五步已实现可选查询，见 [Stage5MemoryQuery.md](Stage5MemoryQuery.md)。下文的 query 拒绝和 memory 不重写描述是第四步交付时的范围；当前仅业务观测仍保持原 cell 精确不变，启用查询允许增加读口及安全的地址宽度归一化。

2026-11-06 审查修复：统一名称作用域包含所有 collected memory 的 `MEMID`，即使未选中或关闭观测。普通/query 接口在文件准备和设计修改前共同预检；prefix 本身与原对象同名不会单独构成冲突。业务及 query 值回归执行 [正式映射流程](InstrumentDesign.md) 并检查最终接口，见 [修复验收记录](ReviewFixResults.md)。

## 实现契约

通过 `Mem::get_all_memories()` 只读解析原 `$mem` / `$mem_v2`，以原 cell 身份对应冻结图；不调用 `emit()`、`narrow()` 或修改业务口。保存所有 memory 的逻辑地址范围、字宽、初始化与读写口静态语义，即使观测关闭或 memory 未选中。逻辑业务口按 helper 原顺序编号；另存原 collected packed 起始编号、`wide_log2` 和分词数，掩码均按逻辑写口编号解释。

已选且 access 开启时，地址、数据、read enable、逐 bit write enable 及有效 clock/reset 生成 `memory_access` item，复用现有 transport/旁路。每个 item 对应一个逻辑口字段，保留原 GraphPort 和 `port_lsb`，source 必须等于原端口的精确切片；普通 item 的 `port_lsb` 为 0。稳定排序键扩充为原 cell 名、原 port 名、port_lsb、role，不合并别名项。无效时钟、恒无效 reset、legacy 不存在的 reset 和零宽字段只保存静态源与未观测原因。

memory 描述区分 read/write，保存原 source、原图端口及切片、nullable monitor_id；静态属性保存 clock enable/polarity、ce_over_srst、read init/arst/srst 值、transparency/collision_x、write priority 及原 memory init。同步读输出表示含保持行为的当前值；access off 时明确说明同步读保持值没有 memory 专用观测。负 offset 的原业务口可观测，不施加第五步查询地址限制。

schema v1 增加上述字段和 role，原有 item/graph/transport 编码继续有效。memory 业务源不属于新增图端口；新增 transport 只登记新接口。规划和正式物化使用同一计划，全部 memory 解析与检查在准备临时产物、修改 RTLIL 之前完成。

## 代码与测试组织

生产代码在 `passes/cmds/cellfuzz/memory_access.h/.cc`，接入 MonitorPlan、manifest 和 cellinstrument help。C++ 沿用 tab、PascalCase 类型与 snake_case 函数，例如：

```cpp
plan_memory_access(analysis, plan);
auto layout = create_transport(plan, analysis.graph->module);
```

测试位于 `tests/cellfuzz/memory_access.*`，使用现有动态 C++ 插件、Python/Bash、独立消费端和实际 Verilog 回读仿真。测试插件只在创建夹具时使用 Mem::emit；验收仍检查插装前后原模块全结构一致，避免 observer 改变数组、初始化、业务绑定、读保持或写优先级。

实施次序：先建立默认 memory 成功的失败用例，再实现只读规划/静态契约，接入旁路及逐位验证，最后补齐实际读写状态机、失败/选项矩阵和完整回归。现有阶段性“业务观测未实现”测试改为成功断言；内容查询拒绝与其原有失败保护继续保留。

验收包括：同步/异步读、正负沿、read enable 保持、两种同步 reset 优先级、异步 reset、初始化、逐 bit mask、多写优先级、透明/碰撞掩码、宽端口、零读/写口、legacy、正/负 offset、关闭与排除选择、确定性及只读性。实际接口按 manifest 独立解包，与原 RTLIL 每项源位及原功能输出比较；专用状态模型单独核对存储更新和读保持，并做适用的有界 SAT。负例验证旧产物与分析批次保留、无临时文件泄漏、源切片越界/错位拒绝。

```sh
cmake --build build -j 4
make -C tests cellfuzz/Makefile various/Makefile
make -C tests/cellfuzz memory_access.sh
make -C tests/cellfuzz -j 2 all
make -C tests/cellfuzz summary
make -C tests/various cellfilter.sh
git diff --check
```

始终保留现有断言和回归；不添加依赖、不改变原 RTLIL memory、不实现内容查询或事件采样协议。需要改变上述既有业务规则时再讨论；本阶段的字段细化和内部实现选择按已授权计划完成。

## 静态契约与消费者

`schema_version` 继续为 1，增加以下字段。原普通项的布局、端口及 monitor 顺序保持；memory 项随原 cell/port/切片排序，不承诺不同观测配置的 monitor ID 相同。

| 位置 | 字段及含义 |
| --- | --- |
| `items[]` | `role: memory_access`；`port_id` 引用完整原 collected GraphPort；`port_lsb` 为 source 在该原端口中的起点，`width/source` 为精确切片 |
| `memories[]` | 保留已有 cell_id/name/type/selected/reason/ports/parameters；增加 memid、width、size、start_offset、end_offset_exclusive、原 INIT 常量、read_ports/write_ports、access_enabled/access_observed、sync_read_state_observed、coverage_notes；content_query 仍为 false |
| `read_ports[]` / `write_ports[]` | helper 逻辑 `index`、原 `packed_index` 起点、wide_log2/words、data_width/address_width、clock_enabled/clock_positive；编号在任何测试转换之前冻结 |
| read 静态语义 | ce_over_srst、init_value/arst_value/srst_value、transparency_mask/collision_x_mask；两种 mask 按逻辑写口编号排列 |
| write 静态语义 | priority_mask 按逻辑写口编号排列，true 表示当前口对该编号写口有优先权 |
| `signals` | address/data/enable/clock；read 另有 async_reset/sync_reset。每项保存原 port_id（legacy 缺失 reset 为 null）、port_lsb、width、source、active、nullable monitor_id 和 observation_reason |

`items[].port_lsb` 是**原业务端口**偏移；`transport.segments[].port_lsb` 是**新增输出块**偏移，两者属于不同地址空间。普通项仍是整个原端口且起点为 0。图端口的 observed 表示至少有一个切片被观测，具体覆盖以 items 为准，不能假定整个 packed 向量都有运行值。

read enable 即使恒为 1 也保留独立业务字段；write enable 保留逐数据 bit 的完整 mask。clock 仅在 clock_enabled 时创建项；reset 恒为 S0 时不创建项，其他实际 reset 保留原源。legacy 的合成 reset 静态绑定 S0，不伪造原图端口。关闭 access 或未选择 memory 时，语义与原绑定仍完整保存，monitor 引用为 null；若存在同步读，coverage_notes 明确指出保持状态没有 memory access 专用观测。

宽端口数据/mask 保留合并后的全部字，地址和控制采用 helper 对应的原 packed 起点；完整 packed 信号、continuation 参数及各 lane 的静态绑定仍在 graph/parameters 中。初始化直接保存原 collected INIT 常量的 width/bits/flags，未知值不伪装为零。地址范围用宽整数求和编码，不因为业务观测而重写地址宽度或限制原负 offset。

## 实际值、状态与证明

每个正式用例先用既有独立 C++ 检查器逐位核对新增连续赋值，再从副本剥离确切的新增 wire/connection/标记，原模块完整结构摘要必须相同，包含所有 memory 参数、初始化、业务口绑定及其他 cell。规划前后 RTLIL 字节和重复 planned JSON 均相同。manifest 的 cell/port ID、原始源切片和逻辑字段到 monitor 的引用由 Python 独立检查。

实际 gate 经过 `write_verilog`、回读及 `check -assert`；Python 消费真实 VCD block 值并复用独立位段解包器，逐项比较原 source 引出的 gold 参考值和双方全部原功能输出。测试不使用 source 符号模拟打包来代替实际值。

Yosys `sim` 不直接支持 clocked memory read，而 `memory_nordff` 不能直接拆解同一口同时带异步、同步 reset；此外，原生 Memory 仿真在同址多写时会在稳定化过程中反复应用写操作。测试先从**原 RTLIL**核对并引出参考 source，再将 gold 行为导出/回读；对仿真副本使用官方 `memory_collect; memory_map` 生成明确的 next-state 逻辑。所有这些归一化仅发生在测试副本，原业务 cell 的不变性此前已独立检查。专用 Python 数组模型单独计算初始化、读旧值、同步读保持、逐 bit mask 和 w1 对 w0 的优先级，分别检查正/负沿写入、正/负沿读、两种同步 reset 优先级及无边沿的异步 reset。覆盖断言要求这些情形确实发生。

透明/碰撞用例另有逐 bit 四态预期值：读口对 w0 透明、对 w1 的碰撞返回 X，读输出与写后数组内容分别计算，验证真实 bypass 与 X 都被触发。同步案例的原功能输出和全部还原 item 还经过行为导出归一化、memory_map/async2sync 后的 4 步、`-set-init-zero` 有界 SAT。该检查针对上述归一化模型，不作为任意时钟相位/异步事件的无限时序证明。

| 专项配置 | items / 位数 | 实际输入向量 | 主要断言 |
| --- | --- | --- | --- |
| stateful 默认 / access off | 33 / 222；26 / 192 | 32 / 仅结构比较 | 原普通项与 analysis 相同，只增加/移除 7 个 memory 字段 |
| 同步多口 | 29 / 91 | 96 | 4 读/2 写、初始化、保持、两种复位优先级、异步 reset、mask/priority、读旧值与有界 SAT |
| 负沿写 | 29 / 91 | 96 | 负沿写 mask/priority，与正负沿读的独立状态模型 |
| off / 未选且 query 配置开启 | 0 / 0 | 96 / 仅结构比较 | off 不改变状态；未选 memory 不启用 query，保留完整静态说明和关闭原因 |
| 33 位字宽、两字宽口 | 17 / 320 | 24 | logical index 为 0/1、packed 起点为 0/2；66 位数据跨 8 位块、地址低位常量 |
| 透明/碰撞 | 29 / 91 | 96 | 静态 mask 及逐 bit bypass/X 预期值 |
| legacy `$mem` | 15 / 61 | 24 | 不存在的 reset 保留常量及 null 原端口；原类型/参数不变 |
| 仅写 / 仅读 | 4 / 19；21 / 53 | 各 24 | 零读/写口、空 mask、不创建虚构业务口 |
| 正 / 负 offset | 29 / 103；29 / 271 | 各 24 | 范围与实际业务地址/值保留 |
| 零宽地址 | 17 / 45 | 24 | 静态宽 0 地址、无零宽 item/interface |
| 单异步写口 | 6 / 29 | 24 | 不观测无效写 clock，保留实际数据/mask；导出回读后的 latch 行为 |

合计 15 组配置、608 个实际输入向量。多个异步写口没有列入上述行为回读验收；多写优先级使用正沿、负沿同步夹具验证。

## 失败、审查与验收记录

负例把源 port_lsb 改为负数/INT_MAX、破坏源 bit、让 memory 字段引用另一个 monitor，均应拒绝，且设计与快照保持不变。on/query 与 off/query 对已选 memory 均在物化前失败，旧正式文件和有效分析批次保留；缺失目录及 /dev/full 的真实文件准备错误同样保留批次，后续计划仍可消费，临时文件无泄漏。

故障注入只修改临时导出 Verilog 的首段 Memory 观测赋值：将实际地址观测取反。原结构保持检查已先通过；实际值消费在 sync 的 timestamp 0、monitor 0 检出 `11` 与原参考 `00` 不符。正式源码与夹具未留下注入改动。独立只读代码审查没有 Required/Critical 问题；已采纳负沿写的补充建议。

2026-11-05 最终验证：完整 CMake 构建通过，无 warning/error；CellFuzz 的 shared_graph、snapshot、type_plan、manifest、materialize、value_roundtrip、memory_access 为 **7 passed / 0 failed**；既有 `various/cellfilter.sh` 为 PASS。新增 Python 语法、`git diff --check` 与新增文件空白检查均通过。生产代码、测试夹具/消费端、代码注释和本阶段文档均保存在工作区。
