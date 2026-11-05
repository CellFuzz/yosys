#include "monitor_plan.h"
#include "kernel/newcelltypes.h"

#include <algorithm>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {
namespace {

static void cell_error(const GraphCell &cell, const char *reason)
{
	log_cmd_error("cellinstrument: cell %s (%s) %s.\n", cell.name, cell.type, reason);
}

static std::vector<RTLIL::IdString> mux_select_ports(RTLIL::IdString type)
{
	if (type.in(ID($mux), ID($pmux), ID($bmux), ID($demux), ID($bwmux), ID($_MUX_), ID($_NMUX_)))
		return {ID::S};
	if (type == ID($_MUX4_))
		return {ID::S, ID::T};
	if (type == ID($_MUX8_))
		return {ID::S, ID::T, ID::U};
	if (type == ID($_MUX16_))
		return {ID::S, ID::T, ID::U, ID::V};
	return {};
}

static bool supported_comb(RTLIL::IdString type)
{
	// Evaluable is the official pure combinational subset, unlike EDGE (a
	// graph role) or cell_known (which also admits formal/stateful cells).
	return StaticCellTypes::categories.is_evaluable(type) ||
			type.in(ID($barrier), ID($tribuf), ID($_TBUF_));
}

static void check_ports(const GraphCell &cell)
{
	for (const auto &connection : cell.cell->connections()) {
		auto direction = cell.cell->port_dir(connection.first);
		if (direction != RTLIL::PD_INPUT && direction != RTLIL::PD_OUTPUT)
			cell_error(cell, "has an unknown or bidirectional port");
		for (auto bit : connection.second)
			if (bit.wire && (bit.wire->module != cell.cell->module || bit.offset < 0 || bit.offset >= bit.wire->width))
				cell_error(cell, "has an invalid source bit");
	}
	// Consult the official table so a missing output cannot silently disappear
	// from the graph's connected-port list. No names such as Y are guessed.
	for (auto port : StaticCellTypes::port_info.inputs(cell.type))
		if (!cell.cell->hasPort(port))
			cell_error(cell, "is missing a required input port");
	for (auto port : StaticCellTypes::port_info.outputs(cell.type))
		// A valid collected memory with zero read ports has empty RD_DATA.
		if (!cell.cell->hasPort(port) || (cell.cell->getPort(port).empty() && cell.node_type != NodeType::MEMORY))
			cell_error(cell, "is missing a nonempty output port");
}

static RegisterInfo register_info(const GraphCell &cell, FfInitVals &initvals)
{
	FfData ff(&initvals, cell.cell);
	if (ff.has_gclk)
		cell_error(cell, "uses an implicit global clock without behavioral Verilog support");
	RegisterInfo info;
	info.cell = cell.id;
	info.features = ff;
	info.is_latch = !ff.has_clk && FfTypeData(cell.type).has_aload;
	info.init = ff.val_init;
	auto control = [&](const char *name, const RTLIL::SigSpec &signal, bool positive, int width) {
		if (GetSize(signal) != width)
			cell_error(cell, "has an invalid register control width");
		info.controls[name] = RegisterControl{signal, positive};
	};
	if (ff.has_clk) control("clock", ff.sig_clk, ff.pol_clk, 1);
	if (ff.has_ce) control("enable", ff.sig_ce, ff.pol_ce, 1);
	if (ff.has_aload) {
		control("async_load", ff.sig_aload, ff.pol_aload, 1);
		if (GetSize(ff.sig_ad) != ff.width)
			cell_error(cell, "has an invalid asynchronous data width");
		info.async_data = ff.sig_ad;
	}
	if (ff.has_arst) control("async_reset", ff.sig_arst, ff.pol_arst, 1);
	if (ff.has_srst) control("sync_reset", ff.sig_srst, ff.pol_srst, 1);
	if (ff.has_sr) {
		control("set", ff.sig_set, ff.pol_set, ff.width);
		control("clear", ff.sig_clr, ff.pol_clr, ff.width);
	}
	if ((ff.has_clk && GetSize(ff.sig_d) != ff.width) ||
			(ff.has_arst && GetSize(ff.val_arst) != ff.width) ||
			(ff.has_srst && GetSize(ff.val_srst) != ff.width))
		cell_error(cell, "has inconsistent register data or reset widths");
	return info;
}

using Json = json11::Json;

static Json signal_record(const RTLIL::SigSpec &signal)
{
	Json::array bits;
	for (auto bit : signal)
		bits.push_back(bit.wire ? Json(Json::array{bit.wire->name.str(), bit.offset}) : Json(int(bit.data)));
	return bits;
}

static Json constant_record(const RTLIL::Const &value)
{
	return Json::object{{"width", GetSize(value)}, {"bits", value.as_string("-")}, {"flags", int(value.flags)}};
}

static const char *role_name(MonitorRole role)
{
	switch (role) {
	case MonitorRole::OUTPUT: return "output";
	case MonitorRole::MUX_SELECT: return "mux_select";
	case MonitorRole::REGISTER_Q: return "register_q";
	}
	log_abort();
}
} // namespace

