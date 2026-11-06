#include "kernel/register.h"
#include "passes/cmds/cellfuzz/analysis_snapshot.h"
#include "libs/sha1/sha1.h"

#include <fstream>
#include <functional>
#include <sstream>

USING_YOSYS_NAMESPACE
using namespace Yosys::CellFuzz;
PRIVATE_NAMESPACE_BEGIN

static void expect_error(const std::function<void()> &action)
{
	bool failed = false;
	{
		auto errors = logger().error_throw_scope();
		try { action(); }
		catch (const log_cmd_error_exception &) { failed = true; }
	}
	log_assert(failed);
}

static RTLIL::Module *value_fixture(RTLIL::Design &design, bool reverse_cells = false)
{
	auto module = design.addModule(RTLIL::IdString("\\module.\"quoted\"[0]\\suffix"));
	module->attributes[ID::src] = RTLIL::Const("source:\"quoted\"\nsecond line");
	module->parameter_default_values[RTLIL::escape_id("wide")] = RTLIL::Const::from_string(std::string(65, '1') + "0xz");
	auto input = module->addWire(RTLIL::IdString("\\a.\"quoted\""), 4);
	input->port_input = true;
	input->start_offset = 9;
	input->upto = true;
	auto alias = module->addWire(RTLIL::escape_id("alias"), 4);
	auto other_alias = module->addWire(RTLIL::escape_id("other_alias"), 4);
	module->connect(alias, input);
	module->connect(other_alias, input);
	auto middle = module->addWire(RTLIL::escape_id("middle"), 6);
	auto output = module->addWire(RTLIL::escape_id("y"), 6);
	output->port_output = true;
	RTLIL::SigSpec raw;
	raw.append(RTLIL::SigBit(input, 3));
	raw.append(RTLIL::SigBit(alias, 1));
	raw.append(RTLIL::State::Sx);
	raw.append(RTLIL::SigBit(alias, 1));
	raw.append(RTLIL::State::Sz);
	raw.append(RTLIL::SigBit(input, 0));
	auto first = [&]() { module->addNot(RTLIL::IdString("\\first.\"[0]"), raw, middle, false, "fixture:2"); };
	auto second = [&]() { module->addNot(RTLIL::escape_id("second"), middle, output, false, "fixture:3"); };
	if (reverse_cells) { second(); first(); }
	else { first(); second(); }
	module->fixup_ports();
	return module;
}

static void test_owned_records_and_rebinding(bool empty_selection)
{
	std::string encoded, batch, dot, digest;
	{
		RTLIL::Design original;
		auto module = value_fixture(original);
		CellGraph graph(module);
		graph.build();
		CellGraphFilterOptions options;
		options.top_k = 1;
		options.region_selector = [empty_selection](const RegionSelectionContext &, const CellRegion &region) {
			RegionCellSelection result;
			if (!empty_selection) {
				result.selected_cells = region.edge_cells;
				std::reverse(result.selected_cells.begin(), result.selected_cells.end());
			}
			return result;
		};
		auto selection = graph.select_objects(options);
		// Both callback results differ from built-in K=1: retain either zero or
		// both cells, then destroy the callback and the original Design.
		log_assert(GetSize(selection.selected_cells) == (empty_selection ? 0 : 2));
		auto snapshot = AnalysisSnapshot::capture(graph, selection,
				SelectionStrategy{empty_selection ? "fixture-empty" : "fixture-all", 7, {{"note", "a\n\"b"}}});
		digest = snapshot.structure_digest;
		encoded = snapshot.encode();
		SnapshotStore::publish(&original, {snapshot});
		batch = original.scratchpad_get_string(SnapshotStore::key);
		std::ostringstream stream;
		graph.dump_dot(stream, 0, graph.create_view(selection));
		dot = stream.str();
	} // Destroy every original module, cell, wire, SigMap and callback.
	RTLIL::Design current;
	auto module = value_fixture(current, true);
	log_assert(structural_digest(module) == digest);
	current.scratchpad_set_string(SnapshotStore::key, batch);
	auto restored = SnapshotStore::restore(&current, module->name);
	log_assert(GetSize(restored.selection.selected_cells) == (empty_selection ? 0 : 2));
	log_assert(restored.strategy.id == (empty_selection ? "fixture-empty" : "fixture-all") && restored.strategy.version == 7);
	log_assert(AnalysisSnapshot::capture(*restored.graph, restored.selection, restored.strategy).encode() == encoded);
	for (const auto &cell : restored.graph->cells)
		log_assert(cell.cell == module->cell(cell.name));
	for (const auto &port : restored.graph->ports)
		for (auto bit : port.raw_signal)
			if (bit.wire) log_assert(bit.wire == module->wire(bit.wire->name));
	std::ostringstream stream;
	restored.graph->dump_dot(stream, 0, restored.view);
	log_assert(stream.str() == dot);
	// Dictionary/connect ordering and temporary analysis state are not changes
	// to the functional design. Canonical representative names stay restorable.
	module->sort();
	std::reverse(module->connections_.begin(), module->connections_.end());
	current.scratchpad_set_string("unrelated", "new timestamp/output path");
	current.selection_stack.emplace_back(false);
	log_assert(structural_digest(module) == digest);
	SnapshotStore::restore(&current, module->name);
	SnapshotStore::invalidate(&current);
	expect_error([&]() { SnapshotStore::restore(&current, module->name); });
}

