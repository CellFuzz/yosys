# 阶段 3A：信号绑定与物化

本阶段执行已确认的 [实施计划 §3A](InstrumentImplementationPlan.md#3a-信号绑定与物化)，依据 [设计 §6/§8](InstrumentDesign.md#6-普通观测值的可逆打包)。复用 2A 类型规则及 2B 布局和静态图；普通旁路、状态转换和原子发布在本阶段交付，Ibex 用例的完整闭环按 3B 交付，memory 能力仍按第四、第五步交付。

后续第四步已复用本物化路径实现 Memory 业务观测，见 [Stage4MemoryAccess.md](Stage4MemoryAccess.md)。下文关于默认 memory 业务拒绝的记录是 3A 当时的阶段边界；当前默认业务观测及第五步可选内容查询均已实现，后者见 [Stage5MemoryQuery.md](Stage5MemoryQuery.md)。

## 目标、接口与边界

2026-11-06 修正：`materialized` 的成功边界是 RTLIL 和 manifest；最终 Verilog 还必须经过插装后的 `opt_clean; check -assert; write_verilog` 及新进程回读/lint。最终业务/观测接口和值的验收见 [主设计](InstrumentDesign.md) 与 [修复手册 §4](ReviewFixGuide.md#4-f2统一支持范围与导出流程)。失败产物不向下游交付；下列原阶段验收仅保留为历史事实。

`cellinstrument -manifest monitor.json` 开始正式物化；`-plan-only` 继续输出只读 planned 契约。开关默认值保持 memory on/off、块宽 256、前缀 cf。已选 memory 请求未实现能力仍明确拒绝，off/off 仅物化普通观测项。

1. 从已校验 item 原 SigSpec 的逻辑位序连接 `<prefix>_obs_*` 顶层输出。块可共享、跨 item，末块实际位宽；Q 直接旁路，空计划不创建端口。只增加 wire 和连续赋值，不修改原 cell、控制或初始化。
2. 名称、宽度和来源在任何写设计之前检查，包括 RTLIL 的 WIDTH_LIMIT。原 module 的完整 Module::check 在临时文件创建前完成，防止致命断言跳过文件清理；物化后对新增 delta 作可捕获的端口与接线验证。原功能接口保留次序，新增接口按 transport 次序追加；调用 fixup_ports 并检查方向、宽度、端口编号、原有接口及逐段连接。
3. 在物化前冻结完整 planned manifest 为无指针 JSON 值，不用修改后的设计重新构图；materialized 产物保持原 graph/selection/view/items，transport 对应实际新接口，status/stage 与 diagnostics 表明已物化。
4. 分离文件的准备与发布：同目录临时文件先完整 write/flush/close，再使整个分析批次失效并物化；验证成功后 rename 发布，最后设置 cellfuzz_instrumented。这使最终发布失败不留下成功标记。plan-only 和物化前错误保留原设计、快照和旧产物；物化开始后的错误保留失效状态，清理临时文件且不发布新产物，明确要求丢弃当前 Design 后重新加载、分析。
5. 成功后拒绝重复物化或再次 plan-only；即使重新 cellfilter，也不能绕过模块标记。空计划同样是成功的一次物化，会失效快照并设置标记。

代码沿用 C++/json11、tab 缩进、PascalCase 类型和 snake_case 函数。新增 `passes/cmds/cellfuzz/instrument.h/.cc`，`artifact_io.h/.cc` 增加有生命周期清理的暂存文件；入口仍在 cellinstrument.cc，schema 仍复用 manifest，不新增第三方依赖。例如：

```cpp
auto report = planned_manifest(analysis, plan, layout);
// Freeze original metadata before any interface exists.
PreparedArtifact artifact(filename, materialized_manifest(report).dump());
SnapshotStore::invalidate(design);
materialize_observations(top, plan, layout);
artifact.publish();
```

始终验证原图、绑定、布局和文件准备，保留已交付测试；只在任务范围内修改。新增能力或依赖需另行确定；不削弱断言、跳过失败测试或自动回滚/继续使用失败后的 Design。保持本工作区已有未提交阶段成果。

## 实施与测试顺序

1. 先增加 CLI 和独立消费端测试，确认正式入口当前失败。覆盖默认物化、plan-only 后物化、空计划和重复拒绝。
2. 实现 artifact 暂存与原普通位段旁路；验证快照失效时机、成功标记、原接口和 cell 不变，以及冻结图中没有新端口。
3. 独立从 manifest 解析原 bit 引用，逐 bit 对照实际 RTLIL 赋值，并从 write_json 比较原 cell、wire 与连接。用专用组合及寄存器用例补充导出和功能等价检查；保持 3B 的真实 Ibex 验收范围。
4. 覆盖块宽 1/8/32/33/64/65/256/超过源宽，跨块、非整块、别名、重复 bit、X/Z、名称冲突、错误位段与非法源；真实 I/O 失败、物化中断和 rename 失败验证状态边界。完成故障注入、回归和代码审查。

```sh
cmake --build build -j 4
make -C tests cellfuzz/Makefile various/Makefile
make -C tests/cellfuzz all summary
make -C tests/various cellfilter.sh
git diff --check
```

测试沿用动态 C++ 插件、Python 3、Bash 与 Yosys 内建检查。验收以实际接口与源映射相符、原功能及状态更新保持、失败不发布/不标记成功为准，不以 JSON 字段存在代替行为验证。

## 实现与消费契约

`materialize_observations()` 的前置条件是当前模块未变，且已通过类型规则、冻结绑定和原 RTLIL 结构检查。入口先生成无指针的完整 JSON，再销毁 RestoredAnalysis；物化只使用本次命令内的原 SigSpec，不再访问旧 graph 或 SigMap。函数再次验证 transport 和源 wire/offset，增加每个输出 wire 与每个位段的连续赋值，按原端口数追加 port_id，随后 fixup_ports 并校验实际接口。

新增连接均为 `new_port.extract(port_lsb, width) = original_source.extract(item_lsb, width)`，没有新 cell、寄存器或采样状态。物化后的 SigMap 从当前模块重新建立，逐段检查实际别名关系。空计划同样完成状态转换，但不创建 wire/connection。原功能 cell、源 wire、端口、初始化、控制与 memory 参数/连接均保持原样。

materialized 与同配置 planned 产物的 `analysis/graph/selection/view/items/registers/unobserved_cells/memories/transport/summary` 相同。只改变 `status` 为 materialized、`stage` 为 observation-materialization，并更新 diagnostics 说明普通输出已物化。analysis.structure_digest 仍描述插装前模块；新接口只在 transport 中，不重算为原图或原分析身份。

PreparedArtifact 构造时完成 rewrite_filename、目标种类检查及同目录临时文件的 write/flush/close。只有 publish 执行 rename；未发布对象析构时清理临时文件。入口用 Yosys error_throw_scope 包住该生命周期，让 batch 模式的命令错误也能经过析构。成功发布后才设置模块标记。此流程不承诺进程崩溃或断电时的持久性，也没有物化后的 Design 回滚。

```yosys
read_verilog tests/cellfuzz/graph.v
hierarchy -top graph
proc; memory_collect; flatten
cellfilter -topk 1
cellinstrument -plan-only -manifest planned.json
cellinstrument -manifest monitor.json
check -assert
write_verilog instrumented.v
```

## 验收记录

| 验收点 | 自动验证 |
| --- | --- |
| 原源与实际接线 | 独立 C++ 消费端从 manifest 解码每个 bit，检查 item 对应的原 cell port，并逐位对照新增 RTLIL 连续赋值；不调用生产端的 SigMap 校验，也不会用错误接线建立的别名掩盖错误 |
| 冻结原图 | plan-only 后正式物化，graph/items/selection/view/registers/transport/analysis 逐字段相同；新接口与实际方向、位宽、编号一一对应，原 module port 次序保持 |
| 原功能与状态保持 | 将物化后模块 clone，只剥离声明的 output/connection/成功标记 delta，完整结构摘要必须等于原模块，包含所有 cell 参数、连接、控制与初始化；graph.v 的原功能输出经 miter/SAT 三个时序步验证等价 |
| 类型与位段 | 专用多输出/全 mux 选择、143 种正式 FF/latch、513 bit 宽向量等三组夹具，各自覆盖块宽 1/8/32/33/64/65/256/1024；直接 Q、别名、重复 bit、X/Z、跨块和尾块保持；独立空计划无新接口 |
| Verilog 可用性 | graph.v 直接 write_verilog 后读回并 hierarchy/proc/check；FF 全表直接行为导出并回读；多输出/门级 mux 夹具先核对原始物化 |
| 生命周期 | 成功时 batch 已失效且模块有标记；首次新连接的 Monitor 回调确认此前已失效且尚无成功标记；重复正式物化、正式物化后 plan-only、重跑 cellfilter 后再次物化均拒绝；空计划也消耗并标记 |
| 修改前拒绝 | 错误位段、非法源 offset、WIDTH_LIMIT、wire/cell/参数冲突、保留字/前缀/未知选项、缺失目录和非普通文件目标；同进程确认设计与快照不变，之后仍可 plan-only |
| 原结构致命错误 | 设置 `$not.Y_WIDTH=7` 而端口仍为 8 bit，真实 CLI 子进程应在文件准备前退出，保留旧产物且无临时文件；该测试先捕获并复现了审查问题，再修复检查顺序 |
| 修改前写入失败 | 用 RLIMIT_FSIZE=512 和忽略 SIGXFSZ 触发真实暂存写入失败；同进程确认 RTLIL、快照和旧文件保持，临时文件清理 |
| 修改后失败 | 用测试插件的 RTLIL Monitor 在首次接线中断命令、破坏输出方向，或将已存在目标临时移成目录以强制真实 rename 失败；均确认旧文件保持、没有成功标记、batch 失效、临时文件清理及 discard Design 诊断；检查后按契约丢弃失败模块 |
| memory 阶段边界 | off/off 物化普通项且原 memory 内容/端口结构不变；默认 on/off 仍因尚未交付的业务观测明确失败 |

正式入口测试先在旧实现的“materialization is not yet implemented”处失败，再实现生产路径。旧 2A 中“省略 -plan-only 应失败”的断言由本阶段的正式物化正例替代，其余类型/预检/I/O 回归保留。

独立审查发现原 Module::check 的 fatal 路径在物化后不能被 error_throw_scope 捕获；真实错误参数负例先复现临时文件残留，随后将原结构检查提前到文件准备前，并保留所有新增 delta 的接口/顺序/绑定验证。修复后独立审查未发现其他需修复项。

故障注入临时移除新增输出的 port_input 检查；真实端口损坏场景不再被拒绝，测试在预期失败断言处报错。随后逐字节恢复生产源码并完整重建，materialize 回归再次通过。

2026-11-05 完整 CMake 构建通过，无编译警告或错误；CellFuzz 全部 shared_graph/snapshot/type_plan/manifest/materialize 为 5 passed / 0 failed，恢复注入后的最终汇总仍为 5/0；既有 various/cellfilter.sh 为 PASS；git diff --check 与新增源码/文档空白检查通过。代码、注释和本文均保存在原工作区，已有 2A/2B 改动保留。后续 3B 的 Ibex 真实用例闭环已另行交付，见 [Stage3BValueRoundtrip.md](Stage3BValueRoundtrip.md)。
