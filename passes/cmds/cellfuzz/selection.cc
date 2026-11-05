#include "selection.h"

#include <algorithm>
#include <deque>
#include <map>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {

static int integer_log2(unsigned long long value)
{
	int result = -1;
	do {
		result++;
		value >>= 1;
	} while (value != 0);
	return result;
}

static int semantic_weight(RTLIL::IdString type)
{
	if (type.in(ID($mux), ID($pmux), ID($bmux), ID($demux), ID($bwmux),
			ID($mul), ID($div), ID($mod), ID($divfloor), ID($modfloor),
			ID($pow), ID($macc), ID($macc_v2), ID($alu)))
		return 8;
	if (type.in(ID($add), ID($sub), ID($shl), ID($shr), ID($sshl), ID($sshr),
			ID($shift), ID($shiftx), ID($lt), ID($le), ID($eq), ID($ne),
			ID($eqx), ID($nex), ID($ge), ID($gt)))
		return 6;
	if (type.in(ID($reduce_and), ID($reduce_or), ID($reduce_xor), ID($reduce_xnor),
			ID($reduce_bool), ID($logic_not), ID($logic_and), ID($logic_or),
			ID($lut), ID($sop), ID($lcu), ID($fa)))
		return 5;
	if (type.in(ID($and), ID($or), ID($xor), ID($xnor)))
		return 4;
	if (type.in(ID($not), ID($neg), ID($pos)))
		return 3;
	if (type.in(ID($buf), ID($slice), ID($concat), ID($barrier)))
		return 1;
	return 4;
}

static std::pair<int, int> boundary_key(const GraphPort &port)
{
	if (port.is_cell_port())
		return std::make_pair(0, port.cell);
	return std::make_pair(1, port.id);
}

static RegionCellSelection select_region_topk(const RegionSelectionContext &context,
		const CellRegion &region, int top_k)
{
	const auto &cells = context.graph.cells;
	const auto &ports = context.graph.ports;
	const auto &connections = context.graph.connections;
	const auto &incoming = context.incoming;
	const auto &outgoing = context.outgoing;
	RegionCellSelection result;
	struct CellScore {
		GraphCellId cell_id;
		int score;
	};

	pool<GraphCellId> region_cells(region.edge_cells.begin(), region.edge_cells.end());
	std::vector<CellScore> scores;
	for (GraphCellId cell_id : region.edge_cells) {
		pool<std::pair<int, int>> unique_fanin, unique_fanout;
		pool<std::pair<int, int>> upstream_boundaries, downstream_boundaries;

		for (PortId input : cells.at(cell_id).inputs)
			for (ConnectionId connection_id : incoming.at(input)) {
				const GraphPort &src = ports.at(connections.at(connection_id).src);
				unique_fanin.insert(boundary_key(src));
			}
		for (PortId output : cells.at(cell_id).outputs)
			for (ConnectionId connection_id : outgoing.at(output)) {
				const GraphPort &dst = ports.at(connections.at(connection_id).dst);
				unique_fanout.insert(boundary_key(dst));
			}

		pool<GraphCellId> seen_upstream, seen_downstream;
		std::deque<GraphCellId> upstream_queue, downstream_queue;
		seen_upstream.insert(cell_id);
		seen_downstream.insert(cell_id);
		upstream_queue.push_back(cell_id);
		downstream_queue.push_back(cell_id);

		while (!upstream_queue.empty()) {
			GraphCellId current = upstream_queue.front();
			upstream_queue.pop_front();
			for (PortId input : cells.at(current).inputs)
				for (ConnectionId connection_id : incoming.at(input)) {
					const GraphPort &src = ports.at(connections.at(connection_id).src);
					if (src.is_cell_port() && region_cells.count(src.cell)) {
						if (seen_upstream.insert(src.cell).second)
							upstream_queue.push_back(src.cell);
					} else {
						upstream_boundaries.insert(boundary_key(src));
						result.upstream_boundaries.insert(boundary_key(src));
					}
				}
		}

		while (!downstream_queue.empty()) {
			GraphCellId current = downstream_queue.front();
			downstream_queue.pop_front();
			for (PortId output : cells.at(current).outputs)
				for (ConnectionId connection_id : outgoing.at(output)) {
					const GraphPort &dst = ports.at(connections.at(connection_id).dst);
					if (dst.is_cell_port() && region_cells.count(dst.cell)) {
						if (seen_downstream.insert(dst.cell).second)
							downstream_queue.push_back(dst.cell);
					} else {
						downstream_boundaries.insert(boundary_key(dst));
						result.downstream_boundaries.insert(boundary_key(dst));
					}
				}
		}

		int data_width = 0;
		for (PortId output : cells.at(cell_id).outputs)
			data_width = std::max(data_width, GetSize(ports.at(output).raw_signal));
		if (data_width == 0)
			for (PortId input : cells.at(cell_id).inputs)
				data_width = std::max(data_width, GetSize(ports.at(input).raw_signal));

		unsigned long long span = (unsigned long long)GetSize(upstream_boundaries) *
				(unsigned long long)GetSize(downstream_boundaries);
		int score = 4 * semantic_weight(cells.at(cell_id).type) +
				2 * integer_log2(1 + GetSize(unique_fanin) + GetSize(unique_fanout)) +
				3 * integer_log2(1 + span) + integer_log2(1 + data_width);
		scores.push_back(CellScore{cell_id, score});
	}

	std::sort(scores.begin(), scores.end(), [&](const CellScore &lhs, const CellScore &rhs) {
		if (lhs.score != rhs.score)
			return lhs.score > rhs.score;
		const GraphCell &lhs_cell = cells.at(lhs.cell_id);
		const GraphCell &rhs_cell = cells.at(rhs.cell_id);
		if (lhs_cell.type.str() != rhs_cell.type.str())
			return lhs_cell.type.str() < rhs_cell.type.str();
		if (lhs_cell.name.str() != rhs_cell.name.str())
			return lhs_cell.name.str() < rhs_cell.name.str();
		return lhs.cell_id < rhs.cell_id;
	});

	int key_count = std::min(top_k, GetSize(scores));
	for (int index = 0; index < key_count; index++) {
		GraphCellId cell_id = scores.at(index).cell_id;
		result.selected_cells.push_back(cell_id);
	}
	std::sort(result.selected_cells.begin(), result.selected_cells.end());
	return result;
}

