#ifndef YOSYS_CELLFUZZ_MANIFEST_H
#define YOSYS_CELLFUZZ_MANIFEST_H

#include "transport.h"

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {

// Freeze the original analysis into a self-contained value document. Export
// IDs are assigned in name order and include the corresponding analysis IDs.
// No new graph objects, live pointers, output path or timestamps are encoded.
json11::Json planned_manifest(const RestoredAnalysis &analysis, const MonitorPlan &plan, const TransportLayout &layout);
// Derive a publication candidate entirely from frozen values. Publish only
// after materialization checks; never rebuild the original graph afterwards.
json11::Json materialized_manifest(const json11::Json &planned);

} // namespace CellFuzz
YOSYS_NAMESPACE_END

#endif
