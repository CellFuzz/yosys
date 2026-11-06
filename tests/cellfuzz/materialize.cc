#include "kernel/register.h"
#include "kernel/mem.h"
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

// Check the exact packed-cell delta independently from the planner/helper
// comparison. Legacy fixtures are normalized only in a private test copy.
static RTLIL::SigSpec remap(RTLIL::SigSpec value, RTLIL::Module *module)
{
	for (auto &bit : value) if (bit.wire) bit.wire = module->wire(bit.wire->name);
	return value;
}

static void check_query_delta(RTLIL::Module *original, RTLIL::Module *module, const Json &memory)
{
	auto name = RTLIL::IdString(memory["name"].string_value());
	// Raw packed input is the oracle. In particular do not emit a gold Mem:
	// using the same normalizer as production would conceal shared mistakes.
	auto before = original->cell(name), after = module->cell(name);
	log_assert(before && after && after->type == ID($mem_v2) && before->attributes == after->attributes);
	int reads = before->getParam(ID::RD_PORTS).as_int(), writes = before->getParam(ID::WR_PORTS).as_int();
	int width = before->getParam(ID::WIDTH).as_int(), old_abits = before->getParam(ID::ABITS).as_int();
	const auto &query = memory["query"];
	int64_t offset = before->getParam(ID::OFFSET).as_int(), size = before->getParam(ID::SIZE).as_int();
	log_assert(offset >= 0 && size > 0);
	int address_width = 1;
	while ((offset + size - 1) >> address_width) address_width++;
	log_assert(query["address_width"].int_value() == address_width && query["data_width"].int_value() == width);
	log_assert(module->wire(RTLIL::escape_id(query["address_port"].string_value()))->width == address_width);
	log_assert(module->wire(RTLIL::escape_id(query["data_port"].string_value()))->width == width);
	int abits = std::max(old_abits, address_width);
	auto params = before->parameters;
	if (before->type == ID($mem)) {
		// Legacy migration per the documented $mem contract: no wide lanes or
		// resets/init on the read FFs; transparency only for matching clocked
		// domains, and later writes have priority within the same domain.
		params.erase(ID::RD_TRANSPARENT);
		for (auto id : {ID::RD_WIDE_CONTINUATION, ID::RD_CE_OVER_SRST}) params[id] = RTLIL::Const(0, std::max(reads, 1));
		for (auto id : {ID::RD_INIT_VALUE, ID::RD_ARST_VALUE, ID::RD_SRST_VALUE})
			params[id] = reads ? RTLIL::Const(RTLIL::State::Sx, reads * width) : RTLIL::Const(0, 1);
		params[ID::WR_WIDE_CONTINUATION] = RTLIL::Const(0, std::max(writes, 1));
		params[ID::RD_COLLISION_X_MASK] = RTLIL::Const(0, std::max(reads * writes, 1));
		RTLIL::Const transparency(0, std::max(reads * writes, 1)), priority(0, std::max(writes * writes, 1));
		auto same_domain = [&](bool read, int a, int b) {
			auto enabled = before->getParam(read ? ID::RD_CLK_ENABLE : ID::WR_CLK_ENABLE)[a];
			auto other_enabled = before->getParam(ID::WR_CLK_ENABLE)[b];
			if (enabled != other_enabled) return false;
			return enabled != RTLIL::State::S1 ||
					(before->getParam(read ? ID::RD_CLK_POLARITY : ID::WR_CLK_POLARITY)[a] == before->getParam(ID::WR_CLK_POLARITY)[b] &&
					before->getPort(read ? ID::RD_CLK : ID::WR_CLK)[a] == before->getPort(ID::WR_CLK)[b]);
		};
		for (int r = 0; r < reads; r++) for (int w = 0; w < writes; w++)
			if (before->getParam(ID::RD_TRANSPARENT)[r] == RTLIL::State::S1 &&
					before->getParam(ID::RD_CLK_ENABLE)[r] == RTLIL::State::S1 && same_domain(true, r, w))
				transparency.set(r * writes + w, RTLIL::State::S1);
		for (int a = 0; a < writes; a++) for (int b = 0; b < a; b++)
			if (same_domain(false, a, b)) priority.set(a * writes + b, RTLIL::State::S1);
		params[ID::RD_TRANSPARENCY_MASK] = transparency;
		params[ID::WR_PRIORITY_MASK] = priority;
	}
	params[ID::ABITS] = RTLIL::Const(abits);
	params[ID::RD_PORTS] = RTLIL::Const(reads + 1);
	for (auto id : {ID::RD_CLK_ENABLE, ID::RD_CLK_POLARITY, ID::RD_CE_OVER_SRST, ID::RD_WIDE_CONTINUATION,
			ID::RD_TRANSPARENCY_MASK, ID::RD_COLLISION_X_MASK, ID::RD_INIT_VALUE, ID::RD_ARST_VALUE, ID::RD_SRST_VALUE}) {
		int unit = id.in(ID::RD_TRANSPARENCY_MASK, ID::RD_COLLISION_X_MASK) ? writes :
				id.in(ID::RD_INIT_VALUE, ID::RD_ARST_VALUE, ID::RD_SRST_VALUE) ? width : 1;
		auto value = params.at(id).extract(0, reads * unit);
		value.append(RTLIL::Const(unit == width && id.in(ID::RD_INIT_VALUE, ID::RD_ARST_VALUE, ID::RD_SRST_VALUE) ?
				RTLIL::State::Sx : id == ID::RD_CLK_POLARITY ? RTLIL::State::S1 : RTLIL::State::S0, unit));
		if (value.empty()) value = RTLIL::State::S0; // zero writers mask sentinel
		params[id] = value;
	}
	for (const auto &entry : params) {
		if (entry.second != after->getParam(entry.first))
			log_error("raw query parameter mismatch: %s\n", log_id(entry.first));
	}
	log_assert(params == after->parameters);
	auto connections = before->connections();
	if (before->type == ID($mem)) {
		connections[ID::RD_ARST] = RTLIL::SigSpec(RTLIL::State::S0, reads);
		connections[ID::RD_SRST] = RTLIL::SigSpec(RTLIL::State::S0, reads);
	}
	for (auto id : {ID::RD_ADDR, ID::WR_ADDR}) {
		RTLIL::SigSpec value;
		int count = id == ID::RD_ADDR ? reads : writes;
		for (int lane = 0; lane < count; lane++) {
			auto address = remap(before->getPort(id).extract(lane * old_abits, old_abits), module);
			address.append(RTLIL::SigSpec(RTLIL::State::S0, abits - old_abits));
			value.append(address);
		}
		connections[id] = value;
	}
	for (auto &connection : connections) connection.second = remap(connection.second, module);
	auto addr = RTLIL::SigSpec(module->wire(RTLIL::escape_id(query["address_port"].string_value())));
	addr.append(RTLIL::SigSpec(RTLIL::State::S0, abits - GetSize(addr)));
	connections[ID::RD_ADDR].append(addr);
	connections[ID::RD_DATA].append(module->wire(RTLIL::escape_id(query["data_port"].string_value())));
	connections[ID::RD_CLK].append(RTLIL::State::Sx);
	connections[ID::RD_EN].append(RTLIL::State::S1);
	connections[ID::RD_ARST].append(RTLIL::State::S0);
	connections[ID::RD_SRST].append(RTLIL::State::S0);
	for (const auto &entry : connections) {
		if (entry.second != after->getPort(entry.first))
			log_error("raw query connection mismatch: %s\n", log_id(entry.first));
	}
	log_assert(connections == after->connections());
}

