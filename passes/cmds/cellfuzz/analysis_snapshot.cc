#include "analysis_snapshot.h"
#include "libs/sha1/sha1.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <set>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {
namespace {
using Json = json11::Json;

static void require(bool valid, const char *reason)
{
	if (!valid)
		log_cmd_error("CellFuzz analysis %s; rerun cellfilter.\n", reason);
}

static const Json::array &array(const Json &value)
{
	require(value.is_array(), "contains an invalid array");
	return value.array_items();
}

static const Json::array &tuple(const Json &value, size_t size)
{
	const auto &items = array(value);
	require(items.size() == size, "contains an invalid record");
	return items;
}

static int integer(const Json &value, int minimum = 0, int maximum = INT_MAX)
{
	double number = value.number_value();
	require(value.is_number() && number >= minimum && number <= maximum && std::floor(number) == number,
			"contains an invalid integer");
	return int(number);
}

static bool boolean(const Json &value)
{
	require(value.is_bool(), "contains an invalid boolean");
	return value.bool_value();
}

static std::string text(const Json &value)
{
	require(value.is_string(), "contains an invalid string");
	return value.string_value();
}

static RTLIL::IdString identifier(const Json &value)
{
	auto name = text(value);
	require(!name.empty() && (name[0] == '\\' || name[0] == '$'), "contains an invalid RTLIL name");
	for (unsigned char ch : name)
		require(ch > ' ', "contains an invalid RTLIL name");
	return RTLIL::IdString(name);
}

static Json parse(const std::string &encoded)
{
	std::string error;
	auto result = Json::parse(encoded, error);
	require(error.empty() && result.is_object(), "is malformed");
	return result;
}

template<typename Range> static Json ids(const Range &values)
{
	Json::array result;
	for (auto value : values)
		result.push_back(value);
	return result;
}

template<typename T> static Json sorted_ids(const pool<T> &values)
{
	std::vector<T> sorted(values.begin(), values.end());
	std::sort(sorted.begin(), sorted.end());
	return ids(sorted);
}

static Json boundaries(const pool<std::pair<int, int>> &values)
{
	std::vector<std::pair<int, int>> sorted(values.begin(), values.end());
	std::sort(sorted.begin(), sorted.end());
	Json::array result;
	for (auto value : sorted)
		result.push_back(Json::array{value.first, value.second});
	return result;
}

static Json selection_record(const CellGraphSelection &selection)
{
	Json::array regions;
	for (const auto &region : selection.regions)
		regions.push_back(Json::array{region.id, ids(region.edge_cells), ids(region.key_cells),
				boundaries(region.upstream_boundaries), boundaries(region.downstream_boundaries)});
	return Json::object{{"cells", sorted_ids(selection.selected_cells)},
			{"module_ports", sorted_ids(selection.selected_module_ports)}, {"regions", regions}};
}

static std::vector<int> read_ids(const Json &value)
{
	std::vector<int> result;
	std::set<int> seen;
	for (const auto &item : array(value)) {
		int id = integer(item);
		require(seen.insert(id).second, "contains duplicate IDs");
		result.push_back(id);
	}
	return result;
}

static pool<std::pair<int, int>> read_boundaries(const Json &value)
{
	pool<std::pair<int, int>> result;
	for (const auto &item : array(value)) {
		const auto &entry = tuple(item, 2);
		require(result.insert({integer(entry[0], 0, 1), integer(entry[1])}).second, "contains duplicate boundaries");
	}
	return result;
}

static CellGraphSelection read_selection(const Json &value)
{
	CellGraphSelection result;
	for (int id : read_ids(value["cells"]))
		result.selected_cells.insert(id);
	for (int id : read_ids(value["module_ports"]))
		result.selected_module_ports.insert(id);
	for (const auto &item : array(value["regions"])) {
		const auto &entry = tuple(item, 5);
		CellRegion region;
		region.id = integer(entry[0]);
		require(region.id == GetSize(result.regions), "contains unordered region IDs");
		region.edge_cells = read_ids(entry[1]);
		region.key_cells = read_ids(entry[2]);
		region.upstream_boundaries = read_boundaries(entry[3]);
		region.downstream_boundaries = read_boundaries(entry[4]);
		result.regions.push_back(std::move(region));
	}
	return result;
}

static Json signal_record(const RTLIL::SigSpec &signal)
{
	Json::array result;
	for (auto bit : signal)
		result.push_back(bit.wire ? Json(Json::array{bit.wire->name.str(), bit.offset}) : Json(int(bit.data)));
	return result;
}

static RTLIL::SigSpec bind_signal(const Json &value, RTLIL::Module *module)
{
	RTLIL::SigSpec result;
	for (const auto &bit : array(value)) {
		if (bit.is_number()) {
			result.append(RTLIL::State(integer(bit, 0, int(RTLIL::State::Sm))));
		} else {
			const auto &entry = tuple(bit, 2);
			auto wire = module->wire(identifier(entry[0]));
			require(wire != nullptr, "references a missing wire");
			int offset = integer(entry[1], 0, wire->width - 1);
			result.append(RTLIL::SigBit(wire, offset));
		}
	}
	return result;
}

static void validate_selection(const CellGraph &graph, const CellGraphSelection &selection)
{
	auto valid_cell = [&](int id) { return id >= 0 && id < GetSize(graph.cells) && graph.cells[id].in_graph; };
	auto valid_port = [&](int id) { return id >= 0 && id < GetSize(graph.ports) && !graph.ports[id].is_cell_port(); };
	for (auto id : selection.selected_cells)
		require(valid_cell(id), "selects an invalid cell");
	for (auto id : selection.selected_module_ports)
		require(valid_port(id), "selects an invalid module port");
	pool<GraphCellId> assigned;
	for (const auto &region : selection.regions) {
		pool<GraphCellId> members;
		for (auto id : region.edge_cells) {
			require(valid_cell(id) && graph.cells[id].is_edge(), "contains an invalid region member");
			require(assigned.insert(id).second && members.insert(id).second, "contains overlapping regions");
		}
		for (auto id : region.key_cells)
			require(members.count(id) && selection.selected_cells.count(id), "contains an invalid region selection");
		for (const auto *set : {&region.upstream_boundaries, &region.downstream_boundaries})
			for (auto boundary : *set)
				require(boundary.first == 0 ? valid_cell(boundary.second) : valid_port(boundary.second),
						"contains an invalid region boundary");
	}
}
} // namespace

