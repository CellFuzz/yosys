#include "artifact_io.h"
#include "kernel/io.h"

#include <cstdio>
#include <fstream>
#include <sys/stat.h>

YOSYS_NAMESPACE_BEGIN
namespace CellFuzz {

PreparedArtifact::PreparedArtifact(std::string target, const std::string &contents) : filename(std::move(target))
{
	rewrite_filename(filename);
	struct stat info;
	if (stat(filename.c_str(), &info) == 0 && !S_ISREG(info.st_mode))
		log_cmd_error("cellinstrument: artifact destination `%s' is not a regular file.\n", filename);
	temporary = make_temp_file(filename + ".tmp.XXXXXX");
	std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
	output << contents << '\n';
	output.flush();
	bool written = bool(output);
	output.close();
	if (!written || !output) {
		std::remove(temporary.c_str());
		log_cmd_error("cellinstrument: can't publish artifact `%s'.\n", filename);
	}
}

PreparedArtifact::~PreparedArtifact()
{
	if (!temporary.empty()) std::remove(temporary.c_str());
}

void PreparedArtifact::publish()
{
	if (temporary.empty() || std::rename(temporary.c_str(), filename.c_str()) != 0)
		log_cmd_error("cellinstrument: can't publish artifact `%s'.\n", filename);
	temporary.clear();
	yosys_output_files.insert(filename);
}

void write_artifact(std::string filename, const std::string &contents)
{
	PreparedArtifact artifact(std::move(filename), contents);
	artifact.publish();
}

} // namespace CellFuzz
YOSYS_NAMESPACE_END
