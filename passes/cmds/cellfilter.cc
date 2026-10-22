#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/rtlil.h"
#include "kernel/log.h"
#include "kernel/celledges.h"

#include <tuple>
#include <utility>
#include <vector>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Register, collected memory and FSM cells are state boundaries in the data
// path graph. In particular, the individual $memrd/$memwr/$meminit cells of
// an uncollected memory are not separate state elements.
enum class NodeType { NONE, REGISTER, MEMORY, FSM };

using NodeId = int;
using EdgeId = int;

static constexpr NodeId INVALID_NODE_ID = -1;
static constexpr EdgeId INVALID_EDGE_ID = -1;

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

static bool is_node_cell(RTLIL::Cell *cell)
{
	return get_node_type(cell) != NodeType::NONE;
}

static bool is_uncollected_memory_port(RTLIL::Cell *cell)
{
	return cell->type.in(ID($memrd), ID($memrd_v2), ID($memwr), ID($memwr_v2),
			ID($meminit), ID($meminit_v2));
}

// One exact cell port bit. raw_bit preserves the connection as written on
// the cell, while canonical_bit is the SigMap result used to match aliases
// created by module-level connections.
struct CellPin {
	RTLIL::Cell *cell = nullptr;
	RTLIL::IdString port;
	int port_offset = -1;
	RTLIL::PortDir direction = RTLIL::PortDir::PD_UNKNOWN;
	RTLIL::SigBit raw_bit;
	RTLIL::SigBit canonical_bit;

	bool is_input() const
	{
		return direction == RTLIL::PortDir::PD_INPUT || direction == RTLIL::PortDir::PD_INOUT;
	}

	bool is_output() const
	{
		return direction == RTLIL::PortDir::PD_OUTPUT || direction == RTLIL::PortDir::PD_INOUT;
	}
};

// One exact bit of a module port. For a module input this is a graph source;
// for a module output this is a graph sink. An inout port can be both.
struct ModulePortPin {
	RTLIL::Wire *wire = nullptr;
	int port_offset = -1;
	RTLIL::PortDir direction = RTLIL::PortDir::PD_UNKNOWN;
	RTLIL::SigBit raw_bit;
	RTLIL::SigBit canonical_bit;

	bool is_input() const
	{
		return direction == RTLIL::PortDir::PD_INPUT || direction == RTLIL::PortDir::PD_INOUT;
	}

	bool is_output() const
	{
		return direction == RTLIL::PortDir::PD_OUTPUT || direction == RTLIL::PortDir::PD_INOUT;
	}
};

struct Node {
	NodeId id = INVALID_NODE_ID;
	RTLIL::Cell *cell = nullptr;
	NodeType type = NodeType::NONE;

	// A pin appears in both lists when its RTLIL direction is inout.
	std::vector<CellPin> inputs;
	std::vector<CellPin> outputs;
};

// One traversal through a non-node cell. Keeping the entry and exit pin on
// every hop is what distinguishes the participating bits from the cell's
// unrelated ports and bits.
struct PathCell {
	CellPin input;
	CellPin output;
};

enum class EndpointType { NONE, NODE_PIN, MODULE_PORT };

struct EdgeEndpoint {
	EndpointType type = EndpointType::NONE;
	NodeId node = INVALID_NODE_ID;
	CellPin cell_pin;
	ModulePortPin module_port;

	static EdgeEndpoint from_node(NodeId node, const CellPin &pin)
	{
		EdgeEndpoint endpoint;
		endpoint.type = EndpointType::NODE_PIN;
		endpoint.node = node;
		endpoint.cell_pin = pin;
		return endpoint;
	}

	static EdgeEndpoint from_module_port(const ModulePortPin &pin)
	{
		EdgeEndpoint endpoint;
		endpoint.type = EndpointType::MODULE_PORT;
		endpoint.module_port = pin;
		return endpoint;
	}

	bool is_source() const
	{
		if (type == EndpointType::NODE_PIN)
			return cell_pin.is_output();
		if (type == EndpointType::MODULE_PORT)
			return module_port.is_input();
		return false;
	}

	bool is_sink() const
	{
		if (type == EndpointType::NODE_PIN)
			return cell_pin.is_input();
		if (type == EndpointType::MODULE_PORT)
			return module_port.is_output();
		return false;
	}

