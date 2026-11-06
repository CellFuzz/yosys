# CellFuzz 审查问题修复指导手册

状态：**修复代码与运行验收已完成，修改未提交**。编写及修复日期：2026-11-06。依据 `f51fc1ed830e08817e98e20737609a2aea7cccc5`（包含）至 `e8b9e9c0ba085b3851005b94b469c6be2b4f3b10` 的五次提交审查；本手册核对的代码基线仍为后一个提交。本次实际修复、故障注入、覆盖边界和性能原始样本见 [ReviewFixResults.md](ReviewFixResults.md) 与 [ReviewFixEvidence.json](ReviewFixEvidence.json)；以下根因和旧行为仍保留为审查历史。

本手册给出修复决策、代码落点、实施步骤和独立验收方法。执行顺序见 [修复计划](../../tasks/plan.md)，逐项检查见 [任务清单](../../tasks/todo.md)。原功能设计见 [InstrumentDesign.md](InstrumentDesign.md)，历史阶段记录从 [InstrumentImplementationPlan.md](InstrumentImplementationPlan.md) 进入。

## 1. 修复范围与证据边界

| 编号 | 级别 | 已确认问题 | 引入提交 | 完成条件 |
| --- | --- | --- | --- | --- |
| F1 | P1 | 新增接口名没有排除 collected memory 的 `MEMID`；插装成功后 Verilog 出现重复声明 | `d36299855`；`e8b9e9c0b` 扩展到 query | 普通输出和 query 地址/数据端口统一预检，冲突在修改设计前拒绝 |
| F2 | P2 | 可求值类型不等于直接行为导出类型；`$fa` 被接受后导出为未解析实例 | `d36299855` | 支持性判断、正式导出流程和最终产物验收一致 |
| F3 | P2 | `$sr` / `$_SR_*` 的 manifest 错写 `latch: false` | `d36299855` | 保留原始存储类型身份，并独立验证静态语义 |
| F4 | P2 | 多模块分析逐根重复序列化全部依赖，链式层次达到二次复杂度 | `f51fc1ed8` | 单次命令共享依赖摘要，保持确定性、循环处理和过期检测 |

`78644868e` 主要增加实际值测试，未确认独立的新生产回归；`42f8af233` 的 memory 业务观测也未确认独立的新功能错误。修复以四个具体问题为边界，不把未证实的怀疑列为缺陷。

审查时完整构建、八个 CellFuzz Python 测试入口和既有 `various/cellfilter.sh` 均通过，但四个问题仍可独立复现。进一步将 manifest 中的 `has_sr` 序列化值取反，重建后八个入口仍全部通过。这证明测试存在漏检，不能据此推断编写者的主观意图。

以下规则适用于所有修复：

- 保持 `cellfilter` 只读分析、`cellinstrument` 消费同会话快照的职责；不在恢复时重选对象。
- 保留已选普通 cell 的全部输出、mux 全部选择位、register Q、默认开启 memory 业务观测及默认关闭 query 的约定。
- 错误必须按实际阶段处理：预检失败保留设计、有效快照和旧 manifest；物化已经开始后的失败按现有契约丢弃设计并重新加载。
- 修复不增加跨进程快照导入、运行时采集器、模型身份绑定或 flatten 后实例筛选功能。
- 历史“测试通过”记录保留为历史事实；新增更强的验收结果应注明提交、命令、断言和覆盖边界。

## 2. F1：完整校验新增接口的名称

### 2.1 根因与修改位置

[transport.cc](../../passes/cmds/cellfuzz/transport.cc) 的 `check_name()` 检查 RTLIL 对象和模块参数，但 memory cell 名称与 `MEMID` 可以不同。[Verilog 后端](../../backends/verilog/verilog_backend.cc) 的 `dump_memory()` 使用 `Mem::memid` 声明数组。因此，重命名 cell 不会释放数组在导出模块中的名称。

普通端口由 `create_transport()` 生成，query 名称由 memory 规划生成，最终均经过 `validate_transport()`。应在这一共享边界补齐校验，不分别为普通/query 路径添加两套规则。

