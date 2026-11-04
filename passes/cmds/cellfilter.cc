#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/rtlil.h"
#include "kernel/log.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

enum class NodeType { NONE, REGISTER, MEMORY, FSM };
enum class GraphCellRole { EXCLUDED, NODE, EDGE };

// Semantic type and graph role are independent. The CLI currently excludes
// disabled state types; an internal caller can assign EDGE explicitly if it
// wants to analyze state cells as ordinary edge cells instead.
struct CellGraphBuildOptions {
	GraphCellRole register_role = GraphCellRole::NODE;
	GraphCellRole memory_role = GraphCellRole::NODE;
	GraphCellRole fsm_role = GraphCellRole::NODE;

	GraphCellRole role_for(NodeType type) const
	{
		switch (type) {
		case NodeType::REGISTER: return register_role;
		case NodeType::MEMORY: return memory_role;
		case NodeType::FSM: return fsm_role;
		case NodeType::NONE: return GraphCellRole::EDGE;
		}
		log_abort();
	}
};

using GraphCellId = int;
using PortId = int;
using ConnectionId = int;

static constexpr GraphCellId INVALID_CELL_ID = -1;
static constexpr PortId INVALID_PORT_ID = -1;

static NodeType get_node_type(RTLIL::Cell *cell)
{
	if (cell->type == ID($fsm))
		return NodeType::FSM;
	if (cell->is_mem_cell())
		return NodeType::MEMORY;
	if (cell->is_builtin_ff())
		return NodeType::REGISTER;
	return NodeType::NONE;
}

static bool is_uncollected_memory_port(RTLIL::Cell *cell)
{
	return cell->type.in(ID($memrd), ID($memrd_v2), ID($memwr), ID($memwr_v2),
			ID($meminit), ID($meminit_v2));
}

static const char *node_type_name(NodeType type)
{
	switch (type) {
	case NodeType::REGISTER:
		return "register";
	case NodeType::MEMORY:
		return "memory";
	case NodeType::FSM:
		return "fsm";
	case NodeType::NONE:
		return "edge cell";
	}
	log_abort();
}

struct GraphPort {
	PortId id = INVALID_PORT_ID;
	GraphCellId cell = INVALID_CELL_ID;
	RTLIL::Wire *module_port = nullptr;
	RTLIL::IdString name;
	RTLIL::PortDir direction = RTLIL::PortDir::PD_UNKNOWN;
	RTLIL::SigSpec raw_signal;
	RTLIL::SigSpec canonical_signal;

	bool is_cell_port() const
	{
		return cell != INVALID_CELL_ID;
	}

	bool is_input() const
	{
		return direction == RTLIL::PortDir::PD_INPUT || direction == RTLIL::PortDir::PD_INOUT;
	}

	bool is_output() const
	{
		return direction == RTLIL::PortDir::PD_OUTPUT || direction == RTLIL::PortDir::PD_INOUT;
	}

	// Module inputs drive the design, while cell inputs consume a net.
	bool drives_net() const
	{
		return is_cell_port() ? is_output() : is_input();
	}

	bool consumes_net() const
	{
		return is_cell_port() ? is_input() : is_output();
	}
};

struct GraphCell {
	GraphCellId id = INVALID_CELL_ID;
	RTLIL::Cell *cell = nullptr;
	NodeType node_type = NodeType::NONE;
	GraphCellRole role = GraphCellRole::EXCLUDED;
	bool in_graph = false;
	std::vector<PortId> inputs;
	std::vector<PortId> outputs;

	bool is_node() const
	{
		return role == GraphCellRole::NODE;
	}

	bool is_edge() const
	{
		return role == GraphCellRole::EDGE;
	}
};

// One port-level connection. bit_count reports how many canonical signal bits
// caused this relation, but their individual mapping is intentionally omitted.
struct GraphConnection {
	ConnectionId id = -1;
	PortId src = INVALID_PORT_ID;
	PortId dst = INVALID_PORT_ID;
	int bit_count = 0;
};

struct CellRegion {
	int id = -1;
	std::vector<GraphCellId> edge_cells;
	std::vector<GraphCellId> key_cells;
	pool<std::pair<int, int>> upstream_boundaries;
	pool<std::pair<int, int>> downstream_boundaries;
};

struct CellGraph;

struct RegionSelectionContext {
	const CellGraph &graph;
	const std::vector<std::vector<ConnectionId>> &incoming;
	const std::vector<std::vector<ConnectionId>> &outgoing;
};

struct RegionCellSelection {
	std::vector<GraphCellId> selected_cells;
	pool<std::pair<int, int>> upstream_boundaries;
	pool<std::pair<int, int>> downstream_boundaries;
};