CellGraphSelection CellGraph::select_objects(const CellGraphFilterOptions &options) const
{
	CellGraphSelection selection;

	for (GraphCellId node_id : nodes)
		if (cells.at(node_id).in_graph)
			selection.selected_cells.insert(node_id);

	if (!options.filtered()) {
		for (GraphCellId cell_id : edge_cells)
			selection.selected_cells.insert(cell_id);
	} else {
		std::vector<std::vector<GraphCellId>> undirected(cells.size());
		for (const GraphConnection &connection : connections) {
			const GraphPort &src = ports.at(connection.src);
			const GraphPort &dst = ports.at(connection.dst);
			if (!src.is_cell_port() || !dst.is_cell_port())
				continue;
			if (cells.at(src.cell).is_node() || cells.at(dst.cell).is_node())
				continue;
			undirected.at(src.cell).push_back(dst.cell);
			undirected.at(dst.cell).push_back(src.cell);
		}

		pool<GraphCellId> visited;
		for (GraphCellId seed : edge_cells) {
			if (!visited.insert(seed).second)
				continue;
			CellRegion region;
			region.id = GetSize(selection.regions);
			std::deque<GraphCellId> queue;
			queue.push_back(seed);
			while (!queue.empty()) {
				GraphCellId cell_id = queue.front();
				queue.pop_front();
				region.edge_cells.push_back(cell_id);
				for (GraphCellId neighbor : undirected.at(cell_id))
					if (visited.insert(neighbor).second)
						queue.push_back(neighbor);
			}
			std::sort(region.edge_cells.begin(), region.edge_cells.end());
			selection.regions.push_back(std::move(region));
		}

		RegionSelectionContext context{*this, incoming, outgoing};
		for (CellRegion &region : selection.regions) {
			RegionCellSelection result = options.region_selector ? options.region_selector(context, region) :
					select_region_topk(context, region, options.top_k);
			pool<GraphCellId> candidates(region.edge_cells.begin(), region.edge_cells.end());
			pool<GraphCellId> selected;
			for (GraphCellId cell_id : result.selected_cells) {
				log_assert(candidates.count(cell_id));
				log_assert(selected.insert(cell_id).second);
				selection.selected_cells.insert(cell_id);
			}
			region.key_cells = std::move(result.selected_cells);
			std::sort(region.key_cells.begin(), region.key_cells.end());
			region.upstream_boundaries = std::move(result.upstream_boundaries);
			region.downstream_boundaries = std::move(result.downstream_boundaries);
		}
	}

	// Preserve the existing DOT boundary set: only module ports incident to
	// retained connections participate in automatic selection.
	for (const GraphConnection &connection : connections) {
		if (!ports.at(connection.src).is_cell_port())
			selection.selected_module_ports.insert(connection.src);
		if (!ports.at(connection.dst).is_cell_port())
			selection.selected_module_ports.insert(connection.dst);
	}
	return selection;
}