AnalysisSnapshot AnalysisSnapshot::capture(const CellGraph &graph, const CellGraphSelection &selection,
		const SelectionStrategy &strategy)
{
	StructuralDigestContext context(graph.module->design, {graph.module});
	return capture(graph, selection, strategy, context);
}

AnalysisSnapshot AnalysisSnapshot::capture(const CellGraph &graph, const CellGraphSelection &selection,
		const SelectionStrategy &strategy, const StructuralDigestContext &context)
{
	validate_selection(graph, selection);
	require(!strategy.id.empty() && strategy.version > 0, "has an invalid strategy");
	AnalysisSnapshot result;
	result.module_name = graph.module_info.name.str();
	result.structure_digest = context.digest(graph.module);
	result.build_options = graph.build_options;
	result.strategy = strategy;
	result.selection = selection;
	Json::array cells, ports, connections;
	for (const auto &cell : graph.cells)
		cells.push_back(Json::array{cell.name.str(), cell.type.str(), int(cell.node_type), int(cell.role),
				cell.in_graph, int(cell.retention_reason)});
	for (const auto &port : graph.ports)
		ports.push_back(Json::array{port.cell, port.name.str(), int(port.direction),
				signal_record(port.raw_signal), signal_record(port.canonical_signal)});
	for (const auto &connection : graph.connections)
		connections.push_back(Json::array{connection.src, connection.dst, connection.bit_count});
	result.graph_record = Json::object{{"cells", cells}, {"ports", ports}, {"connections", connections}};
	result.analysis_id = sha1(result.to_json(false).dump());
	return result;
}

json11::Json AnalysisSnapshot::to_json(bool include_id) const
{
	Json::object record{{"version", version}, {"module", module_name}, {"structure_digest", structure_digest},
			{"build_options", Json::array{int(build_options.register_role), int(build_options.memory_role), int(build_options.fsm_role)}},
			{"strategy", Json::object{{"id", strategy.id}, {"version", strategy.version}, {"parameters", strategy.parameters}}},
			{"graph", graph_record}, {"selection", selection_record(selection)}};
	if (include_id)
		record["analysis_id"] = analysis_id;
	return record;
}

std::string AnalysisSnapshot::encode() const
{
	return to_json().dump();
}