// Select a subset of this region without changing its membership or the graph.
// The result may be empty and need not have a fixed cardinality or use scores.
using RegionCellSelector = std::function<RegionCellSelection(const RegionSelectionContext &, const CellRegion &)>;

struct CellGraphFilterOptions {
	int top_k = 0;
	RegionCellSelector region_selector;

	bool filtered() const
	{
		return top_k > 0 || bool(region_selector);
	}
};

// The object set selected for a particular consumer. This is deliberately
// separate from CellGraphView: selection decides which objects are included,
// while the view decides how connections between them are rendered. A rejected
// object remains in CellGraph and in RTLIL; only the consumer view omits it.
struct CellGraphSelection {
	pool<GraphCellId> selected_cells;
	pool<PortId> selected_module_ports;
	std::vector<CellRegion> regions;
};

struct ViewConnection {
	PortId src = INVALID_PORT_ID;
	PortId dst = INVALID_PORT_ID;
	int bit_count = 0;
	bool collapsed = false;
	std::vector<GraphCellId> hidden_cells;
};

struct CellGraphView {
	pool<GraphCellId> visible_cells;
	pool<PortId> visible_module_ports;
	std::vector<ViewConnection> connections;
	std::vector<CellRegion> regions;

	int visible_edge_cell_count = 0;
	int hidden_edge_cell_count = 0;
};

struct CellGraph {
	RTLIL::Module *module = nullptr;
	SigMap sigmap;
	CellGraphBuildOptions build_options;

	std::vector<GraphCell> cells;
	std::vector<GraphPort> ports;
	std::vector<GraphConnection> connections;
	std::vector<GraphCellId> nodes;
	std::vector<GraphCellId> edge_cells;
	dict<RTLIL::Cell *, GraphCellId> cell_ids;
	std::vector<PortId> module_inputs;
	std::vector<PortId> module_outputs;

	explicit CellGraph(RTLIL::Module *module, const CellGraphBuildOptions &options = {}) :
			module(module), sigmap(module), build_options(options) {}

	void build();
	CellGraphSelection select_objects(const CellGraphFilterOptions &options) const;
	CellGraphView create_view(const CellGraphSelection &selection) const;
	void dump_dot(std::ostream &f, int module_index, const CellGraphView &view) const;

private:
	std::vector<GraphConnection> physical_connections;
	std::vector<std::vector<ConnectionId>> physical_incoming;
	std::vector<std::vector<ConnectionId>> physical_outgoing;

	GraphCellId add_cell(RTLIL::Cell *cell);
	PortId add_cell_port(GraphCellId cell_id, RTLIL::IdString name, RTLIL::PortDir direction,
			const RTLIL::SigSpec &signal);
	PortId add_module_port(RTLIL::Wire *wire);
	void index_cells();
	void index_module_ports();
	void build_physical_connections();
	void retain_data_paths();
	bool port_owner_is_in_graph(PortId port_id) const;
};

GraphCellId CellGraph::add_cell(RTLIL::Cell *cell)
{
	log_assert(cell != nullptr);
	log_assert(!cell_ids.count(cell));
	GraphCellId id = GetSize(cells);
	NodeType node_type = get_node_type(cell);
	GraphCellRole role = build_options.role_for(node_type);
	cells.push_back(GraphCell{id, cell, node_type, role, role == GraphCellRole::NODE, {}, {}});
	cell_ids[cell] = id;
	if (role == GraphCellRole::NODE)
		nodes.push_back(id);
	return id;
}

PortId CellGraph::add_cell_port(GraphCellId cell_id, RTLIL::IdString name,
		RTLIL::PortDir direction, const RTLIL::SigSpec &signal)
{
	log_assert(cell_id >= 0 && cell_id < GetSize(cells));
	PortId id = GetSize(ports);
	ports.push_back(GraphPort{id, cell_id, nullptr, name, direction, signal, sigmap(signal)});
	physical_incoming.emplace_back();
	physical_outgoing.emplace_back();
	if (ports.back().is_input())
		cells[cell_id].inputs.push_back(id);
	if (ports.back().is_output())
		cells[cell_id].outputs.push_back(id);
	return id;
}

PortId CellGraph::add_module_port(RTLIL::Wire *wire)
{
	log_assert(wire != nullptr);
	RTLIL::PortDir direction = RTLIL::PortDir(
			(wire->port_input ? RTLIL::PortDir::PD_INPUT : RTLIL::PortDir::PD_UNKNOWN) |
			(wire->port_output ? RTLIL::PortDir::PD_OUTPUT : RTLIL::PortDir::PD_UNKNOWN));
	RTLIL::SigSpec signal(wire);
	PortId id = GetSize(ports);
	ports.push_back(GraphPort{id, INVALID_CELL_ID, wire, wire->name, direction, signal, sigmap(signal)});
	physical_incoming.emplace_back();
	physical_outgoing.emplace_back();
	if (ports.back().is_input())
		module_inputs.push_back(id);
	if (ports.back().is_output())
		module_outputs.push_back(id);
	return id;
}