	RTLIL::SigBit canonical_bit() const
	{
		if (type == EndpointType::NODE_PIN)
			return cell_pin.canonical_bit;
		if (type == EndpointType::MODULE_PORT)
			return module_port.canonical_bit;
		return RTLIL::SigBit();
	}
};

// A directed bit-level path between state cells and/or module ports. Multiple
// Edge objects may connect the same endpoints. path is ordered from src to dst
// and is empty when the endpoint bits are connected directly.
struct Edge {
	EdgeId id = INVALID_EDGE_ID;
	EdgeEndpoint src;
	EdgeEndpoint dst;
	std::vector<PathCell> path;
};

using CellPinKey = std::tuple<RTLIL::Cell *, RTLIL::IdString, int>;

struct CellGraph {
	RTLIL::Module *module = nullptr;
	SigMap sigmap;

	// Vectors own the stable graph IDs. The dictionaries are lookup indexes,
	// not a second copy of graph objects.
	std::vector<Node> nodes;
	std::vector<Edge> edges;
	dict<RTLIL::Cell *, NodeId> node_by_cell;
	std::vector<std::vector<EdgeId>> incoming_edges;
	std::vector<std::vector<EdgeId>> outgoing_edges;
	dict<RTLIL::SigBit, std::vector<EdgeId>> module_port_incoming_edges;
	dict<RTLIL::SigBit, std::vector<EdgeId>> module_port_outgoing_edges;

	// Connectivity index for all cell pins, keyed by SigMap-normalized bits.
	// It is retained in the graph because path discovery repeatedly queries it.
	dict<RTLIL::SigBit, std::vector<CellPin>> drivers;
	dict<RTLIL::SigBit, std::vector<CellPin>> consumers;
	dict<CellPinKey, CellPin> cell_pins;
	dict<CellPinKey, std::vector<CellPin>> cell_arcs;
	std::vector<ModulePortPin> module_inputs;
	dict<RTLIL::SigBit, std::vector<ModulePortPin>> module_outputs;
	int cycle_cuts = 0;

	explicit CellGraph(RTLIL::Module *module = nullptr) : module(module), sigmap(module) {}

	void build();

	NodeId add_node(RTLIL::Cell *cell)
	{
		log_assert(cell != nullptr);
		log_assert(is_node_cell(cell));
		NodeType type = get_node_type(cell);
		log_assert(type != NodeType::NONE);
		log_assert(!node_by_cell.count(cell));

		NodeId id = GetSize(nodes);
		nodes.push_back(Node{id, cell, type, {}, {}});
		node_by_cell[cell] = id;
		incoming_edges.emplace_back();
		outgoing_edges.emplace_back();
		return id;
	}

	EdgeId add_edge(Edge edge)
	{
		log_assert(edge.src.is_source());
		log_assert(edge.dst.is_sink());
		validate_endpoint(edge.src);
		validate_endpoint(edge.dst);

		RTLIL::SigBit previous_bit = edge.src.canonical_bit();
		for (const PathCell &hop : edge.path) {
			log_assert(hop.input.cell != nullptr);
			log_assert(hop.input.cell == hop.output.cell);
			log_assert(!node_by_cell.count(hop.input.cell));
			log_assert(hop.input.is_input());
			log_assert(hop.output.is_output());
			log_assert(previous_bit == hop.input.canonical_bit);
			previous_bit = hop.output.canonical_bit;
		}
		log_assert(previous_bit == edge.dst.canonical_bit());

		EdgeId id = GetSize(edges);
		edge.id = id;
		edges.push_back(std::move(edge));

		const Edge &stored_edge = edges.back();
		if (stored_edge.src.type == EndpointType::NODE_PIN)
			outgoing_edges[stored_edge.src.node].push_back(id);
		else
			module_port_outgoing_edges[stored_edge.src.module_port.raw_bit].push_back(id);
		if (stored_edge.dst.type == EndpointType::NODE_PIN)
			incoming_edges[stored_edge.dst.node].push_back(id);
		else
			module_port_incoming_edges[stored_edge.dst.module_port.raw_bit].push_back(id);
		return id;
	}

	const CellPin &get_cell_pin(RTLIL::Cell *cell, RTLIL::IdString port, int offset) const
	{
		return cell_pins.at(CellPinKey(cell, port, offset));
	}

