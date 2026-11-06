#include "kernel/register.h"
#include "cellfuzz/cellgraph.h"
#include "cellfuzz/selection.h"
#include "cellfuzz/analysis_snapshot.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fstream>

USING_YOSYS_NAMESPACE
using namespace Yosys::CellFuzz;
PRIVATE_NAMESPACE_BEGIN

static bool is_uncollected_memory_port(RTLIL::Cell *cell)
{
	return cell->type.in(ID($memrd), ID($memrd_v2), ID($memwr), ID($memwr_v2),
			ID($meminit), ID($meminit_v2));
}

static CellGraphBuildOptions parse_node_types(const std::string &text)
{
	CellGraphBuildOptions options;
	if (text == "all")
		return options;
	options.register_role = options.memory_role = options.fsm_role = GraphCellRole::EXCLUDED;
	if (text == "none")
		return options;

	size_t begin = 0;
	while (true) {
		size_t end = text.find(',', begin);
		std::string type = text.substr(begin, end == std::string::npos ? end : end - begin);
		if (type == "register")
			options.register_role = GraphCellRole::NODE;
		else if (type == "memory" || type == "mem")
			options.memory_role = GraphCellRole::NODE;
		else if (type == "fsm")
			options.fsm_role = GraphCellRole::NODE;
		else
			log_cmd_error("Invalid node type `%s': expected register, memory (or mem), fsm; "
					"use all or none for the entire set.\n", type);
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return options;
}

struct CellFilterPass : public Pass {
	CellFilterPass() : Pass("cellfilter", "build a state-cell data-path graph") {}

	void help() override
	{
		log("\n");
		log("    cellfilter [options] [selection]\n");
		log("\n");
		log("Build a port-level data-path graph for all cells in the selected modules.\n");
		log("Registers, collected memories and FSM cells are graph nodes by default.\n");
		log("Select node types with -nodes. Unselected state cells are excluded and\n");
		log("remain traversal barriers; they are not converted to edge cells. Combinational\n");
		log("cells on paths between nodes or module ports are retained as edge cells.\n");
		log("Save the latest analysis batch in the current Design, also without -dot.\n");
		log("\n");
		log("    -dot <filename>\n");
		log("        write the graph in Graphviz DOT format\n");
		log("\n");
		log("    -nodes <list>\n");
		log("        comma-separated node types: register,memory,fsm (default: all)\n");
		log("        memory also accepts mem; all and none select the entire or empty set\n");
		log("        repeated -nodes options replace the previous list\n");
		log("\n");
		log("    -topk <integer>\n");
		log("        for each combinational region, retain at most this many key\n");
		log("        edge cells in the DOT graph and collapse hidden paths\n");
		log("\n");
		log("This pass must be run after memory_collect.\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		// Even a failing invocation supersedes the previous batch. Publish only
		// after every module and requested DOT output have completed successfully.
		SnapshotStore::invalidate(design);
		std::string dot_filename;
		CellGraphBuildOptions build_options;
		CellGraphFilterOptions filter_options;
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-nodes") {
				if (argidx + 1 >= args.size())
					log_cmd_error("Option -nodes requires a node type list.\n");
				build_options = parse_node_types(args[++argidx]);
				continue;
			}
			if (args[argidx] == "-dot" && argidx + 1 < args.size()) {
				dot_filename = args[++argidx];
				continue;
			}
			if (args[argidx] == "-topk") {
				if (argidx + 1 >= args.size())
					log_cmd_error("Option -topk requires a positive integer.\n");
				const char *text = args[++argidx].c_str();
				char *endptr = nullptr;
				errno = 0;
				long value = strtol(text, &endptr, 10);
				if (errno != 0 || endptr == text || *endptr != '\0' || value <= 0 || value > INT_MAX)
					log_cmd_error("Option -topk requires a positive integer.\n");
				filter_options.top_k = int(value);
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);
		log_header(design, "Executing CELLFILTER pass (build data-path graph).\n");

		std::ofstream dot_file;
		if (!dot_filename.empty()) {
			rewrite_filename(dot_filename);
			dot_file.open(dot_filename);
			if (!dot_file)
				log_cmd_error("Can't open DOT file `%s' for writing.\n", dot_filename);
			yosys_output_files.insert(dot_filename);
			dot_file << "digraph cellfilter {\n";
			dot_file << "  rankdir=LR;\n";
			dot_file << "  graph [fontname=\"Helvetica\"];\n";
			dot_file << "  node [fontname=\"Helvetica\"];\n";
			dot_file << "  edge [fontname=\"Helvetica\", fontsize=9];\n";
		}

		std::vector<AnalysisSnapshot> batch;
		SelectionStrategy strategy{filter_options.filtered() ? "region-topk" : "all", 1,
				{{"top_k", std::to_string(filter_options.top_k)}}};
		int module_index = 0;
		auto roots = design->selected_modules();
		StructuralDigestContext digest_context(design, roots);
		for (RTLIL::Module *module : roots) {
			if (module->has_memories())
				log_cmd_error("Module %s still contains RTLIL memories; run memory_collect before cellfilter.\n", module);
			for (RTLIL::Cell *cell : module->cells())
				if (is_uncollected_memory_port(cell))
					log_cmd_error("Module %s contains uncollected memory port cell %s; "
							"run memory_collect before cellfilter.\n", module, cell);

			CellGraph graph(module, build_options);
			graph.build();
			log("Built data-path graph for %s: %d nodes, %d edge cells, %d port connections.\n",
					module, GetSize(graph.nodes), GetSize(graph.edge_cells), GetSize(graph.connections));
			CellGraphSelection selection = graph.select_objects(filter_options);
			batch.push_back(AnalysisSnapshot::capture(graph, selection, strategy, digest_context));
			if (dot_file.is_open()) {
				CellGraphView view = graph.create_view(selection);
				if (filter_options.filtered())
					log("Filtered graph view for %s: %d regions, %d visible edge cells, "
							"%d hidden edge cells, %d view connections.\n", module,
							GetSize(view.regions), view.visible_edge_cell_count,
							view.hidden_edge_cell_count, GetSize(view.connections));
				graph.dump_dot(dot_file, module_index, view);
			}
			module_index++;
		}

		if (dot_file.is_open()) {
			dot_file << "}\n";
			dot_file.flush();
			bool written = bool(dot_file);
			dot_file.close();
			if (!written || !dot_file)
				log_cmd_error("Can't finish writing DOT file `%s'.\n", dot_filename);
			log("Wrote data-path graph to %s.\n", dot_filename);
		}
		SnapshotStore::publish(design, batch);
	}
} CellFilterPass;

PRIVATE_NAMESPACE_END
