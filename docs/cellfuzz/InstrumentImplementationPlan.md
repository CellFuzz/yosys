# CellFuzz：Yosys 侧插装实施计划

状态：2026-11-05 按已确认需求分阶段实施。设计依据为 [InstrumentDesign.md](InstrumentDesign.md)。当前已实现 cellfilter 的共享图服务、选择/view 和会话快照交接，以及 cellinstrument 的 2A 类型规划、2B 确定性静态契约、3A 普通旁路物化、3B Ibex 实际值闭环及 register 补充验收、第四步 Memory 业务端口观测和第五步内容查询。Yosys 侧首版已实现。

审查后续（2026-11-06）：对 `f51fc1ed8` 至 `e8b9e9c0b` 的审查发现名称预检、导出契约、SR latch 元数据和多模块摘要性能四项问题。修复方案、实施细节和独立验收见 [ReviewFixGuide.md](ReviewFixGuide.md)；四项代码修复与运行验收已完成，修改尚未提交；独立证据和边界见 [ReviewFixResults.md](ReviewFixResults.md)。本页原阶段完成记录继续保留为历史事实。

阶段进展：1A 见 [Stage1ASharedGraph.md](Stage1ASharedGraph.md)，1B/1C 见 [Stage1BCSessionSnapshots.md](Stage1BCSessionSnapshots.md)，2A 见 [Stage2ATypePlanning.md](Stage2ATypePlanning.md)，2B 见 [Stage2BManifest.md](Stage2BManifest.md)，3A 见 [Stage3AMaterialization.md](Stage3AMaterialization.md)，3B 见 [Stage3BValueRoundtrip.md](Stage3BValueRoundtrip.md)。

第四步实现、schema 增补及验证边界见 [Stage4MemoryAccess.md](Stage4MemoryAccess.md)；第五步查询接口、归一化检查及验收见 [Stage5MemoryQuery.md](Stage5MemoryQuery.md)。

## 1. 交付目标与顺序

同一 Yosys 会话执行 `cellfilter` 和独立的 `cellinstrument`。前者负责选择并保存结果，后者消费结果、增加顶层接口，输出完整静态图和可逆观测映射。随后执行 `opt_clean; check -assert; write_verilog instrumented.v`，在新进程回读和 lint；完整命令见 [主设计](InstrumentDesign.md)。可选 `write_rtlil` 检查点放在插装后、映射前。

| 步骤 | 可独立审查的交付 | 依赖 |
| --- | --- | --- |
| 1 | 共享图服务与会话内分析结果交接 | 复用现有选择算法 |
| 2 | 固定观测规则、规划与静态图/位段契约 | 1 |
| 3 | 普通信号旁路及可逆打包闭环 | 2 |
| 4 | memory 业务端口观测 | 3 |
| 5 | memory 内容查询与首版完整验收 | 4 |

每一步可以按下述子任务拆成独立提交。默认值始终保持业务观测 on、内容查询 off；某阶段尚未实现的能力被启用时应明确报错。前期用无 memory 的 `ibex_branch_predict` 验证；含已选 memory 的阶段性示例需显式关闭未交付的能力。

本轮不交付 Verilator 采集器、adapter、事件协议、快照/判新算法或 generator。实际模型身份绑定列入后续工作。

## 2. 文件组织

保留两个 pass 入口，共享实现放在 CellFuzz 内部命名空间：

```text
passes/cmds/cellfilter.cc                选择参数、分析结果发布、DOT
passes/cmds/cellinstrument.cc            观测参数与插装编排
passes/cmds/cellfuzz/
    cellgraph.h/.cc                     图模型、构建器、源绑定与快照记录
    selection.h/.cc                     现有 region 策略、明确集合、view
    analysis_snapshot.h/.cc             会话存储、版本、结构摘要、恢复绑定
    monitor_plan.h/.cc                  类型处理、观测项、memory 静态语义
    transport.h/.cc                     有界布局、命名和位段校验
    instrument.h/.cc                    普通旁路和内容查询物化
    manifest.h/.cc                      完整静态图与观测映射输出
    artifact_io.h/.cc                   可检查写入结果的原子文件发布
tests/cellfuzz/                         小型 RTL、Yosys 脚本、产物检查
```

文件随步骤增加，不提前建立空框架。共享源码沿用 `yosys_pass(cellfilter ...)` 组件编译一次，通过 `PROVIDES cellinstrument` 注册独立入口；调用其他组件的 pass 或共享符号时声明对应 REQUIRES。2B 复用 Verilog 后端的标识符/保留字检查，因此声明 `REQUIRES write_verilog`，规划过程中不调用后端或修改设计。

