#include "manifest.h"

#include <algorithm>
#include <numeric>
#include <set>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {
namespace {
using Json = json11::Json;

static Json constant(const RTLIL::Const &value)
{
	return Json::object{{"width", GetSize(value)}, {"bits", value.as_string("-")}, {"flags", int(value.flags)}};
}

static Json values(const dict<RTLIL::IdString, RTLIL::Const> &source)
{
	Json::object result;
	for (const auto &entry : source)
		result[entry.first.str()] = constant(entry.second);
	return result;
}

static Json signal(const RTLIL::SigSpec &source)
{
	Json::array result;
	for (auto bit : source)
		result.push_back(bit.wire ? Json(Json::array{bit.wire->name.str(), bit.offset}) : Json(int(bit.data)));
	return result;
}

static const char *role(GraphCellRole value)
{
	switch (value) {
	case GraphCellRole::NODE: return "node";
	case GraphCellRole::EDGE: return "edge";
	case GraphCellRole::EXCLUDED: return "excluded";
	}
	log_abort();
}

static const char *semantic(NodeType value)
{
	switch (value) {
	case NodeType::NONE: return "combinational";
	case NodeType::REGISTER: return "register";
	case NodeType::MEMORY: return "memory";
	case NodeType::FSM: return "fsm";
	}
	log_abort();
}

static const char *retention(GraphRetentionReason value)
{
	switch (value) {
	case GraphRetentionReason::STATE_NODE: return "state_node";
	case GraphRetentionReason::DATA_PATH: return "data_path";
	case GraphRetentionReason::EXCLUDED_TYPE: return "excluded_type";
	case GraphRetentionReason::NOT_ON_DATA_PATH: return "not_on_data_path";
	}
	log_abort();
}

static const char *direction(RTLIL::PortDir value)
{
	switch (value) {
	case RTLIL::PD_INPUT: return "input";
	case RTLIL::PD_OUTPUT: return "output";
	case RTLIL::PD_INOUT: return "inout";
	case RTLIL::PD_UNKNOWN: return "unknown";
	}
	log_abort();
}

// Keep export IDs separate from graph IDs: dictionary iteration can differ
// after cloning/restoring an identical Design. Never sort or edit live RTLIL.
struct ExportIds {
	std::vector<int> cells, ports, cell_order, port_order;
	explicit ExportIds(const CellGraph &graph) : cells(graph.cells.size()), ports(graph.ports.size()),
			cell_order(graph.cells.size()), port_order(graph.ports.size())
	{
		std::iota(cell_order.begin(), cell_order.end(), 0);
		std::sort(cell_order.begin(), cell_order.end(), [&](int a, int b) { return graph.cells[a].name.str() < graph.cells[b].name.str(); });
		for (int index = 0; index < GetSize(cell_order); index++) cells[cell_order[index]] = index;
		std::iota(port_order.begin(), port_order.end(), 0);
		auto key = [&](int id) {
			const auto &port = graph.ports[id];
			return std::make_tuple(port.is_cell_port(), port.is_cell_port() ? graph.cells[port.cell].name.str() : "", port.name.str());
		};
		std::sort(port_order.begin(), port_order.end(), [&](int a, int b) { return key(a) < key(b); });
		for (int index = 0; index < GetSize(port_order); index++) ports[port_order[index]] = index;
	}
};

template<typename Range> static Json remap(const Range &source, const std::vector<int> &mapping)
{
	std::vector<int> sorted;
	for (auto id : source) sorted.push_back(mapping.at(id));
	std::sort(sorted.begin(), sorted.end());
	return sorted;
}

static Json boundaries(const pool<std::pair<int, int>> &source, const ExportIds &ids)
{
	Json::array result;
	for (auto entry : source)
		result.push_back(Json::object{{"kind", entry.first == 0 ? "cell" : "module_port"},
				{"id", (entry.first == 0 ? ids.cells : ids.ports).at(entry.second)}});
	std::sort(result.begin(), result.end());
	return result;
}
} // namespace