void MonitorOptions::validate() const
{
	if (chunk_width <= 0)
		log_cmd_error("cellinstrument: -chunk-width requires a positive integer.\n");
	auto letter = [](unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; };
	if (prefix.empty() || !letter(prefix[0]))
		log_cmd_error("cellinstrument: -prefix requires an ASCII identifier.\n");
	for (unsigned char ch : prefix)
		if (!letter(ch) && !(ch >= '0' && ch <= '9'))
			log_cmd_error("cellinstrument: -prefix requires an ASCII identifier.\n");
}

RTLIL::Module *monitor_top(RTLIL::Design *design)
{
	RTLIL::Module *top = nullptr;
	int tops = 0;
	for (auto module : design->modules())
		if (module->get_bool_attribute(ID::top)) {
			top = module;
			tops++;
		}
	if (tops == 0 && GetSize(design->modules()) == 1)
		top = *design->modules().begin();
	if (!top || tops > 1)
		log_cmd_error("cellinstrument: require exactly one top module; run hierarchy -top.\n");
	if (top->get_blackbox_attribute() || top->get_bool_attribute(ID::whitebox))
		log_cmd_error("cellinstrument: top must have a supported implementation, not a box.\n");
	auto selected = design->selected_modules();
	if (GetSize(selected) != 1 || selected.front() != top || !design->selected_whole_module(top))
		log_cmd_error("cellinstrument: require whole-module selection of exactly the top module.\n");
	if (top->get_bool_attribute(RTLIL::escape_id("cellfuzz_instrumented")))
		log_cmd_error("cellinstrument: module %s is already instrumented.\n", top);
	if (top->has_processes())
		log_cmd_error("cellinstrument: module %s contains processes; run proc before analysis.\n", top);
	if (top->has_memories())
		log_cmd_error("cellinstrument: module %s has uncollected memory; run memory_collect before analysis.\n", top);
	return top;
}

