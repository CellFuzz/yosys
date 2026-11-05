#include "instrument.h"

#include <algorithm>
#include <climits>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {

void materialize_observations(RTLIL::Module *module, const MonitorPlan &plan, const TransportLayout &layout)
{
	validate_transport(plan, layout, module);
	size_t query_ports = 0;
	for (const auto &info : plan.memories) if (info.query.enabled) query_ports += 2;
	if (layout.ports.size() + query_ports > size_t(INT_MAX) - module->ports.size())
		log_cmd_error("cellinstrument: too many top-level ports.\n");
	for (const auto &item : plan.items)
		for (auto bit : item.source)
			if (bit.wire && (bit.wire->module != module || module->wire(bit.wire->name) != bit.wire ||
					bit.offset < 0 || bit.offset >= bit.wire->width))
				log_cmd_error("cellinstrument: invalid materialization source.\n");
	const auto original_ports = module->ports;
	dict<std::string, RTLIL::Wire *> outputs;
	for (const auto &port : layout.ports) {
		auto wire = module->addWire(RTLIL::escape_id(port.name), port.width);
		wire->port_output = true;
		wire->port_id = GetSize(original_ports) + GetSize(outputs) + 1;
		outputs[port.name] = wire;
	}
	for (const auto &segment : layout.segments)
		module->connect(RTLIL::SigSpec(outputs.at(segment.port_name)).extract(segment.port_lsb, segment.width),
				plan.items[segment.monitor_id].source.extract(segment.item_lsb, segment.width));
	std::vector<std::pair<const MemoryInfo *, Mem>> queries;
	int next_port = GetSize(original_ports) + GetSize(layout.ports);
	for (const auto &info : plan.memories) {
		if (!info.query.enabled) continue;
		auto addr = module->addWire(RTLIL::escape_id(info.query.addr_name), info.query.addr_width);
		auto data = module->addWire(RTLIL::escape_id(info.query.data_name), info.memory.width);
		addr->port_input = true; addr->port_id = ++next_port;
		data->port_output = true; data->port_id = ++next_port;
		queries.emplace_back(&info, memory_with_query(info, addr, data));
	}
	// Every modified helper is prepared and checked before the first emit.
	for (auto &query : queries) {
		query.second.emit();
		// Mem parsing omits fully undefined X/Z words. Preserve the frozen
		// packed INIT exactly, including Z bits and constant flags.
		query.second.cell->setParam(ID::INIT, query.first->original_init);
	}
	if (!queries.empty()) {
		auto actual = Mem::get_all_memories(module);
		for (const auto &query : queries) {
			const auto &info = *query.first;
			auto found = std::find_if(actual.begin(), actual.end(), [&](const Mem &memory) { return memory.cell == info.memory.cell; });
			if (found == actual.end()) log_cmd_error("cellinstrument: emitted query memory missing.\n");
			verify_memory_query(info, *found, module->wire(RTLIL::escape_id(info.query.addr_name)),
					module->wire(RTLIL::escape_id(info.query.data_name)));
		}
	}
	module->fixup_ports();
	if (module->ports.size() != original_ports.size() + layout.ports.size() + query_ports)
		log_cmd_error("cellinstrument: materialized port count differs from plan.\n");
	for (int index = 0; index < GetSize(original_ports); index++)
		if (module->ports[index] != original_ports[index])
			log_cmd_error("cellinstrument: original interface order changed.\n");
	for (int index = 0; index < GetSize(layout.ports); index++) {
		const auto &port = layout.ports[index];
		auto wire = module->wire(RTLIL::escape_id(port.name));
		int position = GetSize(original_ports) + index;
		if (!wire || wire->width != port.width || !wire->port_output || wire->port_input ||
				wire->port_id != position + 1 || module->ports[position] != wire->name)
			log_cmd_error("cellinstrument: materialized interface differs from plan.\n");
	}
	next_port = GetSize(original_ports) + GetSize(layout.ports);
	for (const auto &info : plan.memories) {
		if (!info.query.enabled) continue;
		for (bool input : {true, false}) {
			auto wire = module->wire(RTLIL::escape_id(input ? info.query.addr_name : info.query.data_name));
			int width = input ? info.query.addr_width : info.memory.width;
			if (!wire || wire->width != width || wire->port_input != input || wire->port_output == input ||
					wire->port_id != ++next_port || module->ports[next_port - 1] != wire->name)
				log_cmd_error("cellinstrument: materialized query interface differs from plan.\n");
		}
	}
	// Graph SigMap belongs to the original design. Build current equivalence
	// after connecting the outputs, rather than reusing a stale graph index.
	SigMap actual(module);
	for (const auto &segment : layout.segments)
		if (actual(RTLIL::SigSpec(outputs.at(segment.port_name)).extract(segment.port_lsb, segment.width)) !=
				actual(plan.items[segment.monitor_id].source.extract(segment.item_lsb, segment.width)))
			log_cmd_error("cellinstrument: materialized binding differs from plan.\n");
}

} // namespace CellFuzz
YOSYS_NAMESPACE_END