Json planned_manifest(const RestoredAnalysis &analysis, const MonitorPlan &plan, const TransportLayout &layout)
{
	const auto &graph = *analysis.graph;
	validate_transport(plan, layout, graph.module);
	ExportIds ids(graph);
	// Validate the frozen source identity before asking the type-report helper
	// to index graph vectors. This keeps malformed internal plans on the same
	// diagnostic path as all other manifest contract violations.
	for (const auto &item : plan.items) {
		if (item.cell < 0 || item.cell >= GetSize(graph.cells) || item.port < 0 || item.port >= GetSize(graph.ports) ||
				graph.ports[item.port].cell != item.cell || item.port_lsb < 0 ||
				item.port_lsb > GetSize(graph.ports[item.port].raw_signal) ||
				GetSize(item.source) > GetSize(graph.ports[item.port].raw_signal) - item.port_lsb ||
				graph.ports[item.port].raw_signal.extract(item.port_lsb, GetSize(item.source)) != item.source ||
				(item.role != MonitorRole::MEMORY_ACCESS && (item.port_lsb != 0 || graph.ports[item.port].raw_signal != item.source)) ||
				!analysis.selection.selected_cells.count(item.cell))
			log_cmd_error("cellinstrument: observation source does not match the frozen graph.\n");
	}
	// Reuse the already tested type descriptions; replace every graph ID with
	// its export counterpart before exposing this as the complete contract.
	auto report = plan.type_report(analysis).object_items();
	report["stage"] = "observation-planning";
	report["producer"] = Json::object{{"tool", "yosys"}, {"version", yosys_version_str}};
	report["analysis"] = Json::object{{"analysis_id", analysis.analysis_id}, {"module", graph.module_info.name.str()},
			{"structure_digest", structural_digest(graph.module)},
			{"build_options", Json::object{{"register", role(graph.build_options.register_role)},
					{"memory", role(graph.build_options.memory_role)}, {"fsm", role(graph.build_options.fsm_role)}}},
			{"strategy", Json::object{{"id", analysis.strategy.id}, {"version", analysis.strategy.version}, {"parameters", analysis.strategy.parameters}}}};
	Json::array items;
	std::set<int> observed_cells, observed_ports;
	for (int index = 0; index < GetSize(plan.items); index++) {
		const auto &item = plan.items[index];
		auto entry = report["items"][index].object_items();
		entry["monitor_id"] = index;
		entry["cell_id"] = ids.cells[item.cell];
		entry["port_id"] = ids.ports[item.port];
		items.push_back(entry);
		observed_cells.insert(item.cell);
		observed_ports.insert(item.port);
	}
	report["items"] = items;
	for (auto key : {"registers", "unobserved_cells"}) {
		Json::array remapped;
		for (const auto &item : report[key].array_items()) {
			auto entry = item.object_items();
			entry["cell_id"] = ids.cells.at(item["cell_id"].int_value());
			remapped.push_back(entry);
		}
		std::sort(remapped.begin(), remapped.end(), [](const Json &a, const Json &b) { return a["cell_id"] < b["cell_id"]; });
		report[key] = remapped;
	}
	std::map<int, std::string> omitted;
	for (const auto &entry : plan.unobserved) omitted[entry.cell] = entry.reason;
	std::map<GraphCellId, const MemoryInfo *> memory_info;
	for (const auto &info : plan.memories) memory_info.emplace(info.cell, &info);
	Json::array cells, ports, wires, connections, memories;
	for (auto id : ids.cell_order) {
		const auto &cell = graph.cells[id];
		std::string reason = observed_cells.count(id) ? "observed" :
				memory_info.count(id) && memory_info.at(id)->query.enabled ? "memory_content_query" : omitted.count(id) ? omitted.at(id) : "no_observation_source";
		cells.push_back(Json::object{{"id", ids.cells[id]}, {"analysis_cell_id", id}, {"name", cell.name.str()},
				{"type", cell.type.str()}, {"semantic_type", cell.type == ID($scopeinfo) ? "metadata" : semantic(cell.node_type)},
				{"role", role(cell.role)}, {"retention_reason", retention(cell.retention_reason)}, {"in_graph", cell.in_graph},
				{"selected", bool(analysis.selection.selected_cells.count(id))}, {"observed", bool(observed_cells.count(id))},
				{"observation_reason", reason}, {"parameters", values(cell.parameters)}, {"attributes", values(cell.attributes)},
				{"inputs", remap(cell.inputs, ids.ports)}, {"outputs", remap(cell.outputs, ids.ports)}});
		if (cell.node_type == NodeType::MEMORY) {
			std::vector<int> memory_ports = cell.inputs;
			memory_ports.insert(memory_ports.end(), cell.outputs.begin(), cell.outputs.end());
			auto entry = memory_access_record(*memory_info.at(id), plan, ids.ports).object_items();
			entry.insert({{"cell_id", ids.cells[id]}, {"name", cell.name.str()}, {"type", cell.type.str()},
					{"selected", bool(analysis.selection.selected_cells.count(id))}, {"observation_reason", reason},
					{"ports", remap(memory_ports, ids.ports)}, {"parameters", values(cell.parameters)}});
			memories.push_back(entry);
		}
	}
	for (auto id : ids.port_order) {
		const auto &port = graph.ports[id];
		bool selected = port.is_cell_port() ? analysis.selection.selected_cells.count(port.cell) : analysis.selection.selected_module_ports.count(id);
		std::string reason = observed_ports.count(id) ? "observed" : !selected ? "not_selected" :
				!port.is_cell_port() ? "original_interface_reused" : port.is_input() ? "input_not_observed" : "owner_not_observed";
		ports.push_back(Json::object{{"id", ids.ports[id]}, {"analysis_port_id", id}, {"name", port.name.str()},
				{"cell_id", port.is_cell_port() ? Json(ids.cells[port.cell]) : Json()},
				{"kind", port.is_cell_port() ? "cell_port" : "module_port"}, {"direction", direction(port.direction)},
				{"width", GetSize(port.raw_signal)}, {"raw", signal(port.raw_signal)}, {"canonical", signal(port.canonical_signal)},
				{"selected", selected}, {"observed", bool(observed_ports.count(id))}, {"observation_reason", reason}});
	}
	std::vector<RTLIL::Wire *> wire_order;
	for (auto wire : graph.module->wires()) wire_order.push_back(wire);
	std::sort(wire_order.begin(), wire_order.end(), [](auto a, auto b) { return a->name.str() < b->name.str(); });
	for (auto wire : wire_order)
		wires.push_back(Json::object{{"id", GetSize(wires)}, {"name", wire->name.str()}, {"width", wire->width},
				{"start_offset", wire->start_offset}, {"upto", wire->upto}, {"signed", wire->is_signed}, {"port_id", wire->port_id},
				{"input", wire->port_input}, {"output", wire->port_output}, {"attributes", values(wire->attributes)}});
	std::vector<GraphConnection> ordered_connections = graph.connections;
	std::sort(ordered_connections.begin(), ordered_connections.end(), [&](const auto &a, const auto &b) {
		return std::make_pair(ids.ports[a.src], ids.ports[a.dst]) < std::make_pair(ids.ports[b.src], ids.ports[b.dst]);
	});
	for (const auto &connection : ordered_connections)
		connections.push_back(Json::object{{"id", GetSize(connections)}, {"analysis_connection_id", connection.id},
				{"src", ids.ports[connection.src]}, {"dst", ids.ports[connection.dst]}, {"bit_count", connection.bit_count}});
	report["graph"] = Json::object{{"module", Json::object{{"name", graph.module_info.name.str()},
				{"parameters", values(graph.module_info.parameters)}, {"attributes", values(graph.module_info.attributes)}}},
			{"cells", cells}, {"ports", ports}, {"wires", wires}, {"connections", connections},
			{"module_inputs", remap(graph.module_inputs, ids.ports)}, {"module_outputs", remap(graph.module_outputs, ids.ports)}};
	Json::array regions;
	std::vector<CellRegion> region_order = analysis.selection.regions;
	std::sort(region_order.begin(), region_order.end(), [&](const auto &a, const auto &b) { return remap(a.edge_cells, ids.cells) < remap(b.edge_cells, ids.cells); });
	for (const auto &region : region_order)
		regions.push_back(Json::object{{"id", GetSize(regions)}, {"analysis_region_id", region.id},
				{"edge_cells", remap(region.edge_cells, ids.cells)}, {"key_cells", remap(region.key_cells, ids.cells)},
				{"upstream_boundaries", boundaries(region.upstream_boundaries, ids)},
				{"downstream_boundaries", boundaries(region.downstream_boundaries, ids)}});
	report["selection"] = Json::object{{"cells", remap(analysis.selection.selected_cells, ids.cells)},
			{"module_ports", remap(analysis.selection.selected_module_ports, ids.ports)}, {"regions", regions}};
	Json::array view_connections;
	for (const auto &connection : analysis.view.connections)
		view_connections.push_back(Json::object{{"src", ids.ports.at(connection.src)}, {"dst", ids.ports.at(connection.dst)},
				{"bit_count", connection.collapsed ? Json() : Json(connection.bit_count)}, {"collapsed", connection.collapsed},
				{"hidden_cells", remap(connection.hidden_cells, ids.cells)}});
	std::sort(view_connections.begin(), view_connections.end(), [](const Json &a, const Json &b) {
		return std::make_pair(a["src"].int_value(), a["dst"].int_value()) < std::make_pair(b["src"].int_value(), b["dst"].int_value());
	});
	report["view"] = Json::object{{"cells", remap(analysis.view.visible_cells, ids.cells)},
			{"module_ports", remap(analysis.view.visible_module_ports, ids.ports)}, {"regions", regions}, {"connections", view_connections},
			{"visible_edge_cell_count", analysis.view.visible_edge_cell_count}, {"hidden_edge_cell_count", analysis.view.hidden_edge_cell_count}};
	Json::array transport_ports, segments;
	for (const auto &port : layout.ports)
		transport_ports.push_back(Json::object{{"name", port.name}, {"width", port.width}, {"direction", "output"}});
	for (const auto &segment : layout.segments)
		segments.push_back(Json::object{{"monitor_id", segment.monitor_id}, {"item_lsb", segment.item_lsb},
				{"port_name", segment.port_name}, {"port_lsb", segment.port_lsb}, {"width", segment.width}});
	report["transport"] = Json::object{{"ports", transport_ports}, {"segments", segments}, {"total_bits", double(layout.total_bits)}};
	report["memories"] = memories;
	report["diagnostics"] = Json::array{"planned only; no interface has been materialized"};
	report["summary"] = Json::object{{"cells", GetSize(cells)}, {"ports", GetSize(ports)}, {"connections", GetSize(connections)},
			{"items", GetSize(items)}, {"observed_cells", GetSize(observed_cells)}, {"total_bits", double(layout.total_bits)}};
	return report;
}

Json materialized_manifest(const Json &planned)
{
	auto report = planned.object_items();
	report["status"] = "materialized";
	report["stage"] = "observation-materialization";
	report["diagnostics"] = Json::array{"ordinary bypass outputs and requested memory query interfaces materialized; no sampling state added"};
	return report;
}

} // namespace CellFuzz
YOSYS_NAMESPACE_END
