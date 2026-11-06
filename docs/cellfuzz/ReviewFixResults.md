# CellFuzz 审查修复验收记录

日期：2026-11-06。审查基线为 `e8b9e9c0ba085b3851005b94b469c6be2b4f3b10`。按 [ReviewFixGuide.md](ReviewFixGuide.md) 的 T1–T9 执行；代码修复和运行时验收已完成，修改保留在工作区，**未创建修复提交**。因此这里没有修复提交 SHA；[ReviewFixEvidence.json](ReviewFixEvidence.json) 保存基线、生产源码 SHA-256、实际样本和故障注入断言，不能将其误作提交记录。任务清单中要求提交的项保留未勾选。

## 四项修复及独立证据

| 问题 | 旧行为 | 当前行为及验收 |
| --- | --- | --- |
| F1 名称占用 | collected cell 改名后，MEMID 与新接口重名仍可插装，Verilog 重复声明 | 一次建立全部原作用域及所有冻结 memory MEMID，普通/query 共用检查；`materialize.py`、`memory_query.py` 覆盖关闭/未选 memory、两种执行模式和 addr/data；同进程 RTLIL、scratchpad 批次、旧文件不变，无临时残留，换 prefix 可重试；成功产物独立回读和 lint |
| F3 SR latch | `$sr` / 四种 `$_SR_*` 的 latch=false；旧分类不能完整保留身份 | 从原始 FfTypeData 分类。`register_semantics.py` 的 15 个手写情形分别核对 planned/materialized 的类型、Q、控制位序/极性、flags、init/reset 和优先级；常量 D latch 保留身份，clocked SR 不误标。原 143 类型广度检查仍保留 |
| F2 导出闭环 | `$fa` 等可求值类型被接受，但直接 write_verilog 留下未解析实例 | 显式 direct/mapped/unsupported 允许表；全模块拒绝未知路径。正式插装后映射流程见主设计及 help。75 类组合类型、185 个参数样例、40 个二态向量均检查最终业务输出、独立输出/selector 集合、观测值和位段；最终接口方向/宽度、回读/lint 成功。删掉任一必要映射步骤，独立 hierarchy 检查均拒绝 |
| F4 多根摘要 | 每根重新处理完整依赖闭包；链长翻倍约四倍耗时 | 命令期上下文共享局部序列化，以 SCC 合成依赖摘要。4096 个根的序列化计数精确为 4096，重复 capture 不增加。链/菱形/共享实例/循环/自环/孤立节点、创建顺序、clone/save/load、child 参数/连接/init/process 优先级及无关修改均有定向断言；恢复重新计算摘要，旧版本 1 批次明确拒绝 |

快照内部版本为 2；SelectionStrategy 版本与 manifest schema 不变。旧分析结果需要重跑 `cellfilter`。`cellfilter` 仍不改功能 RTLIL；`cellinstrument` 仍只消费同会话冻结 selection，不重新选择。

## 完整回归

以下命令均在仓库根目录执行，全部通过：

```sh
cmake --build build -j 4
for test_name in shared_graph snapshot type_plan manifest materialize value_roundtrip memory_access memory_query; do
    python3 "tests/cellfuzz/${test_name}.py" || exit 1
done
YOSYS="$PWD/build/yosys" bash tests/various/cellfilter.sh
git diff --check
```

最后一次恢复后的 CMake 构建无 compiler warning/error。八个入口的运行退出码和耗时见 JSON；全组后补充的“未选中未知类型仍拒绝”定向检查也通过。测试插件和可丢弃仿真设计不进入生产 RTLIL。

`tests/cellfuzz/fixtures/fa.il` 可直接复现导出差别：原直接导出命令退出 0，随后独立 `hierarchy -check` 退出 1，缺失 `$fa`；主设计所列两步映射流程退出 0，新进程回读及 Verilator lint 均退出 0。诊断/重试检查点位于插装后、映射前。

## 故障注入

| 变体 | 必须命中的断言/诊断 | 结果 |
| --- | --- | --- |
| 恢复旧 latch 分类 | 手写 `$sr` 的 `latch identity` | 失败，退出 1 |
| 导出 has_sr 取反 | 手写 `$sr` 的 `storage flag / has_sr` | 失败，退出 1 |
| SET 极性取反 | 手写 `$sr` 的 `control polarity/source / set` | 失败，退出 1 |
| 忽略跨分量 child hash | dependency-change `before != structural_digest(root)` | 失败，退出 1 |
| query enable 置 0 | raw query connection mismatch: RD_EN | 特定 raw 诊断拒绝，退出 1 |
| 原始 INIT 位翻转 | raw query parameter mismatch: INIT | 特定 raw 诊断拒绝，退出 1 |
| 同步业务读 collision mask 位翻转 | raw query parameter mismatch: RD_COLLISION_X_MASK | 特定 raw 诊断拒绝，退出 1 |