JSON 序列化可复用现有工具；检查最终写入/flush/close 错误，不能只依赖文件打开成功。分析快照编码属于内部会话存储，不增加面向用户的选择文件解析接口。

## 3. 第一步：会话内结果交接

目标：cellfilter 的选择结果可被后续 pass 精确消费，保持现有 DOT 和 RTLIL 行为。

### 1A. 抽取共享图与选择/view 服务

实现说明：[阶段 1A：共享 CellGraph、选择与 view](Stage1ASharedGraph.md)。

- 将现有 CellGraph、构图器、选择接口和 view 移出 PRIVATE_NAMESPACE，保存 raw/canonical signal 的区别。
- 原样复用 node 角色、region 回调、top-k 和折叠算法。view 只消费明确集合，省略的 NODE 或 module port 不会被穿过或补回。
- 增补 module/cell 原名、类型/参数、来源和筛选原因的值记录。共用邻接查询从保留连接建立。

验收：无配额和多种 K 的 DOT 对象/拓扑兼容；构图和选择不改变功能 RTLIL；自定义回调空集与非固定 K 正确。

### 1B. AnalysisSnapshot 和 SnapshotStore

实现说明：[阶段 1B/1C：会话快照、发布与恢复绑定](Stage1BCSessionSnapshots.md)。

- 规定带版本的内部快照：有效配置、策略、模块标识/结构摘要、明确选中集合及恢复分析结果所需的记录。对象使用无歧义原名和 bit 引用，不序列化裸指针。
- 用当前 Design 关联的存储发布最近一次成功结果批次。新调用使旧批次失效，所有目标模块成功后一起发布；失败后没有可误用的旧结果。
- 把 `select_objects()` 移出当前 DOT 输出条件，未传 `-dot` 也发布结果。分析完成后仍不添加功能 wire、cell、port 或插装标记。

验收：开/关 DOT 发布相同集合；多模块结果可按模块查找；重新运行替换结果；失败后结果不可消费；内部编码正确往返特殊 RTLIL 名称。

### 1C. 恢复绑定与过期检查

接口、结构摘要范围和 Design 操作边界见 [阶段 1B/1C](Stage1BCSessionSnapshots.md)。

- 从当前模块解析对象和 wire，校验源位宽、类型、参数和连接；允许复用构图器恢复结构，保持保存的选择集合及解释。
- 结构摘要规范化排序，并覆盖端口、wire/cell、连接、初始化和必要依赖；保留 SigSpec 位序、重复 bit 和 X/Z。排除 scratchpad、输出路径和临时选择栈。
- 提供供 cellinstrument 使用的加载/校验入口。缺失或过期报错，要求重跑 cellfilter；不自动重新评分。

验收：只读命令后仍可恢复；修改连接/参数/初始化、删除或重命名对象会被检出；不存在跨 Design 的旧指针解引用。检查会话存储对 design reset/save/load 操作的行为，符合不提供持久化契约的约定。

完成标志：可通过测试消费端在同一会话读回图和选择结果，不依赖 DOT 或文件导入。本步不实现 cellinstrument 的 RTLIL 修改。

## 4. 第二步：观测规划和静态契约

目标：建立 `cellinstrument -plan-only`，验证观测规则与可逆映射，保持原设计不变。

### 2A. 入口、预检与类型处理

实现说明：[阶段 2A：cellinstrument 入口、预检与类型规划](Stage2ATypePlanning.md)。2A 原始产物为 `stage: type-planning` 的类型计划；2B 将只读产物升级为 `stage: observation-planning` 的完整图与位段契约。3A 已开放省略 `-plan-only` 的正式物化路径。

- 新入口只接受观测和布局配置：`-manifest`、`-plan-only`、`-mem-access on|off`、`-mem-content off|query`、`-chunk-width`、`-prefix`。建议默认块宽 256、前缀 cf。
- 要求恰好一个已展开 top、whole-module selection、无 process/未收集 memory、无重复插装，并加载该 top 的有效分析快照。
- 普通组合 cell 全输出必选；mux 家族额外选择信号必选。覆盖 `$mux/$pmux/$bmux/$demux/$bwmux` 及设计文档中列出的内建门级 mux，不按 S 名称猜测类型。
- register 用 FfData 解析并生成 Q 项；记录时钟/极性/latch/控制静态语义。若输出初值，使用 FfInitVals；不把 nullptr 解析所得 val_init 当作完整初值。
- 未知类型/方向、显式 FSM、隐式全局时钟 FF 或缺少行为导出支持的对象明确诊断。普通数据输入、register D/控制不生成项，仅保留内部扩展位置。

