#include "memory_access.h"
#include "monitor_plan.h"

#include <algorithm>
#include <climits>
#include <tuple>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {
namespace {
using Json = json11::Json;

static Json signal_record(const RTLIL::SigSpec &source)
{
	Json::array bits;
	for (auto bit : source)
		bits.push_back(bit.wire ? Json(Json::array{bit.wire->name.str(), bit.offset}) : Json(int(bit.data)));
	return bits;
}

static Json constant_record(const RTLIL::Const &value)
{
	return Json::object{{"width", GetSize(value)}, {"bits", value.as_string("-")}, {"flags", int(value.flags)}};
}

using SourceKey = std::tuple<GraphCellId, PortId, int>;
} // namespace

void plan_memory_access(const RestoredAnalysis &analysis, MonitorPlan &plan)
{
	const auto &graph = *analysis.graph;
	std::map<RTLIL::Cell *, GraphCellId> cells;
	std::map<std::pair<GraphCellId, RTLIL::IdString>, PortId> ports;
	for (const auto &cell : graph.cells) cells.emplace(cell.cell, cell.id);
	for (const auto &port : graph.ports)
		if (port.is_cell_port()) ports.emplace(std::make_pair(port.cell, port.name), port.id);
	// get_all_memories checks the helper, including wide-port constraints and
	// mask relationships. It does not mutate collected cells. Fatal RTLIL/helper
	// checks must precede artifact preparation, just like the top-level check.
	for (auto &memory : Mem::get_all_memories(graph.module)) {
		if (!memory.packed || !memory.cell || !cells.count(memory.cell))
			log_cmd_error("cellinstrument: memory must be collected and belong to the frozen graph.\n");
		auto cell = cells.at(memory.cell);
		bool selected = analysis.selection.selected_cells.count(cell);
		MemoryInfo info{cell, memory, selected, selected && plan.options.mem_access, {}, {}, {}, memory.cell->getParam(ID::INIT)};
		int abits = memory.cell->getParam(ID::ABITS).as_int();
		auto add = [&](MemoryAccessPort &owner, const char *name, RTLIL::IdString raw_port,
				int offset, const RTLIL::SigSpec &source, bool active = true) {
			MemorySignal signal;
			signal.name = name; signal.port_lsb = offset; signal.source = source; signal.active = active;
			auto found = ports.find({cell, raw_port});
			if (found != ports.end()) {
				signal.port = found->second;
				const auto &raw = graph.ports[signal.port].raw_signal;
				if (offset < 0 || offset > GetSize(raw) || GetSize(source) > GetSize(raw) - offset || raw.extract(offset, GetSize(source)) != source)
					log_cmd_error("cellinstrument: memory field does not match its frozen port slice.\n");
			} else if (memory.cell->type != ID($mem) || !raw_port.in(ID::RD_ARST, ID::RD_SRST) || source != RTLIL::State::S0) {
				log_cmd_error("cellinstrument: memory field has no original graph port.\n");
			}
			signal.reason = !selected ? "not_selected" : !plan.options.mem_access ? "memory_access_disabled" :
					!active ? "inactive_control" : source.empty() ? "zero_width" :
					signal.port == INVALID_PORT_ID ? "no_original_port" : "observed";
			if (signal.reason == "observed")
				plan.items.push_back({cell, signal.port, MonitorRole::MEMORY_ACCESS, source, offset});
			owner.signals.push_back(signal);
		};
		int packed = 0;
		for (const auto &port : memory.rd_ports) {
			MemoryAccessPort owner{GetSize(info.reads), packed, port.wide_log2, {}};
			add(owner, "address", ID::RD_ADDR, packed * abits, port.addr);
			add(owner, "data", ID::RD_DATA, packed * memory.width, port.data);
			add(owner, "enable", ID::RD_EN, packed, port.en);
			add(owner, "clock", ID::RD_CLK, packed, port.clk, port.clk_enable);
			add(owner, "async_reset", ID::RD_ARST, packed, port.arst, port.clk_enable && port.arst != RTLIL::State::S0);
			add(owner, "sync_reset", ID::RD_SRST, packed, port.srst, port.clk_enable && port.srst != RTLIL::State::S0);
			info.reads.push_back(owner);
			packed += 1 << port.wide_log2;
		}
		packed = 0;
		for (const auto &port : memory.wr_ports) {
			MemoryAccessPort owner{GetSize(info.writes), packed, port.wide_log2, {}};
			add(owner, "address", ID::WR_ADDR, packed * abits, port.addr);
			add(owner, "data", ID::WR_DATA, packed * memory.width, port.data);
			add(owner, "enable", ID::WR_EN, packed * memory.width, port.en);
			add(owner, "clock", ID::WR_CLK, packed, port.clk, port.clk_enable);
			info.writes.push_back(owner);
			packed += 1 << port.wide_log2;
		}
		plan.memories.push_back(std::move(info));
	}
	std::sort(plan.memories.begin(), plan.memories.end(), [&](const auto &a, const auto &b) {
		return graph.cells[a.cell].name.str() < graph.cells[b.cell].name.str();
	});
	for (int index = 0; index < GetSize(plan.memories); index++) {
		auto &info = plan.memories[index];
		if (!info.selected || !plan.options.mem_query) continue;
		const auto &memory = info.memory;
		int64_t last = int64_t(memory.start_offset) + memory.size - 1;
		if (memory.start_offset < 0 || memory.size <= 0 || last > INT_MAX)
			log_cmd_error("cellinstrument: memory query requires a nonnegative address range ending at or below INT_MAX.\n");
		auto &query = info.query;
		query.enabled = true;
		query.addr_width = 1;
		while (last >> query.addr_width) query.addr_width++;
		query.addr_name = stringf("%s_mem_%04d_addr", plan.options.prefix, index);
		query.data_name = stringf("%s_mem_%04d_data", plan.options.prefix, index);
		int64_t reads = memory.cell->getParam(ID::RD_PORTS).as_int(), writes = memory.cell->getParam(ID::WR_PORTS).as_int();
		int64_t abits = std::max(query.addr_width, memory.cell->getParam(ID::ABITS).as_int());
		if ((reads + 1) * memory.width >= RTLIL::WIDTH_LIMIT || (reads + 1) * abits >= RTLIL::WIDTH_LIMIT ||
				writes * abits >= RTLIL::WIDTH_LIMIT || (reads + 1) * writes >= RTLIL::WIDTH_LIMIT)
			log_cmd_error("cellinstrument: normalized memory query exceeds RTLIL width limits.\n");
		// All fatal helper checks run before artifact preparation. emit extends
		// addresses unsigned; no business address bit is truncated or sign-extended.
		memory_with_query(info, RTLIL::SigSpec(RTLIL::State::S0, query.addr_width),
				RTLIL::SigSpec(RTLIL::State::Sx, memory.width));
	}
}

