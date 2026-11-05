#ifndef YOSYS_CELLFUZZ_MONITOR_PLAN_H
#define YOSYS_CELLFUZZ_MONITOR_PLAN_H

#include "analysis_snapshot.h"
#include "kernel/ff.h"

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {

struct MonitorOptions {
	bool mem_access = true;
	bool mem_query = false;
	int chunk_width = 256;
	std::string prefix = "cf";
	void validate() const;
};

enum class MonitorRole { OUTPUT, MUX_SELECT, REGISTER_Q };

struct MonitorItem {
	GraphCellId cell = INVALID_CELL_ID;
	PortId port = INVALID_PORT_ID;
	MonitorRole role = MonitorRole::OUTPUT;
	RTLIL::SigSpec source;
};

struct RegisterControl {
	RTLIL::SigSpec source;
	bool positive = true;
};

struct RegisterInfo {
	GraphCellId cell = INVALID_CELL_ID;
	FfTypeData features;
	bool is_latch = false;
	RTLIL::Const init;
	// Controls and async data describe the original state semantics. They do
	// not create runtime observation items: Q alone is mandatory in this stage.
	std::map<std::string, RegisterControl> controls;
	RTLIL::SigSpec async_data;
};

struct UnobservedCell {
	GraphCellId cell = INVALID_CELL_ID;
	std::string reason;
};

// A command-local plan with live SigSpec bindings. The restored graph must
// remain alive and unchanged. Transport is planned separately without RTLIL edits.
struct MonitorPlan {
	MonitorOptions options;
	std::vector<MonitorItem> items;
	std::vector<RegisterInfo> registers;
	std::vector<UnobservedCell> unobserved;
	// Internal type descriptions reused by the complete manifest serializer.
	json11::Json type_report(const RestoredAnalysis &analysis) const;
};

// Preflight requires exactly one whole selected top. It inspects the entire
// module for unsupported export semantics, including objects not selected by
// cellfilter. It never calls a mutating pass or consumes the analysis batch.
RTLIL::Module *monitor_top(RTLIL::Design *design);
MonitorPlan create_monitor_plan(const RestoredAnalysis &analysis, const MonitorOptions &options);

} // namespace CellFuzz
YOSYS_NAMESPACE_END

#endif
