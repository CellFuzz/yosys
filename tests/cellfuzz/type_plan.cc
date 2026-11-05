#include "kernel/register.h"
#include "kernel/newcelltypes.h"
#include "passes/cmds/cellfuzz/monitor_plan.h"

#include <functional>
#include <set>

USING_YOSYS_NAMESPACE
using namespace Yosys::CellFuzz;
PRIVATE_NAMESPACE_BEGIN

static RTLIL::Wire *port(RTLIL::Module *module, const std::string &name, int width, bool input = false)
{
	auto wire = module->addWire(RTLIL::escape_id(name), width);
	wire->port_input = input;
	wire->port_output = !input;
	return wire;
}

static void fixture(RTLIL::Design *design)
{
	auto module = design->addModule(RTLIL::escape_id("types"));
	module->set_bool_attribute(ID::top);
	auto data = port(module, "data", 65, true);
	auto clk = port(module, "clock", 1, true);
	auto ctrl = port(module, "control", 1, true);
	auto a = RTLIL::SigSpec(data).extract(0, 8);
	auto alias = module->addWire(RTLIL::escape_id("alias"), 65);
	module->connect(alias, data);
	RTLIL::SigSpec selector;
	selector.append(RTLIL::SigBit(data, 3));
	selector.append(RTLIL::SigBit(alias, 1));
	selector.append(RTLIL::State::Sx);
	selector.append(RTLIL::SigBit(alias, 1));
	selector.append(RTLIL::State::Sz);
	selector.append(RTLIL::SigBit(data, 0));
	module->addMux(RTLIL::escape_id("mux"), a, a, ctrl, port(module, "mux_out", 8));
	module->addPmux(RTLIL::escape_id("pmux"), a, RTLIL::SigSpec(data).extract(0, 48), selector, port(module, "pmux_out", 8));
	module->addBmux(RTLIL::escape_id("bmux"), RTLIL::SigSpec(data).extract(0, 32),
			RTLIL::SigSpec(data).extract(0, 2), port(module, "bmux_out", 8));
	module->addDemux(RTLIL::escape_id("demux"), a, RTLIL::SigSpec(data).extract(0, 2), port(module, "demux_out", 32));
	module->addBwmux(RTLIL::escape_id("bwmux"), a, a, a, port(module, "bwmux_out", 8));
	module->addMuxGate(RTLIL::escape_id("gate_mux"), ctrl, clk, ctrl, port(module, "gate_mux_out", 1));
	module->addNmuxGate(RTLIL::escape_id("gate_nmux"), ctrl, clk, ctrl, port(module, "gate_nmux_out", 1));
	for (int bits : {2, 3, 4}) {
		std::string name = "gate_mux" + std::to_string(1 << bits);
		auto type = bits == 2 ? ID($_MUX4_) : bits == 3 ? ID($_MUX8_) : ID($_MUX16_);
		auto cell = module->addCell(RTLIL::escape_id(name), type);
		for (int index = 0; index < (1 << bits); index++)
			cell->setPort(RTLIL::escape_id(std::string(1, 'A' + index)), RTLIL::SigBit(data, index));
		for (int index = 0; index < bits; index++)
			cell->setPort(RTLIL::escape_id(std::string(1, 'S' + index)), RTLIL::SigBit(data, 32 + index));
		cell->setPort(ID::Y, port(module, name + "_out", 1));
	}
	auto alu = module->addCell(RTLIL::escape_id("alu"), ID($alu));
	alu->setPort(ID::A, a); alu->setPort(ID::B, a);
	alu->setPort(ID::CI, ctrl); alu->setPort(ID::BI, ctrl);
	alu->parameters[ID::A_SIGNED] = false; alu->parameters[ID::B_SIGNED] = false;
	alu->parameters[ID::A_WIDTH] = 8; alu->parameters[ID::B_WIDTH] = 8; alu->parameters[ID::Y_WIDTH] = 8;
	for (auto name : {ID::X, ID::Y, ID::CO})
		alu->setPort(name, port(module, "alu_" + name.str(), 8));
	module->addFa(RTLIL::escape_id("fa"), a, a, a, port(module, "fa_x", 8), port(module, "fa_y", 8));
	auto lcu = module->addCell(RTLIL::escape_id("lcu"), ID($lcu));
	lcu->setPort(ID::P, a); lcu->setPort(ID::G, a); lcu->setPort(ID::CI, ctrl);
	lcu->setPort(ID::CO, port(module, "lcu_out", 8)); lcu->parameters[ID::WIDTH] = 8;
	module->addNot(RTLIL::escape_id("wide"), data, port(module, "wide_out", 65));
	module->addNot(RTLIL::escape_id("dead"), data, module->addWire(RTLIL::escape_id("dead_out"), 65));
	module->addDff(RTLIL::escape_id("dff"), clk, a, port(module, "dff_q", 8), false);
	auto q = port(module, "adff_q", 8);
	auto q_alias = module->addWire(RTLIL::escape_id("initial_alias"), 8);
	module->connect(q_alias, q);
	q_alias->attributes[ID::init] = RTLIL::Const::from_string("10x00101");
	module->addAdff(RTLIL::escape_id("adff"), clk, ctrl, a, q, RTLIL::Const(3, 8), true, false);
	module->addDffe(RTLIL::escape_id("dffe"), clk, ctrl, a, port(module, "dffe_q", 8), true, false);
	module->addSdffe(RTLIL::escape_id("sdffe"), clk, ctrl, ctrl, a, port(module, "sdffe_q", 8), RTLIL::Const(5, 8));
	module->addSdffce(RTLIL::escape_id("sdffce"), clk, ctrl, ctrl, a, port(module, "sdffce_q", 8), RTLIL::Const(5, 8));
	module->addAldff(RTLIL::escape_id("aldff"), clk, ctrl, a, port(module, "aldff_q", 8), a, false, false);
	module->addDlatch(RTLIL::escape_id("latch"), ctrl, a, port(module, "latch_q", 8), false);
	module->addDlatch(RTLIL::escape_id("constant_latch"), ctrl, RTLIL::Const(1, 8), port(module, "constant_latch_q", 8));
	module->addDffsr(RTLIL::escape_id("dffsr"), clk, a, a, a, port(module, "dffsr_q", 8), true, false, true);
	module->addDffGate(RTLIL::escape_id("fine_dff"), clk, ctrl, port(module, "fine_dff_q", 1), false);
	module->fixup_ports();
	module->check();
}

