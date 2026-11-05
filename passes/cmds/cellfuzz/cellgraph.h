#ifndef YOSYS_CELLFUZZ_CELLGRAPH_H
#define YOSYS_CELLFUZZ_CELLGRAPH_H

#include "kernel/sigtools.h"

#include <iosfwd>
#include <vector>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {

enum class NodeType { NONE, REGISTER, MEMORY, FSM };
enum class GraphCellRole { EXCLUDED, NODE, EDGE };

NodeType get_node_type(RTLIL::Cell *cell);

// Describes construction, not consumer selection: a retained DATA_PATH cell
// may still be hidden by top-k or by a caller's explicit selection.
enum class GraphRetentionReason { STATE_NODE, DATA_PATH, EXCLUDED_TYPE, NOT_ON_DATA_PATH };

struct GraphModuleInfo {
	RTLIL::IdString name;
	dict<RTLIL::IdString, RTLIL::Const> parameters;
	dict<RTLIL::IdString, RTLIL::Const> attributes;
};

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

struct GraphPort {
	PortId id = INVALID_PORT_ID;
	GraphCellId cell = INVALID_CELL_ID;
	RTLIL::Wire *module_port = nullptr;
	RTLIL::IdString name;
	RTLIL::PortDir direction = RTLIL::PortDir::PD_UNKNOWN;
	// Preserve the original ordered binding separately from the SigMap alias
	// representative used to find adjacency. Neither is a cross-command record.
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
	// Value copies preserve exact RTLIL names, parameter bits and all available
	// source attributes. Missing source attributes remain absent.
	RTLIL::IdString name;
	RTLIL::IdString type;
	dict<RTLIL::IdString, RTLIL::Const> parameters;
	dict<RTLIL::IdString, RTLIL::Const> attributes;
	NodeType node_type = NodeType::NONE;
	GraphCellRole role = GraphCellRole::EXCLUDED;
	bool in_graph = false;
	GraphRetentionReason retention_reason = GraphRetentionReason::NOT_ON_DATA_PATH;
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

struct CellGraphFilterOptions;
struct CellGraphSelection;
struct CellGraphView;

// A read-only analysis of an unchanged module. Build once, then select/view as
// often as needed. Live pointers and SigSpecs must not outlive or be reused
// after changes to that module; durable snapshots are a separate service.
struct CellGraph {
	RTLIL::Module *module = nullptr;
	GraphModuleInfo module_info;
	SigMap sigmap;
	CellGraphBuildOptions build_options;

	std::vector<GraphCell> cells;
	std::vector<GraphPort> ports;
	std::vector<GraphConnection> connections;
	// Indexed by PortId; entries reference only the retained connections above.
	// Physical construction adjacency is private and released after build().
	std::vector<std::vector<ConnectionId>> incoming;
	std::vector<std::vector<ConnectionId>> outgoing;
	std::vector<GraphCellId> nodes;
	std::vector<GraphCellId> edge_cells;
	dict<RTLIL::Cell *, GraphCellId> cell_ids;
	std::vector<PortId> module_inputs;
	std::vector<PortId> module_outputs;

	explicit CellGraph(RTLIL::Module *module, const CellGraphBuildOptions &options = {}) :
			module(module), sigmap(module), build_options(options) {}

	void build();
	// Recreate the common index after restoring validated value records. Does
	// not perform reachability, selection, or any RTLIL modification.
	void index_connections();
	// select_objects decides membership; create_view never reruns selection.
	// Selection and view IDs refer to this graph, not to another build.
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

} // namespace CellFuzz
YOSYS_NAMESPACE_END

#endif