### 2.2 推荐实现

1. 把标识符语法检查与名称占用检查分开。保留 ASCII、Verilog 保留字和 `__` 前缀限制。`-prefix` 本身只做语法检查：已有对象名为 `cf`，不代表 `cf_obs_0000` 冲突。
2. 每次完整布局校验只构建一次原始作用域，占用项包括模块中的 wire、cell、memory、process 等原有对象，模块默认/可用参数，以及**全部** collected memory 的 `MEMID`。已有 `module->count_id()` 覆盖的检查不能丢失。
3. 优先复用 `MonitorPlan::memories` 中冻结的 `MemoryInfo::memory.memid`。现有规划会登记 memory，即使未选中或关闭观测也必须参与占用集合；用测试锁定这个前提。不要对每个生成端口重新解析一遍所有 memory。
4. 统一检查所有普通输出及启用 query 的地址输入、数据输出，同时检测生成名称之间的重复。集合建立后，每个名称只需常数或对数成本的查找。
5. 报错应指出生成名称、占用对象类别、原始 cell 名及 `MEMID`，便于使用者通过 `-prefix` 解决。保持确定性命名，遇冲突直接拒绝，不静默追加后缀。

建议的内部结构如下；这是职责示意，不要求引入公共 API：

```text
validate_transport(plan, layout, module):
    validate_prefix_syntax(plan.options.prefix)
    occupied = collect_original_scope(module, all plan.memories)
    generated = empty set
    for name in layout output names + enabled query address/data names:
        validate_interface_syntax(name)
        require name absent from occupied
        require insertion into generated succeeds
    validate_widths_segments_and_coverage(...)
```

使用 `RTLIL::escape_id(generated_name)` 与 canonical `Mem::memid` 比较，遵循现有 RTLIL 标识符约定。不要通过删除所有反斜杠、删除 `$` 或自行拼接 Verilog 转义字符串构造另一套名称规则。后端自动内部名与外部合法 ASCII 端口名属于不同处理路径；应以实际导出回读检查边界，不能仅凭自制字符串规则声称覆盖所有后端选项。

`create_transport()` 中提前调用的名字检查可以保留轻量语法校验，完整作用域校验集中在 `validate_transport()`。物化入口如果再次调用该验证器，应继续检查；每次验证内只扫描一次 memory，不缓存跨设计状态。

### 2.3 失败时机和验收

校验必须在 [cellinstrument.cc](../../passes/cmds/cellinstrument.cc) 创建 `PreparedArtifact`、失效快照、增加端口和调用 `Mem::emit()` 之前完成。不要等到 `write_verilog` 或 lint 失败才补救。

| 场景 | 必须观察到的行为 |
| --- | --- |
| 数组名为 `cf_obs_0000`，其 cell 已重命名 | 普通插装和 `-plan-only` 均拒绝 |
| 数组名为 `cf_mem_0000_addr` 或 `cf_mem_0000_data`，其 cell 已重命名 | 对应 query 接口在预检阶段拒绝 |
| 占名 memory 未选中，或 memory 观测关闭，但其他对象仍生成同名端口 | 仍拒绝；选择集合不能改变原设计名称的占用 |
| 原对象/参数占名、生成端口间重名 | 保留既有拒绝行为 |
| 只有 `prefix` 本身与对象同名；无完整生成名称冲突 | 可以规划 |
| 使用不冲突的自定义 prefix | 成功；导出后回读及 lint 均成功 |

同进程捕获预检异常，再比较前后 RTLIL、scratchpad 分析批次和已有 manifest 文件字节；均应不变且没有临时文件残留。随后用另一 prefix 规划成功，证明快照仍可消费。独立进程的非零退出只能证明拒绝，不能代替此状态检查。

建议主要修改 `transport.cc`，复用 `tests/cellfuzz/materialize.cc/.py` 的失败捕获机制，向 `memory_query.py` 加入地址/数据名称情形。最小复现见附录 A。

## 3. F3：保留 SR latch 的正确类型身份

### 3.1 根因与实现

