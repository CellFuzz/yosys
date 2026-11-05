#include "kernel/register.h"
#include "cellfuzz/monitor_plan.h"
#include "cellfuzz/artifact_io.h"
#include "cellfuzz/manifest.h"
#include "cellfuzz/instrument.h"

#include <cerrno>
#include <climits>
#include <cstdlib>

USING_YOSYS_NAMESPACE
using namespace Yosys::CellFuzz;
PRIVATE_NAMESPACE_BEGIN

struct CellInstrumentPass : Pass {
	CellInstrumentPass() : Pass("cellinstrument", "export observations from the saved cellfilter selection") {}

	void help() override
	{
		log("\n");
		log("    cellinstrument -manifest <file> [-plan-only] [options]\n");
		log("\n");
		log("Validate the current whole selected top against its cellfilter snapshot.\n");
		log("Plan all combinational outputs, mux selectors and register Q signals.\n");
		log("Write the complete original graph, selection/view, sources and bit layout\n");
		log("as a JSON manifest. By default, add top-level outputs and continuous bypasses,\n");
		log("invalidate the analysis batch and publish status materialized on success.\n");
		log("With -plan-only, publish status planned and preserve RTLIL and the batch.\n");
		log("After any failure once materialization starts, discard the current Design,\n");
		log("reload the prepared design and rerun cellfilter. No automatic rollback.\n");
		log("\n");
		log("    -manifest <file>\n");
		log("        required manifest destination (published atomically)\n");
		log("    -plan-only\n");
		log("        validate and plan without modifying the design\n");
		log("    -mem-access on|off\n");
		log("        business memory observation (default: on)\n");
		log("    -mem-content off|query\n");
		log("        memory content observation (default: off)\n");
		log("        enabled memory capabilities are not yet implemented\n");
		log("    -chunk-width <bits>\n");
		log("        positive transport width limit (default: 256)\n");
		log("    -prefix <name>\n");
		log("        ASCII identifier for observation interfaces (default: cf)\n");
		log("\n");
		log("Run hierarchy, proc, memory_collect and flatten before cellfilter.\n");
		log("Selection options belong to cellfilter and are not accepted here.\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		MonitorOptions options;
		std::string manifest;
		bool plan_only = false;
		for (size_t index = 1; index < args.size(); index++) {
			const auto &arg = args[index];
			if (arg == "-plan-only") {
				plan_only = true;
				continue;
			}
			if (arg != "-manifest" && arg != "-mem-access" && arg != "-mem-content" && arg != "-chunk-width" && arg != "-prefix")
				log_cmd_error("cellinstrument: unknown argument `%s'; selection belongs to cellfilter.\n", arg);
			if (index + 1 == args.size() || args[index + 1].empty() || args[index + 1][0] == '-')
				log_cmd_error("cellinstrument: option %s requires a value.\n", arg);
			const auto &value = args[++index];
			if (arg == "-manifest") manifest = value;
			if (arg == "-prefix") options.prefix = value;
			if (arg == "-mem-access") {
				if (value != "on" && value != "off")
					log_cmd_error("cellinstrument: -mem-access expects on or off.\n");
				options.mem_access = value == "on";
			}
			if (arg == "-mem-content") {
				if (value != "off" && value != "query")
					log_cmd_error("cellinstrument: -mem-content expects off or query.\n");
				options.mem_query = value == "query";
			}
			if (arg == "-chunk-width") {
				char *end = nullptr;
				errno = 0;
				long width = strtol(value.c_str(), &end, 10);
				if (errno || end == value.c_str() || *end || width <= 0 || width > INT_MAX)
					log_cmd_error("cellinstrument: -chunk-width requires a positive integer.\n");
				options.chunk_width = int(width);
			}
		}
		if (manifest.empty())
			log_cmd_error("cellinstrument: -manifest <file> is required.\n");
		options.validate();
		log_header(design, "Executing CELLINSTRUMENT pass (%s).\n", plan_only ? "observation planning" : "materialization");
		auto top = monitor_top(design);
		auto analysis = SnapshotStore::restore(design, top->name);
		auto plan = create_monitor_plan(analysis, options);
		// Full RTLIL validation can use fatal assertions rather than command
		// exceptions. Run it before a temporary file exists or RTLIL changes.
		// The materializer separately checks every added interface and binding.
		top->check();
		auto layout = create_transport(plan, analysis.graph->module);
		auto report = planned_manifest(analysis, plan, layout);
		// All metadata is now values. Release original graph indices before
		// modifying RTLIL; the checked source SigSpecs retain original wires.
		analysis = RestoredAnalysis{};
		bool started = false;
		try {
			// Command errors must unwind PreparedArtifact even in batch mode.
			auto errors = logger().error_throw_scope();
			if (plan_only) {
				write_artifact(manifest, report.dump());
			} else {
				PreparedArtifact artifact(manifest, materialized_manifest(report).dump());
				started = true;
				SnapshotStore::invalidate(design);
				materialize_observations(top, plan, layout);
				artifact.publish();
				// Publication can fail after RTLIL changes. Only mark successful
				// modules after rename, so those failures never look completed.
				top->set_bool_attribute(RTLIL::escape_id("cellfuzz_instrumented"));
			}
		} catch (const log_cmd_error_exception &) {
			if (started)
				log_cmd_error("cellinstrument: materialization failed; discard the current Design, reload the prepared design and rerun cellfilter.\n");
			log_cmd_error("cellinstrument: artifact preparation or publication failed before design modification.\n");
		}
		log("%s %d observation items in %d output blocks for %s.\n",
				plan_only ? "Planned" : "Materialized", GetSize(plan.items), GetSize(layout.ports), top);
	}
} CellInstrumentPass;

PRIVATE_NAMESPACE_END