void CellGraph::index_cells()
{
	for (RTLIL::Cell *cell : module->cells()) {
		GraphCellId cell_id = add_cell(cell);
		for (const auto &connection : cell->connections())
			add_cell_port(cell_id, connection.first, cell->port_dir(connection.first), connection.second);
	}
}

void CellGraph::index_module_ports()
{
	for (RTLIL::IdString port_name : module->ports) {
		RTLIL::Wire *wire = module->wire(port_name);
		log_assert(wire != nullptr);
		add_module_port(wire);
	}
}

void CellGraph::build_physical_connections()
{
	dict<RTLIL::SigBit, pool<PortId>> bit_drivers, bit_consumers;
	for (const GraphPort &port : ports)
		for (RTLIL::SigBit bit : port.canonical_signal) {
			if (bit.wire == nullptr)
				continue;
			if (port.drives_net())
				bit_drivers[bit].insert(port.id);
			if (port.consumes_net())
				bit_consumers[bit].insert(port.id);
		}

	dict<std::pair<PortId, PortId>, int> connected_widths;
	for (const auto &driver_it : bit_drivers) {
		if (!bit_consumers.count(driver_it.first))
			continue;
		for (PortId driver : driver_it.second)
			for (PortId consumer : bit_consumers.at(driver_it.first))
				if (driver != consumer)
					connected_widths[std::make_pair(driver, consumer)]++;
	}

	std::vector<std::tuple<PortId, PortId, int>> sorted_connections;
	for (const auto &it : connected_widths)
		sorted_connections.emplace_back(it.first.first, it.first.second, it.second);
	std::sort(sorted_connections.begin(), sorted_connections.end());

	for (const auto &item : sorted_connections) {
		PortId src = std::get<0>(item);
		PortId dst = std::get<1>(item);
		log_assert(ports[src].drives_net());
		log_assert(ports[dst].consumes_net());
		ConnectionId id = GetSize(physical_connections);
		physical_connections.push_back(GraphConnection{id, src, dst, std::get<2>(item)});
		physical_outgoing[src].push_back(id);
		physical_incoming[dst].push_back(id);
	}
}

bool CellGraph::port_owner_is_in_graph(PortId port_id) const
{
	const GraphPort &port = ports.at(port_id);
	return !port.is_cell_port() || cells.at(port.cell).in_graph;
}

void CellGraph::retain_data_paths()
{
	pool<GraphCellId> forward_cells, backward_cells;
	pool<PortId> seen_forward_ports, seen_backward_ports;
	std::deque<PortId> forward_queue, backward_queue;

	auto enqueue_forward = [&](PortId port_id) {
		if (seen_forward_ports.insert(port_id).second)
			forward_queue.push_back(port_id);
	};
	auto enqueue_backward = [&](PortId port_id) {
		if (seen_backward_ports.insert(port_id).second)
			backward_queue.push_back(port_id);
	};

	for (GraphCellId node_id : nodes)
		for (PortId output : cells[node_id].outputs)
			enqueue_forward(output);
	for (PortId input : module_inputs)
		enqueue_forward(input);

	while (!forward_queue.empty()) {
		PortId driver = forward_queue.front();
		forward_queue.pop_front();
		for (ConnectionId connection_id : physical_outgoing[driver]) {
			const GraphPort &consumer = ports[physical_connections[connection_id].dst];
			if (!consumer.is_cell_port())
				continue;
			GraphCell &cell = cells[consumer.cell];
			if (!cell.is_edge())
				continue;
			if (forward_cells.insert(cell.id).second)
				for (PortId output : cell.outputs)
					enqueue_forward(output);
		}
	}

	for (GraphCellId node_id : nodes)
		for (PortId input : cells[node_id].inputs)
			enqueue_backward(input);
	for (PortId output : module_outputs)
		enqueue_backward(output);

	while (!backward_queue.empty()) {
		PortId consumer = backward_queue.front();
		backward_queue.pop_front();
		for (ConnectionId connection_id : physical_incoming[consumer]) {
			const GraphPort &driver = ports[physical_connections[connection_id].src];
			if (!driver.is_cell_port())
				continue;
			GraphCell &cell = cells[driver.cell];
			if (!cell.is_edge())
				continue;
			if (backward_cells.insert(cell.id).second)
				for (PortId input : cell.inputs)
					enqueue_backward(input);
		}
	}

	for (GraphCell &cell : cells) {
		if (!cell.is_edge())
			continue;
		cell.in_graph = forward_cells.count(cell.id) && backward_cells.count(cell.id);
		if (cell.in_graph)
			edge_cells.push_back(cell.id);
	}

	for (const GraphConnection &connection : physical_connections) {
		if (!port_owner_is_in_graph(connection.src) || !port_owner_is_in_graph(connection.dst))
			continue;
		GraphConnection retained = connection;
		retained.id = GetSize(connections);
		connections.push_back(retained);
	}
}

