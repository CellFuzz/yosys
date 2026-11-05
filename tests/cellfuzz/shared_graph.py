"""Stage 1A: compare with pre-extraction DOT and call the shared C++ API."""

import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
YOSYS = os.environ.get("YOSYS", str(ROOT / "build/yosys"))
YOSYS_CONFIG = os.environ.get("YOSYS_CONFIG", str(Path(YOSYS).with_name("yosys-config")))


def topology(dot):
    # Compare object labels and port-level edges independently of local DOT IDs.
    objects = dict(re.findall(r'^\s+(m\d+_[cp]\d+) \[.*label="(.*)"\];$', dot, re.M))
    edges = []
    for src, dst, attrs in re.findall(r'^\s+(m\d+_[cp]\d+) -> (m\d+_[cp]\d+) \[(.*)\];$', dot, re.M):
        edges.append([objects[src], objects[dst], attrs])
    return {"objects": sorted(objects.values()), "edges": sorted(edges)}


def run(script, **kwargs):
    subprocess.run([YOSYS, "-Q", "-T", "-q", *kwargs.pop("args", []), "-p", script],
                   check=True, timeout=60, cwd=HERE, **kwargs)


with tempfile.TemporaryDirectory(prefix="cellfuzz-graph-") as tmp:
    work = Path(tmp)
    prepare = "read_verilog graph.v; hierarchy -top graph; proc; memory_collect; "
    expected = json.loads((HERE / "dot_compat.json").read_text())
    for options, baseline in expected.items():
        run(prepare + f"write_rtlil {work}/before.il; cellfilter {options} -dot {work}/graph.dot; "
            f"write_rtlil {work}/after.il; check -assert")
        assert (work / "before.il").read_bytes() == (work / "after.il").read_bytes(), options
        dot = (work / "graph.dot").read_text()
        assert topology(dot) == baseline, options
        run(prepare + f"cellfilter {options} -dot {work}/repeat.dot")
        assert dot == (work / "repeat.dot").read_text(), options
    run(prepare + f"write_rtlil {work}/before.il; cellfilter -topk 2; write_rtlil {work}/after.il")
    assert (work / "before.il").read_bytes() == (work / "after.il").read_bytes()

    # Link only the test consumer: shared services must come from the built
    # cellfilter component, not a second copy of the production implementation.
    plugin = work / "shared_graph.so"
    subprocess.run([YOSYS_CONFIG, "--build", str(plugin), f"-I{ROOT}", str(HERE / "shared_graph.cc")],
                   check=True, timeout=120)
    run(prepare + f"write_rtlil {work}/before.il; test_cellfuzz_graph; write_rtlil {work}/after.il",
        args=["-m", str(plugin)])
    assert (work / "before.il").read_bytes() == (work / "after.il").read_bytes()
    for invalid in ("duplicate", "foreign", "node"):
        result = subprocess.run([YOSYS, "-Q", "-T", "-q", "-m", str(plugin), "-p",
                                 prepare + f"test_cellfuzz_graph {invalid}"],
                                capture_output=True, text=True, timeout=60, cwd=HERE)
        assert result.returncode != 0, invalid
        # Fail for the membership/uniqueness check, not the consumer's fallback.
        assert "Assert" in result.stderr and "selection.cc" in result.stderr, result.stderr
print("CellFuzz shared graph, selection/view, DOT compatibility and RTLIL invariance passed.")
