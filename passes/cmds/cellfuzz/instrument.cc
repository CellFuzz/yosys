#include "instrument.h"

#include <climits>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {

void materialize_observations(RTLIL::Module *module, const MonitorPlan &plan, const TransportLayout &layout)
{
	validate_transport(plan, layout, module);
	if (layout.ports.size() > size_t(INT_MAX) - module->ports.size())
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
	module->fixup_ports();
	if (module->ports.size() != original_ports.size() + layout.ports.size())
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