void CellGraph::build()
{
	log_assert(module != nullptr);
	log_assert(cells.empty() && ports.empty() && connections.empty());
	index_cells();
	index_module_ports();
	build_physical_connections();
	retain_data_paths();
	std::vector<GraphConnection>().swap(physical_connections);
	std::vector<std::vector<ConnectionId>>().swap(physical_incoming);
	std::vector<std::vector<ConnectionId>>().swap(physical_outgoing);
}

static int integer_log2(unsigned long long value)
{
	int result = -1;
	do {
		result++;
		value >>= 1;
	} while (value != 0);
	return result;
}

static int semantic_weight(RTLIL::IdString type)
{
	if (type.in(ID($mux), ID($pmux), ID($bmux), ID($demux), ID($bwmux),
			ID($mul), ID($div), ID($mod), ID($divfloor), ID($modfloor),
			ID($pow), ID($macc), ID($macc_v2), ID($alu)))
		return 8;
	if (type.in(ID($add), ID($sub), ID($shl), ID($shr), ID($sshl), ID($sshr),
			ID($shift), ID($shiftx), ID($lt), ID($le), ID($eq), ID($ne),
			ID($eqx), ID($nex), ID($ge), ID($gt)))
		return 6;
	if (type.in(ID($reduce_and), ID($reduce_or), ID($reduce_xor), ID($reduce_xnor),
			ID($reduce_bool), ID($logic_not), ID($logic_and), ID($logic_or),
			ID($lut), ID($sop), ID($lcu), ID($fa)))
		return 5;
	if (type.in(ID($and), ID($or), ID($xor), ID($xnor)))
		return 4;
	if (type.in(ID($not), ID($neg), ID($pos)))
		return 3;
	if (type.in(ID($buf), ID($slice), ID($concat), ID($barrier)))
		return 1;
	return 4;
}

static std::pair<int, int> boundary_key(const GraphPort &port)
{
	if (port.is_cell_port())
		return std::make_pair(0, port.cell);
	return std::make_pair(1, port.id);
}