[monitor_plan.cc](../../passes/cmds/cellfuzz/monitor_plan.cc) 的 `register_info()` 目前只用 `has_aload` 识别无时钟 latch，漏掉只有 `has_sr` 的 SR latch。[kernel/ff.h](../../kernel/ff.h)、[kernel/ff.cc](../../kernel/ff.cc) 和 [word_reg.rst](../source/cell/word_reg.rst) 定义了这些存储类型。

继续通过 `FfData` 取得控制、初值和复位语义，但依据**原 cell 类型**判断 latch 身份。原因是常量 D 的 D latch 可以被 `FfData` 归一化为异步 reset 形式，单看归一化后的 `has_aload` 会丢失身份。

推荐条件：

```cpp
FfTypeData original_type(cell.type);
info.is_latch = !original_type.has_clk && !original_type.has_gclk &&
        (original_type.has_aload || original_type.has_sr);
```

原有全局时钟类型拒绝规则继续生效；clocked `$dffsr` 即使有 `has_sr` 也不是 latch。不要扩大到“所有无 clock 类型都是 latch”。这是元数据修复，不应改变 Q 的观测源、控制极性或运行时电路。

### 3.2 独立语义表

新增或加强测试时，期望值应来自手写夹具及正式 cell 语义。测试不能调用生产 `register_info()`，也不能复制上述分类表达式生成期望。

| 手工输入类型 | `latch` 期望 | 必须同时核对的内容 |
| --- | --- | --- |
| `$sr` | true | `has_clk=false`、`has_sr=true`、SET/CLR 位序及极性 |
| `$_SR_NN_`、`$_SR_NP_`、`$_SR_PN_`、`$_SR_PP_` | true | 四种 SET/CLR 极性组合，门级类型身份 |
| `$dlatch`、`$adlatch`、`$dlatchsr` | true | enable、异步控制及存在的复位值 |
| 常量 D 的 `$dlatch` | true | 原类型身份；归一化后的控制与常量值按文档解释 |
| `$dffsr` / 相应门级有时钟类型 | false | 时钟及 SET/CLR，防止扩大修复条件 |
| 普通 DFF、带 enable/reset 的 FF | false | 时钟极性、enable/reset 优先级、初值和复位位宽 |

对每个手工情形分别检查 planned 和 materialized manifest 中的字段与独立期望；两份 manifest 相等只作为一致性检查。保留 143 种类型的广度检查，但不能把“143 个 Q 项”写成“143 种静态语义均已验证”。

故障注入至少包括：恢复旧 latch 判断、取反导出的 `has_sr`、反转一种控制极性。每个变体都应由指定的语义断言捕获；编译失败、超时或无关 lint 失败不算捕获成功。

## 4. F2：统一支持范围与导出流程

### 4.1 决策：保留观测能力，显式执行插装后映射

推荐保留设计已承诺的 `$fa`、`$alu`、mux 等观测能力，将所需映射纳入正式流程。`is_evaluable()` 只能说明内建组合语义，不能证明后端会内联输出。仅在 [Stage2ATypePlanning.md](Stage2ATypePlanning.md) 中说明有 simlib/simcells 模型，仍不足以支持主设计中的直接导出示例。

不采用以下替代方式：

- 在规划中偷偷调用会修改设计的映射或后端：破坏 `-plan-only` 和快照的只读约定。
- 简单拒绝所有需要映射的类型：虽然能收紧直接导出范围，但会削减已约定的观测能力。
- 仅向 lint 补载 simlib：验证的是带外部模型依赖的产物，必须另立明确契约才能作为替代交付。

该决策是本手册的推荐修复方案，审查基线 HEAD 当时尚未统一实现和验收；现已在工作区完成，见修复验收记录。

### 4.2 类型能力表与预检

在 `monitor_plan.cc` 中把“组合语义允许”与“具有已验证导出路径”分开。可使用小型内部枚举或表，表达 `direct`、`mapped`、`unsupported`；不必为此增加 CLI 模式或 manifest 字段。

