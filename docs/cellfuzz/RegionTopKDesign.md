# CellFilter Region Top-K 过滤设计

## 1. 背景与目标

`cellfilter` 默认将寄存器、收集后的 memory cell 和 FSM cell 作为状态节点，通过 `-nodes` 可选择其中的类型子集，将状态节点或模块端口之间的数据路径组合逻辑作为 edge cell 输出到 DOT。大型设计中，一条逻辑路径可能包含大量组合 cell，分叉和汇合又会进一步放大图规模。

本设计在不修改 RTLIL 设计和原始 `CellGraph` 构建结果的前提下，将完整图转换为一个可消费的 `CellGraphView`。当前 top-k 策略的 view 每个组合逻辑 region 最多保留 K 个关键 edge cell，并将其余连续隐藏逻辑折叠为连接。当前 DOT 输出消费该 view；后续若需要让过滤结果影响其他图算法，可以直接消费相同 view，而不必重写 region、评分和折叠逻辑。

## 2. 图模型与 Region

- node 类型由 `CellGraphBuildOptions` 控制，默认 register、memory、FSM 全选；独立的 module port 继续作为边界。未选状态类型排除，不作为边界遍历种子，且路径到它即停止。
- edge cell：角色为 EDGE 且 `CellGraph::build()` 判定为 `in_graph` 的 cell。当前 CLI 只将普通组合 cell 作为 EDGE。
- region：只观察 edge cell，把任意方向上存在连接的两个 edge cell 视为相邻；每个弱连通分量为一个 region。
- 一个 region 可以连接多个上游和下游边界，因此不按 node pair 划分，避免共享逻辑重复归属。

例如：

```text
node_A -> a -> c -> d -> node_B -> j -> k -> node_C
```

其中 `{a,c,d}` 和 `{j,k}` 是两个 region，`d` 和 `j` 只是与 `node_B` 直接相连的边界 edge cell。

## 3. 转换接口

完整图到展示/消费视图拆成两个只读阶段：

```cpp
CellGraph graph(module, build_options);
graph.build();
CellGraphSelection selection = graph.select_objects(options);
CellGraphView view = graph.create_view(selection);
```

`select_objects(options)` 负责 region 划分并调用 region cell 选择策略；评分 top-k 只是当前内置策略。`create_view(selection)` 只根据明确集合构造可见对象和连接，不读取选择参数或重新选取对象，便于后续替换选择算法或调整参数；不提供 JSON 选择文件输入。

- `CellGraphBuildOptions`：按语义类型分配 NODE/EDGE/EXCLUDED 角色。CLI 的 `-nodes` 将未选状态类型设为 EXCLUDED，默认三类均为 NODE；内部角色字段保留显式设置 EDGE 的入口。
- `CellGraphFilterOptions`：保存当前内置策略参数 `top_k` 和可选 `region_selector` 回调。无回调且 `top_k == 0` 时保留全部 edge cell。
- `RegionCellSelector`：接收只读图/共用邻接索引及当前 region，返回 `RegionCellSelection`，包含选中 cell ID 子集和可选边界说明。返回空集或非固定大小均有效；检查成员合法性、重复并统一排序。回调优先于内置 top-k，不要求沿用评分或 K。
- `CellGraphSelection`：保存选中的 cell、module port 和 region 说明。
- `CellRegion`：保存 region 内全部 edge cell、关键 cell，以及上下游边界。
- `CellGraphView`：保存可见 cell、可见 module port、region 和 view connection。
- `ViewConnection`：表示直接连接或跨越隐藏 cell 的折叠连接。

原始 `CellGraph` 的 cell、port、connection ID 保持不变，view 通过这些稳定 ID 引用原图。未来可增加 `apply_view()`，或者让下游算法直接使用 `CellGraphView`。

所有登记对象仍存在于 `CellGraph::cells` 和 RTLIL 中。默认三类状态都是 NODE；`-nodes` 排除的类型保留语义类型，但 `in_graph` 为 false，不加入 nodes/edge_cells 或保留连接。可达性和 view 遍历只穿过 EDGE，不能穿过 EXCLUDED 或未显示的 NODE。内部调用者只有显式设置状态类型为 EDGE 才会采用“普通 edge”语义，当前 CLI 不启用。自动选择保持现有 DOT 的 module port 范围，仅选择参与保留连接的端口。

## 4. 当前内置 Top-K 评分

以下仅定义当前策略；后续可通过 `region_selector` 使用其他选择方案，不要求固定 K、整数评分或相同排序特征。

每个 region 独立选择 `min(K, region_size)` 个关键 cell。第一版使用稳定整数评分：

```text
score =
    4 * semantic_weight
  + 2 * floor(log2(1 + unique_fanin + unique_fanout))
  + 3 * floor(log2(1 + upstream_boundary_count * downstream_boundary_count))
  +     floor(log2(1 + data_width))
```