CellGraphView CellGraph::create_view(const CellGraphSelection &selection) const
{
	CellGraphView view;
	view.visible_cells = selection.selected_cells;
	view.visible_module_ports = selection.selected_module_ports;
	view.regions = selection.regions;
	for (GraphCellId cell_id : view.visible_cells) {
		log_assert(cell_id >= 0 && cell_id < GetSize(cells));
		log_assert(cells.at(cell_id).in_graph);
		if (!cells.at(cell_id).is_node())
			view.visible_edge_cell_count++;
	}
	for (PortId port_id : view.visible_module_ports) {
		log_assert(port_id >= 0 && port_id < GetSize(ports));
		log_assert(!ports.at(port_id).is_cell_port());
	}
	view.hidden_edge_cell_count = GetSize(edge_cells) - view.visible_edge_cell_count;

	auto port_is_visible = [&](PortId port_id) {
		const GraphPort &port = ports.at(port_id);
		return port.is_cell_port() ? view.visible_cells.count(port.cell) :
				view.visible_module_ports.count(port.id);
	};

	if (view.hidden_edge_cell_count == 0) {
		for (const GraphConnection &connection : connections) {
			if (!port_is_visible(connection.src) || !port_is_visible(connection.dst))
				continue;
			ViewConnection item;
			item.src = connection.src;
			item.dst = connection.dst;
			item.bit_count = connection.bit_count;
			view.connections.push_back(std::move(item));
		}
	} else {
		std::map<std::pair<PortId, PortId>, ViewConnection> folded;
		std::vector<PortId> source_ports;
		for (GraphCellId cell_id : view.visible_cells)
			for (PortId output : cells.at(cell_id).outputs)
				source_ports.push_back(output);
		for (PortId module_input : module_inputs)
			if (view.visible_module_ports.count(module_input))
				source_ports.push_back(module_input);
		std::sort(source_ports.begin(), source_ports.end());

		for (PortId source : source_ports) {
			std::deque<PortId> queue;
			dict<PortId, pool<GraphCellId>> hidden_at_port;
			pool<PortId> queued;
			queue.push_back(source);
			queued.insert(source);
			while (!queue.empty()) {
				PortId current_port = queue.front();
				queue.pop_front();
				queued.erase(current_port);
				const pool<GraphCellId> current_hidden = hidden_at_port[current_port];
				for (ConnectionId connection_id : outgoing.at(current_port)) {
					const GraphConnection &connection = connections.at(connection_id);
					const GraphPort &dst = ports.at(connection.dst);
					if (port_is_visible(dst.id)) {
						std::pair<PortId, PortId> key(source, connection.dst);
						ViewConnection &item = folded[key];
						item.src = source;
						item.dst = connection.dst;
						if (current_hidden.empty()) {
							item.bit_count += connection.bit_count;
						} else {
							item.collapsed = true;
							pool<GraphCellId> merged(item.hidden_cells.begin(), item.hidden_cells.end());
							for (GraphCellId hidden_id : current_hidden)
								merged.insert(hidden_id);
							item.hidden_cells.assign(merged.begin(), merged.end());
							std::sort(item.hidden_cells.begin(), item.hidden_cells.end());
						}
						continue;
					}

					// Only hidden edge cells are traversable. An omitted state
					// node remains a boundary, just like an omitted module port.
					if (!dst.is_cell_port() || !cells.at(dst.cell).is_edge())
						continue;
					GraphCellId hidden_id = dst.cell;
					pool<GraphCellId> next_hidden = current_hidden;
					next_hidden.insert(hidden_id);
					for (PortId output : cells.at(hidden_id).outputs) {
						bool changed = false;
						pool<GraphCellId> &known = hidden_at_port[output];
						for (GraphCellId item : next_hidden)
							changed |= known.insert(item).second;
						if (changed && queued.insert(output).second)
							queue.push_back(output);
					}
				}
			}
		}

		for (auto &item : folded)
			view.connections.push_back(std::move(item.second));
	}
	return view;
}


} // namespace CellFuzz
YOSYS_NAMESPACE_END