static void test_stale_and_malformed()
{
	RTLIL::Design design;
	auto module = value_fixture(design);
	CellGraph graph(module);
	graph.build();
	auto snapshot = AnalysisSnapshot::capture(graph, graph.select_objects(CellGraphFilterOptions{}), SelectionStrategy{});
	auto input = module->wire(RTLIL::IdString("\\a.\"quoted\""));
	auto first = module->cell(RTLIL::IdString("\\first.\"[0]"));
	auto stale = [&]() { expect_error([&]() { snapshot.restore(&design); }); };
	input->attributes[ID::init] = RTLIL::Const(0, 4); stale(); input->attributes.clear();
	input->width++; stale(); input->width--;
	input->start_offset++; stale(); input->start_offset--;
	input->upto = false; stale(); input->upto = true;
	input->is_signed = true; stale(); input->is_signed = false;
	input->port_id++; stale(); input->port_id--;
	input->port_input = false; stale(); input->port_input = true;
	auto old_parameter = first->parameters.at(ID::A_WIDTH);
	first->parameters[ID::A_WIDTH] = RTLIL::Const(99); stale(); first->parameters[ID::A_WIDTH] = old_parameter;
	first->type = ID($pos); stale(); first->type = ID($not);
	auto raw = first->getPort(ID::A), changed = raw;
	std::swap(changed[0], changed[1]); first->setPort(ID::A, changed); stale();
	changed = raw; changed[2] = RTLIL::SigBit(RTLIL::State::Sz); first->setPort(ID::A, changed); stale();
	changed = raw; changed[3] = changed[0]; first->setPort(ID::A, changed); stale(); first->setPort(ID::A, raw);
	auto old_connections = module->connections();
	module->connections_.clear(); stale(); module->new_connections(old_connections);
	module->rename(first, RTLIL::escape_id("renamed")); stale(); module->rename(first, RTLIL::IdString("\\first.\"[0]"));
	auto old_name = module->name;
	design.rename(module, RTLIL::escape_id("renamed_module")); stale(); design.rename(module, old_name);
	snapshot.restore(&design);

	expect_error([&]() { AnalysisSnapshot::decode("{broken"); });
	std::string error;
	auto record = json11::Json::parse(snapshot.encode(), error).object_items();
	record["version"] = 999;
	expect_error([&]() { AnalysisSnapshot::decode(json11::Json(record).dump()); });
	record["version"] = 1;
	expect_error([&]() { AnalysisSnapshot::decode(json11::Json(record).dump()); });
	record["version"] = AnalysisSnapshot::version;
	record["analysis_id"] = "corrupt";
	expect_error([&]() { AnalysisSnapshot::decode(json11::Json(record).dump()); });
	// The integrity ID is not authentication. A manually assembled, self-hashed
	// internal record must still fail range/type validation during binding.
	record = json11::Json::parse(snapshot.encode(), error).object_items();
	auto graph_record = record["graph"].object_items();
	auto ports = graph_record["ports"].array_items();
	auto port = ports.front().array_items();
	auto signal = port[3].array_items();
	signal[0] = json11::Json::array{"\\alias", 999};
	port[3] = signal; ports[0] = port; graph_record["ports"] = ports; record["graph"] = graph_record;
	record.erase("analysis_id");
	record["analysis_id"] = sha1(json11::Json(record).dump());
	expect_error([&]() { AnalysisSnapshot::decode(json11::Json(record).dump()).restore(&design); });
	design.scratchpad_set_string(SnapshotStore::key, "not a batch");
	expect_error([&]() { SnapshotStore::load(&design, module->name); });
	design.scratchpad_set_string(SnapshotStore::key, "{\"version\":999,\"modules\":[]}");
	expect_error([&]() { SnapshotStore::load(&design, module->name); });
	design.scratchpad_set_string(SnapshotStore::key, "{\"version\":1,\"modules\":[]}");
	expect_error([&]() { SnapshotStore::load(&design, module->name); });
	SnapshotStore::publish(&design, {snapshot});
	expect_error([&]() { SnapshotStore::restore(&design, RTLIL::escape_id("missing")); });
	module->remove(first);
	stale();
}