static void verify(const MonitorPlan &plan, const CellGraph &graph)
{
	std::map<std::string, std::set<std::string>> outputs;
	std::map<std::string, std::set<std::string>> selectors;
	for (const auto &item : plan.items) {
		auto cell = RTLIL::unescape_id(graph.cells.at(item.cell).name);
		auto name = RTLIL::unescape_id(graph.ports.at(item.port).name);
		if (item.role == MonitorRole::MUX_SELECT) selectors[cell].insert(name);
		else outputs[cell].insert(name);
	}
	log_assert(outputs.at("alu") == (std::set<std::string>{"X", "Y", "CO"}));
	log_assert(outputs.at("fa") == (std::set<std::string>{"X", "Y"}));
	log_assert(outputs.at("lcu") == (std::set<std::string>{"CO"}));
	for (auto name : {"mux", "pmux", "bmux", "demux", "bwmux", "gate_mux", "gate_nmux"}) {
		log_assert(selectors.at(name) == (std::set<std::string>{"S"}));
		log_assert(outputs.at(name) == (std::set<std::string>{"Y"}));
	}
	log_assert(selectors.at("gate_mux4") == (std::set<std::string>{"S", "T"}));
	log_assert(selectors.at("gate_mux8") == (std::set<std::string>{"S", "T", "U"}));
	log_assert(selectors.at("gate_mux16") == (std::set<std::string>{"S", "T", "U", "V"}));
	log_assert(selectors.size() == 10 && outputs.size() == 24);
	log_assert(!outputs.count("dead"));
	log_assert(plan.registers.size() == 10 && plan.items.size() == 43);
	for (const auto &info : plan.registers) {
		auto name = RTLIL::unescape_id(graph.cells.at(info.cell).name);
		log_assert(outputs.at(name) == (std::set<std::string>{"Q"}));
		if (name == "dff") log_assert(!info.controls.at("clock").positive);
		if (name == "adff") {
			log_assert(!info.controls.at("async_reset").positive);
			log_assert(info.features.val_arst == RTLIL::Const(3, 8));
			log_assert(info.init == RTLIL::Const::from_string("10x00101"));
		}
		if (name == "dffe") log_assert(!info.controls.at("enable").positive);
		if (name == "sdffe") log_assert(!info.features.ce_over_srst);
		if (name == "sdffce") log_assert(info.features.ce_over_srst);
		if (name == "aldff") log_assert(!info.async_data.empty() && !info.controls.at("async_load").positive);
		if (name == "latch") log_assert(info.is_latch && !info.controls.at("async_load").positive);
		if (name == "constant_latch") log_assert(info.is_latch && info.features.has_arst);
		if (name == "dffsr") log_assert(!info.controls.at("set").positive && info.controls.at("clear").positive);
		if (name == "fine_dff") log_assert(info.features.is_fine && !info.controls.at("clock").positive);
	}
}

