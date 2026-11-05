#include "cellgraph.h"
#include "selection.h"

#include <algorithm>
#include <deque>
#include <ostream>
#include <tuple>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {

NodeType get_node_type(RTLIL::Cell *cell)
{
	if (cell->type == ID($fsm))
		return NodeType::FSM;
	if (cell->is_mem_cell())
		return NodeType::MEMORY;
	if (cell->is_builtin_ff())
		return NodeType::REGISTER;
	return NodeType::NONE;
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

GraphCellId CellGraph::add_cell(RTLIL::Cell *cell)
{
	log_assert(cell != nullptr);
	log_assert(!cell_ids.count(cell));
	GraphCellId id = GetSize(cells);
	NodeType node_type = get_node_type(cell);
	GraphCellRole role = build_options.role_for(node_type);
	GraphCell record;
	record.id = id;
	record.cell = cell;
	record.name = cell->name;
	record.type = cell->type;
	record.parameters = cell->parameters;
	record.attributes = cell->attributes;
	record.node_type = node_type;
	record.role = role;
	record.in_graph = role == GraphCellRole::NODE;
	record.retention_reason = role == GraphCellRole::NODE ? GraphRetentionReason::STATE_NODE :
			(role == GraphCellRole::EXCLUDED ? GraphRetentionReason::EXCLUDED_TYPE : GraphRetentionReason::NOT_ON_DATA_PATH);
	cells.push_back(std::move(record));
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
		if (cell.in_graph) {
			cell.retention_reason = GraphRetentionReason::DATA_PATH;
			edge_cells.push_back(cell.id);
		}
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
	module_info.name = module->name;
	module_info.parameters = module->parameter_default_values;
	module_info.attributes = module->attributes;
	index_cells();
	index_module_ports();
	build_physical_connections();
	retain_data_paths();
	index_connections();
	std::vector<GraphConnection>().swap(physical_connections);
	std::vector<std::vector<ConnectionId>>().swap(physical_incoming);
	std::vector<std::vector<ConnectionId>>().swap(physical_outgoing);
}

void CellGraph::index_connections()
{
	incoming.assign(ports.size(), {});
	outgoing.assign(ports.size(), {});
	for (const GraphConnection &connection : connections) {
		outgoing.at(connection.src).push_back(connection.id);
		incoming.at(connection.dst).push_back(connection.id);
	}
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
	f << "    label=\"" << dot_escape(display_id(module_info.name)) << "\";\n";
	f << "    color=\"#bdbdbd\";\n";

	for (const GraphCell &graph_cell : cells) {
		if (!view.visible_cells.count(graph_cell.id))
			continue;
		std::string label = display_id(graph_cell.name) + "\n" +
				display_id(graph_cell.type) + "\n" +
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


} // namespace CellFuzz
YOSYS_NAMESPACE_END