表项需记录 owning backend/model 和必要参数限制。`supported_comb()` 不能继续仅凭 `is_evaluable()` 推导所有类型均满足交付契约。可用官方分类枚举候选，但每个被接受类型都必须被导出矩阵覆盖；新候选类型没有确认路径时应失败，不能默默放行。FF、memory 和无行为 `$scopeinfo` 继续按各自规则处理。

| 类型/类别 | 已知依据 | 修复验收要求 |
| --- | --- | --- |
| `$fa` | 当前直接导出留下未解析实例；仓库提供映射模型 | 必须在正式映射流程后自包含；检查 X/Y 全部输出 |
| `$alu`、`$lcu` 等内部算术类型 | 仓库有模型，既有测试使用额外映射 | 逐类型核对最终网表和多输出值；不能由 `$fa` 的成功外推 |
| `$bmux`、`$demux` | Verilog 后端已有自动映射调用 | 不列为已确认的直接导出失败类型；仍加入完整流程矩阵 |
| 其余允许的组合、门级、FF/latch、memory 类型 | 正式 cell 定义核对 | 验证参数边界与功能/观测端口；保留已有特殊限制 |
| 未知/无已确认导出路径的类型 | 缺少闭环依据 | 整个目标模块预检拒绝，即使该 cell 未被选中 |

预检不执行映射，只识别当前版本已验证的路径和参数约束。不能用选中集合隐藏设计中其他不支持类型。对非法结构仍保留现有 `top->check()`。

### 4.3 正式流程与产物边界

建议把下面的顺序同时写入主设计、实施计划和 `help cellinstrument`；阶段页保留历史记录并增加后续修正链接。

```text
# 先完成项目实际需要的读取、hierarchy、proc、memory_collect、flatten 等准备。
# 以下假定仅选中完整 top，且已经满足现有预检条件。
cellfilter
cellinstrument -manifest monitor.json
write_rtlil instrumented.il
opt_clean
check -assert
write_verilog instrumented.v
```

其中 `write_rtlil` 是可选的诊断/重试检查点。不得把映射步骤移动到分析与插装之间。映射后删除或替换内部 cell 是正常行为，manifest 继续描述插装时冻结的源身份；最终消费契约依赖新增顶层接口、方向、宽度与位段保持一致。

在新进程中至少执行：

```sh
build/yosys -p 'read_verilog instrumented.v; hierarchy -check -top top; proc; check -assert'
verilator --lint-only --top-module top -Wno-fatal instrumented.v
```

`top` 需替换为实际模块名。`-Wno-fatal` 不放宽重复声明、缺失模块等错误；警告仍需审阅并分类，不新增全局忽略项掩盖回归。读取 simcells 时本版本会输出已有 tristate 支持警告，不能把这视为已验证所有 tristate 语义。

`status: materialized` 只表示 RTLIL 插装和对应 manifest 发布成功。后续映射、导出或回读失败时，不能把该 manifest 与失败/旧 Verilog 当作可交付的一对产物。执行脚本应在独立输出目录完成全部检查，成功后才向下游提供该目录。这里不增加跨文件原子发布或运行时模型绑定协议。

### 4.4 独立验收

1. 对照最小 `$fa` 夹具，记录旧流程直接导出失败，新正式流程的回读和 lint 成功。本手册编写时已用当前构建验证后者；这不是完整类型矩阵通过的证据。
2. 对所有接受类型逐项列出必要映射步骤和实际验证。删除任一必需步骤的变体必须暴露缺失依赖；测试脚本不能对某个夹具暗中添加文档没有的步骤。
3. 从最终 Verilog 回读结果独立核对原业务接口及 manifest 声明的所有新增接口，包括 query 地址输入、数据输出和普通打包位段。不能只核对映射前 RTLIL。
4. 比较最终业务输出和观测值。对 `$fa` 的 0/1 输入，逐位验证 `Y=A xor B xor C`、`X=(A&B)|(A&C)|(B&C)`；它的 X 是逐位 carry，并非已经左移的和。四态输入应按官方模型另行定义预期，不能套用二态 oracle。
5. FF/latch 覆盖时钟/复位/保持行为，memory 覆盖初始化、写掩码、非零 offset、同步读和 query。依照类型处理仿真器对 X/Z 的能力；不把二态 lint 或仿真通过等同于四态证明。

