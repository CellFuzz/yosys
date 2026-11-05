# 第五步：Memory 内容查询观测

本阶段按已确认的 InstrumentDesign §5.2 和 InstrumentImplementationPlan §7 实现。默认仍为业务观测 on、内容查询 off；仅已选 memory 的 query 开关生效。

## 契约与实现计划

每个查询 memory 使用按原 cell 名排序的 memory 序号，创建 `<prefix>_mem_NNNN_addr` 输入与 `_data` 单字输出。query 独立于普通 transport，不进入原 graph、items 或业务 read_ports。manifest 的 content_query 表示是否启用，query 记录端口名/宽度、无符号逻辑地址编码、有效半开区间、原业务读口后的 logical/packed 查询编号。未启用时 query 为 null。

支持非负 offset，地址宽度为能覆盖最后一个逻辑地址的最小正位宽；单字零地址也保留一位输入。拒绝空/负范围或最后地址超出 INT_MAX 的查询。业务地址使用 Mem::emit 的无符号补零，新增接口不截断原地址。规划以占位常量建立完整 MemRd 并 check，先于文件准备且不改 RTLIL；物化建立全部实际 helper 后统一 check，再 emit。写回后重读并逐项核对原读写口（地址仅允许高位补零）、控制、掩码、属性、初始化、几何，以及新增查询口绑定。legacy `$mem` 可规范化为 `$mem_v2`，原业务语义保持。由于 Mem 解析会省略全 X/Z 字，emit 后恢复冻结的原始 INIT，并直接比较 packed INIT，保留 Z 位及常量 flags。

物化沿用既有批次失效、失败须丢弃设计、原子发布及最后设置成功标记的边界。原接口顺序不变，先附加普通输出，再按 memory 序号附加 query addr/data。所有名称在文件准备前统一校验，含原对象、参数、Verilog/Verilator 保留名称及新接口之间冲突。

使用示例（先完成层次展开、proc 与 memory_collect）：

```yosys
cellfilter
cellinstrument -mem-content query -manifest monitor.json
```

地址输入取 manifest 有效范围中的逻辑地址，组合求值稳定后读取对应 data 输出；`-plan-only` 可仅导出同样的查询映射。关闭业务观测时追加 `-mem-access off`。

## 代码、测试与边界

生产逻辑继续放在 passes/cmds/cellfuzz 的 memory_access、transport、instrument，以及 cellinstrument 入口；采用现有 tab 缩进、snake_case 函数及 RTLIL/Mem API。测试放在 tests/cellfuzz/memory_query.*，扩展既有独立结构检查和真实 Verilog 回读消费工具。

按薄层推进：先查询成功的失败用例，再规划/静态映射、实际读口物化、独立数组与业务保持验收，最后回归和交付记录。验收覆盖 on/off、off/off、off/query、on/query，初始化、边界/非零 offset、mask/多写优先级/宽口、legacy、零地址与无业务读写口；改变查询地址时原输出、同步读保持与写入状态必须匹配原设计。负 offset 仅在已选 query 时拒绝，且旧产物和快照保留。

始终保留原回归、独立值比较及失败断言；不添加依赖，不实现采集遍历或快照协议，不更改内核 memory 语义。范围内实现细化沿用用户授权。

```sh
cmake --build build -j 4
make -C tests cellfuzz/Makefile various/Makefile
make -C tests/cellfuzz memory_query.sh
make -C tests/cellfuzz -j 2 all
make -C tests/cellfuzz summary
make -C tests/various cellfilter.sh
git diff --check
```

## 静态映射与消费者

schema v1 保持现有字段；每个 `memories[]` 的 `content_query` 为布尔值，`query` 为 null 或以下映射。所属 memory 的 cell_id/name 仍标识原图对象，不创建额外 graph port 或 monitor_id。

| query 字段 | 含义 |
| --- | --- |
| address_port / data_port | 实际顶层输入/输出名，与普通 transport 分开 |
| address_width / data_width | 无符号逻辑地址位宽、原单字位宽 |
| address_encoding | `unsigned_logical`；地址不减 offset |
| start_offset / end_offset_exclusive | 有效查询半开区间；越界值不承诺 |
| read_index / packed_index | 新读口在最终 helper/collected memory 中的起点，按原业务口之后编号 |
| asynchronous / always_enabled | 均为 true，无时钟、reset 或采样寄存器 |

关闭 access 并开启 query 时，read_ports/write_ports 仍完整说明原业务语义，但 monitor_id 为 null。同步读保持值仍没有业务观测，coverage_notes 保留该限制；查询数组不能替代同步读输出状态。graph 中的 observed 仍表示普通 items 覆盖，query-only memory 的 observation_reason 为 memory_content_query。

