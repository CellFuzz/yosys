#ifndef YOSYS_CELLFUZZ_ARTIFACT_IO_H
#define YOSYS_CELLFUZZ_ARTIFACT_IO_H

#include "kernel/yosys.h"

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {

// Fully write/flush/close a sibling temporary file before any design mutation.
// publish() alone replaces the destination; destruction cleans up unpublished
// files. Callers must use an error_throw_scope so command errors unwind it.
class PreparedArtifact {
	std::string filename, temporary;
public:
	PreparedArtifact(std::string filename, const std::string &contents);
	PreparedArtifact(const PreparedArtifact &) = delete;
	PreparedArtifact &operator=(const PreparedArtifact &) = delete;
	~PreparedArtifact();
	void publish();
};

// Convenience for read-only planning; the same checked publication protocol.
void write_artifact(std::string filename, const std::string &contents);

} // namespace CellFuzz
YOSYS_NAMESPACE_END

#endif
