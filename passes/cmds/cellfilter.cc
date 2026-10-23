#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/rtlil.h"
#include "kernel/log.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

enum class NodeType { NONE, REGISTER, MEMORY, FSM };

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
	bool in_graph = false;
	std::string src_line;
	std::vector<PortId> inputs;
	std::vector<PortId> outputs;

	bool is_node() const
	{
		return node_type != NodeType::NONE;
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

struct CellGraph {
	RTLIL::Module *module = nullptr;
	SigMap sigmap;

	std::vector<GraphCell> cells;
	std::vector<GraphPort> ports;
	std::vector<GraphConnection> connections;
	std::vector<GraphCellId> nodes;
	std::vector<GraphCellId> edge_cells;
	dict<RTLIL::Cell *, GraphCellId> cell_ids;
	std::vector<PortId> module_inputs;
	std::vector<PortId> module_outputs;

	explicit CellGraph(RTLIL::Module *module) : module(module), sigmap(module) {}

	void build();
	void dump_dot(std::ostream &f, int module_index) const;

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
	log("cellfilter: module %s: adding cell %s (%s) as %s\n",
			module->name.c_str(), cell->get_string_attribute(ID::src).c_str(), cell->type.c_str(),
			node_type_name(node_type));
	cells.push_back(GraphCell{id, cell, node_type, node_type != NodeType::NONE, {}, {}});
	cell_ids[cell] = id;
	if (node_type != NodeType::NONE)
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
			if (cell.is_node())
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
			if (cell.is_node())
				continue;
			if (backward_cells.insert(cell.id).second)
				for (PortId input : cell.inputs)
					enqueue_backward(input);
		}
	}

	for (GraphCell &cell : cells) {
		if (cell.is_node())
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

static std::string dot_cell_id(int module_index, GraphCellId cell_id)
{
	return stringf("m%d_c%d", module_index, cell_id);
}

static std::string dot_port_id(int module_index, PortId port_id)
{
	return stringf("m%d_p%d", module_index, port_id);
}

void CellGraph::dump_dot(std::ostream &f, int module_index) const
{
	std::string prefix = stringf("m%d", module_index);
	f << "  subgraph cluster_" << prefix << " {\n";
	f << "    label=\"" << dot_escape(module->name.unescape()) << "\";\n";
	f << "    color=\"#bdbdbd\";\n";

	for (const GraphCell &graph_cell : cells) {
		if (!graph_cell.in_graph)
			continue;
		std::string label = graph_cell.cell->name.unescape() + "\n" +
				graph_cell.cell->type.unescape() + "\n" + node_type_name(graph_cell.node_type);
		const char *fill = graph_cell.is_node() ? "#6baed6" : "#fdae6b";
		f << "    " << dot_cell_id(module_index, graph_cell.id)
				<< " [shape=box, style=filled, fillcolor=\"" << fill
				<< "\", label=\"" << dot_escape(label) << "\"];\n";
	}

	pool<PortId> used_module_ports;
	for (const GraphConnection &connection : connections) {
		if (!ports[connection.src].is_cell_port())
			used_module_ports.insert(connection.src);
		if (!ports[connection.dst].is_cell_port())
			used_module_ports.insert(connection.dst);
	}
	std::vector<PortId> sorted_module_ports(used_module_ports.begin(), used_module_ports.end());
	std::sort(sorted_module_ports.begin(), sorted_module_ports.end());
	for (PortId port_id : sorted_module_ports) {
		const GraphPort &port = ports[port_id];
		const char *direction = port.direction == RTLIL::PortDir::PD_INOUT ? "inout" :
				(port.is_input() ? "input" : "output");
		std::string label = port.name.unescape() + "\nmodule " + direction;
		f << "    " << dot_port_id(module_index, port.id)
				<< " [shape=oval, style=filled, fillcolor=\"#d9d9d9\", label=\""
				<< dot_escape(label) << "\"];\n";
	}

	for (const GraphConnection &connection : connections) {
		const GraphPort &src = ports[connection.src];
		const GraphPort &dst = ports[connection.dst];
		std::string src_id = src.is_cell_port() ? dot_cell_id(module_index, src.cell) :
				dot_port_id(module_index, src.id);
		std::string dst_id = dst.is_cell_port() ? dot_cell_id(module_index, dst.cell) :
				dot_port_id(module_index, dst.id);
		std::string label = src.name.unescape() + " -> " + dst.name.unescape();
		if (connection.bit_count > 1)
			label += stringf(" (%d bits)", connection.bit_count);
		f << "    " << src_id << " -> " << dst_id << " [label=\""
				<< dot_escape(label) << "\"];\n";
	}
	f << "  }\n";
}

struct CellFilterPass : public Pass {
	CellFilterPass() : Pass("cellfilter", "build a state-cell data-path graph") {}

	void help() override
	{
		log("\n");
		log("    cellfilter [options] [selection]\n");
		log("\n");
		log("Build a port-level data-path graph for all cells in the selected modules.\n");
		log("Registers, collected memories and FSM cells are graph nodes. Combinational\n");
		log("cells on paths between nodes or module ports are retained as edge cells.\n");
		log("\n");
		log("    -dot <filename>\n");
		log("        write the graph in Graphviz DOT format\n");
		log("\n");
		log("This pass must be run after memory_collect.\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		std::string dot_filename;
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-dot" && argidx + 1 < args.size()) {
				dot_filename = args[++argidx];
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

			CellGraph graph(module);
			graph.build();
			log("Built data-path graph for %s: %d nodes, %d edge cells, %d port connections.\n",
					module, GetSize(graph.nodes), GetSize(graph.edge_cells), GetSize(graph.connections));
			if (dot_file)
				graph.dump_dot(dot_file, module_index);
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
