#ifndef YOSYS_CELLFUZZ_TRANSPORT_H
#define YOSYS_CELLFUZZ_TRANSPORT_H

#include "monitor_plan.h"
#include <cstdint>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {

struct TransportPort {
	std::string name;
	int width = 0;
};

struct TransportSegment {
	int monitor_id = -1;
	int item_lsb = 0;
	std::string port_name;
	int port_lsb = 0;
	int width = 0;
};

// Values only. Port names are external ASCII names, without RTLIL's leading
// escape. Monitor IDs index MonitorPlan::items, not cells or unique signals.
struct TransportLayout {
	std::vector<TransportPort> ports;
	std::vector<TransportSegment> segments;
	int64_t total_bits = 0;
};

TransportLayout create_transport(const MonitorPlan &plan, RTLIL::Module *module);
// Checks ranges before arithmetic, then checks coverage from sorted intervals.
// Does not allocate one validation entry per data bit or assume segment order.
void validate_transport(const MonitorPlan &plan, const TransportLayout &layout, RTLIL::Module *module);

} // namespace CellFuzz
YOSYS_NAMESPACE_END

#endif