static RegionCellSelection select_region_topk(const RegionSelectionContext &context,
		const CellRegion &region, int top_k)
{
	const auto &cells = context.graph.cells;
	const auto &ports = context.graph.ports;
	const auto &connections = context.graph.connections;
	const auto &incoming = context.incoming;
	const auto &outgoing = context.outgoing;
	RegionCellSelection result;
	struct CellScore {
		GraphCellId cell_id;
		int score;
	};

	pool<GraphCellId> region_cells(region.edge_cells.begin(), region.edge_cells.end());
	std::vector<CellScore> scores;
	for (GraphCellId cell_id : region.edge_cells) {
		pool<std::pair<int, int>> unique_fanin, unique_fanout;
		pool<std::pair<int, int>> upstream_boundaries, downstream_boundaries;

		for (PortId input : cells.at(cell_id).inputs)
			for (ConnectionId connection_id : incoming.at(input)) {
				const GraphPort &src = ports.at(connections.at(connection_id).src);
				unique_fanin.insert(boundary_key(src));
			}
		for (PortId output : cells.at(cell_id).outputs)
			for (ConnectionId connection_id : outgoing.at(output)) {
				const GraphPort &dst = ports.at(connections.at(connection_id).dst);
				unique_fanout.insert(boundary_key(dst));
			}

		pool<GraphCellId> seen_upstream, seen_downstream;
		std::deque<GraphCellId> upstream_queue, downstream_queue;
		seen_upstream.insert(cell_id);
		seen_downstream.insert(cell_id);
		upstream_queue.push_back(cell_id);
		downstream_queue.push_back(cell_id);

		while (!upstream_queue.empty()) {
			GraphCellId current = upstream_queue.front();
			upstream_queue.pop_front();
			for (PortId input : cells.at(current).inputs)
				for (ConnectionId connection_id : incoming.at(input)) {
					const GraphPort &src = ports.at(connections.at(connection_id).src);
					if (src.is_cell_port() && region_cells.count(src.cell)) {
						if (seen_upstream.insert(src.cell).second)
							upstream_queue.push_back(src.cell);
					} else {
						upstream_boundaries.insert(boundary_key(src));
						result.upstream_boundaries.insert(boundary_key(src));
					}
				}
		}

		while (!downstream_queue.empty()) {
			GraphCellId current = downstream_queue.front();
			downstream_queue.pop_front();
			for (PortId output : cells.at(current).outputs)
				for (ConnectionId connection_id : outgoing.at(output)) {
					const GraphPort &dst = ports.at(connections.at(connection_id).dst);
					if (dst.is_cell_port() && region_cells.count(dst.cell)) {
						if (seen_downstream.insert(dst.cell).second)
							downstream_queue.push_back(dst.cell);
					} else {
						downstream_boundaries.insert(boundary_key(dst));
						result.downstream_boundaries.insert(boundary_key(dst));
					}
				}
		}

		int data_width = 0;
		for (PortId output : cells.at(cell_id).outputs)
			data_width = std::max(data_width, GetSize(ports.at(output).raw_signal));
		if (data_width == 0)
			for (PortId input : cells.at(cell_id).inputs)
				data_width = std::max(data_width, GetSize(ports.at(input).raw_signal));

		unsigned long long span = (unsigned long long)GetSize(upstream_boundaries) *
				(unsigned long long)GetSize(downstream_boundaries);
		int score = 4 * semantic_weight(cells.at(cell_id).cell->type) +
				2 * integer_log2(1 + GetSize(unique_fanin) + GetSize(unique_fanout)) +
				3 * integer_log2(1 + span) + integer_log2(1 + data_width);
		scores.push_back(CellScore{cell_id, score});
	}

	std::sort(scores.begin(), scores.end(), [&](const CellScore &lhs, const CellScore &rhs) {
		if (lhs.score != rhs.score)
			return lhs.score > rhs.score;
		RTLIL::Cell *lhs_cell = cells.at(lhs.cell_id).cell;
		RTLIL::Cell *rhs_cell = cells.at(rhs.cell_id).cell;
		if (lhs_cell->type.str() != rhs_cell->type.str())
			return lhs_cell->type.str() < rhs_cell->type.str();
		if (lhs_cell->name.str() != rhs_cell->name.str())
			return lhs_cell->name.str() < rhs_cell->name.str();
		return lhs.cell_id < rhs.cell_id;
	});

	int key_count = std::min(top_k, GetSize(scores));
	for (int index = 0; index < key_count; index++) {
		GraphCellId cell_id = scores.at(index).cell_id;
		result.selected_cells.push_back(cell_id);
	}
	std::sort(result.selected_cells.begin(), result.selected_cells.end());
	return result;
}

CellGraphSelection CellGraph::select_objects(const CellGraphFilterOptions &options) const
{
	CellGraphSelection selection;
	std::vector<std::vector<ConnectionId>> incoming(ports.size()), outgoing(ports.size());
	for (const GraphConnection &connection : connections) {
		outgoing.at(connection.src).push_back(connection.id);
		incoming.at(connection.dst).push_back(connection.id);
	}

	for (GraphCellId node_id : nodes)
		if (cells.at(node_id).in_graph)
			selection.selected_cells.insert(node_id);

	if (!options.filtered()) {
		for (GraphCellId cell_id : edge_cells)
			selection.selected_cells.insert(cell_id);
	} else {
		std::vector<std::vector<GraphCellId>> undirected(cells.size());
		for (const GraphConnection &connection : connections) {
			const GraphPort &src = ports.at(connection.src);
			const GraphPort &dst = ports.at(connection.dst);
			if (!src.is_cell_port() || !dst.is_cell_port())
				continue;
			if (cells.at(src.cell).is_node() || cells.at(dst.cell).is_node())
				continue;
			undirected.at(src.cell).push_back(dst.cell);
			undirected.at(dst.cell).push_back(src.cell);
		}

		pool<GraphCellId> visited;
		for (GraphCellId seed : edge_cells) {
			if (!visited.insert(seed).second)
				continue;
			CellRegion region;
			region.id = GetSize(selection.regions);
			std::deque<GraphCellId> queue;
			queue.push_back(seed);
			while (!queue.empty()) {
				GraphCellId cell_id = queue.front();
				queue.pop_front();
				region.edge_cells.push_back(cell_id);
				for (GraphCellId neighbor : undirected.at(cell_id))
					if (visited.insert(neighbor).second)
						queue.push_back(neighbor);
			}
			std::sort(region.edge_cells.begin(), region.edge_cells.end());
			selection.regions.push_back(std::move(region));
		}

		RegionSelectionContext context{*this, incoming, outgoing};
		for (CellRegion &region : selection.regions) {
			RegionCellSelection result = options.region_selector ? options.region_selector(context, region) :
					select_region_topk(context, region, options.top_k);
			pool<GraphCellId> candidates(region.edge_cells.begin(), region.edge_cells.end());
			pool<GraphCellId> selected;
			for (GraphCellId cell_id : result.selected_cells) {
				log_assert(candidates.count(cell_id));
				log_assert(selected.insert(cell_id).second);
				selection.selected_cells.insert(cell_id);
			}
			region.key_cells = std::move(result.selected_cells);
			std::sort(region.key_cells.begin(), region.key_cells.end());
			region.upstream_boundaries = std::move(result.upstream_boundaries);
			region.downstream_boundaries = std::move(result.downstream_boundaries);
		}
	}

	// Preserve the existing DOT boundary set: only module ports incident to
	// retained connections participate in automatic selection.
	for (const GraphConnection &connection : connections) {
		if (!ports.at(connection.src).is_cell_port())
			selection.selected_module_ports.insert(connection.src);
		if (!ports.at(connection.dst).is_cell_port())
			selection.selected_module_ports.insert(connection.dst);
	}
	return selection;
}

