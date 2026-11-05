"""Stage 3B: consume real simulation interface values, independently of layout."""
import copy
import json
import os
from pathlib import Path
import random
import re
import subprocess
import tempfile

from value_consumer import decode_items

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
YOSYS = os.environ.get("YOSYS", str(ROOT / "build/yosys"))
CONFIG = os.environ.get("YOSYS_CONFIG", str(Path(YOSYS).with_name("yosys-config")))


def consumer_checks():
    report = {"schema_version": 1, "status": "materialized",
              "items": [{"monitor_id": 0, "width": 5}, {"monitor_id": 1, "width": 2}],
              "transport": {"ports": [{"name": "a", "width": 3}, {"name": "b", "width": 4}],
                            "total_bits": 7,
                            "segments": [{"monitor_id": 1, "item_lsb": 0, "port_name": "b", "port_lsb": 2, "width": 2},
                                         {"monitor_id": 0, "item_lsb": 3, "port_name": "b", "port_lsb": 0, "width": 2},
                                         {"monitor_id": 0, "item_lsb": 0, "port_name": "a", "port_lsb": 0, "width": 3}]}}
    assert decode_items(report, {"a": "x10", "b": "z101"}) == ["01x10", "z1"]
    for field, value in (("width", 0), ("width", 8), ("item_lsb", 4), ("port_lsb", -1),
                         ("monitor_id", 2), ("port_name", "missing")):
        bad = copy.deepcopy(report)
        bad["transport"]["segments"][0][field] = value
        try:
            decode_items(bad, {"a": "x10", "b": "z101"})
        except ValueError:
            pass
        else:
            raise AssertionError(f"accepted invalid {field}")
    for blocks in ({"a": "x10"}, {"a": "x10", "b": "z10"}, {"a": "x10", "b": "z10?"}):
        try:
            decode_items(report, blocks)
        except ValueError:
            pass
        else:
            raise AssertionError("accepted invalid block values")
    empty = {"schema_version": 1, "status": "materialized", "items": [],
             "transport": {"ports": [], "segments": [], "total_bits": 0}}
    assert decode_items(empty, {}) == []


def vcd_samples(filename):
    """Read the flat, public bench signals emitted by Yosys sim (four states).

    Flush after all changes at a timestamp, so comparison sees settled values.
    Keep width explicitly: VCD may abbreviate leading zeros or unknown bits.
    """
    variables, values, samples, timestamp = {}, {}, [], None
    for line in filename.read_text().splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "$var":
            variables[fields[3]] = (fields[4], int(fields[2]))
        elif line.startswith("#"):
            if timestamp is not None:
                samples.append((timestamp, values.copy()))
            timestamp = int(line[1:])
        elif timestamp is not None and line[0] in "b01xz":
            bits, key = fields[0][1:], fields[1] if line[0] == "b" else line[1:]
            if line[0] != "b":
                bits = line[0]
            name, width = variables[key]
            values[name] = bits.rjust(width, bits[0] if bits[0] in "xz" else "0")
    if timestamp is not None:
        samples.append((timestamp, values.copy()))
    assert samples, "no simulation values"
    return samples


def verilog_id(name):
    return name if re.fullmatch(r"[a-zA-Z_][a-zA-Z0-9_$]*", name) else "\\" + name + " "


