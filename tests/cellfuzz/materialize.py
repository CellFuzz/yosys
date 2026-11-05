"""Stage 3A: actual bypass interfaces and publication state boundaries."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import resource
import signal

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
YOSYS = os.environ.get("YOSYS", str(ROOT / "build/yosys"))
CONFIG = os.environ.get("YOSYS_CONFIG", str(Path(YOSYS).with_name("yosys-config")))

with tempfile.TemporaryDirectory(prefix="cellfuzz-materialize-") as tmp:
    work = Path(tmp)
    plugins = []
    for name in ("materialize", "manifest", "type_plan"):
        plugin = work / (name + ".so")
        subprocess.run([CONFIG, "--build", str(plugin), f"-I{ROOT}", str(HERE / (name + ".cc"))], check=True, timeout=120)
        plugins += ["-m", str(plugin)]
    def run(command, error=None, **kwargs):
        result = subprocess.run([YOSYS, "-Q", "-T", "-q", *plugins, "-p", command], cwd=HERE,
                                capture_output=True, text=True, timeout=60, **kwargs)
        if error is None:
            assert result.returncode == 0, result.stderr
        else:
            assert result.returncode != 0 and error in result.stderr, result.stderr
        return result

    prepare = "read_verilog graph.v; hierarchy -top graph; proc; memory_collect; cellfilter; "
    command = f"cellinstrument -manifest {work}/monitor.json"
    verify = f"test_cellfuzz_materialize success {work}/monitor.json "
    run(prepare + f"write_rtlil {work}/original.il; cellinstrument -plan-only -manifest {work}/plan.json; " + verify + command +
        f"; check -assert; write_json {work}/design.json; write_rtlil {work}/instrumented.il; write_verilog {work}/design.v")
    planned = json.loads((work / "plan.json").read_text())
    materialized = json.loads((work / "monitor.json").read_text())
    assert materialized["status"] == "materialized"
    assert materialized["stage"] == "observation-materialization"
    for key in ("graph", "items", "selection", "view", "registers", "transport", "analysis"):
        assert materialized[key] == planned[key]
    module = json.loads((work / "design.json").read_text())["modules"]["graph"]
    assert int(module["attributes"]["cellfuzz_instrumented"], 2) == 1
    for port in materialized["transport"]["ports"]:
        actual = module["ports"][port["name"]]
        assert actual["direction"] == "output" and len(actual["bits"]) == port["width"]
    run(prepare + command + "; " + command, error="already instrumented")
    run(prepare + command + "; cellinstrument -plan-only -manifest " + str(work / "again.json"), error="already instrumented")
    run(prepare + command + "; cellfilter; " + command, error="already instrumented")
    run(prepare + "test_cellfuzz_materialize invalid")

    # Formally compare original functional outputs over sequential steps,
    # removing only the added port flags from the gate interface.
    run(f"read_rtlil {work}/original.il; rename graph gold; read_rtlil {work}/instrumented.il; "
        "rename graph gate; delete -port gate/cf_obs_*; miter -equiv -flatten gold gate miter; "
        "hierarchy -top miter; sat -seq 3 -set-init-zero -prove trigger 0 -verify")
    run(f"read_verilog {work}/design.v; hierarchy -check -top graph; proc; check -assert")

    # Fixtures include all mux selectors and multi-output cells, 143 official
    # FF/latch kinds, aliases/repeated source bits, constants and wide tails.
    setups = ("test_cellfuzz_type_plan fixture; cellfilter; ",
              "test_cellfuzz_type_plan fixture-ff-library; cellfilter; ",
              "test_cellfuzz_manifest fixture; cellfilter; ")
    for setup in setups:
        for width in (1, 8, 32, 33, 64, 65, 256, 1024):
            run(setup + verify + command + f" -chunk-width {width}")
    for setup in ("test_cellfuzz_type_plan fixture; cellfilter; ",
                  "test_cellfuzz_type_plan fixture-ff-library; cellfilter; "):
        # Internal arithmetic cells such as $lcu have official techmap models
        # but are not all inlined by write_verilog. Verify the raw bypass first,
        # then use the normal mapping flow before behavioral Verilog readback.
        mapping = "techmap; techmap -autoproc -map +/simcells.v; opt_clean; " if "fixture;" in setup else ""
        run(setup + verify + command + f"; check -assert; {mapping}write_verilog {work}/types.v")
        run(f"read_verilog {work}/types.v; hierarchy -check; proc; check -assert")
    run("test_cellfuzz_manifest fixture; cellfilter; test_cellfuzz_manifest empty; " + verify + command)
    empty = json.loads((work / "monitor.json").read_text())
    assert empty["transport"] == {"ports": [], "segments": [], "total_bits": 0}
    run("test_cellfuzz_type_plan fixture; cellfilter -nodes none -topk 1; " + verify + command + " -prefix debug_1")
    run("read_verilog stateful.v; hierarchy -top stateful; proc; memory_collect; cellfilter; " +
        verify + command + " -mem-access off")
    run("read_verilog stateful.v; hierarchy -top stateful; proc; memory_collect; cellfilter; " +
        verify + command)
    run("read_verilog stateful.v; hierarchy -top stateful; proc; memory_collect; cellfilter; " +
        verify + command + " -mem-content query")

    # RTLIL's full structural check exits on malformed cell parameters. It
    # must run before an artifact or any output wire exists, since that fatal
    # path cannot unwind a prepared file through error_throw_scope.
    malformed = "read_verilog graph.v; hierarchy -top graph; proc; memory_collect; setparam -set Y_WIDTH 7 t:$not; cellfilter; "
    (work / "monitor.json").write_text("previous artifact\n")
    run(malformed + command, error="ERROR:")
    assert (work / "monitor.json").read_text() == "previous artifact\n"
    assert not list(work.glob("*.tmp.*")), "fatal RTLIL preflight leaked a prepared artifact"

    # Every pre-mutation failure preserves a consumable analysis batch and the
    # exact design. A valid plan can immediately follow in the same process.
    for destination in (str(work / "missing" / "out.json"), "/dev/full", str(work)):
        failed = "cellinstrument -manifest " + destination
        run(prepare + f"test_cellfuzz_materialize preflight {work}/monitor.json " + failed +
            f"; cellinstrument -plan-only -manifest {work}/still-valid.json")
    for kind in ("wire", "cell", "parameter", "available"):
        run(f"test_cellfuzz_manifest fixture; test_cellfuzz_manifest conflict {kind}; cellfilter; " +
            f"test_cellfuzz_materialize preflight {work}/monitor.json " + command)
    for option in ("-prefix module", "-prefix __debug", "-prefix bad-name", "-nodes all"):
        run(prepare + f"test_cellfuzz_materialize preflight {work}/monitor.json " + command + " " + option)

    def limit_output():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (512, 512))

    (work / "monitor.json").write_text("previous artifact\n")
    run(prepare + f"test_cellfuzz_materialize preflight {work}/monitor.json " + command, preexec_fn=limit_output)
    assert (work / "monitor.json").read_text() == "previous artifact\n"
    # Faults use RTLIL Monitor callbacks at the first new connection, with no
    # production hooks: command interruption, actual port corruption and a
    # destination changed to a directory to force a real rename failure.
    for mode in ("interrupt", "corrupt", "rename"):
        result = run(prepare + f"test_cellfuzz_materialize {mode} {work}/monitor.json " + command)
        assert "discard the current Design" in result.stderr
        assert (work / "monitor.json").read_text() == "previous artifact\n"
        assert not list(work.glob("*.tmp.*"))
    assert not list(work.glob("*.tmp.*"))
print("CellFuzz materialization tests passed.")
