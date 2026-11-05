#include "kernel/register.h"
#include "kernel/mem.h"
#include "passes/cmds/cellfuzz/manifest.h"

#include <climits>

USING_YOSYS_NAMESPACE
using namespace Yosys::CellFuzz;
PRIVATE_NAMESPACE_BEGIN

static RTLIL::Wire *port(RTLIL::Module *module, const std::string &name, int width, bool input = true)
{
	auto wire = module->addWire(RTLIL::escape_id(name), width);
	wire->port_input = input; wire->port_output = !input;
	return wire;
}

static void fixture(RTLIL::Design *design, const std::string &mode)
{
	bool wide = mode == "wide", legacy = mode == "legacy";
	auto module = design->addModule(RTLIL::escape_id("memory_access"));
	module->set_bool_attribute(ID::top);
	auto clk = port(module, "clk", 1);
	int width = wide ? 33 : 8, size = wide ? 8 : 4;
	int offset = wide ? 8 : mode == "negative" ? -4 : mode == "offset" ? 4 : mode == "overflow" ? INT_MAX - 1 : 0;
	int abits = offset < 0 ? 32 : wide || offset != 0 ? 4 : mode == "zero-address" ? 0 : 2;
	if (mode == "zero-address") size = 1;
	if (mode == "extend") abits = 1; // query must widen packed addresses
	if (mode == "overflow") abits = 32;
	Mem memory(module, RTLIL::escape_id("array"), width, offset, size);
	memory.packed = true;
	int write_count = mode == "read-only" || mode == "zero-address" ? 0 : mode == "write-only" || mode == "async-write" ? 1 : 2;
	for (int index = 0; index < write_count; index++) {
		MemWr wr;
		wr.wide_log2 = wide && index == 0 ? 1 : 0;
		int data_width = width << wr.wide_log2;
		std::string stem = stringf("w%d_", index);
		wr.clk_enable = mode != "async-write";
		wr.clk_polarity = mode != "negative-write";
		wr.clk = wr.clk_enable ? RTLIL::SigSpec(clk) : RTLIL::SigSpec(RTLIL::State::Sx);
		wr.addr = port(module, stem + "addr", abits);
		if (wr.wide_log2) wr.addr[0] = RTLIL::State::S0;
		wr.en = port(module, stem + "en", data_width);
		wr.data = port(module, stem + "data", data_width);
		wr.priority_mask.resize(write_count, false);
		if (index) wr.priority_mask[0] = true;
		memory.wr_ports.push_back(wr);
	}
	int read_count = mode == "write-only" ? 0 : mode == "async-write" ? 1 : legacy || wide ? 2 : 4;
	for (int index = 0; index < read_count; index++) {
		MemRd rd;
		rd.wide_log2 = wide && index == 0 ? 1 : 0;
		int data_width = width << rd.wide_log2;
		std::string stem = stringf("r%d_", index);
		rd.clk_enable = index != 0;
		rd.clk_polarity = index != 3;
		rd.ce_over_srst = index == 2;
		rd.clk = rd.clk_enable ? RTLIL::SigSpec(clk) : RTLIL::SigSpec(RTLIL::State::Sx);
		rd.addr = abits ? RTLIL::SigSpec(port(module, stem + "addr", abits)) : RTLIL::SigSpec();
		if (rd.wide_log2) rd.addr[0] = RTLIL::State::S0;
		rd.data = port(module, stem + "data", data_width, false);
		rd.en = rd.clk_enable ? RTLIL::SigSpec(port(module, stem + "en", 1)) : RTLIL::SigSpec(RTLIL::State::S1);
		if (rd.clk_enable && !legacy) {
			rd.arst = port(module, stem + "arst", 1);
			rd.srst = port(module, stem + "srst", 1);
		}
		rd.arst_value = RTLIL::Const(0xa2 + index, data_width);
		rd.srst_value = RTLIL::Const(0xb3 + index, data_width);
		rd.init_value = RTLIL::Const(0x91 + index, data_width);
		if (legacy) rd.arst_value = rd.srst_value = rd.init_value = RTLIL::Const(RTLIL::State::Sx, data_width);
		rd.transparency_mask.resize(write_count, false);
		rd.collision_x_mask.resize(write_count, false);
		if (mode == "semantics" && index == 1) {
			rd.transparency_mask[0] = true;
			rd.collision_x_mask[1] = true;
		}
		memory.rd_ports.push_back(rd);
	}
	MemInit init;
	init.addr = offset;
	init.en = RTLIL::Const(RTLIL::State::S1, width);
	for (int index = 0; index < size - (mode == "partial-init" ? 1 : 0); index++) init.data.append(RTLIL::Const(0x10 + index * 0x11, width));
	memory.inits.push_back(init);
	memory.check();
	// Build the original fixture. Without query, instrumentation preserves
	// this exact cell; query tests independently check the added read-port delta.
	memory.emit();
	if (legacy) {
		auto cell = memory.cell;
		cell->type = ID($mem);
		cell->parameters[ID::RD_TRANSPARENT] = RTLIL::Const(0, 2);
		for (auto name : {ID::RD_WIDE_CONTINUATION, ID::RD_CE_OVER_SRST, ID::RD_ARST_VALUE, ID::RD_SRST_VALUE,
				ID::RD_INIT_VALUE, ID::RD_TRANSPARENCY_MASK, ID::RD_COLLISION_X_MASK, ID::WR_WIDE_CONTINUATION, ID::WR_PRIORITY_MASK})
			cell->unsetParam(name);
		cell->unsetPort(ID::RD_ARST); cell->unsetPort(ID::RD_SRST);
	}
	if (mode == "z-init") {
		auto data = memory.cell->getParam(ID::INIT);
		for (int bit = (size - 1) * width; bit < size * width; bit++) data.set(bit, RTLIL::State::Sz);
		memory.cell->setParam(ID::INIT, data);
	}
	if (mode == "multiple") {
		Mem extra(module, RTLIL::escape_id("zarray"), 13, 5, 3);
		extra.packed = true;
		MemRd rd;
		rd.addr = port(module, "extra_addr", 3);
		rd.data = port(module, "extra_data", 13, false);
		rd.init_value = rd.arst_value = rd.srst_value = RTLIL::Const(RTLIL::State::Sx, 13);
		extra.rd_ports.push_back(rd);
		MemInit init;
		init.addr = 5; init.en = RTLIL::Const(RTLIL::State::S1, 13);
		for (int word : {0x1abc, 0x1234, 0x678}) init.data.append(RTLIL::Const(word, 13));
		extra.inits.push_back(init);
		extra.emit();
	}
	if (mode == "collision-addr" || mode == "collision-data")
		module->addWire(RTLIL::escape_id(mode == "collision-addr" ? "cf_mem_0000_addr" : "cf_mem_0000_data"));
	module->fixup_ports(); module->check();
}

