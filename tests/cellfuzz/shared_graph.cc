#include "kernel/register.h"
#include "passes/cmds/cellfuzz/cellgraph.h"
#include "passes/cmds/cellfuzz/selection.h"

#include <algorithm>

USING_YOSYS_NAMESPACE
using namespace Yosys::CellFuzz;
PRIVATE_NAMESPACE_BEGIN

static RTLIL::Wire *add_port(RTLIL::Module *module, const char *name, bool input, int width = 1)
{
	auto wire = module->addWire(RTLIL::escape_id(name), width);
	wire->port_input = input;
	wire->port_output = !input;
	return wire;
}

static void test_adjacency(const CellGraph &graph)
{
	log_assert(graph.incoming.size() == graph.ports.size());
	log_assert(graph.outgoing.size() == graph.ports.size());
	std::vector<std::vector<ConnectionId>> incoming(graph.ports.size()), outgoing(graph.ports.size());
	for (const auto &connection : graph.connections) {
		incoming.at(connection.dst).push_back(connection.id);
		outgoing.at(connection.src).push_back(connection.id);
	}
	log_assert(graph.incoming == incoming);
	log_assert(graph.outgoing == outgoing);
}

static void test_callbacks(const CellGraph &graph)
{
	CellGraphFilterOptions options;
	options.top_k = 1;
	int calls = 0;
	options.region_selector = [&](const RegionSelectionContext &context, const CellRegion &) {
		log_assert(context.incoming == graph.incoming && context.outgoing == graph.outgoing);
		calls++;
		return RegionCellSelection{};
	};
	auto empty = graph.select_objects(options);
	log_assert(calls == GetSize(empty.regions) && calls > 1);
	log_assert(GetSize(empty.selected_cells) == GetSize(graph.nodes));
	auto empty_view = graph.create_view(empty);
	log_assert(empty_view.visible_edge_cell_count == 0);
	log_assert(empty_view.hidden_edge_cell_count == GetSize(graph.edge_cells));

	// The callback may exceed K, return different sizes, and return unsorted IDs.
	options.region_selector = [](const RegionSelectionContext &, const CellRegion &region) {
		RegionCellSelection result;
		if (region.id % 2 == 0)
			result.selected_cells.assign(region.edge_cells.rbegin(), region.edge_cells.rend());
		return result;
	};
	auto variable = graph.select_objects(options);
	bool exceeds_k = false, has_empty = false;
	for (const auto &region : variable.regions) {
		exceeds_k |= GetSize(region.key_cells) > options.top_k;
		has_empty |= region.key_cells.empty();
		log_assert(std::is_sorted(region.key_cells.begin(), region.key_cells.end()));
		for (auto cell_id : region.edge_cells)
			log_assert(bool(variable.selected_cells.count(cell_id)) == (region.id % 2 == 0));
	}
	log_assert(exceeds_k && has_empty);
	auto view = graph.create_view(variable);
	log_assert(view.visible_cells == variable.selected_cells);
	log_assert(view.visible_module_ports == variable.selected_module_ports);
	log_assert(calls == GetSize(empty.regions));

	// A view of an empty explicit set must not reinsert nodes or module ports.
	auto omitted = graph.create_view(CellGraphSelection{});
	log_assert(omitted.visible_cells.empty() && omitted.visible_module_ports.empty());
	log_assert(omitted.connections.empty());
}

