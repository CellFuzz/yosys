# 阶段 3B：Ibex 值闭环和 register 补充

本阶段按 [实施计划 §3B](InstrumentImplementationPlan.md#3b-ibex-组合逻辑闭环和-register-补充) 验收已交付的普通插装。现有设计已确认观测规则、布局、源绑定与物化，本页记录测试分解和可复现契约，不改变 CLI/schema、选择规则或 memory 能力。

2026-11-06 审查修复：值回归统一使用插装后的两步映射，最终 Verilog 经新进程回读、接口核对和 lint，再进行实际值比较。新增 75 类组合类型、185 个参数样例的导出矩阵与二态数学 oracle；全部输出及 mux selector 的期望集合独立于 manifest 构造。除零未定义位与 shiftx 的越界 X 允许映射精化，X/Z 和三态关闭不纳入二态证明；原始 sim 对混合符号 `$pow` 的求值会清除符号，矩阵先行为导出 gold，再独立按数学公式核对。原阶段记录仍为历史证据，新增结果见 [修复验收记录](ReviewFixResults.md)。

## 目标与范围

实施条目指定 ibex_controller，主设计的纯组合基准为 ibex_branch_predict，因此同时保存并验证两者：分支预测器覆盖组合输出、pmux S 和原功能；controller 补充真实寄存器与控制路径。夹具从已有的 sv2v 转换文件逐模块原样提取，固定内容哈希并随仓库提供，日常回归无需网络或 /root/workflow。来源文件未记录准确 upstream commit，不将夹具误称为某个 upstream tag/commit。

测试采用已有 C++/json11 插件、Python 3、Bash 与 Yosys SAT/sim，不增加运行时采集器或依赖。新增代码位于 tests/cellfuzz，夹具及来源说明在 fixtures/ibex，交付说明在本文件。C++ 使用 tab、snake_case 函数和 PascalCase 类型；Python 解包器只消费 manifest 与实际接口值，不导入生产布局/绑定实现。

成功条件：

1. 新端口实际值按 manifest 独立解包，与插装前原 RTLIL source 每位相等；组合/时序实际值仿真与形式比较分别提供证据，不能用符号打包代替实际值。
2. Ibex 原功能输出保持；观测 item、原图身份、已选输出/mux S/Q 与实际端口相符，包含 top-k 和空计划。
3. 专用 RTL/内建 cell 补充多输出、mux 向量、正负沿 FF、初始化、同步/异步控制、latch，以及 1/8/32/33/64/65/256/513 位源；跨块、共享块、尾块、拼接/切片/重复 bit/X/Z 可还原。
4. 回归完成 check -assert、write_verilog 与回读，证明普通插装闭环；Verilator lint 仅作为可选辅助，不作为测试必需条件。

边界：测试展示当前源值和固定时序刺激下的状态更新；不定义事件采集协议或采样阶段。memory、外部 Verilator 采集模块与实际模型身份绑定仍按后续任务交付。保留现有断言和测试，不以跳过用例或忽略未知 cell 来获得通过。

## 实施顺序与验证

1. 固定 Ibex 原始模块、提取脚本、哈希、许可证与来源说明；先建立失败的解包消费端测试。
2. 实现测试消费端，从物化接口值还原 item；独立插件从原设计解析 manifest source，供 sim 取得参考值，并建立输入输出相同的形式比较模块。
3. 添加实际组合向量、时序刺激和 register/边界用例，比较原功能及所有 item；覆盖配置、选择、空计划与错误映射的敏感性。
4. 故障注入验证，完整 CellFuzz 与 cellfilter 回归、独立代码审查；同步主设计/实施计划与验收记录。

```sh
cmake --build build -j 4
make -C tests cellfuzz/Makefile various/Makefile
make -C tests/cellfuzz -j 2 all
make -C tests/cellfuzz summary
make -C tests/various cellfilter.sh
git diff --check
```

测试所有中间产物在独立临时目录；正式测试命令不访问本机工作流目录。已有 3A 类型/布局/失败路径测试继续保留。

## 实现与参考值隔离

`value_reference.cc` 是测试专用插件，不包含 CellFuzz 头文件或调用图/布局/绑定服务。gold 模块加载插装前的 RTLIL，按 manifest 中的原始 wire 名和逻辑 bit offset 引出每项参考值，并断言该 SigSpec 等于原 cell 的声明端口。gate 的形式比较输出只从物化 transport wire 按 segment 解码。输出前缀仅属于测试框架，不进入正式 manifest。

`value_roundtrip.py` 先对计划与物化的 graph、analysis ID、items、registers、selection、view、transport 逐项比较，核对导出 cell/port ID、原始源与图端口。每个用例还复用 3A 的 `test_cellfuzz_materialize success`：从插装后副本中移除确切的新增 delta，整个模块的结构摘要必须与原始一致，包括 FF 控制、参数和初始值。top-k 用例关闭 register 节点观测，但原设计中的 FF 仍参与功能仿真和结构保持检查。

实际值路径对 gate 执行 `write_verilog`、回读、`proc`、`check -assert`，然后让共享刺激同时驱动 gold 和 gate。公开 bench wire 保存原始参考 item、实际新增 block 和双方全部原功能输出。Yosys `sim` 的 VCD 在每个 timestamp 的所有变化处理完后形成样本。Python 消费端 `value_consumer.py` 仅接收 manifest 和实际 block 值；它不读取 source、不生成打包值，也不复用 C++ 参考插件的解包结果。消费端按 bit0 偏移取位，支持无序 segment、四态值、跨块项和共享块，同时检查范围、覆盖、重叠、ID、端口和总宽度。空计划消费空字典，仍检查原功能输出。

内建类型夹具复用已有 `type_plan.cc`，包含 `$alu` 的 X/Y/CO、`$fa` 的 X/Y、`$lcu` 的 CO、10 类 mux 的 16 项选择信号，以及别名、切片、重复位和 X/Z 常量。转换前已完成原 cell/端口身份和全结构保持检查，四态选择值也从实际导出端口验证。

## 覆盖与证明边界

| 用例 | items / register 描述数 | 观测位数 | 实际输入向量 | 布局与目的 |
| --- | --- | --- | --- | --- |
| ibex_branch_predict | 21 / 0 | 85 | 126 | 默认 256；常规/压缩分支、跳转、符号、valid、PC 进位及随机输入 |
| ibex_controller | 897 / 10 | 1892 | 48 | 默认 256；多块及尾块、真实 FSM/异步复位、全部输出与 mux S/Q |
| value_edges | 17 / 9 | 1044 | 48 | 33 位块及自定义前缀；1/8/32/33/64/65/256/513 位源与寄存器 |
| 内建类型 | 43 / 10 | 285 | 48 | 8 位块；多输出、向量选择、fine mux、FF/latch 与 X/Z |
| controller top-k | 4 / 0 | 36 | 8 | `-nodes none -topk 1`、64 位块；选择与原状态保持 |
| 空选择 | 0 / 0 | 0 | 1 | 无观测接口的正常导出、解包与功能比较 |

共 279 个输入向量；每个 VCD 样本都比较全部 item 和全部原功能输出。controller 的 Q 必须实际变化，避免仅比较不动的状态。value_edges 的 513 位输入先取全零/全一，再使用固定种子的随机值，每个位均覆盖 0/1。

`value_edges.v` 提供 9 路具有明确独立预期值的状态：正沿、负沿、enable、同步复位优先、enable 优先的同步复位、低/高有效异步复位、异步数据装载和 latch。初值与复位值不同，Python 状态机逐样本计算 Q；每路至少出现 4 种值，并断言使能保持、复位优先级、无时钟沿的异步复位/装载和 latch 开/闭都确实发生。数据/控制在 DUT 时钟不变时预置，异步数据早于 load 有效，避免以同时变化的刺激制造采样竞态。

branch 和空选择对原功能与全部重建 item 做二值组合 SAT；controller 对相同输出做 `async2sync` 后的 4 步、`-set-init-zero` 有界 SAT。后者只说明该归一化模型与初始约束下的有界结果，不等同于任意异步事件或无限时序的证明。实际四态仿真、专用状态机预期值和原始 RTLIL 全结构保持检查提供互补证据。接口只给出源值，没有在本阶段定义事件采样协议。

## 验收记录

故障注入使用内存/临时产物副本：将消费端 `[-1 - target_bit]` 改为 `[target_bit]`，四态已知值断言立即失败；对临时导出 Verilog 的 `cf_obs_0000` 首段赋值取反，首个实际样本的 item 0 报告 32 位全一与原始全零不符。后者发生在原始结构保持检查之后，证明实际接口值比较本身能够发现导出值错误。正式源码和夹具没有注入改动。

独立只读代码审查未发现 Required/Critical 问题；已采纳高位随机化的补充建议。夹具的 upstream commit 与 sv2v 版本未记录，故仅对仓库内固定哈希转换结果作上述验收；获取、提取、许可证及哈希见 [fixtures/ibex/README.md](../../tests/cellfuzz/fixtures/ibex/README.md)。

2026-11-05 最终验证：完整 CMake 构建通过，无 warning/error；CellFuzz 的 shared_graph、snapshot、type_plan、manifest、materialize、value_roundtrip 为 **6 passed / 0 failed**；既有 `various/cellfilter.sh` 为 PASS。新增 Python 语法、夹具固定哈希/提取及错误来源拒绝、`git diff --check` 和新增文件空白检查均通过。本阶段交付测试代码、夹具、注释及文档，普通物化实现通过这些验收，无需修改生产 pass。