Mem memory_with_query(const MemoryInfo &info, const RTLIL::SigSpec &addr, const RTLIL::SigSpec &data)
{
	auto memory = info.memory;
	if (!info.query.enabled || GetSize(addr) != info.query.addr_width || GetSize(data) != memory.width)
		log_cmd_error("cellinstrument: invalid memory query binding.\n");
	MemRd query;
	query.wide_log2 = 0;
	query.clk_enable = false; query.clk_polarity = true; query.ce_over_srst = false;
	query.clk = RTLIL::State::Sx; query.en = RTLIL::State::S1;
	query.arst = RTLIL::State::S0; query.srst = RTLIL::State::S0;
	query.init_value = query.arst_value = query.srst_value = RTLIL::Const(RTLIL::State::Sx, memory.width);
	query.transparency_mask.resize(memory.wr_ports.size(), false);
	query.collision_x_mask.resize(memory.wr_ports.size(), false);
	query.addr = addr; query.data = data;
	memory.rd_ports.push_back(query);
	memory.check();
	return memory;
}

void verify_memory_query(const MemoryInfo &info, const Mem &actual, RTLIL::Wire *addr, RTLIL::Wire *data)
{
	const auto &original = info.memory;
	auto expected = memory_with_query(info, addr, data);
	int abits = info.query.addr_width;
	for (const auto &port : original.rd_ports) abits = std::max(abits, GetSize(port.addr));
	for (const auto &port : original.wr_ports) abits = std::max(abits, GetSize(port.addr));
	auto fail = [] { log_cmd_error("cellinstrument: emitted memory differs from query plan or original business semantics.\n"); };
	if (actual.memid != original.memid || actual.width != original.width || actual.size != original.size ||
			actual.start_offset != original.start_offset || actual.attributes != original.attributes ||
			actual.cell->getParam(ID::INIT) != info.original_init || actual.rd_ports.size() != expected.rd_ports.size() ||
			actual.wr_ports.size() != original.wr_ports.size()) fail();
	for (int index = 0; index < GetSize(expected.rd_ports); index++) {
		auto before = expected.rd_ports[index];
		const auto &after = actual.rd_ports[index];
		before.addr.extend_u0(abits, false);
		if (before.addr != after.addr || before.data != after.data || before.en != after.en || before.clk != after.clk ||
				before.arst != after.arst || before.srst != after.srst || before.wide_log2 != after.wide_log2 ||
				before.clk_enable != after.clk_enable || before.clk_polarity != after.clk_polarity || before.ce_over_srst != after.ce_over_srst ||
				before.init_value != after.init_value || before.arst_value != after.arst_value || before.srst_value != after.srst_value ||
				before.transparency_mask != after.transparency_mask || before.collision_x_mask != after.collision_x_mask) fail();
	}
	for (int index = 0; index < GetSize(original.wr_ports); index++) {
		auto before = original.wr_ports[index];
		const auto &after = actual.wr_ports[index];
		before.addr.extend_u0(abits, false);
		if (before.addr != after.addr || before.data != after.data || before.en != after.en || before.clk != after.clk ||
				before.wide_log2 != after.wide_log2 || before.clk_enable != after.clk_enable ||
				before.clk_polarity != after.clk_polarity || before.priority_mask != after.priority_mask) fail();
	}
}

