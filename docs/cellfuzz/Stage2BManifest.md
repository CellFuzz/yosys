# 阶段 2B：确定性布局与静态图输出

本阶段实现 [实施计划 §2B](InstrumentImplementationPlan.md#2b-确定性布局与静态图输出)，依据 [设计 §6/§7](InstrumentDesign.md#6-普通观测值的可逆打包)。用户已确认的范围是只读规划、完整 CellGraph 静态描述和可逆位段，RTLIL 物化及 memory 能力仍按后续阶段交付。

后续状态：3A 已完成普通旁路物化，沿用本页原图与布局编码；正式产物改为 `status: materialized`、`stage: observation-materialization`，原图保持冻结值。物化、暂存发布和失败边界见 [Stage3AMaterialization.md](Stage3AMaterialization.md)。本页记录 2B 的只读交付。

## 接口与实施约定

`cellinstrument -plan-only -manifest monitor.json` 从当前 Design 恢复分析与类型计划，再生成布局并原子发布 `status: planned`、`stage: observation-planning` 的 manifest。默认 memory 开关 on/off、块宽 256、前缀 cf 沿用 2A；已选 memory 请求未交付能力仍失败。命令不修改 RTLIL、不设置成功标记、不消耗分析批次。

代码沿用 C++/json11、tab 缩进、PascalCase 类型名与 snake_case 成员，不增加第三方依赖：

```cpp
MonitorPlan plan = create_monitor_plan(analysis, options);
TransportLayout layout = create_transport(plan, analysis.graph->module);
json11::Json report = planned_manifest(analysis, plan, layout);
```

`transport.h/.cc` 负责块、位段与校验；`manifest.h/.cc` 负责冻结原图的值编码和 ID 对应；入口使用既有 artifact_io。所有 live SigSpec 和计划只在本次命令内有效，manifest 是独立的值记录，不提供分析文件导入。

- cell 按原名排序；port 按 module/cell 身份、owner 原名、port 原名排序；connection 按导出端口 ID 排序。导出 ID 为连续整数，另存 `analysis_*_id` 对应原图。monitor ID 按计划稳定的 owner/port 顺序分配。选择、region、view、items 和 register/memory 描述全部引用同一导出 ID。
- 位引用保持 `[原始wire名, RTLIL逻辑offset]`，常量 bit 保持完整 State 编码。wire 声明提供位宽、start_offset、upto、signed、port ID/方向和属性；参数/属性常量保存 width/bits/flags，保留超过 64 bit 与 X/Z。图中所有原始 wire 都登记，新增观测接口只在 transport 出现。
- 每个 item 独立顺序占用块空间，允许多个 item 共用一个块、单个 item 跨多个块。segment 保存 monitor_id、item_lsb、port_name、port_lsb、width；不按信号去重。末块为实际位宽，空计划没有块或位段。
- 校验 item 和块 ID/名称唯一、位段范围、逐 item 完整覆盖、目标无重叠且没有未规划空位、块宽上限和合计位宽；使用宽整数累计并避免宿主整数溢出。布局名称在发布前检查合法字符、现有对象冲突与保留前缀，供后续物化复用。
- 原图包含全部登记 cell/port、类型/角色/保留原因、参数与全部可用来源属性、raw/canonical 有序信号、保留连接。`selected`、`observed` 与 `in_graph` 分开；不生成项的对象保留原因。selection、region 与 view 独立输出，折叠边不是逐位功能依赖。
- producer/analysis/config 记录工具版本、analysis_id、结构摘要、有效构图配置和策略。register 描述沿用 2A；memory 描述保留原 cell/port 身份和参数，说明能力关闭/未选状态，不宣称本阶段实现业务端口或内容查询。

## Planned schema v1

| 字段 | 当前编码 |
| --- | --- |
| `schema_version/status/stage` | `1` / `planned` / `observation-planning` |
| `producer` | `tool: yosys`、实际 Yosys `version` |
| `module/analysis_id` | 保留 2A 的顶层模块名与快照身份；与 `analysis` 对应值相同 |
| `analysis` | analysis_id、module、structure_digest、register/memory/fsm 构图 role、策略 id/version/parameters |
| `config` | mem_access 布尔值、mem_content 的 off/query、chunk_width、prefix |
| `graph.module` | 原模块名、默认参数及属性 |
| `graph.cells` | 连续 id、analysis_cell_id、原名/类型、semantic_type、role、retention_reason、in_graph/selected/observed、observation_reason、参数/属性、input/output 端口 ID |
| `graph.ports` | 连续 id、analysis_port_id、原名、nullable cell_id、module_port/cell_port、方向、位宽、raw/canonical、selected/observed/reason |
| `graph.wires` | 连续 id、原名、width/start_offset/upto/signed、原 port_id/input/output 和属性 |
| `graph.connections` | 连续 id、analysis_connection_id、src/dst 端口 ID、bit_count |
| `graph.module_inputs/module_outputs` | 原边界端口的导出 ID 集合 |
| `selection` | cells、module_ports 和 regions；region 保存 id/analysis_region_id、edge/key cells、上下游边界 kind/id |
| `view` | cells、module_ports、regions、connections 和可见/隐藏 edge 数量；连接含 src/dst、collapsed、hidden_cells、bit_count |
| `items` | 连续 monitor_id、cell_id/port_id、原 cell/type/port、output/mux_select/register_q role、width 和有序 source |
| `registers/unobserved_cells` | 2A 的寄存器控制/初值静态说明和未观测原因；cell_id 已转为导出 ID |
| `memories` | 原 memory cell_id/name/type、selected/reason、ports、完整参数；access_observed/content_query 均为 false |
| `transport` | ports 数组、segments 数组、total_bits；接口只有规划身份，不出现在原图 wire 中 |
| `diagnostics/summary` | 当前阶段限制；原图对象/端口/连接、item、observed cell 数和总位宽 |

原图名称保留 RTLIL 原始编码，包括已有的前导反斜杠或 `$` 内部名；新增 transport 接口名称是无前导转义的 ASCII 名称。module port 的 `cell_id` 为 null；来源属性不存在时属性键缺失，不用空字符串伪造来源。位序均以逻辑 bit 0 起排列，与 wire 的声明 `[msb:lsb]` 方向分开。常量 bit 的整数编码为 `0/1/2/3/4/5 = S0/S1/Sx/Sz/Sa/Sm`。参数/属性的 `bits` 字符串则为高位在前，使用 `0/1/x/z/-/m`；`flags` 为原 `RTLIL::ConstFlags` 位掩码，包括字符串和有符号标志。

cell/port/connection/wire ID 都在各自数组内从 0 连续分配。region 按导出 edge cell ID 向量排序，region 边界中的 kind 明确区分 cell 与 module_port；与其余集合一样引用导出 ID。`analysis_*_id` 仅用于与原快照对应，不能作为导出数组下标。相同有效快照与配置重复规划的 JSON 逐字节相同，不包含产物路径或时间；改变插入顺序后导出 ID、对象关联和 transport 仍相同，但原分析 ID 和 analysis_id 不承诺相同，也不承诺跨 Yosys 版本稳定。

原连接的 `bit_count` 是端口级保留连接统计。折叠 view 只表达路径可达性，此时 `bit_count` 为 null，`hidden_cells` 为经过的 EDGE 集合；不能解释为逐位功能映射。未折叠 view 保留原统计。独立消费者还原导出的 CellGraph 关系应使用 graph；还原观测值应使用 items 与 segments。

每个 segment 表示 `items[monitor_id][item_lsb + i]` 映射到 `port_name[port_lsb + i]`，其中 `0 <= i < width`。消费端按这两个偏移拼接，不依赖 segment 数组顺序。布局名称为 `<prefix>_obs_%04d`（至少四位十进制编号）；前缀与生成名检查 Verilog/SystemVerilog 保留字，拒绝 Verilator `__` 前缀，生成名还检查现有 wire/cell/参数冲突。检查复用 Verilog 后端 helper，因此构建声明 `REQUIRES write_verilog`；不执行后端 pass。

## 测试与交付顺序

1. 先建立独立消费端与布局负例，确认接口缺失时失败；实现布局及范围/覆盖/重叠校验。
2. 实现完整 manifest，独立读回后与原快照对象/端口/连接/选择比较；用原始绑定向量经布局打包后独立解包，逐 bit 相等。
3. 覆盖位宽 1/8/32/33/64/65/256/超过一块、多个源别名、重复 bit、常量、原 module port 复用、空计划、hidden/EXCLUDED、来源缺失、特殊原名和字典重排。
4. CLI 接入并保留 2A 全部类型及失败测试，验证重复规划确定性、输出路径无关、RTLIL 和 scratchpad 不变、旧产物保留；故障注入检查并记录验收结果。

```sh
cmake --build build -j 4
make -C tests cellfuzz/Makefile various/Makefile
make -C tests/cellfuzz all summary
make -C tests/various cellfilter.sh
git diff --check
```

测试仍使用动态消费插件、Python 3 和 Bash。只读规划可以验证静态图与位段契约；接口实际值和原功能等价性留到第三步物化验收。

## 验收记录

| 验收点 | 自动验证 |
| --- | --- |
| 原图完整性与引用 | 测试插件导出既有 AnalysisSnapshot，Python 按 analysis_*_id 逆映射并逐条比较全部 cell/port/保留 connection、selection/region/边界；检查所有导出 ID 连续、唯一且排序 |
| 参数、来源及位绑定 | 从 live RTLIL 独立枚举每个参数/属性的低位优先 State 与 flags，与 manifest 的高位优先字符串逐位比较；覆盖 82 bit X/Z 参数、module/cell/wire 来源、缺失来源、引号/换行/反斜杠名称、声明 offset/upto/signed |
| 原图、选择和 view 分离 | all、top-k=1、nodes none 与空集合；独立 DFS 对照 view 端点和 hidden cell，不跨 NODE/EXCLUDED；未入图和未选对象仍登记，折叠边 bit_count 为 null |
| 可逆布局 | source 宽 1/8/32/33/64/65/256/513，块宽 1/8/32/33/64/65/256/1024；逐 bit 覆盖恰好一次；符号位身份和固定种子随机值打包/逆序解包一致，覆盖跨块、共享块、非整块尾部 |
| 独立身份与空计划 | 两个 canonical source 完全相同的内部输出仍有不同 monitor ID；选择向量保留重复 bit、X/Z；原 module port 复用且不生成 item；空选择无块、无位段、总宽为 0 |
| 确定性与只读性 | 同一快照两次规划使用不同文件名，JSON 逐字节相同；逆序插入 cell 后导出 ID、布局和静态关联相同；RTLIL 文件逐字节相同，结构摘要和 scratchpad 保持 |
| 位段拒绝 | source/target 越界、INT_MAX 偏移或 monitor ID、零宽、缺口、重复覆盖、完整 source 覆盖但 target 重叠、未知 port、总宽不符、重复块名均拒绝；合法 segment 逆序通过 |
| 冻结绑定拒绝 | 损坏 cell/port ID、source bit 或 owner 关联均用 log_cmd_error 诊断；检查顺序在类型报告访问数组之前 |
| 命名与发布 | 现有 wire/cell/默认参数/可用参数名称冲突、Verilog/SystemVerilog 保留字、Verilator 前缀和非法 ASCII 名称拒绝；旧产物和原快照保留；合法自定义前缀通过 |
| 阶段边界与既有回归 | memory off/off 保留原参数、端口及关闭原因；默认未交付能力仍拒绝；沿用 2A 143 种 FF/latch、类型预检与真实写入失败测试 |

新增消费端最初在缺少 `manifest.h` 时失败，再实现生产接口。补充损坏 item 负例时复现了 `.at()` 先抛 out_of_range 的问题，前移冻结源校验后通过。审查期间临时删除 target 覆盖校验，完整 source 但重叠 target 的负例在 `rejects()` 断言失败，随后恢复源码并重新构建；没有移除或放宽负例。

2026-11-05 完整 CMake 构建通过，无编译警告或错误；CellFuzz 的 shared_graph、snapshot、type_plan、manifest 全部通过，汇总 4 passed / 0 failed；既有 various/cellfilter.sh 为 PASS；`git diff --check` 与新增源码/文档的空白检查通过。独立只读审查已完成，原值 oracle 对照建议已补入测试。本阶段不创建 RTLIL 观测端口、不设置成功标记，不宣称验证了尚未实现的运行接口。
