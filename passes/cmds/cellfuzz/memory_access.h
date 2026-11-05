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

// A read-only helper copy: its SigSpecs and original cell belong to the
// restored graph's lifetime. No helper normalization or emit is performed.
struct MemoryInfo {
	GraphCellId cell;
	Mem memory;
	bool selected, access_enabled;
	std::vector<MemoryAccessPort> reads, writes;
};

struct MonitorPlan;
void plan_memory_access(const RestoredAnalysis &analysis, MonitorPlan &plan);
void bind_memory_monitors(MonitorPlan &plan);
json11::Json memory_access_record(const MemoryInfo &info, const MonitorPlan &plan, const std::vector<int> &port_ids);

} // namespace CellFuzz
YOSYS_NAMESPACE_END

#endif