前四项在明确备份的工作区执行，`finally` 逐字节恢复源码、重建并运行正常用例；生产文件 SHA-256 记录于 JSON。后三项由 `memory_query.py` 的 `raw-fault` 模式在可丢弃进程中修改成功插装后的 packed cell，再调用独立 raw 比较器；没有生产故障钩子或遗留变体。编译失败、超时和无关 lint 不算捕获。

## Memory oracle 与有界行为

`check_query_delta()` 在任何 gold emit 前保存原 packed `$mem_v2` 参数和连接，预期只允许新增异步读口及必要的地址补零；原 INIT、端口次序、控制和关系 mask 逐项比对。最小 query 位宽由原 OFFSET/SIZE 计算，manifest 仅用于定位接口。legacy `$mem` 的迁移显式规定无 wide continuation、读 reset 为零、FF 初始/复位值为 X；透明性仅同一 clocked domain，写优先级按相同 domain 的原顺序计算；没有用 gold Mem::emit 认定归一化等价。

`query_nibble.v` 为手写 mem[5:7]、8-bit、INIT 11/22/33、nibble 写使能及同步业务读。8 个向量在写边沿前建立输入，向地址 6 只写低 nibble b：首个同步读保留旧值 22，随后 Q 和 query 均为 2b。SAT 固定地址 6、数据 ab，第一步 enable=1、第二/三步 enable=0，仅最后一步证明 Q/query=43；这是该输入、该约束和三步的有界结果。非二次幂 memory_map 的未驱动 padding 仅在仿真副本显式设 X；oracle 只查询有效地址。

## 性能样本

环境：同一 x86_64 容器，CMake C++20 构建，`CMAKE_BUILD_TYPE` 为空，`/usr/bin/c++`，相同未 flatten 链及默认全模块选择；计时包括 frontend/hierarchy。每组先预热一次，再取五次中位数，RSS 由 `/usr/bin/time -f %M` 获取。初次样本可能受同容器回归负载影响，因此计时只作补充，不代替操作次数证据。重测后的完整原始样本保存在 JSON。

| 模块数 | 修复前中位秒 | 修复后中位秒 | 修复前最大 RSS KiB | 修复后最大 RSS KiB |
| --- | ---: | ---: | ---: | ---: |
| 100 | 1.1613 | 0.0889 | 72796 | 71712 |
| 200 | 4.5815 | 0.1575 | 75780 | 73192 |
| 400 | 18.0400 | 0.2903 | 80016 | 76456 |
| 800 | 未测 | 0.5588 | 未测 | 83468 |

修复后可重复命令：

```sh
python3 tests/cellfuzz/digest_benchmark.py --counts 100 200 400 800 --repeats 5 --output /tmp/cellfuzz-benchmark.json
```

基线对照使用相同生成器/命令和前三种规模。SCC 与依赖收集使用显式栈/队列；每个局部记录只序列化/哈希一次，没有为每个根复制传递依赖 JSON。

## 审查及剩余边界

按 code-review-and-quality 进行独立只读审查；输出项缺失可绕过断言、query 期望位宽来自 manifest 的两项 Required 问题均已修复，再审无剩余 Critical/Required 代码问题。文档完成后再次核对链接和状态。修改范围只涉及四项修复及其独立验收，没有删除测试、缩小原类型集合、隐藏映射步骤或新增全局 warning suppressions。

本轮不证明所有参数组合或全部四态语义。`$bweqx`/eqx 等经映射后的 X/Z 行为不能从二态通过外推；三态测试 enable 恒为 1，不覆盖关闭/多驱动；除零及 shiftx 越界的未定义位允许映射精化。原始 `CellTypes::eval` 对混合符号 `$pow` 清除符号，矩阵将 gold 也行为导出后用独立数学 oracle 核对。

警告已分类：组合矩阵的三个 WIDTHEXPAND 来自有意不等宽 `$pow`；nibble 网表的一个 UNOPTFLAT 为同一 packed 观测总线不同位段之间的依赖，最终回读的 `scc -expect 0` 确认没有组合 SCC，值和有界 SAT 已核对；加载 simcells 的已有 tristate 支持警告不代表三态语义通过。均未新增忽略选项。`materialized` 只覆盖 RTLIL 与 manifest 发布；最终映射、导出、接口/值检查全部成功后才可交付输出目录，不提供跨文件原子事务或运行时身份绑定。