class Roundtrip:
    def __init__(self, work):
        self.work, self.plugins, self.serial = work, [], 0
        for name in ("value_reference", "materialize", "type_plan"):
            plugin = work / (name + ".so")
            subprocess.run([CONFIG, "--build", str(plugin), f"-I{ROOT}", str(HERE / (name + ".cc"))],
                           check=True, timeout=120)
            self.plugins += ["-m", str(plugin)]

    def run(self, commands):
        self.serial += 1
        script = self.work / f"run{self.serial}.ys"
        script.write_text(commands.replace("; ", ";\n") + "\n")
        result = subprocess.run([YOSYS, "-Q", "-T", "-q", *self.plugins, "-s", str(script)],
                                cwd=HERE, capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, f"{script}:\n{result.stdout[-3000:]}\n{result.stderr}"
        return result.stdout

    def prepare(self, name, setup, top, options="", selection="", mapped=False, gold_verilog=False):
        directory = self.work / name
        directory.mkdir()
        reportfile = directory / "monitor.json"
        self.run(setup + f"; check -assert; write_rtlil {directory}/original.il; write_json {directory}/original.json; "
                 f"cellfilter {selection}; cellinstrument -plan-only -manifest {directory}/planned.json {options}; "
                 f"test_cellfuzz_materialize success {reportfile} cellinstrument -manifest {reportfile} {options}; "
                 f"check -assert; write_rtlil {directory}/actual.il; " +
                 ("techmap; techmap -autoproc -map +/simcells.v; opt_clean; " if mapped else "") +
                 f"write_verilog {directory}/actual.v")
        report = json.loads(reportfile.read_text())
        planned = json.loads((directory / "planned.json").read_text())
        for key in ("analysis_id", "graph", "items", "registers", "memories", "transport", "selection", "view"):
            assert report[key] == planned[key], key
        graph = report["graph"]
        for item in report["items"]:
            cell, port = graph["cells"][item["cell_id"]], graph["ports"][item["port_id"]]
            assert cell["name"] == item["cell"] and cell["type"] == item["type"] and cell["selected"]
            assert port["cell_id"] == cell["id"] and port["name"] == item["port"]
            offset = item["port_lsb"]
            assert port["raw"][offset:offset + item["width"]] == item["source"] and port["observed"]
            if item["role"] != "memory_access":
                assert offset == 0 and port["width"] == item["width"]
        gold = (f"read_rtlil {directory}/original.il; rename {top} gold; "
                f"test_cellfuzz_reference gold {reportfile} gold item_; ")
        # Formal uses the original RTLIL cells, while the value tests consume
        # the exported/reimported Verilog interface, including mapped cells.
        self.run(gold + f"read_rtlil {directory}/actual.il; rename {top} gate; "
                 f"test_cellfuzz_reference gate {reportfile} gate item_; write_rtlil {directory}/formal.il")
        value_gold = gold + ("techmap; techmap -autoproc -map +/simcells.v; opt_clean; " if mapped else "")
        if gold_verilog:
            # sim requires asynchronous memory reads; memory_nordff cannot
            # directly extract a collected read port with both resets. Expose
            # raw source references first, then lower this test-only model
            # through behavioral Verilog. Independent state oracles cover it.
            value_gold += (f"write_verilog {directory}/gold.v; design -reset; "
                           f"read_verilog {directory}/gold.v; proc; ")
        self.run(value_gold + f"read_verilog {directory}/actual.v; rename {top} gate; setattr -mod -unset top; hierarchy -check; proc; "
                 f"check -assert; write_rtlil {directory}/pair.il")
        original = json.loads((directory / "original.json").read_text())["modules"][top]
        print(f"{name}: {len(report['items'])} items, {len(report['registers'])} registers, "
              f"{report['transport']['total_bits']} bits", flush=True)
        return directory, report, original

    def formal(self, case, sequential=False):
        directory, _, _ = case
        self.run(f"read_rtlil {directory}/formal.il; " + ("async2sync; " if sequential else "") +
                 "miter -equiv -flatten gold gate miter; hierarchy -top miter; "
                 "sat " + ("-seq 4 -set-init-zero " if sequential else "") + "-prove trigger 0 -verify")

    def simulate(self, case, vectors, preprocess=""):
        directory, report, original = case
        inputs = {name: len(port["bits"]) for name, port in original["ports"].items() if port["direction"] == "input"}
        outputs = {name: len(port["bits"]) for name, port in original["ports"].items() if port["direction"] == "output"}
        assert all(vector.keys() == inputs.keys() for vector in vectors)
        lines = ["module bench(input driver);", "reg [9:0] tick = 0;", "always @(posedge driver) tick <= tick + 1;"]
        shared = []
        for name, width in inputs.items():
            lines += [f"reg [{width-1}:0] stim_{name};", "always @* case (tick)"]
            for index, vector in enumerate(vectors):
                bits = vector[name] if isinstance(vector[name], str) else format(vector[name], f"0{width}b")
                assert len(bits) == width
                lines += [f"10'd{index}: stim_{name} = {width}'b{bits};"]
            lines += [f"default: stim_{name} = {width}'b{bits};", "endcase"]
            shared += [f".{name}(stim_{name})"]
        gold, gate = list(shared), list(shared)
        for index, (name, width) in enumerate(outputs.items()):
            lines += [f"wire [{width-1}:0] g_func{index}, a_func{index};"]
            gold += [f".{verilog_id(name)}(g_func{index})"]
            gate += [f".{verilog_id(name)}(a_func{index})"]
        for item in report["items"]:
            index, width = item["monitor_id"], item["width"]
            lines += [f"wire [{width-1}:0] g_item{index};"]
            gold += [f".item_{index:04d}(g_item{index})"]
        for index, port in enumerate(report["transport"]["ports"]):
            lines += [f"wire [{port['width']-1}:0] a_block{index};"]
            gate += [f".{port['name']}(a_block{index})"]
        lines += ["gold gold (" + ", ".join(gold) + ");", "gate gate (" + ", ".join(gate) + ");", "endmodule"]
        (directory / "bench.v").write_text("\n".join(lines) + "\n")
        self.run(f"read_rtlil {directory}/pair.il; read_verilog {directory}/bench.v; hierarchy -top bench; "
                 f"proc; flatten; {preprocess} check -assert; sim -clock driver -n {len(vectors)-1} -vcd {directory}/trace.vcd -q")
        samples = vcd_samples(directory / "trace.vcd")
        for timestamp, values in samples:
            blocks = {port["name"]: values[f"a_block{index}"] for index, port in enumerate(report["transport"]["ports"])}
            expected = [values[f"g_item{index}"] for index in range(len(report["items"]))]
            actual = decode_items(report, blocks)
            assert actual == expected, (directory.name, timestamp, next((i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b))
            for index, name in enumerate(outputs):
                assert values[f"g_func{index}"] == values[f"a_func{index}"], (directory.name, timestamp, name)
        assert {int(values["tick"], 2) for _, values in samples} == set(range(len(vectors)))
        print(f"  {len(vectors)} input vectors: every item and functional output matched", flush=True)
        return samples, list(outputs)


def register_vectors():
    vectors = []
    rng = random.Random(0xFF3B)
    # Both uniform extremes plus seeded values exercise every wide bit.
    wide_values = [0, (1 << 513) - 1] + [rng.getrandbits(513) for _ in range(46)]
    for cycle in range(12):
        for phase in range(4):
            # Data/controls change only while the DUT clock is stationary.
            # Phase 2 exercises asynchronous controls without a rising edge.
            step = cycle * 2 + (phase >= 2)
            vectors.append(dict(clk=int(phase in (1, 2)), data=(step * 37 + 11) % 256,
                                # Set async data well before asserting load.
                                async_data=((step // 6) * 19 + 7) % 256,
                                wide=wide_values[len(vectors)],
                                en=int(step % 3 != 1), srst=int(step % 5 in (1, 2)),
                                arst_n=int(step % 7 != 3), arst_p=int(step % 7 == 5),
                                load=int(step % 6 == 3), latch_en=int(step % 4 == 2)))
    return vectors


def check_registers(samples, outputs):
    assert {"0" * 513, "1" * 513} <= {values[f"g_func{outputs.index('w513')}"] for _, values in samples}
    state = dict(q_pos=0xa5, q_neg=0x5a, q_en=0x3c, q_sync=0x96, q_sync_en=0x69,
                 q_async_n=0x12, q_async_p=0x34, q_load=0x56, q_latch=0x78)
    previous_clk = 0
    seen = {name: set() for name in state}
    exercised = set()
    for timestamp, values in samples:
        get = lambda name: int(values["stim_" + name], 2)
        clk, data = get("clk"), get("data")
        if clk and not previous_clk:
            exercised.add("posedge")
            if not get("en") and data != state["q_en"]:
                exercised.add("enable_hold")
            if get("srst") and not get("en"):
                exercised.add("reset_priority")
            state["q_pos"] = data
            if get("en"):
                state["q_en"] = data
                state["q_sync_en"] = 0xd2 if get("srst") else data
            if get("srst"):
                state["q_sync"] = 0xe1
            elif get("en"):
                state["q_sync"] = data
            for name in ("q_async_n", "q_async_p", "q_load"):
                state[name] = data
        if not clk and previous_clk:
            exercised.add("negedge")
            state["q_neg"] = data
        if not get("arst_n"):
            if clk == previous_clk and state["q_async_n"] != 0xc3:
                exercised.add("async_n")
            state["q_async_n"] = 0xc3
        if get("arst_p"):
            if clk == previous_clk and state["q_async_p"] != 0xb4:
                exercised.add("async_p")
            state["q_async_p"] = 0xb4
        if get("load"):
            if clk == previous_clk and state["q_load"] != get("async_data"):
                exercised.add("async_load")
            state["q_load"] = get("async_data")
        if get("latch_en"):
            if clk == previous_clk and state["q_latch"] != data:
                exercised.add("latch_open")
            state["q_latch"] = data
        elif state["q_latch"] != data:
            exercised.add("latch_hold")
        for name, expected in state.items():
            actual = values[f"g_func{outputs.index(name)}"]
            assert actual == format(expected, "08b"), (timestamp, name, actual, expected)
            seen[name].add(actual)
        previous_clk = clk
    assert all(len(values) >= 4 for values in seen.values()), seen
    assert exercised == {"posedge", "negedge", "enable_hold", "reset_priority", "async_n", "async_p",
                         "async_load", "latch_open", "latch_hold"}, exercised


def main():
    consumer_checks()
    rng = random.Random(0xCE11F022)
    subprocess.run(["python3", str(HERE / "fixtures/ibex/extract.py")], check=True)
    with tempfile.TemporaryDirectory(prefix="cellfuzz-values-") as tmp:
        fixture_checks(Path(tmp))
        suite = Roundtrip(Path(tmp))
        branch_setup = "read_verilog fixtures/ibex/ibex_branch_predict.v; hierarchy -top ibex_branch_predict; proc; memory_collect; flatten"
        branch = suite.prepare("branch", branch_setup, "ibex_branch_predict")
        assert len(branch[1]["items"]) == 21 and not branch[1]["registers"]
        suite.formal(branch)
        vectors = []
        instructions = [0x00000013, 0x0000006f, 0x8000006f, 0x00000063, 0x80000063,
                        0x0000a001, 0x0000b001, 0x00002001, 0x00003001,
                        0x0000c001, 0x0000d001, 0x0000e001, 0x0000f001]
        for instruction in instructions:
            for pc in (0, 0x1000, 0xfffffffc):
                for valid in (0, 1):
                    vectors.append(dict(clk_i=0, rst_ni=1, fetch_rdata_i=instruction, fetch_pc_i=pc, fetch_valid_i=valid))
        for _ in range(48):
            vectors.append(dict(clk_i=0, rst_ni=1, fetch_rdata_i=rng.getrandbits(32),
                                fetch_pc_i=rng.getrandbits(32), fetch_valid_i=rng.randrange(2)))
        suite.simulate(branch, vectors)
        controller_setup = "read_verilog fixtures/ibex/ibex_controller.v; hierarchy -top ibex_controller; proc; memory_collect; flatten"
        controller = suite.prepare("controller", controller_setup, "ibex_controller")
        assert len(controller[1]["registers"]) == 10
        suite.formal(controller, sequential=True)
        ports = controller[2]["ports"]
        vectors = []
        for cycle in range(24):
            # Release/reset between edges; keep ordinary inputs stable over
            # both phases, including interrupt/debug/error requests.
            vector = {name: rng.getrandbits(len(port["bits"])) for name, port in ports.items() if port["direction"] == "input"}
            vector.update(rst_ni=int(cycle not in (0, 1, 13)))
            for clk in (0, 1):
                vectors.append(dict(vector, clk_i=clk))
        samples, _ = suite.simulate(controller, vectors)
        q_ids = [item["monitor_id"] for item in controller[1]["items"] if item["role"] == "register_q"]
        assert sum(len({values[f"g_item{i}"] for _, values in samples}) > 1 for i in q_ids) >= 3
        edges_setup = "read_verilog value_edges.v; hierarchy -top value_edges; proc; opt_dff; opt_clean"
        edges = suite.prepare("edges", edges_setup, "value_edges", options="-chunk-width 33 -prefix debug_1")
        assert {1, 8, 32, 33, 64, 65, 256, 513} <= {item["width"] for item in edges[1]["items"]}
        assert len(edges[1]["registers"]) == 9
        samples, outputs = suite.simulate(edges, register_vectors())
        check_registers(samples, outputs)
        types = suite.prepare("types", "test_cellfuzz_type_plan fixture", "types", options="-chunk-width 8", mapped=True)
        vectors = [dict(data=rng.getrandbits(65), clock=i % 2, control=(i // 2) % 2) for i in range(48)]
        samples, _ = suite.simulate(types, vectors)
        selectors = [item for item in types[1]["items"] if item["role"] == "mux_select"]
        assert len(selectors) == 16
        assert any("x" in values[f"g_item{item['monitor_id']}"] and "z" in values[f"g_item{item['monitor_id']}"]
                   for _, values in samples for item in selectors)
        limited = suite.prepare("topk", controller_setup, "ibex_controller", selection="-nodes none -topk 1", options="-chunk-width 64")
        assert 0 < len(limited[1]["items"]) < len(controller[1]["items"])
        suite.simulate(limited, vectors_controller(ports, rng))
        empty = suite.prepare("empty", branch_setup, "ibex_branch_predict", selection="; test_cellfuzz_type_plan empty-selection")
        assert not empty[1]["items"] and empty[1]["transport"]["total_bits"] == 0
        suite.formal(empty)
        suite.simulate(empty, [dict(clk_i=0, rst_ni=1, fetch_rdata_i=0, fetch_pc_i=4, fetch_valid_i=1)])
    print("CellFuzz value roundtrip tests passed.")


def vectors_controller(ports, rng):
    return [dict({name: rng.getrandbits(len(port["bits"])) for name, port in ports.items() if port["direction"] == "input"},
                 clk_i=i % 2, rst_ni=int(i > 1)) for i in range(8)]


def fixture_checks(work):
    fixture = HERE / "fixtures/ibex"
    names = ("ibex_branch_predict.v", "ibex_controller.v")
    bundle = work / "bundle.v"
    content = "\n".join((fixture / name).read_text() for name in names)
    bundle.write_text(content)
    command = ["python3", str(fixture / "extract.py"), str(bundle), "--output", str(work / "extracted")]
    subprocess.run(command, check=True)
    for name in names:
        assert (work / "extracted" / name).read_bytes() == (fixture / name).read_bytes()
    for bad, reason in ((content.replace("module ibex_branch_predict", "module changed", 1), "expected exactly one"),
                        (content + (fixture / names[0]).read_text(), "expected exactly one"),
                        (content.replace("fetch_pc_i + branch_imm", "fetch_pc_i - branch_imm", 1), "source differs")):
        bundle.write_text(bad)
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode != 0 and reason in result.stderr


if __name__ == "__main__":
    main()