## 5. F4：单次命令共享结构摘要

### 5.1 根因和性能目标

[structural_digest.cc](../../passes/cmds/cellfuzz/structural_digest.cc) 对每个根独立收集可达定义、序列化并哈希；[cellfilter.cc](../../passes/cmds/cellfilter.cc) 对所有选中模块逐个 capture。N 层链因此处理 `N+(N-1)+...+1` 份模块记录。

审查时，同一二进制、同一输入及默认无 top-k 配置，包含前端与 hierarchy 的命令墙钟结果如下。这是回归证据，不是跨机器性能门槛：

| 链长 | 提交前 cellfilter | 当前 cellfilter |
| --- | ---: | ---: |
| 100 | 0.052 s | 1.016 s |
| 200 | 0.082 s | 3.968 s |
| 400 | 0.138 s | 15.953 s |

目标是每个可达模块局部记录只序列化/哈希一次，每条依赖只参与有界次数的处理。仅缓存 `module_record()`，但仍为每个根组装全部传递依赖的 JSON，仍然是二次工作，不能作为完成方案。

### 5.2 摘要上下文与生命周期

建议增加内部 `StructuralDigestContext`，由一个 `Design` 和本次目标根集合构造，只存在于一次只读分析阶段。上下文收集所有目标根的可达模块并集；未被任一根引用的模块不进入计算。

接口职责示意：

```text
context = StructuralDigestContext(design, selected_roots)
for module in selected_roots:
    graph = build_graph(module)
    selection = graph.select_objects(options)
    batch += AnalysisSnapshot.capture(graph, selection, strategy, context)
publish_complete_batch(batch)
```

`capture()` 校验 context 对应的 Design 和根模块；不要开放任意 digest 字符串参数绕过绑定。保留现有单根 `structural_digest(module)` 包装器，在内部创建新的上下文，方便现有单根调用方和恢复路径使用。

context 不进入 snapshot、scratchpad、全局静态变量或跨命令缓存；不存放跨设计切换仍可使用的裸指针。`restore()` 必须针对当前 Design 新建上下文计算摘要，不能复用分析时的缓存判断“未变化”。设计修改、`design -load`、clone 切换和物化开始前都必须结束旧上下文生命周期。

### 5.3 确定性依赖哈希与循环

建议使用按依赖合成的摘要；以下编码规则应写进源码注释并由测试固定。

1. **局部记录。** 保留当前 `module_record()` 的内容：模块/wire/cell 名、类型、参数、属性、init、memory、process、端口和连接。保持 SigSpec 位序、重复 bit、X/Z、常量 flags、process 优先顺序；仅排序本来无序的容器。不原地排序或修改 RTLIL。
2. **局部摘要。** 用带算法版本的域标签和局部记录计算 `local_hash(module)`。继续使用现有内容哈希库即可；该摘要用于变化检测，不承担认证用途。
3. **依赖图。** 建边 `module -> design.module(cell.type)`。同一目标定义的依赖边可去重；实例名称、参数、连接与重复实例仍完整保存在局部记录中。模块名字按 `IdString::str()` 的字节序排序，不依赖 intern ID、指针或创建顺序。
4. **强连通分量。** 用 SCC 把循环和自引用收敛为 DAG，依赖分量先计算。可复用 [kernel/topo_scc.h](../../kernel/topo_scc.h) 的 `TopoSortedSccs`；模块图需提供节点/后继枚举和 DFS 索引。直接复用 `IntGraph` 时注意它从边推导节点范围，必须显式覆盖孤立节点；更清晰的做法是提供含完整节点范围的小型适配器。
5. **分量摘要。** 对分量成员的有序 `(原模块名, local_hash)` 列表，及跨分量依赖的有序去重 `(目标模块名, target_component_hash)` 列表进行哈希。内部边拓扑已由成员记录的 cell type 引用确定。序列化必须使用 JSON 数组或带长度编码，避免简单字符串拼接歧义。
6. **根摘要。** 对 `(域标签, 根模块原名, 所属 component_hash)` 哈希。同一循环内不同根仍有不同身份；下游变化使所有可达祖先失效，单纯新增无关模块或修改上游调用者不改变子模块摘要。

