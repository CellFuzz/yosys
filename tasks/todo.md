# CellFuzz 审查修复任务清单

状态：T1–T9 的代码、运行验收与独立代码复核已完成；修改未提交。基线与决策见 [plan.md](plan.md)，实现细节见 [指导手册](../docs/cellfuzz/ReviewFixGuide.md)。勾选任务时附上提交、运行命令及证据位置；不能仅写“测试通过”。

## T1：名称冲突在物化前拒绝

依赖：无。规模：M，预计 4 个文件。

预计文件：`passes/cmds/cellfuzz/transport.cc`、`tests/cellfuzz/materialize.cc`、`tests/cellfuzz/materialize.py`、`tests/cellfuzz/memory_query.py`。

实施：按手册 §2 建立包含全部 `MEMID` 的作用域，统一验证普通及 query 名称，避免每个端口重复解析 memory。

验收：

- [x] 普通、query 地址/数据、未选 memory 和关闭观测的占名冲突都在预检阶段拒绝。
- [x] 失败后 RTLIL、分析批次、旧文件不变，没有临时文件；同进程换 prefix 后仍可规划。
- [x] 无冲突设计保留现有命名和位段；最终 Verilog 回读与 lint 成功。

验证：构建 `cmake --build build -j 4`；运行 `python3 tests/cellfuzz/materialize.py` 和 `python3 tests/cellfuzz/memory_query.py`；执行手册 A.1 的旧行为/修复后对照。手工检查错误诊断包含具体占名来源。

## T2：SR latch 与静态语义独立检查

依赖：无。规模：M，预计 3–4 个文件。

预计文件：`passes/cmds/cellfuzz/monitor_plan.cc`、`tests/cellfuzz/type_plan.cc`、`tests/cellfuzz/type_plan.py`；如需分离夹具，增加一个 `tests/cellfuzz/` 下的 RTLIL 文件。

实施：按手册 §3 从原始类型判断 latch；在既有类型入口中加入手写的 SR、D latch、常量 D latch 和 clocked SR 期望。

验收：

- [x] `$sr` 及四种 `$_SR_*` 都为 latch；clocked SR 仍为非 latch，常量 D latch 身份保留。
- [x] planned/materialized 分别与独立表比较控制、极性、flags、初值和复位值，Q 观测不变。
- [x] 旧分类、`has_sr` 取反、控制极性反转均触发对应语义断言；恢复后正常通过。

验证：构建；运行 `python3 tests/cellfuzz/type_plan.py`；执行 A.3；记录三种故障注入与恢复证据。手工检查 oracle 未调用生产分类函数生成期望。

## 检查点 A：预检和元数据

- [x] T1/T2 均有实际旧行为复现、修复结果和独立断言。
- [x] 审查 diff 没有放宽已有失败条件；检查人记录语义依据及遗留限制。
- [ ] 构建及上述定向回归通过，修复提交分别可审查。

## T3：导出能力预检与最终产物矩阵

依赖：无；建议在 T2 后执行，避免同时改 `monitor_plan.cc`。规模：M，预计 4–5 个文件。

预计文件：`passes/cmds/cellfuzz/monitor_plan.cc`、`tests/cellfuzz/materialize.py`、`tests/cellfuzz/value_roundtrip.py`、`tests/cellfuzz/type_plan.cc`，以及可能新增的一个夹具文件。若完整类型矩阵超出此范围，按类型族拆分子任务，保留全部覆盖要求。

实施：按手册 §4 为组合类型区分语义和已确认导出路径。把统一映射流程接入相关导出/数值回归，完成所有接受类型的能力矩阵；矩阵可就近写入测试中的说明和静态表。

验收：

- [x] 每个接受类型都有 owning backend/model、必要参数限制与最终导出用例；未知类型在全模块预检拒绝。
- [x] `$fa` 最终自包含，所有输出和新增接口与独立预期一致；其他承诺类型逐项覆盖。
- [x] 仅在插装后映射；删除必需映射步骤会由导出/回读断言捕获，没有夹具专用的隐式额外流程。

