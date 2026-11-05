"""Session snapshot publication, binding and invalidation regression tests."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
YOSYS = os.environ.get("YOSYS", str(ROOT / "build/yosys"))
CONFIG = os.environ.get("YOSYS_CONFIG", str(Path(YOSYS).with_name("yosys-config")))

with tempfile.TemporaryDirectory(prefix="cellfuzz-snapshot-") as tmp:
    work = Path(tmp)
    plugin = work / "snapshot.so"
    subprocess.run([CONFIG, "--build", str(plugin), f"-I{ROOT}", str(HERE / "snapshot.cc")],
                   check=True, timeout=120)
    result = subprocess.run([YOSYS, "-Q", "-T", "-q", "-m", str(plugin), "-p", "test_cellfuzz_snapshot"],
                            capture_output=True, text=True, timeout=60, cwd=HERE)
    assert result.returncode == 0, result.stderr

    def run(script, success=True):
        result = subprocess.run([YOSYS, "-Q", "-T", "-q", "-m", str(plugin), "-p", script],
                                capture_output=True, text=True, timeout=60, cwd=HERE)
        if success:
            assert result.returncode == 0, result.stderr
        else:
            assert result.returncode != 0 and "rerun cellfilter" in result.stderr, result.stderr
        return result

    prepare = "read_verilog graph.v; proc; memory_collect; "
    for options in ("", "-topk 1", "-topk 2", "-nodes none -topk 1"):
        for dot in ("", f"-dot {work}/graph.dot"):
            run(prepare + f"write_rtlil {work}/before.il; cellfilter {options} {dot}; "
                f"test_cellfuzz_snapshot inspect graph {work}/snapshot.json; "
                f"write_rtlil {work}/after.il")
            assert (work / "before.il").read_bytes() == (work / "after.il").read_bytes()
            snapshot = json.loads((work / "snapshot.json").read_text())
            if not dot:
                without_dot = snapshot
            else:
                assert snapshot == without_dot, options

    inspect = f"test_cellfuzz_snapshot inspect graph {work}/snapshot.json; "
    run(prepare + inspect, success=False)
    run(prepare + "cellfilter -topk 1; stat; ls; select -clear; " + inspect)
    run(prepare + "cellfilter; select -none; cellfilter; " + inspect, success=False)

    # A fresh invocation replaces the entire batch, including previously
    # analyzed modules that are no longer targets.
    multi = prepare + "copy graph second; "
    run(multi + "cellfilter -topk 1; " + inspect +
        f"test_cellfuzz_snapshot inspect second {work}/second.json")
    assert json.loads((work / "second.json").read_text())["module"] == "\\second"
    run(multi + "cellfilter; cellfilter graph; " +
        f"test_cellfuzz_snapshot inspect second {work}/second.json", success=False)
    run(prepare + "cellfilter -topk 1; " + inspect + "cellfilter -topk 2; " +
        f"test_cellfuzz_snapshot inspect graph {work}/replacement.json")
    first = json.loads((work / "snapshot.json").read_text())
    second = json.loads((work / "replacement.json").read_text())
    assert first["analysis_id"] != second["analysis_id"]
    assert first["selection"] != second["selection"]
    assert second["strategy"]["parameters"]["top_k"] == "2"

    # Catch producer errors inside the same Yosys process and assert that the
    # old batch is gone. The multi-module case fails after a valid first module.
    for options in ("-nodes unknown", "-topk 0", f"-dot {work}/missing/graph.dot", "-dot /dev/full"):
        run(prepare + "cellfilter; test_cellfuzz_snapshot failing-filter " + options)
    run(multi + "cellfilter; test_cellfuzz_snapshot poison-last; "
        "test_cellfuzz_snapshot failing-filter -topk 1")

    for change in ("rename graph renamed_graph", "delete t:$not", "setparam -set A_WIDTH 99 t:$not"):
        run(prepare + "cellfilter; " + change + "; " + inspect, success=False)
    run(prepare + "cellfilter; design -reset; " + inspect, success=False)
    run(prepare + "cellfilter; design -push; " + inspect, success=False)
    for switch in ("design -save prepared; design -load prepared", "design -push; design -pop",
                   "design -push-copy; design -pop"):
        run(prepare + "cellfilter -topk 1; " + switch + "; " + inspect)
    # design save/load does not save scratchpad: the latest batch remains the
    # latest batch, provided its structure still matches the loaded modules.
    run(prepare + "cellfilter -topk 1; design -save old; cellfilter -topk 2; design -load old; " + inspect)
    assert json.loads((work / "snapshot.json").read_text())["strategy"]["parameters"]["top_k"] == "2"
    for switch in ("design -save original; delete t:$not; cellfilter; design -load original",
                   "design -push-copy; delete t:$not; cellfilter; design -pop"):
        run(prepare + switch + "; " + inspect, success=False)

    # Exercise restored FF, collected memory and explicit FSM records, not only
    # the combinational graph fixture and its single register.
    stateful = "read_verilog stateful.v; proc; memory_collect; fsm -nomap; "
    for options in ("-topk 2", "-nodes none -topk 1"):
        run(stateful + f"write_rtlil {work}/before.il; cellfilter {options}; "
            f"test_cellfuzz_snapshot inspect stateful {work}/stateful.json; "
            f"write_rtlil {work}/after.il; check -assert")
        assert (work / "before.il").read_bytes() == (work / "after.il").read_bytes()
        cells = json.loads((work / "stateful.json").read_text())["graph"]["cells"]
        assert {1, 2, 3} <= {cell[2] for cell in cells}
print("CellFuzz snapshot tests passed.")