示意定义：

```text
L(m) = H(["cellfuzz-local-v2", module_record(m)])
C(s) = H(["cellfuzz-component-v2",
          sorted [(name(m), L(m)) for m in s],
          sorted_unique [(name(d), C(component(d))) for outgoing d outside s]])
D(r) = H(["cellfuzz-root-v2", name(r), C(component(r))])
```

不要将 SCC 临时编号、DFS 访问顺序或传递依赖全文放入摘要。深链遍历使用显式栈/队列，避免引入递归栈溢出。

设 B 为可达局部记录总大小，V/E 为模块/依赖边数，目标成本为局部规范化排序的成本，加 `O(B + (V+E) log(V+E))` 的处理；缓存保留局部摘要和图，而非每个根的传递闭包。完成的判据首先是实际操作次数，墙钟计时作为补充。

### 5.4 接入和版本

修改落点为 `structural_digest.cc`、`analysis_snapshot.h/.cc`、`cellfilter.cc`。可先增加上下文并通过单根包装器使用，再接入多根 capture，分开审查算法正确性和性能收益。

摘要算法改变会改变 `structure_digest` 及依赖它的 `analysis_id`。建议将 `AnalysisSnapshot::version` 从 1 升到 2，保持旧批次被明确拒绝并要求重新运行 `cellfilter`；不要误改 `SelectionStrategy::version`。同一新版本内的重复运行和等价 clone 应继续稳定。

不要求保留旧 digest 的具体字符串，不将摘要冻结为长期 ABI。若 manifest JSON 字段结构和语义没有改变，仅不透明摘要值改变无需单独提升 manifest schema 版本；文档应说明分析身份变化。批次仍只能在所有目标成功后一次性发布，失败不能遗留部分新批次。

### 5.5 正确性与性能验收

| 用例 | 必须满足 |
| --- | --- |
| 链、菱形、多实例共享子模块 | 一次 context 中每个局部记录只序列化一次；根摘要依赖正确 |
| 自环、双模块循环、循环下挂叶子、孤立模块 | 终止且确定；叶子改变影响循环内各根；孤立模块不漏算 |
| 模块/对象创建顺序不同，RTLIL 内容相同 | 摘要一致 |
| 修改 child 参数、连接、init 或 process 优先顺序 | 祖先的旧快照被拒绝 |
| 修改无关模块、scratchpad、临时 selection | 不误判相同根结构过期 |
| 相同名字的不同 Design、save/load、clone | 重新绑定当前对象；结构匹配可恢复，结构变化拒绝 |
| 版本 1 批次 | 明确要求重新分析，不降级跳过校验 |

用测试侧计数器或受限测试钩子确认局部序列化次数等于可达不同模块数，不增加生产 CLI 性能选项。计时固定机器、构建方式、输入、选择，预热后至少取五次中位数，记录峰值内存。对 100/200/400 及可承受的更长链观察增长；翻倍接近四倍即需继续调查。可把充分摊薄启动成本后不超过约三倍作为建议告警线，不能以计时波动推翻操作次数或过期检测失败。

## 6. 修复验证方式，避免同源自证

### 6.1 证据分层

| 证据 | 能证明什么 | 不能单独证明什么 |
| --- | --- | --- |
| 构建、已有回归 | 编译兼容及已覆盖行为未回归 | 未覆盖静态字段、最终导出或全部 memory 组合正确 |
| planned 与 materialized 相等 | 两阶段输出一致 | 两阶段没有共同错误 |
| 最终 Verilog 回读、lint | 语法/引用/接口等结构可用 | 数值语义、四态语义全部正确 |
| 手写语义表、独立数学/数组模型 | 指定输入域中的行为符合预期 | 域外行为或更长时序的普遍正确性 |
| 有界 SAT | 指定约束和步数内的性质 | 无界完整证明 |
| 可定位的故障注入 | 对某类真实错误存在有效断言 | 所有潜在错误都能被发现 |