验证：构建；运行 `type_plan.py`、`materialize.py`、`value_roundtrip.py`；在新进程中回读最终 Verilog 并 lint。手工审查完整类型矩阵和最终 manifest 接口映射。

## T4：主设计与帮助使用统一导出流程

依赖：T3。规模：M，预计 5 个文件。

预计文件：`passes/cmds/cellinstrument.cc`（帮助文字）、`docs/cellfuzz/InstrumentDesign.md`、`InstrumentImplementationPlan.md`、`Stage2ATypePlanning.md`、`Stage3AMaterialization.md`（后四项均在 `docs/cellfuzz/`）。

实施：把 T3 已验证的插装后映射流程写成正式用法，解释 materialized 的边界及最终产物检查；历史阶段页增加更正说明，保留原验收记录的日期/阶段背景。

验收：

- [x] 主设计、实施计划、帮助和阶段说明对必要映射没有矛盾。
- [x] 使用者按文档复制命令即可生成通过回读/lint 的 `$fa` 示例；接口检查要求明确。
- [x] 没有宣称自动导出、跨文件事务或运行时模型绑定已经实现。

验证：帮助改动后构建并查看 `build/yosys -p 'help cellinstrument'`；执行文档示例；检查相对链接和 `git diff --check`。不为文字改动重复整组仿真。

## 检查点 B：可交付导出路径

- [x] T3/T4 对同一流程进行说明和验收，完整类型矩阵有证据。
- [x] 审查人核对最终 Verilog、manifest 和数值 oracle，不能仅检查映射前 RTLIL。
- [x] 后续失败与 materialized 的边界已写清；没有通过缩小既定能力绕开 F2。

## T5：确定性依赖摘要引擎与版本

依赖：无。规模：M，预计 5 个文件。

预计文件：`passes/cmds/cellfuzz/structural_digest.cc`、`analysis_snapshot.h`、`analysis_snapshot.cc`（前三项在同一目录）、`tests/cellfuzz/snapshot.cc`、`tests/cellfuzz/snapshot.py`。

实施：按手册 §5 加入局部摘要、SCC 依赖合成和命令期 context API；单根包装器先使用新算法。内部 snapshot 版本升级，旧批次明确拒绝。

验收：

- [x] 链、菱形、自环、循环、孤立模块均终止且确定；有语义的位序和 process 优先顺序未被排序丢失。
- [x] child 改动传播，无关模块不影响根；不同创建顺序、clone 和当前 Design 重新绑定正确。
- [x] 旧版本拒绝并提示重新分析；同一新版本内摘要稳定，无跨命令缓存。

验证：构建；运行 `python3 tests/cellfuzz/snapshot.py`；手工检查哈希编码与 lifecycle，注入忽略 child 的变体应触发 stale 断言。此任务完成仍不能关闭 F4 性能项。

## T6：cellfilter 多模块共享摘要与规模验证

依赖：T5。规模：M，预计 3–5 个文件。

预计文件：`passes/cmds/cellfilter.cc`、`tests/cellfuzz/snapshot.cc`、`tests/cellfuzz/snapshot.py`；若 T5 没有提供测试侧计数支持，另改 `structural_digest.cc` 和其声明所在头文件。

实施：在一次 cellfilter 调用中为全部根复用 context；维持现有批次失效/完整发布语义。将 A.4 生成器和操作计数检查纳入可重复验证。

验收：

- [x] 每个不同可达模块的局部序列化次数为一；没有逐根复制/哈希传递依赖全文。
- [x] 默认多模块、单模块选择、失败批次、DOT 开关和恢复检查维持契约。
- [x] 同环境的链式增长、共享依赖及峰值内存数据表明二次重复工作已消除；证据附样本和运行条件。

验证：构建；运行 `snapshot.py`、`shared_graph.py` 和 `YOSYS="$PWD/build/yosys" bash tests/various/cellfilter.sh`；执行 A.4 规模比较并审查操作计数。必要时扩大链长区分启动开销与算法成本。

## 检查点 C：摘要正确性与性能

- [x] T5/T6 的语义和性能证据同时成立；缓存没有绕过 freshness 检查。
- [x] 审查人核对 SCC 成员/依赖编码、孤立节点覆盖和深链非递归处理。
- [x] 多根分析仍不修改功能 RTLIL，不发布不完整批次。

