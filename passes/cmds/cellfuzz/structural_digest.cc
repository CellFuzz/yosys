#include "analysis_snapshot.h"
#include "libs/sha1/sha1.h"

#include <algorithm>
#include <deque>
#include <set>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {
namespace {
using Json = json11::Json;

static Json constant(const RTLIL::Const &value)
{
	// as_string preserves width and X/Z; flags distinguish string/signed/real
	// parameters from equally sized bit vectors. No host integer conversion.
	return Json::array{int(value.flags), value.as_string("-")};
}

static Json attributes(const dict<RTLIL::IdString, RTLIL::Const> &values)
{
	Json::object result;
	for (const auto &entry : values)
		result[entry.first.str()] = constant(entry.second);
	return result;
}

static Json signal(const RTLIL::SigSpec &value)
{
	Json::array result;
	for (auto bit : value)
		result.push_back(bit.wire ? Json(Json::array{bit.wire->name.str(), bit.offset}) : Json(int(bit.data)));
	return result;
}

static Json actions(const std::vector<RTLIL::SigSig> &values)
{
	Json::array result;
	for (const auto &value : values)
		result.push_back(Json::array{signal(value.first), signal(value.second)});
	return result;
}

static Json case_rule(const RTLIL::CaseRule &value)
{
	Json::array compare, switches;
	for (const auto &sig : value.compare)
		compare.push_back(signal(sig));
	for (auto sw : value.switches) {
		Json::array cases;
		for (auto branch : sw->cases)
			cases.push_back(case_rule(*branch));
		switches.push_back(Json::array{attributes(sw->attributes), signal(sw->signal), cases});
	}
	// Case, switch and action order has priority semantics and must not be sorted.
	return Json::array{attributes(value.attributes), compare, actions(value.actions), switches};
}

static Json module_record(RTLIL::Module *module)
{
	Json::object wires, cells, memories, processes;
	Json::array ports, parameters;
	for (auto name : module->ports)
		ports.push_back(name.str());
	for (auto name : module->avail_parameters)
		parameters.push_back(name.str());
	std::sort(parameters.begin(), parameters.end());
	for (auto wire : module->wires())
		wires[wire->name.str()] = Json::array{wire->width, wire->start_offset, wire->upto, wire->is_signed,
				wire->port_id, wire->port_input, wire->port_output, attributes(wire->attributes)};
	for (auto cell : module->cells()) {
		Json::object connections;
		for (const auto &conn : cell->connections())
			connections[conn.first.str()] = signal(conn.second);
		cells[cell->name.str()] = Json::array{cell->type.str(), attributes(cell->parameters),
				attributes(cell->attributes), connections};
	}
	for (const auto &entry : module->memories) {
		auto mem = entry.second;
		memories[entry.first.str()] = Json::array{mem->width, mem->start_offset, mem->size, attributes(mem->attributes)};
	}
	for (const auto &entry : module->processes) {
		auto process = entry.second;
		Json::array syncs;
		for (auto sync : process->syncs) {
			Json::array writes;
			for (const auto &write : sync->mem_write_actions)
				writes.push_back(Json::array{write.memid.str(), signal(write.address), signal(write.data),
						signal(write.enable), constant(write.priority_mask), attributes(write.attributes)});
			syncs.push_back(Json::array{int(sync->type), signal(sync->signal), actions(sync->actions), writes});
		}
		processes[entry.first.str()] = Json::array{attributes(process->attributes), case_rule(process->root_case), syncs};
	}
	// Module connects are unordered continuous assignments. Sorting whole records
	// preserves the ordered bit references within each side, including duplicates.
	auto connections = actions(module->connections()).array_items();
	std::sort(connections.begin(), connections.end());
	return Json::object{{"name", module->name.str()}, {"attributes", attributes(module->attributes)},
			{"parameters", attributes(module->parameter_default_values)}, {"available_parameters", parameters},
			{"ports", ports}, {"wires", wires}, {"cells", cells}, {"connections", connections},
			{"memories", memories}, {"processes", processes}};
}
} // namespace

std::string structural_digest(RTLIL::Module *module)
{
	log_assert(module != nullptr && module->design != nullptr);
	Json::object definitions;
	std::set<std::string> visited;
	std::deque<RTLIL::Module *> pending{module};
	while (!pending.empty()) {
		auto current = pending.front();
		pending.pop_front();
		if (!visited.insert(current->name.str()).second)
			continue;
		definitions[current->name.str()] = module_record(current);
		for (auto cell : current->cells())
			if (auto dependency = module->design->module(cell->type))
				pending.push_back(dependency);
	}
	return sha1(Json(Json::array{AnalysisSnapshot::version, module->name.str(), definitions}).dump());
}

} // namespace CellFuzz
YOSYS_NAMESPACE_END