验收：全部已选普通输出、mux 选择向量和 register Q 准确生成项；被选择排除的对象没有项；所有项能定位回原图对象/端口。

### 2B. 确定性布局与静态图输出

实现说明：[阶段 2B：确定性布局与静态图输出](Stage2BManifest.md)。已实现 `transport.h/.cc`、`manifest.h/.cc` 及独立读回测试。

- 按稳定对象/端口顺序分配导出 ID 和 monitor ID，同时保存 `analysis_*_id` 对应原快照；不修改原分析 ID。每个 item 独立布局，支持跨块和任意位宽，验证位段完整覆盖及目标互不重叠。
- 默认输出完整 CellGraph 快照，保留未观测对象和原因；原图连接、selection 与折叠 view 分别登记。冻结原图，避免新增接口被当成原功能对象。
- 保存类型/参数/来源、有序 raw/canonical bit 绑定、引用 wire 的声明索引信息，以及 items/transport/memory 描述。参数/常量用位宽加字符串保留 X/Z。
- 输出 `status: planned`，无 RTLIL 修改或成功标记；规划结果和临时绑定仅在命令生命周期内有效。

验收：静态记录独立读回后与导出的原图对象/端口/连接相符；全部 item 位段可拼回逻辑位向量；多个观测源别名、重复 bit、常量和空计划均可表达。

此阶段用无 memory 的用例验证。含已选 memory 而启用了尚未交付能力时明确失败；不改变最终默认值来绕过检查。

完成标志：可审查的 planned manifest，静态图与观测映射契约经测试消费者往返验证。消费者仅为测试工具，不是 Verilator 采集模块。

## 5. 第三步：普通旁路与可逆打包

目标：完整打通分析结果到新增顶层 output 和正式 manifest。

### 3A. 信号绑定与物化

实现说明：[阶段 3A：信号绑定与物化](Stage3AMaterialization.md)。`cellinstrument -manifest <file>` 已创建普通观测接口，`-plan-only` 仍为只读路径；两条路径共用原图、类型和布局预检。

- 从已检查计划的原 SigSpec 创建 `cf_obs_*` 输出块并连续连接，Q 直接旁路；末块实际宽度，空计划不创建零宽端口。
- 检查受控 ASCII 名称、语言保留字、已有/生成名称和 Verilator 保留前缀冲突；调用 fixup_ports 并核对实际端口、RTLIL 和计划。
- 正式插装开始使原分析结果失效；成功检查并发布后设置模块插装标记，输出 `status: materialized`、`stage: observation-materialization`。plan-only 后仍可正式插装，正式插装后拒绝再次物化。
- 原 module 的完整 RTLIL 检查与 manifest 的同目录临时文件 write/flush/close 都在物化前完成；物化后检查新增端口、原接口顺序与实际源连接，确认成功再 rename 发布。物化后失败清理临时文件、没有成功标记，并要求丢弃本次 Design、重新加载 prepared 设计后重跑分析。

### 3B. Ibex 组合逻辑闭环和 register 补充

- 以 `ibex_controller` 为首个真实用例，覆盖其输出与 pmux S。提供可独立获取的测试夹具/获取说明，避免回归依赖本机绝对路径。
- 测试从新增接口值按 manifest 独立解包，与原 RTLIL 源位比较；同时检查原功能输出保持一致。采用 Yosys eval/SAT/等价检查等合适手段。
- 小型专用 RTL 补充多输出 cell、不同 mux 选择向量、register Q、初值、正负沿和受支持的异步控制/latch；接口提供源值，不声称已实现逐事件采集。

验收：1/8/32/33/64/65/256 bit、超过一块、非整块尾部、拼接/切片/重复 bit/常量和无观测项均可逆；图 ID、源绑定与实际端口一致；功能输出和原状态更新语义保持。

原阶段完成标志：第一处可使用的 Yosys 插装闭环。审查修正后的正式交付还要求插装后两步映射、清理、最终回读及 Verilator lint，命令与 materialized 边界见 [主设计](InstrumentDesign.md)。

3B 已交付：`tests/cellfuzz/value_roundtrip.sh` 使用仓库内固定哈希 Ibex 夹具，验证导出 Verilog 回读后的实际接口值与全部原功能输出。独立 Python 消费端按 manifest 解包，C++ 测试插件只从原 RTLIL 解析参考源；另有组合 SAT、controller 4 步有界 SAT、9 路 register/latch 的独立状态预期值和既有全结构保持断言。详情与证明边界见阶段文档。

