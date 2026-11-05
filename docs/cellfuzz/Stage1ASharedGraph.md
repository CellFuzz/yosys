# 阶段 1A：共享 CellGraph、选择与 view

本阶段实现 [InstrumentImplementationPlan.md §1A](InstrumentImplementationPlan.md#1a-抽取共享图与选择view-服务)，依据 [InstrumentDesign.md §3.1](InstrumentDesign.md#31-选择与-view-的边界) 和 [RegionTopKDesign.md](RegionTopKDesign.md)。交付边界是抽取共享分析服务，保持 DOT 对象/拓扑兼容、功能 RTLIL 不变。会话快照、结构摘要、结果发布和 `cellinstrument` 分别留给 1B/1C 及后续阶段。

后续的 1B/1C 已交付，见 [会话快照、发布与恢复绑定](Stage1BCSessionSnapshots.md)。本文仍仅记录 1A 的命令内图接口和阶段验收。

## 代码组织与调用

| 路径 | 职责 |
| --- | --- |
| `passes/cmds/cellfilter.cc` | CLI 参数、memory 前置检查、模块遍历和 DOT 文件编排 |
| `passes/cmds/cellfuzz/cellgraph.h/.cc` | 图类型、RTLIL 绑定、构图、保留连接及共用邻接索引、DOT 渲染 |
| `passes/cmds/cellfuzz/selection.h/.cc` | region、回调、现有 top-k、明确选择集合、view 和隐藏路径折叠 |
| `tests/cellfuzz/` | 兼容性基线、RTL 夹具、独立 C++ 消费端及 Python 检查器 |

共享服务位于 `Yosys::CellFuzz`，不再处于 `PRIVATE_NAMESPACE`。两个 `.cc` 通过 `yosys_pass(cellfilter ...)` 编译一次。测试插件仅包含消费端代码，运行时解析构建出的 Yosys 中的共享服务符号；不把实现再次编入插件，不引入 GTest 依赖。将来增加 `cellinstrument` 时按总实施计划添加入口和 `PROVIDES`，本阶段不注册空入口。

调用者包含两个头文件，按以下顺序使用：

```cpp
#include "passes/cmds/cellfuzz/cellgraph.h"
#include "passes/cmds/cellfuzz/selection.h"

using namespace Yosys::CellFuzz;

CellGraph graph(module, build_options);
graph.build();
CellGraphSelection selection = graph.select_objects(filter_options);
CellGraphView view = graph.create_view(selection);
```

沿用仓库的 tab 缩进、结构体 PascalCase 和成员/函数 snake_case。接口注释主要说明对象生命周期、索引语义和不能跨越的边界。

## 图记录与生命周期

`GraphModuleInfo` 保存模块原始 `IdString`、参数默认值和属性字典。每个 `GraphCell` 除原有 live cell 绑定外，增加原名、类型、参数和属性的值拷贝。属性保留所有可用来源信息，包括 `src`；属性缺失以字典中没有该键表达，不合成来源。名称不经过 DOT 的显示反转义后再保存，参数仍为保留位宽和 X/Z 的 `RTLIL::Const`。

`GraphCell::retention_reason` 只说明构图结果：

| 值 | 含义 |
| --- | --- |
| `STATE_NODE` | 以 NODE 角色保留的状态边界 |
| `DATA_PATH` | 被双向可达性规则保留的 EDGE |
| `EXCLUDED_TYPE` | 因 node 类型配置被排除的对象 |
| `NOT_ON_DATA_PATH` | 未满足保留路径条件的 EDGE |

保留原因与选择状态分开：`DATA_PATH` 对象可以被 top-k 隐藏；`STATE_NODE` 可以被消费端明确集合省略。`selected_cells` 才决定本次消费集合，view 不覆盖原图状态。显式把 register 设为 EDGE 时，保留原因按 EDGE 路径规则计算，语义类型仍为 REGISTER。

`GraphPort::raw_signal` 保留端口原始有序 `SigSpec`，`canonical_signal` 为构图时 `SigMap` 的别名归一化结果。连接建立使用后者；恢复观测源必须保留前者。切片、拼接、重复 bit、X/Z 均不在抽取时重新编码或重排。`GraphConnection::bit_count` 仍只是端口关系涉及的 canonical bit 统计，不表示逐 bit 功能映射。

图只对构建时未修改的模块有效。`Cell*`、`Wire*`、`SigMap` 和 `SigSpec` 的 live wire 绑定仍是命令内数据；元数据的值拷贝不构成整个图的跨命令快照。调用者在模块修改、移除或 Design 切换后必须丢弃该图。无指针记录、版本编码、重新绑定和过期检查由 1B/1C 实现。

## 共用邻接、选择和折叠

构图期间的物理连接和物理邻接仍为私有临时数据。路径筛选完成后，从 `connections` 建立公开只读使用的 `incoming` / `outgoing`：外层按 `PortId` 索引，条目为保留连接的 `ConnectionId`。随后释放物理连接及其索引。region 回调上下文、内置评分和 view 折叠均使用同一份保留邻接，避免消费端自行把已排除路径带回来。

本次保留原有 region 划分、整数评分、同分排序及固定点折叠算法：

- 无回调且 K=0 时保留全部 edge；CLI 仍只接受正数 `-topk`。
- 回调优先于 K，可返回空集或任意大小的本 region 子集；重复或跨 region 成员触发原有内部断言，合法结果统一排序。
- 自动选择的 module port 范围仍为参与保留连接的端口。
- `create_view(selection)` 仅使用明确对象集合；不重新评分，不补回省略的 NODE 或 module port。只穿过隐藏 EDGE，不穿过 NODE/EXCLUDED。
- 折叠连接保留隐藏 cell 的唯一集合；组合环路按集合单调合并收敛。直接连接继续保留原端口名和 bit count。

DOT 渲染从值记录读取名称和类型，格式、ID 分配和连接顺序保持既有行为。CLI 选项、默认值、memory 前置错误及原功能设计不因抽取改变。本阶段不增加选择文件接口。

## 测试与验收证据

`dot_compat.json` 在抽取前从提交 `79fc9fadf` 的已构建命令生成，输入为仓库内 `graph.v`，使用相对路径读入以避免 checkout 路径进入自动 cell 名。基线保存完整对象标签和端口级边（包含位宽、折叠数量和样式），比较时解析 DOT 局部 ID 为对象标签，不靠文件存在或对象数量验收。

| 验收条件 | 测试与检查 |
| --- | --- |
| 无配额及多种 K 的 DOT 兼容 | `shared_graph.py` 对无配额、K=1/2/100、`-nodes none` 和 `-nodes none -topk 1` 比较原始基线；每组再次执行，要求 DOT 字节一致 |
| 构图、选择/view 不改变 RTLIL | 上述 CLI 用例、有/无 DOT、共享 API 消费前后 `write_rtlil` 字节比较；成功 CLI 路径还运行 `check -assert` |
| 共享 API 可以独立消费 | `shared_graph.cc` 插件只链接头文件与测试代码，使用 Yosys 中的实现 |
| 回调空集和非固定 K | 空集覆盖；不同 region 返回不同数量，可超过 K，逆序结果被规范排序；view 集合与显式 selection 相等 |
| 回调结果校验 | 独立进程传入重复成员、其他 region 的 EDGE、NODE 成员，确认在生产 `selection.cc` 的断言处失败 |
| 隐藏边界不被穿过或补回 | `a -> before -> register -> after -> y` 省略 NODE；单独省略 module port；空显式集合；显式内部 EDGE 作为对照 |
| 保留邻接不混入物理连接 | 所有 retained connection 对应入/出索引；排除 register 后相关路径和邻接均为空 |
| 元数据和值绑定 | 模块/cell 名称、参数和来源的值拷贝，合法转义标点名称；原始/归一化信号区别、重复位、X/Z；无路径 cell 的排除原因 |
| 环路和空图 | 隐藏组合环路收敛，唯一隐藏集合准确；空图、缺失来源属性合法 |
| 既有 node 配置兼容 | 原有 `tests/various/cellfilter.sh` 覆盖 register/memory/FSM 子集、别名、非法值和排除状态屏障 |

开发时先确认独立消费端在原代码上因共享接口缺失而失败；新增值记录和邻接测试也在实现前确认失败。审查时临时反转 EXCLUDED 类型的保留原因判断，行为测试必须失败，再恢复代码，避免只有成功路径覆盖。

从仓库根目录复现：

```sh
cmake --build build -j 4
make -C tests cellfuzz/Makefile various/Makefile
make -C tests/cellfuzz all summary
make -C tests/various cellfilter.sh
git diff --check
```

也可以独立执行：

```sh
YOSYS="$PWD/build/yosys" YOSYS_CONFIG="$PWD/build/yosys-config" bash tests/cellfuzz/shared_graph.sh
YOSYS="$PWD/build/yosys" bash tests/various/cellfilter.sh
```

测试依赖现有 C++ 编译器、启用动态插件的 Yosys、同一构建目录的 `yosys-config`、Python 3 和 Bash。临时插件、DOT 与 RTLIL 放入自动清理的临时目录。测试目录已接入 `tests/Makefile`、`gen_tests_makefile.py` 和 `common.mk`；验收以相应测试结果及 summary 为准，不能只根据 make 包装器的退出码判断。

本阶段验收记录：现有 CMake 配置的完整构建成功，无编译告警；`shared_graph.sh` 通过（1 个 Bash 测试目标，内部执行上述多项检查），summary 为 1 passed / 0 failed；既有 `cellfilter.sh` 结果为 PASS；`git diff --check` 通过。故障注入被 `dead_record.retention_reason` 断言捕获，恢复后的构建和两组回归均通过。这些证据只验收 1A，不代表 1B/1C 或后续插装阶段完成。
