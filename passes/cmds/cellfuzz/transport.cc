#include "transport.h"
#include "backends/verilog/verilog_backend.h"

#include <algorithm>
#include <climits>
#include <map>
#include <set>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {
namespace {

static void require(bool condition, const char *reason)
{
	if (!condition)
		log_cmd_error("cellinstrument: invalid transport: %s.\n", reason);
}

static void check_name_syntax(const std::string &name)
{
	require(!name.empty() && !VERILOG_BACKEND::id_is_verilog_escaped(name), "illegal or reserved interface name");
	require(name.compare(0, 2, "__") != 0,
			"Verilator reserved interface prefix");
}

using Interval = std::pair<int, int>;

static void cover(std::vector<Interval> intervals, int width)
{
	std::sort(intervals.begin(), intervals.end());
	int end = 0;
	for (auto interval : intervals) {
		require(interval.first == end, "overlapping or incomplete bit coverage");
		end = interval.second;
	}
	require(end == width, "incomplete bit coverage");
}
} // namespace

TransportLayout create_transport(const MonitorPlan &plan, RTLIL::Module *module)
{
	plan.options.validate();
	check_name_syntax(plan.options.prefix);
	TransportLayout layout;
	for (int monitor = 0; monitor < GetSize(plan.items); monitor++) {
		int width = GetSize(plan.items[monitor].source);
		require(width > 0, "zero width observation item");
		for (int offset = 0; offset < width;) {
			if (layout.ports.empty() || layout.ports.back().width == plan.options.chunk_width) {
				require(layout.ports.size() < INT_MAX, "too many interface ports");
				auto name = stringf("%s_obs_%04d", plan.options.prefix, GetSize(layout.ports));
				check_name_syntax(name);
				layout.ports.push_back({name, 0});
			}
			auto &port = layout.ports.back();
			int size = std::min(width - offset, plan.options.chunk_width - port.width);
			layout.segments.push_back({monitor, offset, port.name, port.width, size});
			offset += size;
			port.width += size;
		}
		layout.total_bits += width;
	}
	validate_transport(plan, layout, module);
	return layout;
}

void validate_transport(const MonitorPlan &plan, const TransportLayout &layout, RTLIL::Module *module)
{
	plan.options.validate();
	check_name_syntax(plan.options.prefix);
	// Collect once per validation, including memories excluded from selection
	// or observation. A collected cell's name can differ from its Verilog MEMID.
	std::map<RTLIL::IdString, std::string> occupied;
	for (auto wire : module->wires()) occupied.emplace(wire->name, "wire");
	for (auto cell : module->cells()) occupied.emplace(cell->name, "cell");
	for (const auto &entry : module->memories) occupied.emplace(entry.first, "memory");
	for (const auto &entry : module->processes) occupied.emplace(entry.first, "process");
	for (const auto &entry : module->parameter_default_values) occupied.emplace(entry.first, "parameter");
	for (auto name : module->avail_parameters) occupied.emplace(name, "available parameter");
	for (const auto &info : plan.memories)
		occupied[info.memory.memid] = stringf("collected memory cell %s, MEMID %s",
				log_id(info.memory.cell), log_id(info.memory.memid));
	std::set<std::string> all_names;
	auto check_name = [&](const std::string &name) {
		check_name_syntax(name);
		auto id = RTLIL::escape_id(name);
		auto found = occupied.find(id);
		if (found != occupied.end())
			log_cmd_error("cellinstrument: interface name %s collides with an existing object (%s).\n",
					name.c_str(), found->second.c_str());
		// Retain count_id's complete RTLIL namespace check as the owner evolves.
		require(!module->count_id(id), "interface name collides with an existing object");
		require(all_names.insert(name).second, "duplicate query/transport interface name");
	};
	std::map<std::string, int> widths;
	std::map<std::string, std::vector<Interval>> port_intervals;
	std::vector<std::vector<Interval>> item_intervals(plan.items.size());
	int64_t item_total = 0, port_total = 0;
	for (const auto &item : plan.items) {
		require(!item.source.empty(), "zero width observation item");
		item_total += GetSize(item.source);
	}
	for (const auto &port : layout.ports) {
		check_name(port.name);
		require(port.width > 0 && port.width <= plan.options.chunk_width, "interface width out of range");
		require(port.width < RTLIL::WIDTH_LIMIT, "interface width exceeds RTLIL limit");
		require(widths.emplace(port.name, port.width).second, "duplicate interface name");
		port_total += port.width;
	}
	for (const auto &info : plan.memories) {
		if (!info.query.enabled) continue;
		for (const auto &name : {info.query.addr_name, info.query.data_name}) {
			check_name(name);
		}
		require(info.query.addr_width > 0 && info.query.addr_width <= 31 && info.memory.width > 0 &&
				info.memory.width < RTLIL::WIDTH_LIMIT, "query interface width out of range");
	}
	require(module->ports.size() <= size_t(INT_MAX) && all_names.size() <= size_t(INT_MAX) - module->ports.size(),
			"too many top-level ports");
	// json11 numbers are doubles. Do not silently round aggregate widths in
	// the exported contract, even though practical plans are much smaller.
	require(item_total <= 9007199254740991LL && port_total == item_total && layout.total_bits == item_total,
			"total width mismatch or overflow");
	for (const auto &segment : layout.segments) {
		require(segment.monitor_id >= 0 && segment.monitor_id < GetSize(plan.items), "unknown monitor ID");
		auto port = widths.find(segment.port_name);
		require(port != widths.end(), "unknown interface port");
		int width = GetSize(plan.items[segment.monitor_id].source);
		require(segment.width > 0 && segment.item_lsb >= 0 && segment.item_lsb <= width &&
				segment.width <= width - segment.item_lsb, "source segment out of range");
		require(segment.port_lsb >= 0 && segment.port_lsb <= port->second &&
				segment.width <= port->second - segment.port_lsb, "target segment out of range");
		item_intervals[segment.monitor_id].push_back({segment.item_lsb, segment.item_lsb + segment.width});
		port_intervals[segment.port_name].push_back({segment.port_lsb, segment.port_lsb + segment.width});
	}
	for (int index = 0; index < GetSize(plan.items); index++)
		cover(item_intervals[index], GetSize(plan.items[index].source));
	for (const auto &port : layout.ports)
		cover(port_intervals[port.name], port.width);
}

} // namespace CellFuzz
YOSYS_NAMESPACE_END