struct TestCellFuzzMemory : Pass {
	TestCellFuzzMemory() : Pass("test_cellfuzz_memory", "memory access fixtures and frozen slice rejection") {}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		if (GetSize(args) == 3 && args[1] == "fixture") {
			fixture(design, args[2]);
			return;
		}
		log_assert(GetSize(args) == 2 && args[1] == "invalid");
		auto analysis = SnapshotStore::restore(design, design->top_module()->name);
		auto plan = create_monitor_plan(analysis, MonitorOptions{});
		auto layout = create_transport(plan, analysis.graph->module);
		auto batch = design->scratchpad_get_string(SnapshotStore::key);
		auto digest = structural_digest(analysis.graph->module);
		int id = -1;
		for (int index = 0; index < GetSize(plan.items); index++)
			if (plan.items[index].role == MonitorRole::MEMORY_ACCESS) id = index;
		log_assert(id >= 0);
		for (int fault = 0; fault < 4; fault++) {
			auto broken = plan;
			if (fault == 0) broken.items[id].port_lsb = -1;
			if (fault == 1) broken.items[id].port_lsb = INT_MAX;
			if (fault == 2) broken.items[id].source[0] = RTLIL::State::Sm;
			if (fault == 3) broken.memories.front().reads.front().signals.front().monitor_id = id;
			bool failed = false;
			{
				auto errors = logger().error_throw_scope();
				try { planned_manifest(analysis, broken, layout); }
				catch (const log_cmd_error_exception &) { failed = true; }
			}
			log_assert(failed);
		}
		log_assert(batch == design->scratchpad_get_string(SnapshotStore::key));
		log_assert(digest == structural_digest(analysis.graph->module));
	}
} TestCellFuzzMemory;

PRIVATE_NAMESPACE_END
