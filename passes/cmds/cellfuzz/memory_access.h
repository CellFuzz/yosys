#ifndef YOSYS_CELLFUZZ_MEMORY_ACCESS_H
#define YOSYS_CELLFUZZ_MEMORY_ACCESS_H

#include "analysis_snapshot.h"
#include "kernel/mem.h"

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {

struct MemorySignal {
	std::string name;
	PortId port = INVALID_PORT_ID;
	int port_lsb = 0;
	RTLIL::SigSpec source;
	bool active = true;
	std::string reason;
	int monitor_id = -1;
};

struct MemoryAccessPort {
	int index = 0, packed_index = 0, wide_log2 = 0;
	std::vector<MemorySignal> signals;
};

struct MemoryQuery {
	bool enabled = false;
	std::string addr_name, data_name;
	int addr_width = 0;
};

// Frozen business helper: never append query ports to this copy. Its sources
// remain original wires even if emit later normalizes the collected cell.
struct MemoryInfo {
	GraphCellId cell;
	Mem memory;
	bool selected, access_enabled;
	std::vector<MemoryAccessPort> reads, writes;
	MemoryQuery query;
	RTLIL::Const original_init;
};

struct MonitorPlan;
void plan_memory_access(const RestoredAnalysis &analysis, MonitorPlan &plan);
void bind_memory_monitors(MonitorPlan &plan);
// Build and check all modified helpers before emitting any of them. Placeholder
// constant bindings permit the same shape checks during read-only planning.
Mem memory_with_query(const MemoryInfo &info, const RTLIL::SigSpec &addr, const RTLIL::SigSpec &data);
void verify_memory_query(const MemoryInfo &info, const Mem &actual, RTLIL::Wire *addr, RTLIL::Wire *data);
json11::Json memory_access_record(const MemoryInfo &info, const MonitorPlan &plan, const std::vector<int> &port_ids);

} // namespace CellFuzz
YOSYS_NAMESPACE_END

#endif
