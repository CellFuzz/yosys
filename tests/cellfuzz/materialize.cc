#include "kernel/register.h"
#include "passes/cmds/cellfuzz/instrument.h"
#include "passes/cmds/cellfuzz/manifest.h"

#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

USING_YOSYS_NAMESPACE
using namespace Yosys::CellFuzz;
PRIVATE_NAMESPACE_BEGIN
using Json = json11::Json;

static bool fails(const std::function<void()> &action)
{
	auto errors = logger().error_throw_scope();
	try { action(); } catch (const log_cmd_error_exception &) { return true; }
	return false;
}

static RTLIL::SigSpec source(const Json &record, RTLIL::Module *module)
{
	RTLIL::SigSpec result;
	for (const auto &bit : record.array_items()) {
		if (bit.is_number()) result.append(RTLIL::State(bit.int_value()));
		else {
			auto wire = module->wire(RTLIL::IdString(bit[0].string_value()));
			log_assert(wire && bit[1].int_value() >= 0 && bit[1].int_value() < wire->width);
			result.append(RTLIL::SigBit(wire, bit[1].int_value()));
		}
	}
	return result;
}

static Json read_report(const std::string &filename)
{
	std::ifstream file(filename);
	std::string content((std::istreambuf_iterator<char>(file)), {}), error;
	auto result = Json::parse(content, error);
	log_assert(error.empty());
	return result;
}

struct FaultMonitor : RTLIL::Monitor {
	std::string mode, filename;
	bool hit = false;
	void notify_connect(RTLIL::Module *module, const RTLIL::SigSig &conn) override
	{
		if (hit) return;
		hit = true;
		log_assert(module->design->scratchpad_get_string(SnapshotStore::key).empty());
		log_assert(!module->get_bool_attribute(RTLIL::escape_id("cellfuzz_instrumented")));
		if (mode == "interrupt") log_cmd_error("test: interrupted at first bypass connection.\n");
		if (mode == "corrupt") conn.first[0].wire->port_input = true;
		if (mode == "rename") {
			log_assert(std::rename(filename.c_str(), (filename + ".old").c_str()) == 0);
			log_assert(mkdir(filename.c_str(), 0700) == 0);
		}
	}
};