static void test_bindings_and_metadata()
{
	RTLIL::Design design;
	auto module = design.addModule(RTLIL::IdString("\\module.with[punctuation]"));
	module->attributes[ID::src] = RTLIL::Const("fixture.v:1.1-4.1");
	module->parameter_default_values[ID::WIDTH] = RTLIL::Const(6);
	auto input = add_port(module, "input.with[punctuation]", true, 4);
	auto output = add_port(module, "output", false, 6);
	auto alias = module->addWire(RTLIL::escape_id("alias"), 4);
	module->connect(alias, input);
	RTLIL::SigSpec raw;
	raw.append(RTLIL::SigBit(input, 3));
	raw.append(RTLIL::SigBit(alias, 1));
	raw.append(RTLIL::State::Sx);
	raw.append(RTLIL::SigBit(alias, 1));
	raw.append(RTLIL::State::Sz);
	raw.append(RTLIL::SigBit(input, 0));
	auto cell = module->addNot(RTLIL::IdString("\\cell.with[punctuation]"), raw, output, false, "fixture.v:2.1-2.9");
	auto dead = module->addNot(RTLIL::escape_id("dead"), RTLIL::SigSpec(RTLIL::State::S0),
		module->addWire(RTLIL::escape_id("unused")));
	module->fixup_ports();
	CellGraph graph(module);
	graph.build();
	test_adjacency(graph);
	log_assert(graph.module_info.name == module->name);
	log_assert(graph.module_info.parameters == module->parameter_default_values);
	log_assert(graph.module_info.attributes == module->attributes);
	const auto &record = graph.cells.at(graph.cell_ids.at(cell));
	log_assert(record.name == cell->name && record.type == cell->type);
	log_assert(record.parameters == cell->parameters && record.attributes == cell->attributes);
	log_assert(record.retention_reason == GraphRetentionReason::DATA_PATH);
	const auto &dead_record = graph.cells.at(graph.cell_ids.at(dead));
	log_assert(!dead_record.in_graph);
	log_assert(dead_record.retention_reason == GraphRetentionReason::NOT_ON_DATA_PATH);
	const auto &port = graph.ports.at(record.inputs.at(0));
	log_assert(port.raw_signal == raw);
	log_assert(port.canonical_signal == graph.sigmap(raw));
	log_assert(port.raw_signal != port.canonical_signal);
	log_assert(port.canonical_signal[1] == port.canonical_signal[3]);
	log_assert(port.canonical_signal[2] == RTLIL::SigBit(RTLIL::State::Sx));
	log_assert(port.canonical_signal[4] == RTLIL::SigBit(RTLIL::State::Sz));
	// Metadata is copied, not fetched lazily through the live RTLIL pointers.
	cell->attributes.clear();
	cell->parameters[ID::A_WIDTH] = RTLIL::Const(99);
	module->attributes.clear();
	log_assert(record.attributes.count(ID::src));
	log_assert(record.parameters.at(ID::A_WIDTH) == RTLIL::Const(6));
	log_assert(graph.module_info.attributes.count(ID::src));
}

static void test_state_barriers()
{
	RTLIL::Design design;
	auto module = design.addModule(RTLIL::escape_id("barrier"));
	auto input = add_port(module, "a", true);
	auto clock = add_port(module, "clk", true);
	auto output = add_port(module, "y", false);
	auto d = module->addWire(RTLIL::escape_id("d"));
	auto q = module->addWire(RTLIL::escape_id("q"));
	module->addNot(RTLIL::escape_id("before"), input, d);
	auto state = module->addDff(RTLIL::escape_id("state"), clock, d, q);
	module->addNot(RTLIL::escape_id("after"), q, output);
	module->fixup_ports();
	CellGraph graph(module);
	graph.build();
	log_assert(GetSize(graph.nodes) == 1 && GetSize(graph.edge_cells) == 2);
	log_assert(graph.cells.at(graph.cell_ids.at(state)).retention_reason == GraphRetentionReason::STATE_NODE);
	CellGraphFilterOptions options;
	options.region_selector = [](const RegionSelectionContext &, const CellRegion &) { return RegionCellSelection{}; };
	auto selection = graph.select_objects(options);
	selection.selected_cells.clear();
	auto view = graph.create_view(selection);
	log_assert(view.connections.empty()); // Hidden NODE blocks a -> before -> state -> after -> y.
	selection.selected_cells.insert(graph.cell_ids.at(state));
	selection.selected_module_ports.clear();
	log_assert(graph.create_view(selection).connections.empty());

	CellGraphBuildOptions excluded;
	excluded.register_role = GraphCellRole::EXCLUDED;
	CellGraph no_state(module, excluded);
	no_state.build();
	test_adjacency(no_state);
	log_assert(no_state.nodes.empty() && no_state.edge_cells.empty() && no_state.connections.empty());
	const auto &excluded_state = no_state.cells.at(no_state.cell_ids.at(state));
	log_assert(excluded_state.node_type == NodeType::REGISTER);
	log_assert(excluded_state.retention_reason == GraphRetentionReason::EXCLUDED_TYPE);

	// Preserve the explicitly supported internal EDGE role for state cells.
	excluded.register_role = GraphCellRole::EDGE;
	CellGraph edge_state(module, excluded);
	edge_state.build();
	log_assert(edge_state.nodes.empty() && GetSize(edge_state.edge_cells) == 3);
	auto edge_view = edge_state.create_view(edge_state.select_objects(options));
	log_assert(GetSize(edge_view.connections) == 2); // a and clk reach y across the explicit EDGE.
	for (const auto &connection : edge_view.connections)
		log_assert(connection.collapsed);
}

