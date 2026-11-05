"""Read planned graph/transport independently of the production serializers."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import random
import copy


def check_report(report, oracle):
    assert report["schema_version"] == 1 and report["status"] == "planned"
    assert report["stage"] == "observation-planning"
    assert report["analysis"]["analysis_id"] == oracle["analysis_id"]
    assert report["analysis"]["structure_digest"] == oracle["structure_digest"]
    assert report["analysis"]["strategy"] == oracle["strategy"]
    graph = report["graph"]
    cells = graph["cells"]
    ports = graph["ports"]
    assert [cell["name"] for cell in cells] == sorted(cell["name"] for cell in cells)
    assert [cell["id"] for cell in cells] == list(range(len(cells)))
    assert [port["id"] for port in ports] == list(range(len(ports)))
    port_keys = [(port["cell_id"] is not None, cells[port["cell_id"]]["name"] if port["cell_id"] is not None else "",
                  port["name"]) for port in ports]
    assert port_keys == sorted(port_keys)
    assert [wire["id"] for wire in graph["wires"]] == list(range(len(graph["wires"])))
    assert [wire["name"] for wire in graph["wires"]] == sorted(wire["name"] for wire in graph["wires"])
    assert [conn["id"] for conn in graph["connections"]] == list(range(len(graph["connections"])))
    assert [(conn["src"], conn["dst"]) for conn in graph["connections"]] == sorted((conn["src"], conn["dst"]) for conn in graph["connections"])
    assert sorted(cell["analysis_cell_id"] for cell in cells) == list(range(len(cells)))
    assert sorted(port["analysis_port_id"] for port in ports) == list(range(len(ports)))
    cell_ids = {cell["analysis_cell_id"]: cell["id"] for cell in cells}
    port_ids = {port["analysis_port_id"]: port["id"] for port in ports}
    roles = {"excluded": 0, "node": 1, "edge": 2}
    kinds = {"combinational": 0, "metadata": 0, "register": 1, "memory": 2, "fsm": 3}
    reasons = {"state_node": 0, "data_path": 1, "excluded_type": 2, "not_on_data_path": 3}
    directions = {"unknown": 0, "input": 1, "output": 2, "inout": 3}
    # Reconstruct the complete original records by analysis ID. This tests
    # one ID namespace through every exported relationship, not only counts.
    original_cells = [None] * len(cells)
    for cell in cells:
        original_cells[cell["analysis_cell_id"]] = [cell["name"], cell["type"], kinds[cell["semantic_type"]],
                                                   roles[cell["role"]], cell["in_graph"], reasons[cell["retention_reason"]]]
        assert cell["selected"] == (cell["analysis_cell_id"] in oracle["selection"]["cells"])
    assert original_cells == oracle["graph"]["cells"]
    original_ports = [None] * len(ports)
    for port in ports:
        cell = -1 if port["cell_id"] is None else cells[port["cell_id"]]["analysis_cell_id"]
        original_ports[port["analysis_port_id"]] = [cell, port["name"], directions[port["direction"]], port["raw"], port["canonical"]]
        assert port["width"] == len(port["raw"]) == len(port["canonical"])
        if cell >= 0:
            assert port["id"] in cells[port["cell_id"]]["inputs" if port["direction"] == "input" else "outputs"]
    assert original_ports == oracle["graph"]["ports"]
    original_connections = [None] * len(graph["connections"])
    for conn in graph["connections"]:
        original_connections[conn["analysis_connection_id"]] = [ports[conn["src"]]["analysis_port_id"],
                                                                 ports[conn["dst"]]["analysis_port_id"], conn["bit_count"]]
    assert original_connections == oracle["graph"]["connections"]
    assert report["selection"]["cells"] == sorted(cell_ids[i] for i in oracle["selection"]["cells"])
    assert report["selection"]["module_ports"] == sorted(port_ids[i] for i in oracle["selection"]["module_ports"])
    assert [region["id"] for region in report["selection"]["regions"]] == list(range(len(report["selection"]["regions"])))
    for region in report["selection"]["regions"]:
        original = oracle["selection"]["regions"][region["analysis_region_id"]]
        assert region["edge_cells"] == sorted(cell_ids[i] for i in original[1])
        assert region["key_cells"] == sorted(cell_ids[i] for i in original[2])
        for key, boundary in (("upstream_boundaries", original[3]), ("downstream_boundaries", original[4])):
            expected = {(kind, (cell_ids if kind == 0 else port_ids)[index]) for kind, index in boundary}
            assert {(0 if obj["kind"] == "cell" else 1, obj["id"]) for obj in region[key]} == expected
    assert report["view"]["cells"] == report["selection"]["cells"]
    assert report["view"]["module_ports"] == report["selection"]["module_ports"]
    assert report["view"]["regions"] == report["selection"]["regions"]
    wires = {wire["name"]: wire for wire in graph["wires"]}
    for port in ports:
        for bit in port["raw"] + port["canonical"]:
            if isinstance(bit, list):
                assert bit[0] in wires and 0 <= bit[1] < wires[bit[0]]["width"]
            else:
                assert bit in range(6)
    for owner in [graph["module"], *cells, *graph["wires"]]:
        for key in ("parameters", "attributes"):
            for value in owner.get(key, {}).values():
                assert value["width"] == len(value["bits"]) and isinstance(value["flags"], int)
    def values(records):
        alphabet = "01xz-m"
        return {name: [[alphabet.index(bit) for bit in reversed(value["bits"])], value["flags"]]
                for name, value in records.items()}
    assert [values(graph["module"][key]) for key in ("parameters", "attributes")] == oracle["metadata"]["module"]
    assert {cell["name"]: [values(cell[key]) for key in ("parameters", "attributes")] for cell in cells} == oracle["metadata"]["cells"]
    assert {wire["name"]: values(wire["attributes"]) for wire in graph["wires"]} == oracle["metadata"]["wires"]

    # Independently enumerate acyclic paths in these fixtures to verify the
    # exported view. Hidden EDGE cells may be crossed; NODE/EXCLUDED may not.
    outgoing = {port["id"]: [] for port in ports}
    for conn in graph["connections"]:
        outgoing[conn["src"]].append(conn)
    visible = {port["id"] for port in ports if (port["cell_id"] in report["view"]["cells"]
               if port["cell_id"] is not None else port["id"] in report["view"]["module_ports"])}
    expected_view = {}
    for source in visible:
        port = ports[source]
        if port["direction"] != ("output" if port["cell_id"] is not None else "input"):
            continue
        def walk(current, hidden):
            for edge in outgoing[current]:
                target = edge["dst"]
                if target in visible:
                    entry = expected_view.setdefault((source, target), {"hidden": set(), "width": 0})
                    entry["hidden"].update(hidden)
                    if not hidden:
                        entry["width"] += edge["bit_count"]
                else:
                    owner = ports[target]["cell_id"]
                    if owner is not None and cells[owner]["role"] == "edge" and owner not in hidden:
                        for output in cells[owner]["outputs"]:
                            walk(output, hidden | {owner})
        walk(source, set())
    actual_view = {(conn["src"], conn["dst"]): conn for conn in report["view"]["connections"]}
    assert actual_view.keys() == expected_view.keys()
    for key, value in expected_view.items():
        assert set(actual_view[key]["hidden_cells"]) == value["hidden"]
        assert actual_view[key]["collapsed"] == bool(value["hidden"])
        assert actual_view[key]["bit_count"] == (None if value["hidden"] else value["width"])

    items = report["items"]
    assert [item["monitor_id"] for item in items] == list(range(len(items)))
    assert [(item["cell"], item["port"]) for item in items] == sorted((item["cell"], item["port"]) for item in items)
    assert all(item["source"] == ports[item["port_id"]]["raw"] for item in items)
    assert all(item["cell_id"] == ports[item["port_id"]]["cell_id"] for item in items)
    assert {cell["id"] for cell in cells if cell["observed"]} == {item["cell_id"] for item in items}
    assert {port["id"] for port in ports if port["observed"]} == {item["port_id"] for item in items}
    for info in report["registers"] + report["unobserved_cells"] + report["memories"]:
        assert info.get("cell", info.get("name")) == cells[info["cell_id"]]["name"]
    for port in ports:
        if port["cell_id"] is None:
            assert not port["observed"]
            assert port["observation_reason"] in {"original_interface_reused", "not_selected"}
    transport = report["transport"]
    blocks = {port["name"]: [None] * port["width"] for port in transport["ports"]}
    item_coverage = [[0] * item["width"] for item in items]
    block_coverage = {name: [0] * len(bits) for name, bits in blocks.items()}
    for segment in transport["segments"]:
        item = items[segment["monitor_id"]]
        assert 0 <= segment["item_lsb"] < item["width"] and segment["width"] > 0
        assert segment["item_lsb"] + segment["width"] <= item["width"]
        assert 0 <= segment["port_lsb"] and segment["port_lsb"] + segment["width"] <= len(blocks[segment["port_name"]])
        for bit in range(segment["width"]):
            item_coverage[segment["monitor_id"]][segment["item_lsb"] + bit] += 1
            block_coverage[segment["port_name"]][segment["port_lsb"] + bit] += 1
    assert all(all(count == 1 for count in row) for row in item_coverage + list(block_coverage.values()))
    assert sum(item["width"] for item in items) == transport["total_bits"] == sum(len(block) for block in blocks.values())
    if transport["ports"]:
        assert all(port["width"] == report["config"]["chunk_width"] for port in transport["ports"][:-1])
        assert 0 < transport["ports"][-1]["width"] <= report["config"]["chunk_width"]
    # A symbolic pack/unpack proves bit identity, including constants and
    # repeated references. Random values check the same contract as data.
    rng = random.Random(2026)
    for symbolic in (True, False):
        expected = [[tuple(bit) if isinstance(bit, list) else bit for bit in item["source"]] if symbolic
                    else [rng.randrange(4) for _ in item["source"]] for item in items]
        for segment in transport["segments"]:
            start, width = segment["item_lsb"], segment["width"]
            offset = segment["port_lsb"]
            blocks[segment["port_name"]][offset:offset + width] = expected[segment["monitor_id"]][start:start + width]
        decoded = [[None] * item["width"] for item in items]
        for segment in reversed(transport["segments"]):
            start, width = segment["item_lsb"], segment["width"]
            offset = segment["port_lsb"]
            decoded[segment["monitor_id"]][start:start + width] = blocks[segment["port_name"]][offset:offset + width]
        assert decoded == expected
    return report


def export_identity(report):
    """Compare the export contract while permitting original snapshot IDs."""
    result = copy.deepcopy(report)
    for key in ("analysis_id",):
        result["analysis"].pop(key)
        result.pop(key, None)
    for collection in (result["graph"]["cells"], result["graph"]["ports"], result["graph"]["connections"],
                       result["selection"]["regions"], result["view"]["regions"]):
        for record in collection:
            for key in list(record):
                if key.startswith("analysis_"):
                    del record[key]
    return result

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
YOSYS = os.environ.get("YOSYS", str(ROOT / "build/yosys"))
CONFIG = os.environ.get("YOSYS_CONFIG", str(Path(YOSYS).with_name("yosys-config")))

with tempfile.TemporaryDirectory(prefix="cellfuzz-manifest-") as tmp:
    work = Path(tmp)
    plugin = work / "manifest.so"
    subprocess.run([CONFIG, "--build", str(plugin), f"-I{ROOT}", str(HERE / "manifest.cc")], check=True, timeout=120)

    def run(command, error=None):
        result = subprocess.run([YOSYS, "-Q", "-T", "-q", "-m", str(plugin), "-p", command],
                                capture_output=True, text=True, cwd=HERE, timeout=60)
        if error is None:
            assert result.returncode == 0, result.stderr
        else:
            assert result.returncode != 0 and error in result.stderr, result.stderr

    def planned(setup, width=256, options=""):
        command = (setup + f"write_rtlil {work}/before.il; test_cellfuzz_manifest oracle {work}/oracle.json; "
                   f"test_cellfuzz_manifest unchanged cellinstrument -plan-only -manifest {work}/manifest.json "
                   f"-chunk-width {width} {options}; write_rtlil {work}/after.il; "
                   f"cellinstrument -plan-only -manifest {work}/again.json -chunk-width {width} {options}")
        run(command)
        assert (work / "before.il").read_bytes() == (work / "after.il").read_bytes()
        assert (work / "manifest.json").read_bytes() == (work / "again.json").read_bytes()
        return check_report(json.loads((work / "manifest.json").read_text()), json.loads((work / "oracle.json").read_text()))

    prepare = "read_verilog graph.v; hierarchy -top graph; proc; memory_collect; "
    for options in ("", "-topk 1", "-nodes none -topk 1"):
        report = planned(prepare + "cellfilter " + options + "; ")
    run(prepare + "cellfilter; test_cellfuzz_manifest")
    for width in (1, 8, 32, 33, 64, 65, 256, 1024):
        report = planned("test_cellfuzz_manifest fixture; cellfilter; ", width)
        assert {1, 8, 32, 33, 64, 65, 256, 513} <= {item["width"] for item in report["items"]}
        assert report["graph"]["module"]["parameters"]["\\WIDE"]["bits"] == "1" * 80 + "xz"
        data = next(wire for wire in report["graph"]["wires"] if wire["name"].startswith("\\data."))
        assert data["start_offset"] == 7 and data["upto"] and data["signed"]
        dead = next(cell for cell in report["graph"]["cells"] if cell["name"] == "\\dead")
        assert not dead["in_graph"] and not dead["selected"] and not dead["observed"]
    run("test_cellfuzz_manifest fixture; cellfilter; test_cellfuzz_manifest")
    forward = planned("test_cellfuzz_manifest fixture; cellfilter; ")
    backward = planned("test_cellfuzz_manifest fixture-reverse; cellfilter; ")
    assert export_identity(forward) == export_identity(backward)
    aliases = [item for item in forward["items"] if item["cell"] in {"\\aliased_cell", "\\logic1"}]
    assert len(aliases) == 2 and aliases[0]["monitor_id"] != aliases[1]["monitor_id"]
    ports = forward["graph"]["ports"]
    assert ports[aliases[0]["port_id"]]["canonical"] == ports[aliases[1]["port_id"]]["canonical"]
    mux_select = next(item for item in forward["items"] if item["role"] == "mux_select")
    assert mux_select["source"][1] == mux_select["source"][3]
    assert mux_select["source"][2::2] == [2, 3]  # RTLIL X, Z
    assert "\\src" not in next(cell for cell in forward["graph"]["cells"] if cell["name"] == "\\logic1")["attributes"]
    assert next(cell for cell in forward["graph"]["cells"] if cell["name"] == "\\logic33")["attributes"]["\\src"]["width"] > 0
    report = planned("test_cellfuzz_manifest fixture; cellfilter; test_cellfuzz_manifest empty; ")
    assert report["transport"] == {"ports": [], "segments": [], "total_bits": 0}
    report = planned("test_cellfuzz_manifest fixture; cellfilter -nodes none; ")
    assert any(cell["retention_reason"] == "excluded_type" for cell in report["graph"]["cells"])
    memory = planned("read_verilog stateful.v; hierarchy -top stateful; proc; memory_collect; cellfilter; ",
                     options="-mem-access off")
    assert memory["memories"] and not memory["memories"][0]["access_observed"]
    assert memory["memories"][0]["observation_reason"] == "memory_observation_disabled"

    command = f"cellinstrument -plan-only -manifest {work}/manifest.json"
    for kind in ("wire", "cell", "parameter", "available"):
        setup = f"test_cellfuzz_manifest fixture; test_cellfuzz_manifest conflict {kind}; cellfilter; "
        (work / "manifest.json").write_text("previous artifact\n")
        run(setup + command, error="collides with an existing object")
        run(setup + "test_cellfuzz_manifest failure " + command)
        assert (work / "manifest.json").read_text() == "previous artifact\n"
    for prefix, diagnostic in (("module", "illegal or reserved"), ("always_ff", "illegal or reserved"),
                               ("__debug", "Verilator reserved"), ("bad$name", "ASCII identifier")):
        run("test_cellfuzz_manifest fixture; cellfilter; " + command + " -prefix " + prefix, error=diagnostic)
        assert (work / "manifest.json").read_text() == "previous artifact\n"
    report = planned("test_cellfuzz_manifest fixture; cellfilter; ", 33, "-prefix debug_1")
    assert all(port["name"].startswith("debug_1_obs_") for port in report["transport"]["ports"])
print("CellFuzz manifest and transport tests passed.")