## T7：memory 查询的独立结构 oracle

依赖：无。规模：M，预计 3–4 个文件。

预计文件：`tests/cellfuzz/materialize.cc`、`tests/cellfuzz/memory_query.py`，以及一个独立行为夹具；必要时修改 `tests/cellfuzz/memory_access.py`。

实施：按手册 §6.2 区分 raw `$mem_v2` 与 legacy 归一化；从原始 packed 输入独立推导允许增量，补入非零 offset、nibble 写掩码和已知初值的行为检查。

验收：

- [x] `$mem_v2` 的基准在任何 emit 前保存，归一化差异逐项解释；legacy 预期不由被测 helper 自动生成。
- [x] 原业务端口、INIT、控制/mask 保持预期，仅增加合法 query；小模型业务 Q/query 均达到 `0x2b`。
- [x] query 控制、INIT、业务 mask 故障分别被对应断言捕获，正常版本恢复后通过。

验证：运行 `materialize.py`、`memory_access.py`、`memory_query.py`；测试插件按入口构建。保存有界 SAT/独立数组模型的约束、步数和结果；手工检查 oracle 来源。

## T8：更新受影响阶段文档与修复记录

依赖：T1–T7。规模：M，预计 5 个文件。

预计文件：`docs/cellfuzz/Stage1BCSessionSnapshots.md`、`Stage3BValueRoundtrip.md`、`Stage4MemoryAccess.md`、`Stage5MemoryQuery.md`、`ReviewFixGuide.md`（均在同一目录）。

实施：注明摘要版本/性能改进、统一映射后的值验证、名称预检与 memory oracle 边界。在本手册增加实际修复提交和证据索引，明确哪些原建议已经实现。

验收：

- [ ] 历史验收事实与新增修复结果有清楚区分，所有四项都有提交和证据。
- [x] 无夸大的“所有语义正确”或把有限 SAT 写成无界证明的描述。
- [x] 命令、路径和相对链接与实际实现一致。

验证：人工逐条核对证据及 `git diff --check`；不因本任务重复无关运行时测试。

## T9：整体验收与独立复核

依赖：T1–T8。规模：S，生产文件不修改，记录更新限于本清单和 `ReviewFixGuide.md`。

验收：

- [x] 完整构建、八个既有入口、新增定向检查及 various/cellfilter 均通过；四项旧问题均有可追溯的关闭依据。
- [x] 所有故障注入逐字节恢复并重建，工作区不含注入变体；没有跳过测试、降低断言或隐藏导出依赖。
- [x] 审查人核对实现、最终产物和证据，记录剩余限制；未通过项保持未勾选。

验证：执行手册 §6.3 整组命令，检查 `git diff --check` 与工作区状态，复核性能记录及最终接口/值比较。不以单次“全绿”替代前述证据。

## 本次执行证据

- 基线：`e8b9e9c0ba085b3851005b94b469c6be2b4f3b10`；修复修改保留工作区，未创建提交。因此需要提交 SHA 的两项保持未勾选，不伪造完成。
- T1/T2：名称冲突先红后绿；15 个手写静态情形逐字段核对两阶段，三种生产源码故障注入均由指定断言捕获。
- T3/T4：75 类组合、185 参数样例，最终输出/选择位完整集合、独立数学值、回读/lint；两种映射遗漏均被拒绝。help 与主设计流程一致。
- T5/T6：4096 根计数、循环/孤立/依赖变化、版本 1 拒绝、ignore-child 故障捕获；100/200/400/800 链计时与 RSS 原始样本已保存。
- T7：raw packed oracle、显式 legacy 迁移、手写 nibble 及时序/SAT、三个特定 raw 故障检查；无 gold emit。
- T8/T9：完整构建、八个 CellFuzz 入口、various/cellfilter、diff 检查通过；独立再审无剩余 Critical/Required 代码问题。
- 详细命令、断言、限制与源码哈希：[ReviewFixResults.md](../docs/cellfuzz/ReviewFixResults.md)、[ReviewFixEvidence.json](../docs/cellfuzz/ReviewFixEvidence.json)。