### 4.1 语义权重

- 8：mux/routing、乘除、复杂算术。
- 6：普通算术、比较和移位。
- 5：归约、逻辑判断、LUT 和进位逻辑。
- 4：位运算、未知或自定义 edge cell。
- 3：普通 unary cell。
- 1：`$buf/$slice/$concat/$barrier` 等 wire-like cell。

### 4.2 结构指标

- `unique_fanin/fanout` 按唯一相邻 cell 或 module port 计数，同一对象的多个 port 或 bit 只计一次。
- `upstream/downstream_boundary_count` 在 region 内沿有向连接遍历，统计不经过状态节点能够到达的不同边界。
- `data_width` 优先使用最大输出 port 位宽；无输出时使用最大输入 port 位宽。
- 对数项限制大型 fanout 或超宽总线对总分的支配。

同分时按 cell 类型名、实例名和 `GraphCellId` 排序，保证相同输入产生稳定结果。第一版不加入相邻惩罚或多样性贪心，也不特殊分类 clock、reset 和 enable。

## 5. 隐藏路径折叠

状态节点、module port 和选中的关键 edge cell可见。从每个可见驱动端口沿原有有向连接遍历：

1. 遇到隐藏 edge cell，记录该 cell 并从它的所有输出继续。
2. 遇到下一个可见对象时停止，生成一条 view connection。
3. 没有隐藏 cell 的连接保持原始实线、端口名和 bit count。
4. 跨越隐藏 cell 的连接使用灰色虚线，标注 `collapsed N cells`。
5. 相同源端口和目标端口合并；`N` 是这些折叠路径涉及的唯一隐藏 cell 数。
6. 对每个传播端口单调合并已穿过的隐藏 cell 集合；集合达到固定点后停止，避免组合环路无限循环。

折叠连接不报告 bit count，因为经过分叉、汇合或不同算子后，简单累加不能可靠表示端到端位宽。

## 6. 命令接口

完整图保持兼容：

```text
cellfilter -dot graph.dot
```

启用 region top-k：

```text
cellfilter -dot graph.dot -topk 3
```

只将 register 或 memory 作为 node：

```text
cellfilter -nodes register -dot registers.dot
cellfilter -nodes mem -topk 3 -dot memories.dot
```

`-nodes` 支持 register、memory（别名 mem）、fsm 的逗号列表，以及单独的 all/none；默认 all，重复时最后一个列表生效。none 仍允许 module port 之间的纯组合路径。非法类型或空列表项报错。

`-topk` 必须是正整数。过滤日志报告 region 数、可见/隐藏 edge cell 数和 view connection 数。

## 7. 验证计划

- 完整 view 与过滤前 DOT 拓扑一致。
- 简单链路验证语义权重和折叠连接。
- 多个独立 region 验证每个 region 独立获得 K 个名额。
- 分叉、汇合、多边界共享 region 验证边界可达指标。
- `K >= region_size` 时全部 edge cell 可见。
- 组合环路不会导致无限遍历。
- node 类型子集、别名、none、重复覆盖及非法值正确；排除状态不会导致跨状态折叠。
- 自定义 region 回调可以返回空集、任意大小的子集，与 top-k 参数解耦。
- 非法 `-topk` 参数给出明确错误。
- 重复运行产生稳定 DOT，并通过完整 Release 构建。

## 8. 后续 TODO

- 扩展 `CellGraphFilterOptions`，例如只保留 mux 类型。先按原候选图划分 region，再执行类型条件与 region 选择；未选 edge cell 仍可折叠。本次不实现类型条件或新增 CLI。
- 增加其他 region 选择策略及其专用参数，复用 `RegionCellSelector`，记录策略标识和版本；不将 top-k 固化为调用者契约。
- 若需要从 CLI 将未选状态类型作为普通 EDGE，先定义明确的遍历与折叠语义，复用内部角色接口；当前只支持排除。

## 9. 与独立插装 pass 的对接计划

本节描述后续集成，当前 cellfilter 尚未实现结果持久到会话存储。

- 同一会话先运行 cellfilter，再运行 cellinstrument。node 配置和选择参数只由 cellfilter 接收；插装消费其已发布集合，不重新评分。
- cellfilter 应在没有 DOT 输出时同样计算并发布选择结果。会话结果需要版本和结构校验，按对象身份恢复绑定，不传递跨命令裸指针。
- 静态描述导出完整 CellGraph 分析快照，并分别记录选中/观测状态。原图保留连接与折叠 view 分开输出；未观测对象仍有结构说明。
- 本文 top-k/region/折叠算法保持原样。新增结果交接、观测点和可逆打包的契约见 [InstrumentDesign.md](InstrumentDesign.md)，实施顺序见 [InstrumentImplementationPlan.md](InstrumentImplementationPlan.md)。
