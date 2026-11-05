#include "kernel/register.h"
#include "passes/cmds/cellfuzz/manifest.h"

#include <fstream>
#include <functional>
#include <climits>

USING_YOSYS_NAMESPACE
using namespace Yosys::CellFuzz;
PRIVATE_NAMESPACE_BEGIN

static void rejects(const std::function<void()> &action)
{
	bool failed = false;
	{
		auto errors = logger().error_throw_scope();
		try { action(); } catch (const log_cmd_error_exception &) { failed = true; }
	}
	log_assert(failed);
}

// Read live RTLIL independently of the manifest value encoder. Store each
// constant as low-bit-first State integers, so strings/widths are not copied
// through the production constant serialization helper.
static json11::Json metadata(const dict<RTLIL::IdString, RTLIL::Const> &values)
{
	json11::Json::object result;
	for (const auto &value : values) {
		std::vector<int> bits;
		for (int index = 0; index < GetSize(value.second); index++) bits.push_back(int(value.second[index]));
		result[value.first.str()] = json11::Json::array{bits, int(value.second.flags)};
	}
	return result;
}

static void fixture(RTLIL::Design *design, bool reverse)
{
	auto module = design->addModule(RTLIL::escape_id("manifest"));
	module->set_bool_attribute(ID::top);
	module->attributes[ID::src] = RTLIL::Const("fixture:\"quoted\"\nline two");
	module->parameter_default_values[RTLIL::escape_id("WIDE")] = RTLIL::Const::from_string(std::string(80, '1') + "xz");
	auto data = module->addWire(RTLIL::IdString("\\data.\"[0]\\tail"), 513);
	data->port_input = true; data->start_offset = 7; data->upto = true; data->is_signed = true;
	auto alias = module->addWire(RTLIL::escape_id("alias"), 513);
	module->connect(alias, data);
	// Public names and all object declarations are identical across the two
	// fixtures. Only cell insertion order differs, so export IDs must match.
	std::vector<RTLIL::Wire *> outputs;
	for (int width : {1, 8, 32, 33, 64, 65, 256, 513}) {
		auto output = module->addWire(RTLIL::escape_id("out" + std::to_string(width)), width);
		output->port_output = true; outputs.push_back(output);
	}
	std::vector<int> order{0, 1, 2, 3, 4, 5, 6, 7};
	if (reverse) std::reverse(order.begin(), order.end());
	for (int index : order) {
		auto cell = module->addNot(RTLIL::escape_id("logic" + std::to_string(outputs[index]->width)),
				RTLIL::SigSpec(alias).extract(0, outputs[index]->width), outputs[index]);
		if (index == 3) cell->attributes[ID::src] = RTLIL::Const("rtl:3.1-4.2");
	}
	auto selected = module->addWire(RTLIL::escape_id("mux_out"), 8); selected->port_output = true;
	RTLIL::SigSpec selectors;
	selectors.append(RTLIL::SigBit(data, 3)); selectors.append(RTLIL::SigBit(alias, 1));
	selectors.append(RTLIL::State::Sx); selectors.append(RTLIL::SigBit(alias, 1)); selectors.append(RTLIL::State::Sz);
	module->addPmux(RTLIL::escape_id("mux.\"name"), RTLIL::SigSpec(data).extract(0, 8),
			RTLIL::SigSpec(data).extract(0, 40), selectors, selected);
	// Independent item identities must survive exact source aliasing.
	auto aliased = module->addWire(RTLIL::escape_id("aliased_out"), 1);
	aliased->port_output = true; module->connect(aliased, outputs.front());
	module->addNot(RTLIL::escape_id("aliased_cell"), RTLIL::SigBit(data, 0), aliased);
	auto unused = module->addWire(RTLIL::escape_id("unused"), 8);
	module->addNot(RTLIL::escape_id("dead"), RTLIL::SigSpec(data).extract(0, 8), unused);
	auto clk = module->addWire(RTLIL::escape_id("clk")); clk->port_input = true;
	auto q = module->addWire(RTLIL::escape_id("q"), 8); q->port_output = true;
	q->attributes[ID::init] = RTLIL::Const::from_string("10x00101");
	module->addDff(RTLIL::escape_id("register"), clk, RTLIL::SigSpec(data).extract(0, 8), q);
	module->fixup_ports();
}

