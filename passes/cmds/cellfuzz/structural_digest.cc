#include "analysis_snapshot.h"
#include "libs/sha1/sha1.h"
#include "kernel/topo_scc.h"

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

// Complete integer node range, including isolated modules. TopoSortedSccs uses
// explicit DFS/component stacks, so deep module chains do not recurse in C++.
struct ModuleDependencies {
	using node_type = int;
	using node_enumerator = IntGraph::node_enumerator;
	struct successor_enumerator {
		std::vector<int>::const_iterator current, end;
		bool finished() const { return current == end; }
		int next() { return *current++; }
	};
	std::vector<std::vector<int>> edges;
	std::vector<int> indices;
	explicit ModuleDependencies(int size) : edges(size), indices(size, -1) {}
	node_enumerator enumerate_nodes() { return {0, GetSize(edges)}; }
	successor_enumerator enumerate_successors(int node) { return {edges[node].begin(), edges[node].end()}; }
	int &dfs_index(int node) { return indices[node]; }
};
} // namespace

StructuralDigestContext::StructuralDigestContext(RTLIL::Design *design, const std::vector<RTLIL::Module *> &selected_roots) : design(design)
{
	log_assert(design != nullptr);
	for (auto root : selected_roots) {
		if (!root || root->design != design || design->module(root->name) != root)
			log_cmd_error("cellfuzz: structural digest context has a root from another Design.\n");
		roots.emplace(root->name.str(), root);
	}
	std::map<std::string, RTLIL::Module *> reachable;
	std::deque<RTLIL::Module *> pending;
	for (const auto &root : roots) pending.push_back(root.second);
	while (!pending.empty()) {
		auto current = pending.front();
		pending.pop_front();
		if (!reachable.emplace(current->name.str(), current).second)
			continue;
		for (auto cell : current->cells())
			if (auto dependency = design->module(cell->type))
				pending.push_back(dependency);
	}
	std::vector<RTLIL::Module *> modules;
	std::map<std::string, int> ids;
	for (const auto &entry : reachable) {
		ids.emplace(entry.first, GetSize(modules));
		modules.push_back(entry.second);
	}
	ModuleDependencies graph(GetSize(modules));
	std::vector<std::string> local_hashes, component_hashes(modules.size());
	for (int index = 0; index < GetSize(modules); index++) {
		auto module = modules[index];
		local_hashes.push_back(sha1(Json(Json::array{"cellfuzz-local-v2", module_record(module)}).dump()));
		local_records++;
		std::set<int> dependencies;
		for (auto cell : module->cells())
			if (auto dependency = design->module(cell->type)) dependencies.insert(ids.at(dependency->name.str()));
		graph.edges[index].assign(dependencies.begin(), dependencies.end());
	}
	// Names/IDs here are byte-ordered IdString::str(), never intern IDs or
	// pointers. Length-delimited JSON arrays and versioned domains define:
	// L(m)=H([local-v2, record(m)]), C(s)=H([component-v2, members, outgoing]),
	// D(r)=H([root-v2, name(r), C(s)]). SCC traversal IDs never enter the hash.
	// Internal edges remain in local cell types; external edges name each target
	// definition and its already computed component hash, once per component.
	std::vector<int> components(modules.size(), -1);
	int component_index = 0;
	auto compute = [&](int *begin, int *end) {
		std::sort(begin, end);
		for (auto member = begin; member != end; ++member) components[*member] = component_index;
		Json::array members, outgoing;
		std::set<int> dependencies;
		for (auto member = begin; member != end; ++member) {
			members.push_back(Json::array{modules[*member]->name.str(), local_hashes[*member]});
			for (auto dependency : graph.edges[*member])
				if (components[dependency] != component_index) dependencies.insert(dependency);
		}
		for (auto dependency : dependencies) {
			log_assert(!component_hashes[dependency].empty());
			outgoing.push_back(Json::array{modules[dependency]->name.str(), component_hashes[dependency]});
		}
		auto hash = sha1(Json(Json::array{"cellfuzz-component-v2", members, outgoing}).dump());
		for (auto member = begin; member != end; ++member) component_hashes[*member] = hash;
		component_index++;
	};
	TopoSortedSccs<ModuleDependencies, decltype(compute)>(graph, compute).process_all();
	for (const auto &root : roots)
		digests[root.first] = sha1(Json(Json::array{"cellfuzz-root-v2", root.first, component_hashes[ids.at(root.first)]}).dump());
}

std::string StructuralDigestContext::digest(RTLIL::Module *module) const
{
	if (!module || module->design != design || design->module(module->name) != module ||
			!roots.count(module->name.str()) || roots.at(module->name.str()) != module)
		log_cmd_error("cellfuzz: structural digest context does not belong to this Design/root.\n");
	return digests.at(module->name.str());
}

std::string structural_digest(RTLIL::Module *module)
{
	log_assert(module != nullptr && module->design != nullptr);
	return StructuralDigestContext(module->design, {module}).digest(module);
}

} // namespace CellFuzz
YOSYS_NAMESPACE_END