一次修复应同时有 owning source 的语义推导、独立失败用例和相应验收。不要仅增加测试数量或 coverage 数字。

### 6.2 memory 结构 oracle 的补强

当前 [tests/cellfuzz/materialize.cc](../../tests/cellfuzz/materialize.cc) 的 `check_query_delta()` 对 gold 中所有目标 memory（包括 `$mem_v2`）先调用 `Mem::emit()`，再做参数比较。被测路径也使用该 helper，因此某些归一化错误会同时出现在 gold 和实际结果中。

修复测试时分两类处理：

- **原始 `$mem_v2`：** 在任何 `Mem::emit()` 前保存 packed 参数和连接，独立计算新增异步读口的预期增量：`RD_PORTS`、读地址/数据/控制 lane、读写关系 mask，以及 ABITS 扩展所需的原读写地址补零。业务 INIT、端口顺序、写掩码、时钟/复位/透明性语义必须从原始输入核对。若存在合法归一化，逐项记录其规则和独立预期，不能用一次 gold emit 掩盖所有差异。
- **legacy `$mem`：** 显式列出迁移到 `$mem_v2` 的字段和默认语义，使用手工预期及独立行为模型验证。需要 helper 比较的部分标为“增量一致性检查”，不要声称它独立证明了归一化等价。

保留现有 Python 数组模型的有效覆盖。新增一个可理解的小例子：`mem[5:7]`、8-bit 字、初始化 `11/22/33`（十六进制）、按 nibble 写使能、同步业务读；向地址 6 仅写低 nibble `b`，在约定时序后业务 Q 和 query 均应为 `2b`。该例审查时已通过三步 SAT，但验收说明必须保留“该输入、该约束、该步数”的限定。

对 query 新读口控制位、原始 INIT 或一个业务 mask 位分别注入错误，确认 raw delta/独立行为断言确实失败。测试预期不得从本次生成的 manifest 再推导出同一字段的“正确值”。manifest 可以用来定位接口，不能成为自身语义的唯一 oracle。

### 6.3 验证执行与交付记录

当前仓库可从根目录使用以下构建和既有回归入口；执行修复时还需加入任务对应的新用例：

```sh
cmake --build build -j 4
for test_name in shared_graph snapshot type_plan manifest materialize value_roundtrip memory_access memory_query; do
    python3 "tests/cellfuzz/${test_name}.py" || exit 1
done
YOSYS="$PWD/build/yosys" bash tests/various/cellfilter.sh
git diff --check
```

每项修复先运行能击中问题的定向检查，全部完成后运行整组回归。故障注入在可丢弃副本或明确备份的工作区进行，保存变体、预期失败断言、命令及退出码；结束后逐字节恢复、重建并运行正常用例。不能把注入变体留在生产提交中。

交付记录至少包含：修复提交 SHA、复现输入、旧行为、新行为、验证命令和结果、剩余限制。独立审查应检查是否通过删除测试、缩小类型集合、跳过导出或放宽断言来获得通过。

重复的 signal/constant JSON 编码辅助函数可以后续单独整理，保持格式逐字段一致；本轮不要求为了四项修复同时重构这些辅助函数。

## 附录 A：可复制的最小输入与复现步骤

以下从仓库根目录执行，输出均放在新临时目录。它们用于复现和制定回归，不代替上述同进程状态、完整类型与数值检查。

### A.1 memory 名称冲突

```sh
CELLFUZZ_REVIEW_DIR=$(mktemp -d /tmp/cellfuzz-repair.XXXXXX)
export CELLFUZZ_REVIEW_DIR
cat > "$CELLFUZZ_REVIEW_DIR/mem_name.v" <<'VERILOG'
module top(input clk, input [1:0] a, input [7:0] d, output [7:0] q);
  reg [7:0] cf_obs_0000 [0:3];
  always @(posedge clk) cf_obs_0000[a] <= d;
  assign q = cf_obs_0000[a];
endmodule
VERILOG
build/yosys -p "read_verilog $CELLFUZZ_REVIEW_DIR/mem_name.v; hierarchy -top top; proc; memory_collect; cd top; rename cf_obs_0000 arrcell; cd ..; cellfilter; cellinstrument -manifest $CELLFUZZ_REVIEW_DIR/mem.json; check -assert; write_verilog $CELLFUZZ_REVIEW_DIR/mem.v"
verilator --lint-only --top-module top -Wno-fatal "$CELLFUZZ_REVIEW_DIR/mem.v"
```