## 6. 第四步：Memory 业务端口观测

目标：默认导出已选 memory 的业务信号，内容查询继续默认关闭。

- 用 Mem helper 关联原 collected memory，冻结原业务读写端口身份；生成地址、数据、enable/mask、存在的 clock/reset 等观测项，复用普通布局和旁路。
- 保存时钟有效性/极性、读同步性、宽端口分词、写优先级、透明性/碰撞、初始化/reset 与控制优先级。静态属性与动态信号分开表达。
- 支持 access on/off。off 时保留对象和关闭原因，说明同步读保持值的覆盖减少。业务观测不改变 memory 参数、端口及初始化。

验收：同步/异步读、使能保持、reset、掩码写、多端口及宽端口描述和源绑定正确；开/关只影响预期观测项；实际业务端口与原图对应；原功能和存储更新保持。

完成标志：默认命令可处理包含已选 memory 的设计，产出业务观测项。内容查询被显式请求时仍报告本阶段尚未交付。

第四步已交付：只读 Mem helper 关联、逐逻辑业务字段的原图切片与 memory_access item、完整读写静态语义，以及既有旁路物化和原子发布。专项测试检查实际接口、原功能及数组状态，覆盖同步读保持、两种复位优先级、正负沿写、mask/priority、透明读/碰撞 X、宽口、legacy、零读/写口与正负 offset；第四步当时保留内容查询拒绝；第五步现已实现查询并扩展失败保护。

## 7. 第五步：Memory 内容查询

目标：按地址读取实际存储内容，完成两个独立开关的四种组合。

- 为每个启用 query 的 memory 增加独立 addr input 和单字 data output。MemRd 为异步、恒使能、reset 无效；完整设置 init/reset 字段及匹配原写口数量的掩码。
- 规划逻辑地址范围、位宽和编码，支持经过验证的非负 offset。负 offset 或无法证明安全的地址扩展仅在请求查询时拒绝；关闭内容查询仍可使用业务观测。
- 先对所有 memory helper 变更执行 check，再批量 emit。核对 emit 的地址宽度归一化和原业务地址语义，保留原端口、初始化、写优先级与同步读响应。
- 静态查询映射关联原 memory ID，新增调试读口不计作原业务口。查询返回实际数组值，遍历、快照和判新留给后续采集系统。

验收：on/off（默认）、off/off、off/query、on/query 四组合的接口及描述一致；覆盖初始化、边界/非零 offset、掩码、多写同址和宽端口；改变查询地址不改变原输出、状态更新或读保持行为；memory check、RTLIL 检查和 Verilog 导出通过。

完成标志：Yosys 侧首版交付完成：两个 pass、全部约定观测规则、静态图与可逆映射、可选实际内容查询均可独立验证。

第五步已实现：冻结原业务 helper，按排序原 memory 序号规划独立 addr/data 接口；预检非负地址范围和 RTLIL 宽度，先 check 全部 helper 再 emit，并核对原业务语义与实际查询绑定。manifest 的 query 映射独立于 graph/items/transport；原子发布、批次失效和成功标记沿用既有边界。专项覆盖四组合、实际数组值、未知初始化、地址扩展、多个 memory 及物化中断/损坏/发布失败。

## 8. 验证门槛和本次之后

测试接入现有 `tests/gen_tests_makefile.py` / `tests/common.mk`，随步骤增加可独立运行的行为用例。无需新增运行整个测试集的 C++ 测试框架依赖。

| 里程碑 | 必须看到的证据 |
| --- | --- |
| 步骤 1 | 无 DOT 也有结果；快照往返、过期拒绝；现有选择行为不变 |
| 步骤 2 | planned manifest 可读回图和位段；规划前后 RTLIL 不变 |
| 步骤 3 | Ibex 与专用 RTL 的普通观测逐位正确，原功能保持 |
| 步骤 4 | 业务观测默认开启，读写端口语义和同步读保持正确 |
| 步骤 5 | 两开关四组合、实际内容查询和完整导出验证通过 |

反例测试必须覆盖：省略仍为 NODE 的状态节点时不跨越折叠；回调空集/可变大小；过期快照；错误位段和命名冲突；重复插装；产物写入失败。仅检查 JSON 字段存在不足以验收。

后续工作单独规划：Verilator 采集与事件契约、memory 扫描/快照、联合状态判新和 generator、实际模型与 manifest 的绑定验证。扩展观测点、FSM 语义和内部事件捕获按各自需求立项。
