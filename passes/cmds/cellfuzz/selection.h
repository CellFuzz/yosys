#ifndef YOSYS_CELLFUZZ_SELECTION_H
#define YOSYS_CELLFUZZ_SELECTION_H

#include "cellgraph.h"

#include <functional>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {

struct CellRegion {
	int id = -1;
	std::vector<GraphCellId> edge_cells;
	std::vector<GraphCellId> key_cells;
	pool<std::pair<int, int>> upstream_boundaries;
	pool<std::pair<int, int>> downstream_boundaries;
};

struct CellGraph;

struct RegionSelectionContext {
	const CellGraph &graph;
	// The graph's common retained-connection index, never physical adjacency.
	const std::vector<std::vector<ConnectionId>> &incoming;
	const std::vector<std::vector<ConnectionId>> &outgoing;
};

struct RegionCellSelection {
	std::vector<GraphCellId> selected_cells;
	pool<std::pair<int, int>> upstream_boundaries;
	pool<std::pair<int, int>> downstream_boundaries;
};

// Select a subset of this region without changing its membership or the graph.
// The result may be empty and need not have a fixed cardinality or use scores.
using RegionCellSelector = std::function<RegionCellSelection(const RegionSelectionContext &, const CellRegion &)>;

struct CellGraphFilterOptions {
	int top_k = 0;
	RegionCellSelector region_selector;

	bool filtered() const
	{
		return top_k > 0 || bool(region_selector);
	}
};

// The object set selected for a particular consumer. This is deliberately
// separate from CellGraphView: selection decides which objects are included,
// while the view decides how connections between them are rendered. A rejected
// object remains in CellGraph and in RTLIL; only the consumer view omits it.
struct CellGraphSelection {
	pool<GraphCellId> selected_cells;
	pool<PortId> selected_module_ports;
	std::vector<CellRegion> regions;
};

struct ViewConnection {
	PortId src = INVALID_PORT_ID;
	PortId dst = INVALID_PORT_ID;
	int bit_count = 0;
	bool collapsed = false;
	std::vector<GraphCellId> hidden_cells;
};

struct CellGraphView {
	pool<GraphCellId> visible_cells;
	pool<PortId> visible_module_ports;
	std::vector<ViewConnection> connections;
	std::vector<CellRegion> regions;

	int visible_edge_cell_count = 0;
	int hidden_edge_cell_count = 0;
};

} // namespace CellFuzz
YOSYS_NAMESPACE_END

#endif
