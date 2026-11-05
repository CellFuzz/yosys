#include "kernel/register.h"
#include "libs/json11/json11.hpp"

#include <fstream>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN
using Json = json11::Json;

static RTLIL::SigSpec original_source(const Json &bits, RTLIL::Module *module)
{
	RTLIL::SigSpec result;
	for (const auto &bit : bits.array_items()) {
		if (bit.is_number()) {
			log_assert(bit.int_value() >= 0 && bit.int_value() <= 5);
			result.append(RTLIL::State(bit.int_value()));
		} else {
			auto wire = module->wire(RTLIL::IdString(bit[0].string_value()));
			log_assert(wire && bit[1].int_value() >= 0 && bit[1].int_value() < wire->width);
			result.append(RTLIL::SigBit(wire, bit[1].int_value()));
		}
	}
	return result;
}

// Test-only reference exposure. This deliberately includes no CellFuzz header
// or graph/planner API: the gold side resolves frozen raw names in the original
// RTLIL; the gate side can only consume the materialized transport interface.
struct TestCellFuzzReference : Pass {
	TestCellFuzzReference() : Pass("test_cellfuzz_reference", "expose independent reference or decoded item values") {}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_assert(GetSize(args) == 5 && (args[1] == "gold" || args[1] == "gate"));
		std::ifstream file(args[2]);
		log_assert(file.is_open());
		std::string content((std::istreambuf_iterator<char>(file)), {}), error;
		auto report = Json::parse(content, error);
		log_assert(error.empty() && report["schema_version"] == 1 && report["status"] == "materialized");
		auto module = design->module(RTLIL::escape_id(args[3]));
		log_assert(module);
		// The final argument is the output prefix used only by the harness.
		std::vector<RTLIL::SigSpec> values;
		std::vector<std::vector<bool>> covered;
		for (const auto &item : report["items"].array_items()) {
			log_assert(item["monitor_id"] == GetSize(values) && item["width"].int_value() > 0);
			int width = item["width"].int_value();
			values.push_back(args[1] == "gold" ? original_source(item["source"], module) : RTLIL::SigSpec(RTLIL::State::Sx, width));
			log_assert(GetSize(values.back()) == width);
			covered.emplace_back(width, false);
			if (args[1] == "gold") {
				auto owner = module->cell(RTLIL::IdString(item["cell"].string_value()));
				log_assert(owner && owner->getPort(RTLIL::IdString(item["port"].string_value())) == values.back());
			}
		}
		if (args[1] == "gate") {
			for (const auto &segment : report["transport"]["segments"].array_items()) {
				int id = segment["monitor_id"].int_value(), offset = segment["item_lsb"].int_value();
				auto wire = module->wire(RTLIL::escape_id(segment["port_name"].string_value()));
				int position = segment["port_lsb"].int_value(), width = segment["width"].int_value();
				log_assert(id >= 0 && id < GetSize(values) && wire && width > 0 && offset >= 0 && position >= 0);
				log_assert(width <= GetSize(values[id]) - offset && width <= wire->width - position);
				for (int bit = 0; bit < width; bit++) {
					log_assert(!covered[id][offset + bit]);
					covered[id][offset + bit] = true;
					values[id][offset + bit] = RTLIL::SigBit(wire, position + bit);
				}
			}
			for (const auto &item : covered)
				for (bool bit : item) log_assert(bit);
			// Compare only original functional ports and reconstructed values.
			// Transport wires stay in the module but are no longer extra ports.
			for (const auto &port : report["transport"]["ports"].array_items()) {
				auto wire = module->wire(RTLIL::escape_id(port["name"].string_value()));
				log_assert(wire && wire->port_output && !wire->port_input && wire->width == port["width"].int_value());
				wire->port_output = false;
			}
		}
		for (int id = 0; id < GetSize(values); id++) {
			auto name = RTLIL::escape_id(stringf("%s%04d", args[4], id));
			log_assert(!module->count_id(name));
			auto wire = module->addWire(name, GetSize(values[id]));
			wire->port_output = true;
			module->connect(wire, values[id]);
		}
		module->fixup_ports();
		module->check();
	}
} TestCellFuzzReference;

PRIVATE_NAMESPACE_END