AnalysisSnapshot AnalysisSnapshot::from_json(const json11::Json &record)
{
	require(integer(record["version"]) == version, "has an unsupported snapshot version");
	AnalysisSnapshot result;
	result.module_name = identifier(record["module"]).str();
	result.structure_digest = text(record["structure_digest"]);
	result.analysis_id = text(record["analysis_id"]);
	const auto &options = tuple(record["build_options"], 3);
	result.build_options.register_role = GraphCellRole(integer(options[0], 0, 2));
	result.build_options.memory_role = GraphCellRole(integer(options[1], 0, 2));
	result.build_options.fsm_role = GraphCellRole(integer(options[2], 0, 2));
	result.strategy.id = text(record["strategy"]["id"]);
	result.strategy.version = integer(record["strategy"]["version"], 1);
	require(!result.strategy.id.empty() && record["strategy"]["parameters"].is_object(), "has an invalid strategy");
	for (const auto &parameter : record["strategy"]["parameters"].object_items())
		result.strategy.parameters[parameter.first] = text(parameter.second);
	result.selection = read_selection(record["selection"]);
	result.graph_record = record["graph"];
	require(result.graph_record.is_object(), "has no graph record");
	require(result.analysis_id == sha1(result.to_json(false).dump()), "has a corrupt snapshot identity");
	return result;
}

AnalysisSnapshot AnalysisSnapshot::decode(const std::string &encoded)
{
	return from_json(parse(encoded));
}

RestoredAnalysis AnalysisSnapshot::restore(RTLIL::Design *design) const
{
	require(analysis_id == sha1(to_json(false).dump()), "has a corrupt snapshot identity");
	auto module = design->module(identifier(Json(module_name)));
	require(module != nullptr, "refers to a missing module");
	require(structural_digest(module) == structure_digest, "is stale (module structure or dependency changed)");
	auto graph = std::make_unique<CellGraph>(module, build_options);
	graph->module_info = GraphModuleInfo{module->name, module->parameter_default_values, module->attributes};
	const auto &cells = array(graph_record["cells"]);
	require(GetSize(cells) == GetSize(module->cells_), "has an inconsistent cell count");
	for (const auto &item : cells) {
		const auto &entry = tuple(item, 6);
		GraphCell cell;
		cell.id = GetSize(graph->cells);
		cell.name = identifier(entry[0]);
		cell.type = identifier(entry[1]);
		cell.cell = module->cell(cell.name);
		require(cell.cell && cell.cell->type == cell.type, "references a missing or changed cell");
		require(!graph->cell_ids.count(cell.cell), "contains duplicate cells");
		cell.parameters = cell.cell->parameters;
		cell.attributes = cell.cell->attributes;
		cell.node_type = NodeType(integer(entry[2], 0, 3));
		cell.role = GraphCellRole(integer(entry[3], 0, 2));
		cell.in_graph = boolean(entry[4]);
		cell.retention_reason = GraphRetentionReason(integer(entry[5], 0, 3));
		require(cell.node_type == get_node_type(cell.cell), "has an inconsistent semantic type");
		require(cell.role == build_options.role_for(cell.node_type), "has an inconsistent graph role");
		auto expected_reason = GraphRetentionReason::NOT_ON_DATA_PATH;
		if (cell.is_node())
			expected_reason = GraphRetentionReason::STATE_NODE;
		else if (cell.role == GraphCellRole::EXCLUDED)
			expected_reason = GraphRetentionReason::EXCLUDED_TYPE;
		else if (cell.in_graph)
			expected_reason = GraphRetentionReason::DATA_PATH;
		require(cell.retention_reason == expected_reason, "has an inconsistent retention reason");
		graph->cell_ids[cell.cell] = cell.id;
		if (cell.is_node()) {
			require(cell.in_graph, "has an excluded NODE");
			graph->nodes.push_back(cell.id);
		} else if (cell.in_graph) {
			require(cell.is_edge(), "retains an EXCLUDED cell");
			graph->edge_cells.push_back(cell.id);
		}
		graph->cells.push_back(std::move(cell));
	}
	std::set<std::pair<int, std::string>> seen_ports;
	int port_count = GetSize(module->ports);
	for (auto cell : module->cells())
		port_count += GetSize(cell->connections());
	require(GetSize(array(graph_record["ports"])) == port_count, "has an inconsistent port count");
	for (const auto &item : array(graph_record["ports"])) {
		const auto &entry = tuple(item, 5);
		GraphPort port;
		port.id = GetSize(graph->ports);
		port.cell = integer(entry[0], -1, GetSize(graph->cells) - 1);
		port.name = identifier(entry[1]);
		port.direction = RTLIL::PortDir(integer(entry[2], 0, 3));
		port.raw_signal = bind_signal(entry[3], module);
		port.canonical_signal = bind_signal(entry[4], module);
		require(seen_ports.insert({port.cell, port.name.str()}).second, "contains duplicate ports");
		if (port.is_cell_port()) {
			auto &cell = graph->cells[port.cell];
			require(cell.cell->hasPort(port.name) && cell.cell->port_dir(port.name) == port.direction &&
					cell.cell->getPort(port.name) == port.raw_signal, "has a changed cell port binding");
			if (port.is_input()) cell.inputs.push_back(port.id);
			if (port.is_output()) cell.outputs.push_back(port.id);
		} else {
			port.module_port = module->wire(port.name);
			require(port.module_port && std::find(module->ports.begin(), module->ports.end(), port.name) != module->ports.end(),
					"references a missing module port");
			require(port.raw_signal == RTLIL::SigSpec(port.module_port) &&
					port.is_input() == port.module_port->port_input && port.is_output() == port.module_port->port_output,
					"has a changed module port binding");
			if (port.is_input()) graph->module_inputs.push_back(port.id);
			if (port.is_output()) graph->module_outputs.push_back(port.id);
		}
		require(graph->sigmap(port.raw_signal) == graph->sigmap(port.canonical_signal), "has a changed canonical binding");
		graph->ports.push_back(std::move(port));
	}
	std::set<std::pair<int, int>> seen_connections;
	for (const auto &item : array(graph_record["connections"])) {
		const auto &entry = tuple(item, 3);
		GraphConnection connection{GetSize(graph->connections), integer(entry[0], 0, GetSize(graph->ports) - 1),
				integer(entry[1], 0, GetSize(graph->ports) - 1), integer(entry[2], 1)};
		const auto &src = graph->ports[connection.src], &dst = graph->ports[connection.dst];
		require(src.drives_net() && dst.consumes_net() && connection.src != connection.dst &&
				(!src.is_cell_port() || graph->cells[src.cell].in_graph) &&
				(!dst.is_cell_port() || graph->cells[dst.cell].in_graph), "has an invalid retained connection");
		require(seen_connections.insert({connection.src, connection.dst}).second, "contains duplicate connections");
		graph->connections.push_back(connection);
	}
	graph->index_connections();
	validate_selection(*graph, selection);
	auto view = graph->create_view(selection);
	return RestoredAnalysis{std::move(graph), selection, std::move(view), analysis_id, strategy};
}

