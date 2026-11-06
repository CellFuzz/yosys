#ifndef YOSYS_CELLFUZZ_ANALYSIS_SNAPSHOT_H
#define YOSYS_CELLFUZZ_ANALYSIS_SNAPSHOT_H

#include "selection.h"
#include "libs/json11/json11.hpp"

#include <map>
#include <memory>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {

// A versioned, deterministic digest of the module and reachable module
// definitions. It excludes scratchpad/selection state and never sorts RTLIL
// in place. SigSpec bit order, repeated bits and all constant states matter.
std::string structural_digest(RTLIL::Module *module);

// Command-local, read-only computation for a fixed Design and root set. Never
// retain this across mutation, design switching, or snapshot restoration.
class StructuralDigestContext {
	RTLIL::Design *design;
	std::map<std::string, RTLIL::Module *> roots;
	std::map<std::string, std::string> digests;
	size_t local_records = 0;

public:
	StructuralDigestContext(RTLIL::Design *design, const std::vector<RTLIL::Module *> &roots);
	std::string digest(RTLIL::Module *module) const;
	// Internal accounting for tests; incremented at the serialization site.
	size_t local_record_count() const { return local_records; }
};

struct SelectionStrategy {
	std::string id = "all";
	int version = 1;
	std::map<std::string, std::string> parameters;
};

struct RestoredAnalysis;

// Owns values only: no Cell*, Wire*, SigSpec, SigMap or selector callback.
// The private graph record retains original IDs, port bindings and connections
// so restoration cannot accidentally rescore or depend on dictionary order.
struct AnalysisSnapshot {
	static constexpr int version = 2;
	std::string module_name;
	std::string structure_digest;
	std::string analysis_id;
	CellGraphBuildOptions build_options;
	SelectionStrategy strategy;
	CellGraphSelection selection;

	static AnalysisSnapshot capture(const CellGraph &graph, const CellGraphSelection &selection,
			const SelectionStrategy &strategy);
	static AnalysisSnapshot capture(const CellGraph &graph, const CellGraphSelection &selection,
			const SelectionStrategy &strategy, const StructuralDigestContext &context);
	std::string encode() const;
	static AnalysisSnapshot decode(const std::string &encoded);
	// Missing, stale or malformed records are command errors asking the caller
	// to rerun cellfilter. No selection algorithm is invoked here.
	RestoredAnalysis restore(RTLIL::Design *design) const;

private:
	json11::Json graph_record;
	json11::Json to_json(bool include_id = true) const;
	static AnalysisSnapshot from_json(const json11::Json &record);
	friend struct SnapshotStore;
};

// These live bindings belong to this call only. Drop them before modifying or
// switching the design; the AnalysisSnapshot itself is safe to retain by value.
struct RestoredAnalysis {
	std::unique_ptr<CellGraph> graph;
	CellGraphSelection selection;
	CellGraphView view;
	std::string analysis_id;
	SelectionStrategy strategy;
};

struct SnapshotStore {
	static constexpr const char *key = "cellfuzz.analysis";
	static void invalidate(RTLIL::Design *design);
	// One scratchpad write publishes the complete batch; no partial per-module
	// publication. Call invalidate() at the start of the producing command.
	static void publish(RTLIL::Design *design, const std::vector<AnalysisSnapshot> &batch);
	// Decode values for inspection; load() alone does not establish freshness.
	// Consumers needing usable live bindings must use restore() below, or call
	// the loaded snapshot's restore() immediately before consuming it.
	static AnalysisSnapshot load(RTLIL::Design *design, RTLIL::IdString module_name);
	static RestoredAnalysis restore(RTLIL::Design *design, RTLIL::IdString module_name);
};

} // namespace CellFuzz
YOSYS_NAMESPACE_END

#endif