static void test_cycle()
{
	RTLIL::Design design;
	auto module = design.addModule(RTLIL::escape_id("cycle"));
	auto a = add_port(module, "a", true);
	auto y = add_port(module, "y", false);
	auto n = module->addWire(RTLIL::escape_id("n"));
	module->addAnd(RTLIL::escape_id("and"), a, y, n);
	module->addNot(RTLIL::escape_id("not"), n, y);
	module->fixup_ports();
	CellGraph graph(module);
	graph.build();
	CellGraphFilterOptions options;
	options.region_selector = [](const RegionSelectionContext &, const CellRegion &) { return RegionCellSelection{}; };
	auto view = graph.create_view(graph.select_objects(options));
	log_assert(GetSize(view.connections) == 1);
	log_assert(view.connections.at(0).collapsed && GetSize(view.connections.at(0).hidden_cells) == 2);
}

static void test_empty_graph()
{
	RTLIL::Design design;
	auto module = design.addModule(RTLIL::escape_id("empty"));
	CellGraph graph(module);
	graph.build();
	test_adjacency(graph);
	log_assert(graph.module_info.attributes.empty());
	auto selection = graph.select_objects(CellGraphFilterOptions{});
	log_assert(selection.selected_cells.empty() && selection.selected_module_ports.empty());
	log_assert(graph.create_view(selection).connections.empty());
}

struct TestCellFuzzGraphPass : Pass {
	TestCellFuzzGraphPass() : Pass("test_cellfuzz_graph", "test the shared CellFuzz graph API") {}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		bool invalid = GetSize(args) == 2;
		extra_args(args, invalid ? 2 : 1, design);
		CellGraph graph(design->module(RTLIL::escape_id("graph")));
		graph.build();
		if (invalid) {
			CellGraphFilterOptions options;
			options.region_selector = [&](const RegionSelectionContext &, const CellRegion &region) {
				RegionCellSelection result;
				auto id = region.edge_cells.front();
				if (args[1] == "duplicate")
					result.selected_cells = {id, id};
				else if (args[1] == "node")
					result.selected_cells = graph.nodes;
				else
					for (auto other : graph.edge_cells)
						if (std::find(region.edge_cells.begin(), region.edge_cells.end(), other) == region.edge_cells.end()) {
							result.selected_cells = {other};
							break;
						}
				return result;
			};
			graph.select_objects(options);
			log_error("Invalid region selection was accepted.\n");
		}
		test_adjacency(graph);
		test_callbacks(graph);
		test_bindings_and_metadata();
		test_state_barriers();
		test_cycle();
		test_empty_graph();
	}
} TestCellFuzzGraphPass;

PRIVATE_NAMESPACE_END