static void test_dependencies_and_processes()
{
	RTLIL::Design design;
	auto root = design.addModule(RTLIL::escape_id("root"));
	auto child = design.addModule(RTLIL::escape_id("child"));
	auto leaf = design.addModule(RTLIL::escape_id("leaf"));
	auto unrelated = design.addModule(RTLIL::escape_id("unrelated"));
	root->addCell(RTLIL::escape_id("instance"), child->name);
	child->addCell(RTLIL::escape_id("instance"), leaf->name);
	leaf->addCell(RTLIL::escape_id("cycle"), child->name);
	CellGraph graph(root);
	graph.build();
	auto snapshot = AnalysisSnapshot::capture(graph, graph.select_objects(CellGraphFilterOptions{}), SelectionStrategy{});
	auto before = structural_digest(root);
	unrelated->attributes[ID::init] = RTLIL::Const(1);
	log_assert(before == structural_digest(root));
	leaf->attributes[ID::init] = RTLIL::Const(1);
	log_assert(before != structural_digest(root));
	expect_error([&]() { snapshot.restore(&design); });
	leaf->attributes.clear();
	log_assert(before == structural_digest(root));
	auto process = leaf->addProcess(RTLIL::escape_id("process"));
	before = structural_digest(root);
	process->root_case.actions.push_back({RTLIL::SigSpec(RTLIL::State::Sx), RTLIL::SigSpec(RTLIL::State::Sz)});
	log_assert(before != structural_digest(root));
}

static void test_digest_and_roundtrip()
{
	RTLIL::Design design;
	auto module = design.addModule(RTLIL::escape_id("snapshot"));
	auto input = module->addWire(RTLIL::escape_id("a"), 4);
	auto output = module->addWire(RTLIL::escape_id("y"), 4);
	input->port_input = output->port_output = true;
	module->addNot(RTLIL::escape_id("logic"), input, output);
	module->fixup_ports();
	CellGraph graph(module);
	graph.build();
	auto selection = graph.select_objects(CellGraphFilterOptions{});
	auto snapshot = AnalysisSnapshot::capture(graph, selection, SelectionStrategy{});
	log_assert(snapshot.structure_digest == structural_digest(module));
	auto restored = AnalysisSnapshot::decode(snapshot.encode()).restore(&design);
	log_assert(restored.selection.selected_cells == selection.selected_cells);
	log_assert(restored.graph->ports.size() == graph.ports.size());
	for (const auto &port : graph.ports) {
		log_assert(restored.graph->ports.at(port.id).raw_signal == port.raw_signal);
		log_assert(restored.graph->ports.at(port.id).canonical_signal == port.canonical_signal);
	}
	SnapshotStore::publish(&design, {snapshot});
	log_assert(SnapshotStore::load(&design, module->name).analysis_id == snapshot.analysis_id);
	auto before = structural_digest(module);
	input->attributes[ID::init] = RTLIL::Const(0, 4);
	log_assert(before != structural_digest(module));
	auto empty = design.addModule(RTLIL::escape_id("empty"));
	CellGraph empty_graph(empty);
	empty_graph.build();
	auto empty_snapshot = AnalysisSnapshot::capture(empty_graph, CellGraphSelection{}, SelectionStrategy{});
	SnapshotStore::publish(&design, {empty_snapshot});
	log_assert(SnapshotStore::restore(&design, empty->name).graph->ports.empty());
	expect_error([&]() { SnapshotStore::load(&design, module->name); });
}