MonitorPlan create_monitor_plan(const RestoredAnalysis &analysis, const MonitorOptions &options)
{
	options.validate();
	const auto &graph = *analysis.graph;
	auto top = monitor_top(graph.module->design);
	if (graph.module != top)
		log_cmd_error("cellinstrument: analysis does not belong to the current top.\n");
	MonitorPlan plan;
	plan.options = options;
	FfInitVals initvals(&graph.sigmap, top);
	std::map<GraphCellId, RegisterInfo> registers;
	for (const auto &cell : graph.cells) {
		if (top->design->module(cell.type))
			cell_error(cell, "is a module instance; flatten and rerun cellfilter");
		if (cell.node_type == NodeType::FSM)
			cell_error(cell, "is an explicit FSM; run fsm_map and rerun cellfilter");
		if (cell.cell->is_mem_cell() && !cell.type.in(ID($mem), ID($mem_v2)))
			cell_error(cell, "is an uncollected memory port; run memory_collect and rerun cellfilter");
		if (!StaticCellTypes::categories.is_known(cell.type))
			cell_error(cell, "has unknown semantics or no supported Verilog model");
		check_ports(cell);
		if (cell.node_type == NodeType::REGISTER)
			registers.emplace(cell.id, register_info(cell, initvals));
		// flatten preserves source scopes as portless metadata; the Verilog
		// backend explicitly ignores these cells. They are never observations.
		else if (cell.node_type != NodeType::MEMORY && cell.type != ID($scopeinfo) && !supported_comb(cell.type))
			cell_error(cell, "has unsupported state, side effects or Verilog export semantics");
	}
	std::vector<PortId> ports(graph.ports.size());
	for (int i = 0; i < GetSize(ports); i++) ports[i] = i;
	// The original graph IDs are retained; ordering by raw object/port name
	// makes this plan readable and deterministic without changing those IDs.
	std::sort(ports.begin(), ports.end(), [&](PortId a, PortId b) {
		const auto &pa = graph.ports[a], &pb = graph.ports[b];
		std::string ca = pa.is_cell_port() ? graph.cells[pa.cell].name.str() : "";
		std::string cb = pb.is_cell_port() ? graph.cells[pb.cell].name.str() : "";
		return std::make_pair(ca, pa.name.str()) < std::make_pair(cb, pb.name.str());
	});
	for (const auto &cell : graph.cells) {
		if (!analysis.selection.selected_cells.count(cell.id)) {
			plan.unobserved.push_back({cell.id, cell.in_graph ? "not_selected" : "not_in_graph"});
			continue;
		}
		if (cell.node_type == NodeType::MEMORY) {
			if (options.mem_access || options.mem_query)
				cell_error(cell, "requests memory observation not yet implemented; use -mem-access off -mem-content off");
			plan.unobserved.push_back({cell.id, "memory_observation_disabled"});
		}
		if (registers.count(cell.id))
			plan.registers.push_back(registers.at(cell.id));
	}
	for (auto id : ports) {
		const auto &port = graph.ports[id];
		if (!port.is_cell_port() || !analysis.selection.selected_cells.count(port.cell))
			continue;
		const auto &cell = graph.cells[port.cell];
		if (cell.node_type == NodeType::MEMORY)
			continue;
		if (cell.node_type == NodeType::REGISTER) {
			if (port.name == ID::Q)
				plan.items.push_back({cell.id, id, MonitorRole::REGISTER_Q, port.raw_signal});
			continue;
		}
		if (port.is_output())
			plan.items.push_back({cell.id, id, MonitorRole::OUTPUT, port.raw_signal});
		auto selectors = mux_select_ports(cell.type);
		if (std::find(selectors.begin(), selectors.end(), port.name) != selectors.end()) {
			if (port.raw_signal.empty()) cell_error(cell, "has an empty mux selector");
			plan.items.push_back({cell.id, id, MonitorRole::MUX_SELECT, port.raw_signal});
		}
	}
	return plan;
}

Json MonitorPlan::type_report(const RestoredAnalysis &analysis) const
{
	const auto &graph = *analysis.graph;
	Json::array observations, state, omitted;
	for (const auto &item : items) {
		const auto &cell = graph.cells.at(item.cell);
		const auto &port = graph.ports.at(item.port);
		observations.push_back(Json::object{{"cell_id", item.cell}, {"port_id", item.port},
				{"cell", cell.name.str()}, {"type", cell.type.str()}, {"port", port.name.str()},
				{"role", role_name(item.role)}, {"width", GetSize(item.source)}, {"source", signal_record(item.source)}});
	}
	for (const auto &info : registers) {
		const auto &ff = info.features;
		Json::object controls;
		for (const auto &entry : info.controls)
			controls[entry.first] = Json::object{{"source", signal_record(entry.second.source)}, {"positive", entry.second.positive}};
		state.push_back(Json::object{{"cell_id", info.cell}, {"cell", graph.cells.at(info.cell).name.str()},
				{"fine", ff.is_fine}, {"latch", info.is_latch}, {"has_clk", ff.has_clk}, {"has_ce", ff.has_ce},
				{"has_aload", ff.has_aload}, {"has_arst", ff.has_arst}, {"has_srst", ff.has_srst}, {"has_sr", ff.has_sr},
				{"ce_over_srst", ff.ce_over_srst}, {"init", constant_record(info.init)},
				{"arst_value", constant_record(ff.val_arst)}, {"srst_value", constant_record(ff.val_srst)},
				{"controls", controls}, {"async_data", signal_record(info.async_data)}});
	}
	for (const auto &cell : unobserved)
		omitted.push_back(Json::object{{"cell_id", cell.cell}, {"cell", graph.cells.at(cell.cell).name.str()}, {"reason", cell.reason}});
	return Json::object{{"schema_version", 1}, {"status", "planned"}, {"stage", "type-planning"},
			{"module", graph.module_info.name.str()}, {"analysis_id", analysis.analysis_id},
			{"config", Json::object{{"mem_access", options.mem_access}, {"mem_content", options.mem_query ? "query" : "off"},
					{"chunk_width", options.chunk_width}, {"prefix", options.prefix}}},
			{"items", observations}, {"registers", state}, {"unobserved_cells", omitted}};
}

} // namespace CellFuzz
YOSYS_NAMESPACE_END