struct TestCellFuzzTypePlan : Pass {
	TestCellFuzzTypePlan() : Pass("test_cellfuzz_type_plan", "check typed observation sources") {}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		if (GetSize(args) == 2 && args[1] == "fixture") {
			fixture(design);
			return;
		}
		if (GetSize(args) == 3 && args[1] == "poison") {
			auto module = design->top_module();
			auto cell = module->cell(RTLIL::escape_id("mux"));
			if (args[2] == "unknown") cell->type = RTLIL::escape_id("custom");
			else if (args[2] == "direction") cell->setPort(RTLIL::escape_id("mystery"), RTLIL::State::S0);
			else if (args[2] == "missing-output") module->cell(RTLIL::escape_id("alu"))->unsetPort(ID::X);
			else if (args[2] == "global-clock") {
				auto ff = module->cell(RTLIL::escape_id("dff"));
				ff->type = ID($ff); ff->unsetPort(ID::CLK); ff->unsetParam(ID::CLK_POLARITY);
			} else if (args[2] == "fine-global-clock") {
				auto ff = module->cell(RTLIL::escape_id("fine_dff"));
				ff->type = ID($_FF_); ff->unsetPort(ID::C);
			} else if (args[2] == "register-width") {
				module->cell(RTLIL::escape_id("dff"))->setPort(ID::CLK, module->wire(RTLIL::escape_id("data")));
			} else if (args[2] == "unsupported") {
				cell->type = ID($anyseq);
				cell->parameters.clear(); cell->parameters[ID::WIDTH] = 8;
				cell->unsetPort(ID::A); cell->unsetPort(ID::B); cell->unsetPort(ID::S);
			} else log_cmd_error("Unknown poison case.\n");
			return;
		}
		if (GetSize(args) == 2 && args[1] == "fixture-ff-library") {
			auto module = design->addModule(RTLIL::escape_id("ff_library"));
			module->set_bool_attribute(ID::top);
			for (const auto &entry : StaticCellTypes::builder.cells) {
				if (!StaticCellTypes::categories.is_ff(entry.type)) continue;
				FfTypeData type(entry.type);
				if (type.has_gclk) continue;
				auto stem = entry.type.str().substr(1);
				auto cell = module->addCell(RTLIL::escape_id(stem), entry.type);
				for (auto name : entry.inputs)
					cell->setPort(name, port(module, stem + "_" + name.str(), 1, true));
				cell->setPort(ID::Q, port(module, stem + "_q", 1));
				if (!type.is_fine) {
					cell->parameters[ID::WIDTH] = 1;
					if (type.has_clk) cell->parameters[ID::CLK_POLARITY] = false;
					if (type.has_ce) cell->parameters[ID::EN_POLARITY] = false;
					if (type.has_aload && type.has_clk) cell->parameters[ID::ALOAD_POLARITY] = false;
					if (type.has_aload && !type.has_clk) cell->parameters[ID::EN_POLARITY] = false;
					if (type.has_arst) { cell->parameters[ID::ARST_POLARITY] = false; cell->parameters[ID::ARST_VALUE] = RTLIL::Const(1, 1); }
					if (type.has_srst) { cell->parameters[ID::SRST_POLARITY] = false; cell->parameters[ID::SRST_VALUE] = RTLIL::Const(1, 1); }
					if (type.has_sr) { cell->parameters[ID::SET_POLARITY] = false; cell->parameters[ID::CLR_POLARITY] = true; }
				}
			}
			module->fixup_ports();
			module->check();
			return;
		}
		if (GetSize(args) == 2 && args[1] == "empty-selection") {
			auto analysis = SnapshotStore::restore(design, design->top_module()->name);
			SnapshotStore::publish(design, {AnalysisSnapshot::capture(*analysis.graph, CellGraphSelection{}, SelectionStrategy{"test-empty", 1, {}})});
			return;
		}
		if (GetSize(args) >= 3 && (args[1] == "failure" || args[1] == "unchanged")) {
			auto before = design->scratchpad_get_string(SnapshotStore::key);
			auto digest = structural_digest(design->top_module());
			bool failed = false;
			{
				auto errors = logger().error_throw_scope();
				try { Pass::call(design, std::vector<std::string>(args.begin() + 2, args.end())); }
				catch (const log_cmd_error_exception &) { failed = true; }
			}
			log_assert(failed == (args[1] == "failure"));
			log_assert(before == design->scratchpad_get_string(SnapshotStore::key));
			log_assert(digest == structural_digest(design->top_module()));
			return;
		}
		extra_args(args, 1, design);
		auto analysis = SnapshotStore::restore(design, design->top_module()->name);
		auto before = design->scratchpad_get_string(SnapshotStore::key);
		auto digest = structural_digest(analysis.graph->module);
		auto plan = create_monitor_plan(analysis, MonitorOptions{});
		log_assert(!plan.items.empty());
		for (const auto &item : plan.items) {
			const auto &port = analysis.graph->ports.at(item.port);
			log_assert(port.cell == item.cell);
			log_assert(item.source == port.raw_signal);
			log_assert(analysis.selection.selected_cells.count(item.cell));
		}
		if (analysis.graph->module->name == RTLIL::escape_id("types"))
			verify(plan, *analysis.graph);
		if (analysis.graph->module->name == RTLIL::escape_id("ff_library")) {
			log_assert(plan.items.size() == analysis.graph->cells.size());
			log_assert(plan.registers.size() == plan.items.size());
			for (const auto &item : plan.items)
				log_assert(item.role == MonitorRole::REGISTER_Q);
		}
		log_assert(before == design->scratchpad_get_string(SnapshotStore::key));
		log_assert(digest == structural_digest(analysis.graph->module));
	}
} TestCellFuzzTypePlan;

PRIVATE_NAMESPACE_END