struct FaultMonitor : RTLIL::Monitor {
	std::string mode, filename;
	bool hit = false;
	void notify_connect(RTLIL::Cell *cell, RTLIL::IdString name, const RTLIL::SigSpec &, const RTLIL::SigSpec &signal) override
	{
		if (hit || mode.compare(0, 6, "query-") != 0 || name != ID::RD_DATA) return;
		hit = true;
		log_assert(cell->module->design->scratchpad_get_string(SnapshotStore::key).empty());
		log_assert(!cell->module->get_bool_attribute(RTLIL::escape_id("cellfuzz_instrumented")));
		if (mode == "query-interrupt") log_cmd_error("test: interrupted during query memory emit.\n");
		if (mode == "query-corrupt") const_cast<RTLIL::SigSpec &>(signal)[GetSize(signal) - 1] = RTLIL::State::S0;
		if (mode == "query-rename") {
			log_assert(std::rename(filename.c_str(), (filename + ".old").c_str()) == 0);
			log_assert(mkdir(filename.c_str(), 0700) == 0);
		}
	}
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
		if (GetSize(args) == 4 && args[1] == "raw-fault") {
			auto module = design->top_module();
			RTLIL::Design gold;
			auto original = module->clone(); gold.add(original);
			Pass::call(design, {"cellinstrument", "-manifest", args[3], "-mem-content", "query"});
			auto report = read_report(args[3]);
			const auto &memory = report["memories"][0];
			auto cell = module->cell(RTLIL::IdString(memory["name"].string_value()));
			if (args[2] == "query-control") {
				auto value = cell->getPort(ID::RD_EN);
				value[GetSize(value)-1] = RTLIL::State::S0;
				cell->setPort(ID::RD_EN, value);
			} else {
				auto param = args[2] == "init" ? ID::INIT : ID::RD_COLLISION_X_MASK;
				log_assert(args[2] == "init" || args[2] == "business-mask");
				auto value = cell->getParam(param);
				int bit = args[2] == "init" ? 0 : cell->getParam(ID::WR_PORTS).as_int();
				value.set(bit, value[bit] == RTLIL::State::S1 ? RTLIL::State::S0 : RTLIL::State::S1);
				cell->setParam(param, value);
			}
			check_query_delta(original, module, memory);
			log_error("raw fault survived\n");
		}
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
		RTLIL::Design original_design;
		auto original = module->clone(); original_design.add(original);
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
		if (mode == "rename" || mode == "query-rename") {
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
		for (const auto &memory : report["memories"].array_items()) {
			if (!memory["content_query"].bool_value()) continue;
			check_query_delta(original, module, memory);
			for (auto field : {"address_port", "data_port"}) {
				auto wire = module->wire(RTLIL::escape_id(memory["query"][field].string_value()));
				log_assert(wire && wire->port_id == GetSize(original_ports) + ++index);
				log_assert(wire->port_input == (std::string(field) == "address_port") && wire->port_output != wire->port_input);
				log_assert(wire->width == memory["query"][std::string(field) == "address_port" ? "address_width" : "data_width"].int_value());
			}
		}
		log_assert(module->ports.size() == original_ports.size() + index);
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
			auto cell = original->cell(RTLIL::IdString(item["cell"].string_value()));
			log_assert(cell);
			auto raw = cell->getPort(RTLIL::IdString(item["port"].string_value()));
			int offset = item["port_lsb"].int_value();
			log_assert(offset >= 0 && offset <= GetSize(raw) && GetSize(expected) <= GetSize(raw) - offset);
			log_assert(remap(raw.extract(offset, GetSize(expected)), module) == expected);
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
		for (const auto &memory : report["memories"].array_items()) {
			if (!memory["content_query"].bool_value()) continue;
			auto name = RTLIL::IdString(memory["name"].string_value());
			auto before = original->cell(name), after = stripped->cell(name);
			after->type = before->type; after->parameters = before->parameters; after->attributes = before->attributes;
			auto ports = before->connections();
			for (auto &connection : ports) connection.second = remap(connection.second, stripped);
			auto old_connections = after->connections();
			for (const auto &connection : old_connections) after->unsetPort(connection.first);
			for (const auto &connection : ports) after->setPort(connection.first, connection.second);
			for (auto field : {"address_port", "data_port"})
				remove.insert(stripped->wire(RTLIL::escape_id(memory["query"][field].string_value())));
		}
		stripped->remove(remove);
		stripped->attributes.erase(RTLIL::escape_id("cellfuzz_instrumented"));
		stripped->fixup_ports();
		log_assert(digest == structural_digest(stripped));
	}
} TestCellFuzzMaterialize;

PRIVATE_NAMESPACE_END