CellGraphView CellGraph::create_view(const CellGraphSelection &selection) const
{
	CellGraphView view;
	view.visible_cells = selection.selected_cells;
	view.visible_module_ports = selection.selected_module_ports;
	view.regions = selection.regions;
	for (GraphCellId cell_id : view.visible_cells) {
		log_assert(cell_id >= 0 && cell_id < GetSize(cells));
		log_assert(cells.at(cell_id).in_graph);
		if (!cells.at(cell_id).is_node())
			view.visible_edge_cell_count++;
	}
	for (PortId port_id : view.visible_module_ports) {
		log_assert(port_id >= 0 && port_id < GetSize(ports));
		log_assert(!ports.at(port_id).is_cell_port());
	}
	view.hidden_edge_cell_count = GetSize(edge_cells) - view.visible_edge_cell_count;

	auto port_is_visible = [&](PortId port_id) {
		const GraphPort &port = ports.at(port_id);
		return port.is_cell_port() ? view.visible_cells.count(port.cell) :
				view.visible_module_ports.count(port.id);
	};

	if (view.hidden_edge_cell_count == 0) {
		for (const GraphConnection &connection : connections) {
			if (!port_is_visible(connection.src) || !port_is_visible(connection.dst))
				continue;
			ViewConnection item;
			item.src = connection.src;
			item.dst = connection.dst;
			item.bit_count = connection.bit_count;
			view.connections.push_back(std::move(item));
		}
	} else {
		std::vector<std::vector<ConnectionId>> outgoing(ports.size());
		for (const GraphConnection &connection : connections)
			outgoing.at(connection.src).push_back(connection.id);

		std::map<std::pair<PortId, PortId>, ViewConnection> folded;
		std::vector<PortId> source_ports;
		for (GraphCellId cell_id : view.visible_cells)
			for (PortId output : cells.at(cell_id).outputs)
				source_ports.push_back(output);
		for (PortId module_input : module_inputs)
			if (view.visible_module_ports.count(module_input))
				source_ports.push_back(module_input);
		std::sort(source_ports.begin(), source_ports.end());

		for (PortId source : source_ports) {
			std::deque<PortId> queue;
			dict<PortId, pool<GraphCellId>> hidden_at_port;
			pool<PortId> queued;
			queue.push_back(source);
			queued.insert(source);
			while (!queue.empty()) {
				PortId current_port = queue.front();
				queue.pop_front();
				queued.erase(current_port);
				const pool<GraphCellId> current_hidden = hidden_at_port[current_port];
				for (ConnectionId connection_id : outgoing.at(current_port)) {
					const GraphConnection &connection = connections.at(connection_id);
					const GraphPort &dst = ports.at(connection.dst);
					if (port_is_visible(dst.id)) {
						std::pair<PortId, PortId> key(source, connection.dst);
						ViewConnection &item = folded[key];
						item.src = source;
						item.dst = connection.dst;
						if (current_hidden.empty()) {
							item.bit_count += connection.bit_count;
						} else {
							item.collapsed = true;
							pool<GraphCellId> merged(item.hidden_cells.begin(), item.hidden_cells.end());
							for (GraphCellId hidden_id : current_hidden)
								merged.insert(hidden_id);
							item.hidden_cells.assign(merged.begin(), merged.end());
							std::sort(item.hidden_cells.begin(), item.hidden_cells.end());
						}
						continue;
					}

					// Only hidden edge cells are traversable. An omitted state
					// node remains a boundary, just like an omitted module port.
					if (!dst.is_cell_port() || !cells.at(dst.cell).is_edge())
						continue;
					GraphCellId hidden_id = dst.cell;
					pool<GraphCellId> next_hidden = current_hidden;
					next_hidden.insert(hidden_id);
					for (PortId output : cells.at(hidden_id).outputs) {
						bool changed = false;
						pool<GraphCellId> &known = hidden_at_port[output];
						for (GraphCellId item : next_hidden)
							changed |= known.insert(item).second;
						if (changed && queued.insert(output).second)
							queue.push_back(output);
					}
				}
			}
		}

		for (auto &item : folded)
			view.connections.push_back(std::move(item.second));
	}
	return view;
}