void SnapshotStore::invalidate(RTLIL::Design *design)
{
	design->scratchpad_unset(key);
}

void SnapshotStore::publish(RTLIL::Design *design, const std::vector<AnalysisSnapshot> &batch)
{
	Json::array records;
	std::set<std::string> modules;
	for (const auto &snapshot : batch) {
		require(modules.insert(snapshot.module_name).second, "contains duplicate modules");
		records.push_back(snapshot.to_json());
	}
	design->scratchpad_set_string(key, Json(Json::object{{"version", AnalysisSnapshot::version}, {"modules", records}}).dump());
}

AnalysisSnapshot SnapshotStore::load(RTLIL::Design *design, RTLIL::IdString module_name)
{
	auto encoded = design->scratchpad_get_string(key);
	require(!encoded.empty(), "is missing");
	auto batch = parse(encoded);
	require(integer(batch["version"]) == AnalysisSnapshot::version, "has an unsupported batch version");
	std::set<std::string> seen;
	AnalysisSnapshot result;
	bool found = false;
	for (const auto &record : array(batch["modules"])) {
		auto snapshot = AnalysisSnapshot::from_json(record);
		require(seen.insert(snapshot.module_name).second, "contains duplicate modules");
		if (snapshot.module_name == module_name.str()) {
			result = std::move(snapshot);
			found = true;
		}
	}
	require(found, "is missing for the requested module");
	return result;
}

RestoredAnalysis SnapshotStore::restore(RTLIL::Design *design, RTLIL::IdString module_name)
{
	return load(design, module_name).restore(design);
}

} // namespace CellFuzz
YOSYS_NAMESPACE_END