规划与正式输出的原 graph/items/业务描述完全相同。物化后不得用原 GraphPort 的 packed bit 偏移读取已重写 cell；普通旁路已经直接绑定原 wire，消费端使用 transport 位段及独立 query 接口。地址需要扩展时，原业务地址仅在每个 packed lane 高位补零；已有更宽地址不缩短，宽口的原 lane 低位编码保持。

## 验证方式与限制

独立 C++ 检查器核对新增 packed 参数、读口与逐 lane 地址增量；legacy 仅在私有测试副本中规范化，INIT 始终直接比较原 packed 常量。剥离已核对的查询增量、旁路及成功标记后，整模块摘要必须等于原设计。原有无 query 用例继续要求原 cell 精确不变。规划前后 RTLIL 字节与重复计划 JSON 相同，原图中不出现任何新增接口。

真实 gate 经 Verilog 导出/回读，普通项逐位解包匹配原 source，原功能输出逐个匹配 gold。Python 独立数组模型按时钟边沿、逐位 mask、写优先级与宽口分词更新状态，逐样本检查实际 query 输出；首次写之前遍历全部初始化字，随后在业务输入静止时改变 query 地址，覆盖上下边界与非零 offset。同步多口与负沿写复用完整读保持/reset 模型；透明/碰撞另有四态读输出模型。13 位、offset 5、深度 3 的第二 memory 验证独立地址、数据与排序编号。

仿真副本沿用第四步行为导出和 memory_map，不在生产设计中展开数组。非二次幂范围的 memory_map padding 未驱动位仅在该测试副本通过 setundef -undriven -undef 显式设为 X；独立 oracle 始终查询有效范围。Z 初始化用原 packed 常量精确保持验收；官方导出可能将全 X/Z 字视为未指定，故不把回读后的 Z 电气行为作为承诺。同步模型经过行为归一化、memory_map/async2sync 后做 4 步 SAT，query 地址是任意共同输入；这是有界非干扰检查，不是任意时钟事件的无限证明。

失败用例覆盖：已选负 offset、最后地址超 INT_MAX、addr/data 命名冲突、缺失目录、/dev/full；拒绝必须保留旧产物及可消费快照。未选负 offset 即使配置 query 也可交付。旁路阶段和 query-only emit 阶段各注入中断、实际绑定损坏、发布 rename 失败，确认批次失效、无成功标记、旧产物保留且临时文件清理。失败设计按公开契约丢弃，无自动回滚。

## 验收记录

2026-11-05 最终交付验证：

| 用例 | 配置 / 向量数 | 证据 |
| --- | --- | --- |
| 四开关组合 | 4 / 584 | 接口与描述一致，业务功能与读保持/reset 模型一致；on/query 保持默认原图和 items |
| 负沿写、透明/碰撞 | 2 / 392 | 写优先级和 mask 更新实际数组；同步读输出另按四态模型核对 |
| 33 位字、66 位宽口 | 1 / 104 | 跨输出块的普通观测、宽口各词和单字查询一致 |
| legacy、仅写、仅读、offset、异步写、地址扩展、部分 X 初始化 | 7 / 700 | 实际查询值与数组模型逐样本一致，原功能及普通项全匹配 |
| 原业务 ABITS 0 | 1 / 49 | 新增一位查询地址，原零宽业务地址仅补零，单字值正确 |
| 两个不同几何尺寸 memory | 1 / 36 | 8/13 位、offset 0/5、深度 4/3 的独立接口、编号与实际值正确 |
| 全 Z 初始化 / 未选负 offset | 结构验收 | INIT 位和 flags 精确保留；关闭查询限制不影响未选 memory |

合计 **16 组实际仿真、1865 个输入向量**，另有结构、预检、故障路径及 4 步 SAT。Z 原始 INIT 反例先在 packed 参数精确比较处失败，修复后通过。查询出口故障实验仅修改临时导出的 Verilog，将单字 data 取反；普通项及原功能仍匹配，独立数组模型在 timestamp 0 检出预期 00010000、实际 11101111。正式代码与夹具没有保留该破坏。

CMake 构建通过，无 warning/error；CellFuzz 完整 **8 passed / 0 failed**（shared_graph、snapshot、type_plan、manifest、materialize、value_roundtrip、memory_access、memory_query），既有 various/cellfilter.sh 为 PASS。Python 语法、git diff --check 与新文件空白检查通过。独立只读审查及 INIT 修复后复查均无 Critical/Required 代码问题。代码、注释、测试和阶段文档均保存在工作区，未创建提交。
