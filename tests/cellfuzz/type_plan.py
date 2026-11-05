"""Stage 2A: exact source roles, preflight diagnostics and read-only planning."""
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
YOSYS = os.environ.get("YOSYS", str(ROOT / "build/yosys"))
CONFIG = os.environ.get("YOSYS_CONFIG", str(Path(YOSYS).with_name("yosys-config")))

with tempfile.TemporaryDirectory(prefix="cellfuzz-types-") as tmp:
    work = Path(tmp)
    plugin = work / "type_plan.so"
    subprocess.run([CONFIG, "--build", str(plugin), f"-I{ROOT}", str(HERE / "type_plan.cc")],
                   check=True, timeout=120)

    def run(script, error=None, **kwargs):
        result = subprocess.run([YOSYS, "-Q", "-T", "-q", "-m", str(plugin), "-p", script],
                                capture_output=True, text=True, timeout=60, cwd=HERE, **kwargs)
        if error is None:
            assert result.returncode == 0, result.stderr
        else:
            assert result.returncode != 0 and error in result.stderr, result.stderr
        return result

    plan_command = f"cellinstrument -plan-only -manifest {work}/plan.json"
    prepare = "read_verilog graph.v; hierarchy -top graph; proc; memory_collect; "
    fixture = "test_cellfuzz_type_plan fixture; "
    for setup in (prepare, fixture):
        run(setup + "cellfilter; " + f"write_rtlil {work}/before.il; test_cellfuzz_type_plan; " +
            "test_cellfuzz_type_plan unchanged " + plan_command + f"; write_rtlil {work}/after.il; " +
            f"cellinstrument -plan-only -manifest {work}/again.json; test_cellfuzz_type_plan; check -assert")
        assert (work / "before.il").read_bytes() == (work / "after.il").read_bytes()
        assert (work / "plan.json").read_bytes() == (work / "again.json").read_bytes()
    plan = json.loads((work / "plan.json").read_text())
    assert plan["status"] == "planned" and plan["stage"] == "observation-planning"
    assert plan["config"] == {"mem_access": True, "mem_content": "off", "chunk_width": 256, "prefix": "cf"}
    items = plan["items"]
    assert len(items) == 43
    assert len({(item["cell_id"], item["port_id"], item["role"]) for item in items}) == len(items)
    assert all(item["width"] == len(item["source"]) for item in items)
    pmux = next(item for item in items if item["cell"] == "\\pmux" and item["role"] == "mux_select")
    assert pmux["source"] == [["\\data", 3], ["\\alias", 1], 2, ["\\alias", 1], 3, ["\\data", 0]]
    assert next(item for item in items if item["cell"] == "\\wide")["width"] == 65
    registers = {info["cell"]: info for info in plan["registers"]}
    assert registers["\\adff"]["init"]["bits"] == "10x00101"
    assert not registers["\\sdffe"]["ce_over_srst"] and registers["\\sdffce"]["ce_over_srst"]
    run("test_cellfuzz_type_plan fixture-ff-library; cellfilter; test_cellfuzz_type_plan; " + plan_command)
    ff_plan = json.loads((work / "plan.json").read_text())
    assert len(ff_plan["registers"]) > 100
    ff_count = len(ff_plan["registers"])
    # The Verilog backend must behaviorally expand these FFs. Read the export
    # back as Verilog so unresolved internal FF instantiations cannot pass.
    run("test_cellfuzz_type_plan fixture-ff-library; cellfilter; " + plan_command +
        f"; write_verilog {work}/ff_library.v; design -reset; read_verilog {work}/ff_library.v; hierarchy -check; proc; check -assert")

    # Selection is consumed as saved, including an entirely empty cell set.
    run(fixture + "cellfilter -nodes none; " + plan_command)
    assert not json.loads((work / "plan.json").read_text())["registers"]
    run(prepare + "cellfilter -topk 1; " + plan_command)
    selected = json.loads((work / "plan.json").read_text())
    # graph.v has three combinational regions, each with its own K=1 quota.
    assert len({item["cell_id"] for item in selected["items"] if item["role"] != "register_q"}) == 3
    run(fixture + "cellfilter; test_cellfuzz_type_plan empty-selection; " + plan_command)
    empty = json.loads((work / "plan.json").read_text())
    assert not empty["items"] and not empty["registers"]
    assert all(cell["reason"] in {"not_selected", "not_in_graph"} for cell in empty["unobserved_cells"])

    # Memory defaults stay on/off. Only selected memories require the staged
    # capabilities; disabled memories never leak into ordinary output items.
    memory = "read_verilog stateful.v; hierarchy -top stateful; proc; memory_collect; "
    for options in ("", "-mem-access on -mem-content off"):
        run(memory + "cellfilter; " + plan_command + " " + options)
        assert any(item["role"] == "memory_access" for item in json.loads((work / "plan.json").read_text())["items"])
    for options in ("-mem-access off -mem-content query", "-mem-access on -mem-content query"):
        run(memory + "cellfilter; " + plan_command + " " + options, error="memory content query not yet implemented")
    run(memory + "cellfilter; " + plan_command + " -mem-access off")
    mem_plan = json.loads((work / "plan.json").read_text())
    assert any(cell["reason"] == "memory_observation_disabled" for cell in mem_plan["unobserved_cells"])
    assert not any(item["type"].startswith("$mem") for item in mem_plan["items"])
    run(memory + "cellfilter -nodes register; " + plan_command)
    write_only = work / "write_only.v"
    write_only.write_text("module write_only(input clk, input [1:0] addr, input [7:0] data);\n"
                          "reg [7:0] mem [0:3]; always @(posedge clk) mem[addr] <= data; endmodule\n")
    run(f"read_verilog {write_only}; hierarchy -top write_only; proc; memory_collect; cellfilter; " +
        plan_command + " -mem-access off")
    run("read_verilog stateful.v; hierarchy -top stateful; proc; " + plan_command, error="uncollected memory")
    run(memory + "fsm -nomap; cellfilter; " + plan_command, error="explicit FSM")

    hierarchy = work / "hierarchy.v"
    hierarchy.write_text("module child(input a, output y); assign y = ~a; endmodule\n"
                         "module parent(input a, output y); child u(a,y); endmodule\n")
    run(f"read_verilog {hierarchy}; hierarchy -top parent; cellfilter; select parent; " + plan_command,
        error="module instance; flatten")
    run(f"read_verilog {hierarchy}; hierarchy -top parent; proc; memory_collect; flatten; cellfilter; " + plan_command)
    run(f"read_verilog {hierarchy}; cellfilter; " + plan_command, error="exactly one top")
    run(prepare + "copy graph second; setattr -mod -set top 1 second; cellfilter; " + plan_command, error="exactly one top")
    run(prepare + "copy graph second; setattr -mod -unset top second; cellfilter; " + plan_command,
        error="whole-module selection")
    for kind in ("blackbox", "whitebox"):
        run(prepare + f"setattr -mod -set {kind} 1; " + plan_command, error="not a box")

    for options in ("-mem-access off -mem-content off -chunk-width 33 -prefix debug_1",
                    "-mem-content query -chunk-width 1 -prefix cf"):
        run(prepare + "cellfilter; " + plan_command + " " + options)
    assert json.loads((work / "plan.json").read_text())["config"]["mem_content"] == "query"

    for arguments, diagnostic in (
            ("-plan-only", "-manifest <file> is required"),
            ("-plan-only -manifest", "requires a value"),
            ("-plan-only -manifest -plan-only", "requires a value")):
        run(prepare + "cellfilter; cellinstrument " + arguments, error=diagnostic)
    for argument in ("-topk 1", "-nodes all", "-edge-topk 1", "-objects all", "graph",
                     "-mem-access yes", "-mem-content on", "-chunk-width 0", "-chunk-width -1",
                     "-chunk-width 2147483648", "-chunk-width 1x", "-prefix 1bad", "-prefix bad-name"):
        run(prepare + "cellfilter; " + plan_command + " " + argument, error="cellinstrument:")

    run(prepare + plan_command, error="rerun cellfilter")
    run(prepare + "cellfilter; setparam -set WIDTH 7 t:$dff; " + plan_command, error="is stale")
    run(prepare + "cellfilter; select graph/q; " + plan_command, error="whole-module selection")
    run(prepare + "cellfilter; select -none; " + plan_command, error="whole-module selection")
    run("cellinstrument -plan-only -manifest " + str(work / "plan.json"), error="exactly one top")
    run(prepare + "setattr -mod -set cellfuzz_instrumented 1; cellfilter; " + plan_command,
        error="already instrumented")
    run("read_verilog graph.v; hierarchy -top graph; cellfilter; " + plan_command, error="contains processes")

    for poison, diagnostic in (("unknown", "unknown semantics"), ("direction", "unknown or bidirectional"),
                               ("missing-output", "missing a nonempty output"),
                               ("global-clock", "implicit global clock"),
                               ("fine-global-clock", "implicit global clock"),
                               ("register-width", "invalid register control width"),
                               ("unsupported", "unsupported state")):
        run(fixture + f"test_cellfuzz_type_plan poison {poison}; cellfilter; " + plan_command, error=diagnostic)

    # Failures preserve a valid batch and a pre-existing artifact in the same
    # process; a second plan must still be usable after the failed command.
    for command in (plan_command + " -nodes all", f"cellinstrument -plan-only -manifest {work}/missing/out.json",
                    "cellinstrument -plan-only -manifest /dev/full"):
        run(prepare + f"cellfilter; test_cellfuzz_type_plan failure {command}; " + plan_command)

    def limit_output():
        # Exercise flush/close failure on a real sibling temporary file. An
        # ignored SIGXFSZ makes the stream report EFBIG instead of killing Yosys.
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (512, 512))

    (work / "plan.json").write_text("previous artifact\n")
    run(prepare + "cellfilter; test_cellfuzz_type_plan failure " + plan_command + "; test_cellfuzz_type_plan",
        preexec_fn=limit_output)
    assert (work / "plan.json").read_text() == "previous artifact\n"
    (work / "plan.json").write_text("previous artifact\n")
    run(fixture + "test_cellfuzz_type_plan poison unsupported; cellfilter; " + plan_command, error="unsupported state")
    assert (work / "plan.json").read_text() == "previous artifact\n"
    assert not list(work.glob("*.tmp.*"))
print(f"CellFuzz type planning tests passed ({ff_count} official FF/latch types).")