	void add_cell_arc(RTLIL::Cell *cell, RTLIL::IdString input_port, int input_offset,
			RTLIL::IdString output_port, int output_offset)
	{
		const CellPin &input = get_cell_pin(cell, input_port, input_offset);
		const CellPin &output = get_cell_pin(cell, output_port, output_offset);
		log_assert(input.is_input());
		log_assert(output.is_output());
		cell_arcs[CellPinKey(cell, input_port, input_offset)].push_back(output);
	}

private:
	void validate_endpoint(const EdgeEndpoint &endpoint) const
	{
		if (endpoint.type == EndpointType::NODE_PIN) {
			log_assert(endpoint.node >= 0 && endpoint.node < GetSize(nodes));
			log_assert(endpoint.cell_pin.cell == nodes[endpoint.node].cell);
			return;
		}
		log_assert(endpoint.type == EndpointType::MODULE_PORT);
		log_assert(endpoint.module_port.wire != nullptr);
		log_assert(endpoint.module_port.wire->module == module);
	}

	void index_cells();
	void index_module_ports();
	void build_cell_arcs();
	void add_fallback_cell_arcs(RTLIL::Cell *cell);
	void walk_signal(const EdgeEndpoint &source, RTLIL::SigBit signal,
			std::vector<PathCell> &path, pool<RTLIL::SigBit> &active_signals);
};

struct CellArcCollector : public AbstractCellEdgesDatabase {
	CellGraph &graph;

	explicit CellArcCollector(CellGraph &graph) : graph(graph) {}

	void add_edge(RTLIL::Cell *cell, RTLIL::IdString from_port, int from_bit,
			RTLIL::IdString to_port, int to_bit, int) override
	{
		graph.add_cell_arc(cell, from_port, from_bit, to_port, to_bit);
	}
};

void CellGraph::index_cells()
{
	for (RTLIL::Cell *cell : module->cells()) {
		NodeId node_id = INVALID_NODE_ID;
		if (is_node_cell(cell))
			node_id = add_node(cell);

		for (const auto &connection : cell->connections()) {
			RTLIL::IdString port = connection.first;
			RTLIL::PortDir direction = cell->port_dir(port);
			for (int offset = 0; offset < GetSize(connection.second); offset++) {
				RTLIL::SigBit raw_bit = connection.second[offset];
				CellPin pin{cell, port, offset, direction, raw_bit, sigmap(raw_bit)};
				cell_pins[CellPinKey(cell, port, offset)] = pin;

				if (pin.canonical_bit.wire != nullptr) {
					if (pin.is_output())
						drivers[pin.canonical_bit].push_back(pin);
					if (pin.is_input())
						consumers[pin.canonical_bit].push_back(pin);
				}

				if (node_id != INVALID_NODE_ID) {
					if (pin.is_input())
						nodes[node_id].inputs.push_back(pin);
					if (pin.is_output())
						nodes[node_id].outputs.push_back(pin);
				}
			}
		}
	}
}

void CellGraph::index_module_ports()
{
	for (RTLIL::IdString port_name : module->ports) {
		RTLIL::Wire *wire = module->wire(port_name);
		log_assert(wire != nullptr);
		RTLIL::PortDir direction = RTLIL::PortDir(
				(wire->port_input ? RTLIL::PortDir::PD_INPUT : RTLIL::PortDir::PD_UNKNOWN) |
				(wire->port_output ? RTLIL::PortDir::PD_OUTPUT : RTLIL::PortDir::PD_UNKNOWN));

		for (int offset = 0; offset < wire->width; offset++) {
			RTLIL::SigBit raw_bit(wire, offset);
			ModulePortPin pin{wire, offset, direction, raw_bit, sigmap(raw_bit)};
			if (pin.canonical_bit.wire == nullptr)
				continue;
			if (pin.is_input())
				module_inputs.push_back(pin);
			if (pin.is_output())
				module_outputs[pin.canonical_bit].push_back(pin);
		}
	}
}

void CellGraph::add_fallback_cell_arcs(RTLIL::Cell *cell)
{
	std::vector<CellPin> inputs, outputs;
	for (const auto &connection : cell->connections())
		for (int offset = 0; offset < GetSize(connection.second); offset++) {
			const CellPin &pin = get_cell_pin(cell, connection.first, offset);
			if (pin.is_input())
				inputs.push_back(pin);
			if (pin.is_output())
				outputs.push_back(pin);
		}

	for (const CellPin &input : inputs)
		for (const CellPin &output : outputs)
			cell_arcs[CellPinKey(cell, input.port, input.port_offset)].push_back(output);
}