static void test_shared_digest_context()
{
	RTLIL::Design design, reversed;
	std::vector<std::string> names{"root", "left", "right", "leaf", "cycle1", "cycle2", "self", "isolated", "unrelated"};
	for (auto name : names) design.addModule(RTLIL::escape_id(name));
	for (auto it = names.rbegin(); it != names.rend(); ++it) reversed.addModule(RTLIL::escape_id(*it));
	std::vector<std::pair<std::string, std::string>> edges{{"root", "left"}, {"root", "right"},
			{"left", "leaf"}, {"right", "leaf"}, {"root", "cycle1"}, {"cycle1", "cycle2"},
			{"cycle2", "cycle1"}, {"cycle2", "leaf"}, {"self", "self"}, {"root", "left"}};
	for (auto current : {&design, &reversed}) {
		int index = 0;
		for (const auto &edge : edges)
			current->module(RTLIL::escape_id(edge.first))->addCell(RTLIL::escape_id("u" + std::to_string(index++)), RTLIL::escape_id(edge.second));
	}
	auto root = design.module(RTLIL::escape_id("root"));
	auto leaf = design.module(RTLIL::escape_id("leaf"));
	auto isolated = design.module(RTLIL::escape_id("isolated"));
	auto self = design.module(RTLIL::escape_id("self"));
	std::vector<RTLIL::Module *> roots{root, self, isolated};
	std::map<std::string, std::string> hashes;
	{
		StructuralDigestContext context(&design, roots);
		log_assert(context.local_record_count() == 8); // unrelated is unreachable
		for (auto module : roots) {
			hashes[module->name.str()] = context.digest(module);
			log_assert(context.digest(module) == structural_digest(module));
			log_assert(context.digest(module) == structural_digest(reversed.module(module->name)));
			CellGraph graph(module); graph.build();
			log_assert(AnalysisSnapshot::capture(graph, graph.select_objects({}), {}, context).structure_digest == context.digest(module));
		}
		log_assert(context.local_record_count() == 8); // repeated roots do no serialization
		expect_error([&]() { context.digest(leaf); }); // reachable is not necessarily a root
		expect_error([&]() { context.digest(reversed.module(root->name)); });
		CellGraph other(reversed.module(root->name)); other.build();
		expect_error([&]() { AnalysisSnapshot::capture(other, other.select_objects({}), {}, context); });
		expect_error([&]() { StructuralDigestContext invalid(&design, {other.module}); });
	}
	// A changed caller must not invalidate a child. Only outgoing dependency
	// hashes participate; no incoming edges or design-global version is used.
	auto child_before = structural_digest(leaf);
	root->attributes[ID::src] = RTLIL::Const("changed caller");
	log_assert(child_before == structural_digest(leaf));
	root->attributes.clear();
	auto unrelated = design.module(RTLIL::escape_id("unrelated"));
	unrelated->attributes[ID::src] = RTLIL::Const("unrelated change");
	log_assert(hashes.at(root->name.str()) == structural_digest(root));
	auto cycle1 = design.module(RTLIL::escape_id("cycle1")), cycle2 = design.module(RTLIL::escape_id("cycle2"));
	auto cycle1_before = structural_digest(cycle1), cycle2_before = structural_digest(cycle2);
	log_assert(cycle1_before != cycle2_before); // root identity inside one SCC
	leaf->attributes[ID::init] = RTLIL::Const(1);
	log_assert(hashes.at(root->name.str()) != structural_digest(root));
	log_assert(cycle1_before != structural_digest(cycle1) && cycle2_before != structural_digest(cycle2));
	log_assert(hashes.at(self->name.str()) == structural_digest(self));
	log_assert(hashes.at(isolated->name.str()) == structural_digest(isolated));
	leaf->attributes.clear();
	log_assert(hashes.at(root->name.str()) == structural_digest(root));
	// All these changes live only in a dependency. Binding the unchanged root
	// graph must nevertheless reject its saved snapshot after each mutation.
	auto a = leaf->addWire(RTLIL::escape_id("a"), 4), y = leaf->addWire(RTLIL::escape_id("y"), 4);
	auto logic = leaf->addNot(RTLIL::escape_id("logic"), a, y);
	auto process = leaf->addProcess(RTLIL::escape_id("priority"));
	process->root_case.actions = {{y, RTLIL::Const(1, 4)}, {y, RTLIL::Const(2, 4)}};
	CellGraph root_graph(root); root_graph.build();
	auto snapshot = AnalysisSnapshot::capture(root_graph, root_graph.select_objects({}), {});
	auto stale = [&]() { expect_error([&]() { snapshot.restore(&design); }); };
	leaf->parameter_default_values[ID::WIDTH] = RTLIL::Const(9); stale(); leaf->parameter_default_values.clear();
	a->attributes[ID::init] = RTLIL::Const(3, 4); stale(); a->attributes.clear();
	RTLIL::SigSpec reversed_bits;
	for (int i = 3; i >= 0; i--) reversed_bits.append(RTLIL::SigBit(a, i));
	logic->setPort(ID::A, reversed_bits); stale(); logic->setPort(ID::A, a);
	std::reverse(process->root_case.actions.begin(), process->root_case.actions.end()); stale();
	std::reverse(process->root_case.actions.begin(), process->root_case.actions.end());
	snapshot.restore(&design);
	// A deep chain and all its roots: counts, not noisy wall-clock thresholds,
	// establish bounded local work. Duplicate instances above still count once.
	RTLIL::Design chain;
	std::vector<RTLIL::Module *> modules;
	for (int i = 0; i < 4096; i++) modules.push_back(chain.addModule(RTLIL::escape_id("m" + std::to_string(i))));
	for (int i = 1; i < GetSize(modules); i++) modules[i-1]->addCell(RTLIL::escape_id("u"), modules[i]->name);
	StructuralDigestContext context(&chain, modules);
	log_assert(context.local_record_count() == modules.size());
	for (auto module : modules) {
		CellGraph graph(module); graph.build();
		log_assert(!AnalysisSnapshot::capture(graph, graph.select_objects({}), {}, context).structure_digest.empty());
	}
	log_assert(context.local_record_count() == modules.size());
}