struct TestCellFuzzMaterialize : Pass {
	TestCellFuzzMaterialize() : Pass("test_cellfuzz_materialize", "verify actual observation interfaces and failures") {}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		if (GetSize(args) == 2 && args[1] == "invalid") {
			auto analysis = SnapshotStore::restore(design, design->top_module()->name);
			auto plan = create_monitor_plan(analysis, MonitorOptions{});
			auto layout = create_transport(plan, analysis.graph->module);
			auto batch = design->scratchpad_get_string(SnapshotStore::key);
			auto digest = structural_digest(analysis.graph->module);
			auto invalid = layout;
			invalid.segments.front().port_lsb = -1;
			log_assert(fails([&] { materialize_observations(analysis.graph->module, plan, invalid); }));
			auto broken = plan;
			broken.items.front().source[0] = RTLIL::SigBit(analysis.graph->module->wire(ID::a), -1);
			log_assert(fails([&] { materialize_observations(analysis.graph->module, broken, layout); }));
			invalid = layout; invalid.ports.front().width = RTLIL::WIDTH_LIMIT;
			broken.options.chunk_width = RTLIL::WIDTH_LIMIT;
			log_assert(fails([&] { validate_transport(broken, invalid, analysis.graph->module); }));
			log_assert(batch == design->scratchpad_get_string(SnapshotStore::key));
			log_assert(digest == structural_digest(analysis.graph->module));
			return;
		}
		log_assert(GetSize(args) >= 5);
		auto mode = args[1], filename = args[2];
		auto module = design->top_module();
		auto digest = structural_digest(module);
		auto batch = design->scratchpad_get_string(SnapshotStore::key);
		auto original_ports = module->ports;
		int original_connections = GetSize(module->connections());
		FaultMonitor monitor;
		monitor.mode = mode; monitor.filename = filename;
		if (mode != "success" && mode != "preflight") module->monitors.insert(&monitor);
		bool failed = fails([&] { Pass::call(design, std::vector<std::string>(args.begin() + 3, args.end())); });
		module->monitors.erase(&monitor);
		log_assert(failed == (mode != "success"));
		if (mode == "rename") {
			log_assert(rmdir(filename.c_str()) == 0);
			log_assert(std::rename((filename + ".old").c_str(), filename.c_str()) == 0);
		}
		if (mode == "preflight") {
			log_assert(batch == design->scratchpad_get_string(SnapshotStore::key));
			log_assert(digest == structural_digest(module));
			return;
		}
		log_assert(design->scratchpad_get_string(SnapshotStore::key).empty());
		if (mode != "success") {
			log_assert(monitor.hit && digest != structural_digest(module));
			log_assert(!module->get_bool_attribute(RTLIL::escape_id("cellfuzz_instrumented")));
			// A failed mutation may deliberately leave an incomplete interface.
			// Follow the public discard contract instead of returning it to the
			// pass manager's normal successful-design invariant checks.
			design->remove(module);
			return;
		}
		log_assert(module->get_bool_attribute(RTLIL::escape_id("cellfuzz_instrumented")));
		log_assert(fails([&] { SnapshotStore::restore(design, module->name); }));
		auto report = read_report(filename);
		log_assert(report["status"] == "materialized");
		pool<RTLIL::Wire *> outputs;
		int index = 0;
		for (const auto &port : report["transport"]["ports"].array_items()) {
			auto wire = module->wire(RTLIL::escape_id(port["name"].string_value()));
			log_assert(wire && wire->width == port["width"].int_value() && wire->port_output && !wire->port_input);
			log_assert(wire->port_id == GetSize(original_ports) + ++index);
			outputs.insert(wire);
		}
		log_assert(module->ports.size() == original_ports.size() + outputs.size());
		for (int i = 0; i < GetSize(original_ports); i++) log_assert(module->ports[i] == original_ports[i]);
		// Compare raw continuous assignments, without using the production
		// alias check. An accidental source short must not conceal a bad bind.
		std::map<RTLIL::SigBit, RTLIL::SigBit> actual;
		for (int i = original_connections; i < GetSize(module->connections()); i++) {
			const auto &conn = module->connections()[i];
			log_assert(GetSize(conn.first) == GetSize(conn.second));
			for (int bit = 0; bit < GetSize(conn.first); bit++) {
				log_assert(outputs.count(conn.first[bit].wire));
				log_assert(actual.emplace(conn.first[bit], conn.second[bit]).second);
			}
		}
		int covered = 0;
		for (const auto &segment : report["transport"]["segments"].array_items()) {
			auto item = report["items"][segment["monitor_id"].int_value()];
			auto expected = source(item["source"], module);
			auto cell = module->cell(RTLIL::IdString(item["cell"].string_value()));
			log_assert(cell && cell->getPort(RTLIL::IdString(item["port"].string_value())) == expected);
			auto wire = module->wire(RTLIL::escape_id(segment["port_name"].string_value()));
			for (int bit = 0; bit < segment["width"].int_value(); bit++) {
				log_assert(actual.at(RTLIL::SigBit(wire, segment["port_lsb"].int_value() + bit)) == expected[segment["item_lsb"].int_value() + bit]);
				covered++;
			}
		}
		log_assert(covered == int(actual.size()) && covered == report["transport"]["total_bits"].int_value());
		// Remove only the declared delta from a clone. Its digest must equal
		// the entire original module, including parameters, init and controls.
		RTLIL::Design copy;
		auto stripped = module->clone(); copy.add(stripped);
		auto connections = stripped->connections(); connections.resize(original_connections);
		stripped->new_connections(connections);
		pool<RTLIL::Wire *> remove;
		for (auto wire : outputs) remove.insert(stripped->wire(wire->name));
		stripped->remove(remove);
		stripped->attributes.erase(RTLIL::escape_id("cellfuzz_instrumented"));
		stripped->fixup_ports();
		log_assert(digest == structural_digest(stripped));
	}
} TestCellFuzzMaterialize;

PRIVATE_NAMESPACE_END