void CellGraph::build_cell_arcs()
{
	CellArcCollector collector(*this);
	for (RTLIL::Cell *cell : module->cells()) {
		if (is_node_cell(cell))
			continue;
		if (!collector.add_edges_from_cell(cell))
			add_fallback_cell_arcs(cell);
	}
}

void CellGraph::walk_signal(const EdgeEndpoint &source, RTLIL::SigBit signal,
		std::vector<PathCell> &path, pool<RTLIL::SigBit> &active_signals)
{
	// Constants are values rather than nets. Matching all S0 or S1 pins would
	// incorrectly create connections between otherwise unrelated logic.
	if (signal.wire == nullptr)
		return;
	if (active_signals.count(signal)) {
		cycle_cuts++;
		return;
	}
	active_signals.insert(signal);

	if (module_outputs.count(signal))
		for (const ModulePortPin &output : module_outputs.at(signal))
			add_edge(Edge{INVALID_EDGE_ID, source, EdgeEndpoint::from_module_port(output), path});

	if (consumers.count(signal)) {
		for (const CellPin &input : consumers.at(signal)) {
			if (node_by_cell.count(input.cell)) {
				NodeId dst_node = node_by_cell.at(input.cell);
				add_edge(Edge{INVALID_EDGE_ID, source, EdgeEndpoint::from_node(dst_node, input), path});
				continue;
			}

			CellPinKey input_key(input.cell, input.port, input.port_offset);
			if (!cell_arcs.count(input_key))
				continue;
			for (const CellPin &output : cell_arcs.at(input_key)) {
				path.push_back(PathCell{input, output});
				walk_signal(source, output.canonical_bit, path, active_signals);
				path.pop_back();
			}
		}
	}

	active_signals.erase(signal);
}

void CellGraph::build()
{
	log_assert(module != nullptr);
	log_assert(nodes.empty() && edges.empty());
	index_cells();
	index_module_ports();
	build_cell_arcs();

	std::vector<PathCell> path;
	pool<RTLIL::SigBit> active_signals;
	for (const Node &node : nodes)
		for (const CellPin &output : node.outputs)
			walk_signal(EdgeEndpoint::from_node(node.id, output), output.canonical_bit, path, active_signals);
	for (const ModulePortPin &input : module_inputs)
		walk_signal(EdgeEndpoint::from_module_port(input), input.canonical_bit, path, active_signals);
	log_assert(path.empty());
	log_assert(active_signals.empty());
}

// ---------------------------------------------------------------------------
// CellFilter pass
// ---------------------------------------------------------------------------

struct CellFilterPass : public Pass {
	CellFilterPass() : Pass("cellfilter", "build a state-cell data-path graph") {}

	void help() override
	{
		log("\n");
		log("    cellfilter [selection]\n");
		log("\n");
		log("Build a bit-level data-path graph for all cells in the selected modules.\n");
		log("Registers, collected memories and FSM cells are graph nodes. The cells and\n");
		log("module-level connections between those nodes are represented as paths.\n");
		log("Module input and output bits are graph boundary endpoints.\n");
		log("\n");
		log("This pass must be run after memory_collect.\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		extra_args(args, 1, design);
		log_header(design, "Executing CELLFILTER pass (build data-path graph).\n");

		for (RTLIL::Module *module : design->selected_modules()) {
			if (module->has_memories())
				log_cmd_error("Module %s still contains RTLIL memories; run memory_collect before cellfilter.\n", module);
			for (RTLIL::Cell *cell : module->cells())
				if (is_uncollected_memory_port(cell))
					log_cmd_error("Module %s contains uncollected memory port cell %s; "
							"run memory_collect before cellfilter.\n", module, cell);

			CellGraph graph(module);
			graph.build();
			log("Built data-path graph for %s: %d nodes, %d edges, %d cycle cuts.\n",
					module, GetSize(graph.nodes), GetSize(graph.edges), graph.cycle_cuts);
		}
	}
} CellFilterPass;

PRIVATE_NAMESPACE_END