static std::string dot_escape(const std::string &text)
{
	std::string result;
	result.reserve(text.size());
	for (char ch : text) {
		if (ch == '\\' || ch == '"')
			result += '\\';
		if (ch == '\n')
			result += "\\n";
		else if (ch != '\r')
			result += ch;
	}
	return result;
}

static std::string display_id(RTLIL::IdString id)
{
	return RTLIL::unescape_id(id.str());
}

static std::string dot_cell_id(int module_index, GraphCellId cell_id)
{
	return stringf("m%d_c%d", module_index, cell_id);
}

static std::string dot_port_id(int module_index, PortId port_id)
{
	return stringf("m%d_p%d", module_index, port_id);
}

void CellGraph::dump_dot(std::ostream &f, int module_index, const CellGraphView &view) const
{
	std::string prefix = stringf("m%d", module_index);
	f << "  subgraph cluster_" << prefix << " {\n";
	f << "    label=\"" << dot_escape(display_id(module->name)) << "\";\n";
	f << "    color=\"#bdbdbd\";\n";

	for (const GraphCell &graph_cell : cells) {
		if (!view.visible_cells.count(graph_cell.id))
			continue;
		std::string label = display_id(graph_cell.cell->name) + "\n" +
				display_id(graph_cell.cell->type) + "\n" +
				(graph_cell.is_node() ? node_type_name(graph_cell.node_type) : "edge cell");
		const char *fill = graph_cell.is_node() ? "#6baed6" : "#fdae6b";
		f << "    " << dot_cell_id(module_index, graph_cell.id)
				<< " [shape=box, style=filled, fillcolor=\"" << fill
				<< "\", label=\"" << dot_escape(label) << "\"];\n";
	}

	std::vector<PortId> sorted_module_ports(view.visible_module_ports.begin(), view.visible_module_ports.end());
	std::sort(sorted_module_ports.begin(), sorted_module_ports.end());
	for (PortId port_id : sorted_module_ports) {
		const GraphPort &port = ports[port_id];
		const char *direction = port.direction == RTLIL::PortDir::PD_INOUT ? "inout" :
				(port.is_input() ? "input" : "output");
		std::string label = display_id(port.name) + "\nmodule " + direction;
		f << "    " << dot_port_id(module_index, port.id)
				<< " [shape=oval, style=filled, fillcolor=\"#d9d9d9\", label=\""
				<< dot_escape(label) << "\"];\n";
	}

	for (const ViewConnection &connection : view.connections) {
		const GraphPort &src = ports[connection.src];
		const GraphPort &dst = ports[connection.dst];
		std::string src_id = src.is_cell_port() ? dot_cell_id(module_index, src.cell) :
				dot_port_id(module_index, src.id);
		std::string dst_id = dst.is_cell_port() ? dot_cell_id(module_index, dst.cell) :
				dot_port_id(module_index, dst.id);
		std::string label = display_id(src.name) + " -> " + display_id(dst.name);
		if (connection.collapsed)
			label += stringf(" (collapsed %d cells)", GetSize(connection.hidden_cells));
		else if (connection.bit_count > 1)
			label += stringf(" (%d bits)", connection.bit_count);
		f << "    " << src_id << " -> " << dst_id << " [";
		if (connection.collapsed)
			f << "style=dashed, color=\"#7f7f7f\", ";
		f << "label=\"" << dot_escape(label) << "\"];\n";
	}
	f << "  }\n";
}