struct TestCellFuzzSnapshotPass : Pass {
	TestCellFuzzSnapshotPass() : Pass("test_cellfuzz_snapshot", "test CellFuzz session snapshots") {}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		if (GetSize(args) == 2 && args[1] == "poison-last") {
			auto modules = design->selected_modules();
			log_assert(GetSize(modules) > 1);
			auto memory = new RTLIL::Memory;
			memory->name = RTLIL::escape_id("uncollected");
			memory->width = 8;
			memory->size = 4;
			memory->start_offset = 0;
			modules.back()->memories[memory->name] = memory;
			return;
		}
		if (GetSize(args) == 4 && args[1] == "inspect") {
			auto snapshot = SnapshotStore::load(design, RTLIL::escape_id(args[2]));
			auto restored = snapshot.restore(design);
			auto roundtrip = AnalysisSnapshot::capture(*restored.graph, restored.selection, restored.strategy);
			log_assert(roundtrip.encode() == snapshot.encode());
			std::ofstream output(args[3]);
			output << snapshot.encode();
			output.close();
			log_assert(bool(output));
			return;
		}
		if (GetSize(args) >= 3 && args[1] == "failing-filter") {
			bool failed = false;
			{
				auto errors = logger().error_throw_scope();
				std::vector<std::string> command{"cellfilter"};
				command.insert(command.end(), args.begin() + 2, args.end());
				try { Pass::call(design, command); }
				catch (const log_cmd_error_exception &) { failed = true; }
			}
			log_assert(failed);
			log_assert(!design->scratchpad.count(SnapshotStore::key));
			return;
		}
		extra_args(args, 1, design);
		test_digest_and_roundtrip();
		test_owned_records_and_rebinding(true);
		test_owned_records_and_rebinding(false);
		test_stale_and_malformed();
		test_dependencies_and_processes();
		test_shared_digest_context();
	}
} TestCellFuzzSnapshotPass;

PRIVATE_NAMESPACE_END