旧版本的第一个命令成功，第二个报重复声明。修复后第一个命令应在 `cellinstrument` 预检时报错，没有新 manifest/Verilog；此时不再执行第二个命令。把数组名换为 `cf_mem_0000_data`，同步修改 rename 的旧名，并使用 `-mem-access off -mem-content query`，得到 query 数据端口复现；地址端口同理。成功对照使用 `-prefix safe`，再导出、回读和 lint。

### A.2 `$fa` 导出

继续使用上面的临时目录：

```sh
cat > "$CELLFUZZ_REVIEW_DIR/fa.il" <<'RTLIL'
autoidx 1
attribute \top 1
module \top
  wire width 8 input 1 \a
  wire width 8 input 2 \b
  wire width 8 input 3 \c
  wire width 8 output 4 \x
  wire width 8 output 5 \y
  cell $fa \fa
    parameter \WIDTH 8
    connect \A \a
    connect \B \b
    connect \C \c
    connect \X \x
    connect \Y \y
  end
end
RTLIL
build/yosys -p "read_rtlil $CELLFUZZ_REVIEW_DIR/fa.il; cellfilter; cellinstrument -manifest $CELLFUZZ_REVIEW_DIR/fa.json; write_verilog $CELLFUZZ_REVIEW_DIR/fa.v"
build/yosys -p "read_verilog $CELLFUZZ_REVIEW_DIR/fa.v; hierarchy -check -top top"
```

第二个命令应复现缺失 `$fa` 模块。采用第 4 节正式流程重跑时，在 `cellinstrument` 后、`write_verilog` 前加入映射和检查；新的独立回读及 lint 必须成功。

### A.3 SR latch 元数据

```sh
cat > "$CELLFUZZ_REVIEW_DIR/sr.il" <<'RTLIL'
autoidx 1
attribute \top 1
module \top
  wire input 1 \set
  wire input 2 \clr
  wire output 3 \q
  cell $sr \state
    parameter \WIDTH 1
    parameter \SET_POLARITY 1'1
    parameter \CLR_POLARITY 1'1
    connect \SET \set
    connect \CLR \clr
    connect \Q \q
  end
end
RTLIL
build/yosys -p "read_rtlil $CELLFUZZ_REVIEW_DIR/sr.il; cellfilter; cellinstrument -manifest $CELLFUZZ_REVIEW_DIR/sr.json"
python3 - <<'PY'
import json
import os
from pathlib import Path
report = json.loads((Path(os.environ['CELLFUZZ_REVIEW_DIR']) / 'sr.json').read_text())
assert len(report['registers']) == 1
assert report['registers'][0]['latch'] is True
PY
```

该断言在旧版本失败，修复后通过。完整验收还需第 3 节的控制与极性断言。

### A.4 多模块链性能输入

```sh
python3 - <<'PY'
import os
from pathlib import Path
work = Path(os.environ['CELLFUZZ_REVIEW_DIR'])
for count in (100, 200, 400):
    modules = []
    for index in range(count):
        body = (f'm{index + 1} u(.a(a), .y(y));' if index + 1 < count
                else 'assign y = ~a;')
        modules.append(f'module m{index}(input a, output y); {body} endmodule\n')
    (work / f'chain{count}.v').write_text(''.join(modules))
PY
time build/yosys -q -p "read_verilog $CELLFUZZ_REVIEW_DIR/chain400.v; hierarchy -top m0; cellfilter"
```

不要 flatten，也不要只选择 m0，否则会消除多根重复处理的触发条件。对修复前后使用相同输入与命令；该夹具不需要调用仅支持单个完整 top 插装的 `cellinstrument`。