struct TestCellFuzzManifest : Pass {
	TestCellFuzzManifest() : Pass("test_cellfuzz_manifest", "verify static observation layout") {}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		if (GetSize(args) >= 3 && (args[1] == "unchanged" || args[1] == "failure")) {
			auto before = design->scratchpad_get_string(SnapshotStore::key);
			auto digest = structural_digest(design->top_module());
			auto command = [&] { Pass::call(design, std::vector<std::string>(args.begin() + 2, args.end())); };
			if (args[1] == "failure") rejects(command); else command();
			log_assert(before == design->scratchpad_get_string(SnapshotStore::key));
			log_assert(digest == structural_digest(design->top_module()));
			return;
		}
		if (GetSize(args) == 2 && (args[1] == "fixture" || args[1] == "fixture-reverse")) {
			fixture(design, args[1] == "fixture-reverse"); return;
		}
		if (GetSize(args) == 3 && args[1] == "conflict") {
			auto module = design->top_module();
			auto name = RTLIL::escape_id("cf_obs_0000");
			if (args[2] == "wire") module->addWire(name);
			else if (args[2] == "cell")
				module->addNot(name, RTLIL::State::S0, module->addWire(NEW_ID));
			else if (args[2] == "parameter") module->parameter_default_values[name] = RTLIL::Const(1);
			else if (args[2] == "available") module->avail_parameters(name);
			else log_abort();
			return;
		}
		if (GetSize(args) == 2 && args[1] == "empty") {
			auto analysis = SnapshotStore::restore(design, design->top_module()->name);
			SnapshotStore::publish(design, {AnalysisSnapshot::capture(*analysis.graph, CellGraphSelection{}, SelectionStrategy{"empty", 1, {}})});
			return;
		}
		if (GetSize(args) == 3 && args[1] == "oracle") {
			auto snapshot = SnapshotStore::load(design, design->top_module()->name);
			std::string error;
			auto oracle = json11::Json::parse(snapshot.encode(), error).object_items();
			log_assert(error.empty());
			auto module = design->top_module();
			json11::Json::object cell_values, wire_values;
			for (auto cell : module->cells())
				cell_values[cell->name.str()] = json11::Json::array{metadata(cell->parameters), metadata(cell->attributes)};
			for (auto wire : module->wires()) wire_values[wire->name.str()] = metadata(wire->attributes);
			oracle["metadata"] = json11::Json::object{{"module", json11::Json::array{metadata(module->parameter_default_values), metadata(module->attributes)}},
					{"cells", cell_values}, {"wires", wire_values}};
			std::ofstream out(args[2]); out << json11::Json(oracle).dump(); out.close();
			log_assert(bool(out));
			return;
		}
		extra_args(args, 1, design);
		auto analysis = SnapshotStore::restore(design, design->top_module()->name);
		auto plan = create_monitor_plan(analysis, MonitorOptions{});
		auto layout = create_transport(plan, analysis.graph->module);
		validate_transport(plan, layout, analysis.graph->module);
		log_assert(!layout.segments.empty());
		auto invalid = layout;
		invalid.segments.front().item_lsb++;
		rejects([&] { validate_transport(plan, invalid, analysis.graph->module); });
		invalid = layout; invalid.segments.push_back(invalid.segments.front());
		rejects([&] { validate_transport(plan, invalid, analysis.graph->module); });
		invalid = layout; invalid.segments.front().width = 0;
		rejects([&] { validate_transport(plan, invalid, analysis.graph->module); });
		invalid = layout; invalid.ports.front().width++;
		rejects([&] { validate_transport(plan, invalid, analysis.graph->module); });
		invalid = layout; invalid.segments.front().port_lsb = INT_MAX;
		rejects([&] { validate_transport(plan, invalid, analysis.graph->module); });
		invalid = layout; invalid.segments.front().monitor_id = INT_MAX;
		rejects([&] { validate_transport(plan, invalid, analysis.graph->module); });
		invalid = layout; invalid.segments.front().port_name = "missing";
		rejects([&] { validate_transport(plan, invalid, analysis.graph->module); });
		invalid = layout; invalid.total_bits++;
		rejects([&] { validate_transport(plan, invalid, analysis.graph->module); });
		invalid = layout; invalid.ports.push_back(invalid.ports.front());
		rejects([&] { validate_transport(plan, invalid, analysis.graph->module); });
		invalid = layout; std::reverse(invalid.segments.begin(), invalid.segments.end());
		validate_transport(plan, invalid, analysis.graph->module);
		if (plan.items.size() > 1) {
			invalid = layout; invalid.segments.front().monitor_id = 1;
			rejects([&] { validate_transport(plan, invalid, analysis.graph->module); });
		}
		// Preserve complete source coverage but overlap two destination ranges.
		for (size_t index = 1; index < layout.segments.size(); index++) {
			if (layout.segments[index].port_name != layout.segments[index - 1].port_name) continue;
			invalid = layout;
			invalid.segments[index].port_lsb = invalid.segments[index - 1].port_lsb;
			rejects([&] { validate_transport(plan, invalid, analysis.graph->module); });
			break;
		}
		for (int fault = 0; fault < 4; fault++) {
			auto broken = plan;
			if (fault == 0) broken.items.front().cell = INT_MAX;
			if (fault == 1) broken.items.front().port = INT_MAX;
			if (fault == 2) broken.items.front().source[0] = RTLIL::State::Sm;
			if (fault == 3) broken.items.front().cell = (broken.items.front().cell + 1) % GetSize(analysis.graph->cells);
			rejects([&] { planned_manifest(analysis, broken, layout); });
		}
	}
} TestCellFuzzManifest;

PRIVATE_NAMESPACE_END