static CellGraphBuildOptions parse_node_types(const std::string &text)
{
	CellGraphBuildOptions options;
	if (text == "all")
		return options;
	options.register_role = options.memory_role = options.fsm_role = GraphCellRole::EXCLUDED;
	if (text == "none")
		return options;

	size_t begin = 0;
	while (true) {
		size_t end = text.find(',', begin);
		std::string type = text.substr(begin, end == std::string::npos ? end : end - begin);
		if (type == "register")
			options.register_role = GraphCellRole::NODE;
		else if (type == "memory" || type == "mem")
			options.memory_role = GraphCellRole::NODE;
		else if (type == "fsm")
			options.fsm_role = GraphCellRole::NODE;
		else
			log_cmd_error("Invalid node type `%s': expected register, memory (or mem), fsm; "
					"use all or none for the entire set.\n", type);
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return options;
}

struct CellFilterPass : public Pass {
	CellFilterPass() : Pass("cellfilter", "build a state-cell data-path graph") {}

	void help() override
	{
		log("\n");
		log("    cellfilter [options] [selection]\n");
		log("\n");
		log("Build a port-level data-path graph for all cells in the selected modules.\n");
		log("Registers, collected memories and FSM cells are graph nodes by default.\n");
		log("Select node types with -nodes. Unselected state cells are excluded and\n");
		log("remain traversal barriers; they are not converted to edge cells. Combinational\n");
		log("cells on paths between nodes or module ports are retained as edge cells.\n");
		log("\n");
		log("    -dot <filename>\n");
		log("        write the graph in Graphviz DOT format\n");
		log("\n");
		log("    -nodes <list>\n");
		log("        comma-separated node types: register,memory,fsm (default: all)\n");
		log("        memory also accepts mem; all and none select the entire or empty set\n");
		log("        repeated -nodes options replace the previous list\n");
		log("\n");
		log("    -topk <integer>\n");
		log("        for each combinational region, retain at most this many key\n");
		log("        edge cells in the DOT graph and collapse hidden paths\n");
		log("\n");
		log("This pass must be run after memory_collect.\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		std::string dot_filename;
		CellGraphBuildOptions build_options;
		CellGraphFilterOptions filter_options;
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-nodes") {
				if (argidx + 1 >= args.size())
					log_cmd_error("Option -nodes requires a node type list.\n");
				build_options = parse_node_types(args[++argidx]);
				continue;
			}
			if (args[argidx] == "-dot" && argidx + 1 < args.size()) {
				dot_filename = args[++argidx];
				continue;
			}
			if (args[argidx] == "-topk") {
				if (argidx + 1 >= args.size())
					log_cmd_error("Option -topk requires a positive integer.\n");
				const char *text = args[++argidx].c_str();
				char *endptr = nullptr;
				errno = 0;
				long value = strtol(text, &endptr, 10);
				if (errno != 0 || endptr == text || *endptr != '\0' || value <= 0 || value > INT_MAX)
					log_cmd_error("Option -topk requires a positive integer.\n");
				filter_options.top_k = int(value);
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);
		log_header(design, "Executing CELLFILTER pass (build data-path graph).\n");

		std::ofstream dot_file;
		if (!dot_filename.empty()) {
			rewrite_filename(dot_filename);
			dot_file.open(dot_filename);
			if (!dot_file)
				log_cmd_error("Can't open DOT file `%s' for writing.\n", dot_filename);
			yosys_output_files.insert(dot_filename);
			dot_file << "digraph cellfilter {\n";
			dot_file << "  rankdir=LR;\n";
			dot_file << "  graph [fontname=\"Helvetica\"];\n";
			dot_file << "  node [fontname=\"Helvetica\"];\n";
			dot_file << "  edge [fontname=\"Helvetica\", fontsize=9];\n";
		}

		int module_index = 0;
		for (RTLIL::Module *module : design->selected_modules()) {
			if (module->has_memories())
				log_cmd_error("Module %s still contains RTLIL memories; run memory_collect before cellfilter.\n", module);
			for (RTLIL::Cell *cell : module->cells())
				if (is_uncollected_memory_port(cell))
					log_cmd_error("Module %s contains uncollected memory port cell %s; "
							"run memory_collect before cellfilter.\n", module, cell);

			CellGraph graph(module, build_options);
			graph.build();
			log("Built data-path graph for %s: %d nodes, %d edge cells, %d port connections.\n",
					module, GetSize(graph.nodes), GetSize(graph.edge_cells), GetSize(graph.connections));
			if (dot_file) {
				CellGraphSelection selection = graph.select_objects(filter_options);
				CellGraphView view = graph.create_view(selection);
				if (filter_options.filtered())
					log("Filtered graph view for %s: %d regions, %d visible edge cells, "
							"%d hidden edge cells, %d view connections.\n", module,
							GetSize(view.regions), view.visible_edge_cell_count,
							view.hidden_edge_cell_count, GetSize(view.connections));
				graph.dump_dot(dot_file, module_index, view);
			}
			module_index++;
		}

		if (dot_file) {
			dot_file << "}\n";
			dot_file.close();
			log("Wrote data-path graph to %s.\n", dot_filename);
		}
	}
} CellFilterPass;

PRIVATE_NAMESPACE_END
