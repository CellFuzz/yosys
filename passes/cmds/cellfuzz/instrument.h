#ifndef YOSYS_CELLFUZZ_INSTRUMENT_H
#define YOSYS_CELLFUZZ_INSTRUMENT_H

#include "transport.h"

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {

// Consume command-local sources and original RTLIL already fully checked
// against the frozen graph (including Module::check before file preparation).
// Adds only output wires and continuous assignments. The caller owns batch
// invalidation, artifact publication and the final successful-module marker.
void materialize_observations(RTLIL::Module *module, const MonitorPlan &plan, const TransportLayout &layout);

} // namespace CellFuzz
YOSYS_NAMESPACE_END

#endif