void bind_memory_monitors(MonitorPlan &plan)
{
	std::map<SourceKey, int> monitors;
	for (int index = 0; index < GetSize(plan.items); index++) {
		const auto &item = plan.items[index];
		if (item.role == MonitorRole::MEMORY_ACCESS)
			log_assert(monitors.emplace(SourceKey{item.cell, item.port, item.port_lsb}, index).second);
	}
	for (auto &info : plan.memories)
		for (auto owners : {&info.reads, &info.writes})
			for (auto &owner : *owners)
				for (auto &signal : owner.signals)
					if (signal.reason == "observed") signal.monitor_id = monitors.at({info.cell, signal.port, signal.port_lsb});
}

Json memory_access_record(const MemoryInfo &info, const MonitorPlan &plan, const std::vector<int> &port_ids)
{
	const auto &memory = info.memory;
	bool observed = false, has_sync = false, sync_observed = false;
	auto signals = [&](const MemoryAccessPort &owner) {
		Json::object result;
		for (const auto &signal : owner.signals) {
			if (signal.monitor_id >= 0) {
				if (signal.monitor_id >= GetSize(plan.items))
					log_cmd_error("cellinstrument: invalid memory monitor reference.\n");
				const auto &item = plan.items[signal.monitor_id];
				if (item.role != MonitorRole::MEMORY_ACCESS || item.cell != info.cell || item.port != signal.port ||
						item.port_lsb != signal.port_lsb || item.source != signal.source)
					log_cmd_error("cellinstrument: memory monitor does not match its business field.\n");
				observed = true;
			}
			result[signal.name] = Json::object{{"port_id", signal.port == INVALID_PORT_ID ? Json() : Json(port_ids.at(signal.port))},
					{"port_lsb", signal.port_lsb}, {"width", GetSize(signal.source)}, {"source", signal_record(signal.source)},
					{"active", signal.active}, {"monitor_id", signal.monitor_id < 0 ? Json() : Json(signal.monitor_id)},
					{"observation_reason", signal.reason}};
		}
		return result;
	};
	Json::array reads, writes;
	for (const auto &owner : info.reads) {
		const auto &port = memory.rd_ports.at(owner.index);
		auto fields = signals(owner);
		has_sync |= port.clk_enable;
		sync_observed |= port.clk_enable && !fields.at("data")["monitor_id"].is_null();
		reads.push_back(Json::object{{"index", owner.index}, {"packed_index", owner.packed_index},
				{"wide_log2", owner.wide_log2}, {"words", 1 << owner.wide_log2}, {"data_width", GetSize(port.data)},
				{"address_width", GetSize(port.addr)}, {"clock_enabled", port.clk_enable}, {"clock_positive", port.clk_polarity},
				{"ce_over_srst", port.ce_over_srst}, {"init_value", constant_record(port.init_value)},
				{"arst_value", constant_record(port.arst_value)}, {"srst_value", constant_record(port.srst_value)},
				{"transparency_mask", port.transparency_mask}, {"collision_x_mask", port.collision_x_mask}, {"signals", fields}});
	}
	for (const auto &owner : info.writes) {
		const auto &port = memory.wr_ports.at(owner.index);
		writes.push_back(Json::object{{"index", owner.index}, {"packed_index", owner.packed_index},
				{"wide_log2", owner.wide_log2}, {"words", 1 << owner.wide_log2}, {"data_width", GetSize(port.data)},
				{"address_width", GetSize(port.addr)}, {"clock_enabled", port.clk_enable}, {"clock_positive", port.clk_polarity},
				{"priority_mask", port.priority_mask}, {"signals", signals(owner)}});
	}
	Json::array notes;
	if (has_sync && !sync_observed) notes.push_back("synchronous read output holding state has no memory access observation");
	Json query;
	if (info.query.enabled) {
		int packed = 0;
		for (const auto &port : memory.rd_ports) packed += 1 << port.wide_log2;
		query = Json::object{{"address_port", info.query.addr_name}, {"data_port", info.query.data_name},
				{"address_width", info.query.addr_width}, {"data_width", memory.width},
				{"address_encoding", "unsigned_logical"}, {"start_offset", memory.start_offset},
				{"end_offset_exclusive", double(int64_t(memory.start_offset) + memory.size)},
				{"read_index", GetSize(memory.rd_ports)}, {"packed_index", packed}, {"asynchronous", true}, {"always_enabled", true}};
	}
	return Json::object{{"memid", memory.memid.str()}, {"width", memory.width}, {"size", memory.size},
			{"start_offset", memory.start_offset}, {"end_offset_exclusive", double(int64_t(memory.start_offset) + memory.size)},
			{"init", constant_record(info.original_init)}, {"read_ports", reads}, {"write_ports", writes},
			{"access_enabled", info.access_enabled}, {"access_observed", observed}, {"content_query", info.query.enabled}, {"query", query},
			{"sync_read_state_observed", sync_observed}, {"coverage_notes", notes}};
}

} // namespace CellFuzz
YOSYS_NAMESPACE_END
