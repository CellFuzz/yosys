"""Stage 4: real business interface values and independent memory state oracle."""
from pathlib import Path
import random
import subprocess
import tempfile

from value_roundtrip import CONFIG, HERE, Roundtrip

# Normalize only simulation copies. Native sim repeatedly applies overlapping
# writes during stabilization; memory_map implements the actual priority muxes
# and gives the simulator an explicit next-state function.
SIM_PREPARE = "memory_collect; memory_map;"


def check_description(report):
    referenced = []
    cells, ports = report["graph"]["cells"], report["graph"]["ports"]
    for memory in report["memories"]:
        cell = cells[memory["cell_id"]]
        assert cell["name"] == memory["name"] and cell["parameters"] == memory["parameters"]
        assert memory["end_offset_exclusive"] == memory["start_offset"] + memory["size"]
        assert memory["init"] == memory["parameters"]["\\INIT"]
        sync_observed = False
        observed = False
        for key, prefix in (("read_ports", "RD"), ("write_ports", "WR")):
            packed = 0
            for index, owner in enumerate(memory[key]):
                assert owner["index"] == index and owner["packed_index"] == packed
                assert owner["words"] == 1 << owner["wide_log2"]
                assert owner["data_width"] == memory["width"] * owner["words"]
                fields = owner["signals"]
                assert fields.keys() == ({"address", "data", "enable", "clock", "async_reset", "sync_reset"}
                                         if prefix == "RD" else {"address", "data", "enable", "clock"})
                for name, signal in fields.items():
                    assert len(signal["source"]) == signal["width"]
                    if signal["port_id"] is None:
                        assert memory["type"] == "$mem" and name in ("async_reset", "sync_reset") and signal["source"] == [0]
                    else:
                        port = ports[signal["port_id"]]
                        suffix = {"address": "ADDR", "data": "DATA", "enable": "EN", "clock": "CLK",
                                  "async_reset": "ARST", "sync_reset": "SRST"}[name]
                        assert port["name"] == "\\" + prefix + "_" + suffix and port["cell_id"] == cell["id"]
                        offset = signal["port_lsb"]
                        assert 0 <= offset <= port["width"] and signal["width"] <= port["width"] - offset
                        assert signal["source"] == port["raw"][offset:offset + signal["width"]]
                    monitor = signal["monitor_id"]
                    if monitor is not None:
                        item = report["items"][monitor]
                        assert signal["observation_reason"] == "observed" and signal["active"]
                        assert memory["selected"] and memory["access_enabled"]
                        assert item["role"] == "memory_access" and item["cell_id"] == cell["id"]
                        for field in ("source", "port_id", "port_lsb", "width"):
                            assert item[field] == signal[field]
                        observed = True
                        referenced.append(monitor)
                    if prefix == "RD" and name == "data" and owner["clock_enabled"] and monitor is not None:
                        sync_observed = True
                packed += owner["words"]
        assert observed == memory["access_observed"] and sync_observed == memory["sync_read_state_observed"]
        has_sync = any(port["clock_enabled"] for port in memory["read_ports"])
        assert bool(memory["coverage_notes"]) == (has_sync and not sync_observed)
    assert sorted(referenced) == [item["monitor_id"] for item in report["items"] if item["role"] == "memory_access"]


def sync_vectors():
    vectors = []
    for cycle in range(24):
        for phase in range(4):
            step = cycle * 2 + int(phase >= 2)
            vector = dict(clk=int(phase in (1, 2)),
                          w0_addr=cycle % 4, w1_addr=cycle % 4 if cycle % 3 == 0 else (cycle + 1) % 4,
                          w0_en=(0xff, 0x0f, 0xf0, 0x55, 0, 0xaa)[cycle % 6],
                          w1_en=(0x33, 0xcc, 0, 0xff)[cycle % 4],
                          w0_data=(step * 37 + 17) % 256, w1_data=(step * 53 + 6) % 256,
                          r0_addr=step % 4, r1_addr=cycle % 4, r2_addr=(step + 2) % 4, r3_addr=(step + 3) % 4)
            for index in (1, 2, 3):
                vector[f"r{index}_en"] = int(cycle % (5 if index == 3 else 4) != (2 if index == 3 else 1))
                vector[f"r{index}_srst"] = int(cycle % (7 if index == 3 else 6) == (2 if index == 3 else 1))
                vector[f"r{index}_arst"] = int(step % 11 == {1: 3, 2: 7, 3: 9}[index])
            vectors.append(vector)
    return vectors


def check_memory_state(samples, outputs, write_positive=True, query=None):
    memory, state, previous_clk = [0x10, 0x21, 0x32, 0x43], {1: 0x92, 2: 0x93, 3: 0x94}, 0
    exercised, previous_reads = set(), {}
    for timestamp, values in samples:
        get = lambda name: int(values["stim_" + name], 2)
        clk = get("clk")
        rising, falling = clk == 1 and previous_clk == 0, clk == 0 and previous_clk == 1
        # Registered reads observe the old word on the sampling edge. Apply
        # writes afterwards, in explicit fixture priority order (w1 over w0).
        for index in (1, 2, 3):
            edge = falling if index == 3 else rising
            en, srst = get(f"r{index}_en"), get(f"r{index}_srst")
            if edge:
                if srst and (index != 2 or en):
                    state[index] = 0xb3 + index
                    if not en:
                        exercised.add("reset_over_enable")
                elif en:
                    state[index] = memory[get(f"r{index}_addr")]
                    exercised.add("negative_read" if index == 3 else "synchronous_read")
                elif srst and index == 2:
                    exercised.add("enable_over_reset")
            if (not en and not srst and not get(f"r{index}_arst") and previous_reads and
                    state[index] == previous_reads[index] and memory[get(f"r{index}_addr")] != state[index]):
                exercised.add("holding_output")
            if get(f"r{index}_arst"):
                if not rising and not falling and state[index] != 0xa2 + index:
                    exercised.add("async_reset_between_edges")
                state[index] = 0xa2 + index
        write_edge = rising if write_positive else falling
        if write_edge:
            read_index = 1 if write_positive else 3
            if (get(f"r{read_index}_en") and not get(f"r{read_index}_srst") and not get(f"r{read_index}_arst") and
                    get(f"r{read_index}_addr") == get("w0_addr") and get("w0_en") and
                    memory[get("w0_addr")] != get("w0_data")):
                exercised.add("read_before_write")
            for index in (0, 1):
                address, mask, data = get(f"w{index}_addr"), get(f"w{index}_en"), get(f"w{index}_data")
                if mask not in (0, 0xff):
                    exercised.add("masked_write")
                memory[address] = (memory[address] & (~mask & 0xff)) | (data & mask)
            if get("w0_addr") == get("w1_addr") and get("w0_en") & get("w1_en"):
                exercised.add("write_priority")
        expected = {"r0_data": memory[get("r0_addr")], **{f"r{i}_data": state[i] for i in (1, 2, 3)}}
        for name, number in expected.items():
            assert values[f"g_func{outputs.index(name)}"] == format(number, "08b"), (timestamp, name, number, values[f"g_func{outputs.index(name)}"])
        if query:
            address = get(query["address_port"])
            assert values["a_query0"] == format(memory[address], "08b"), (timestamp, address, memory, values["a_query0"])
        previous_clk, previous_reads = clk, state.copy()
    assert exercised == {"reset_over_enable", "enable_over_reset", "holding_output", "async_reset_between_edges",
                         "synchronous_read", "negative_read", "masked_write", "write_priority", "read_before_write"}, exercised
    assert len(set(memory)) > 1 and memory != [0x10, 0x21, 0x32, 0x43]


def generic_vectors(original, report, rng, count=24):
    memory = report["memories"][0]
    inputs = {name: len(port["bits"]) for name, port in original["ports"].items() if port["direction"] == "input"}
    vectors = []
    for index in range(count):
        vector = {name: rng.getrandbits(width) for name, width in inputs.items()}
        vector["clk"] = index % 2
        for name, width in inputs.items():
            if name.endswith("addr"):
                vector[name] = (memory["start_offset"] + (index // 2) % memory["size"]) & ((1 << width) - 1)
            if name.endswith(("arst", "srst")):
                vector[name] = 0
        vectors.append(vector)
    return vectors


def check_transparency(samples, outputs):
    memory, expected, previous_clk = [0x10, 0x21, 0x32, 0x43], format(0x92, "08b"), 0
    bypass_seen = collision_seen = False
    for timestamp, values in samples:
        get = lambda name: int(values["stim_" + name], 2)
        clk = get("clk")
        if clk and not previous_clk:
            if get("r1_srst"):
                expected = format(0xb4, "08b")
            elif get("r1_en"):
                address = get("r1_addr")
                bits = list(format(memory[address], "08b"))
                # r1 is transparent to w0 and explicitly X on w1 collisions.
                # Apply these per-bit effects independently of the new array
                # contents, which still follow the original write priority.
                for index in (0, 1):
                    if get(f"w{index}_addr") != address:
                        continue
                    for bit in range(8):
                        if get(f"w{index}_en") & (1 << bit):
                            bits[-1-bit] = "x" if index == 1 else str((get("w0_data") >> bit) & 1)
                            if index == 0:
                                bypass_seen = True
                            else:
                                collision_seen = True
                expected = "".join(bits)
            for index in (0, 1):
                address, mask = get(f"w{index}_addr"), get(f"w{index}_en")
                memory[address] = (memory[address] & (~mask & 0xff)) | (get(f"w{index}_data") & mask)
        if get("r1_arst"):
            expected = format(0xa3, "08b")
        actual = values[f"g_func{outputs.index('r1_data')}"]
        assert actual == expected, (timestamp, actual, expected)
        previous_clk = clk
    assert bypass_seen and collision_seen


def main():
    rng = random.Random(0x4CE11)
    with tempfile.TemporaryDirectory(prefix="cellfuzz-memory-") as tmp:
        work = Path(tmp)
        suite = Roundtrip(work)
        plugin = work / "memory_access.so"
        subprocess.run([CONFIG, "--build", str(plugin), f"-I{HERE.parent.parent}", str(HERE / "memory_access.cc")], check=True, timeout=120)
        suite.plugins += ["-m", str(plugin)]
        baseline_setup = "read_verilog stateful.v; hierarchy -top stateful; proc; memory_collect"
        baseline = suite.prepare("baseline", baseline_setup, "stateful")
        check_description(baseline[1])
        assert any(item["role"] == "memory_access" for item in baseline[1]["items"])
        vectors = [dict(clk=i % 2, rst=int(i < 2), enable=int(i % 4 != 0), address=(i // 2) % 4, data=(i * 13) % 256) for i in range(32)]
        suite.simulate(baseline, vectors, SIM_PREPARE)
        baseline_off = suite.prepare("baseline-off", baseline_setup, "stateful", options="-mem-access off")
        check_description(baseline_off[1])
        ordinary = lambda report: [{key: value for key, value in item.items() if key != "monitor_id"}
                                   for item in report["items"] if item["role"] != "memory_access"]
        assert ordinary(baseline[1]) == ordinary(baseline_off[1])
        assert baseline[1]["analysis"] == baseline_off[1]["analysis"]
        setup = "test_cellfuzz_memory fixture sync"
        sync = suite.prepare("sync", setup, "memory_access", options="-chunk-width 33", gold_verilog=True)
        check_description(sync[1])
        description = sync[1]["memories"][0]
        assert [p["clock_enabled"] for p in description["read_ports"]] == [False, True, True, True]
        assert not description["read_ports"][3]["clock_positive"]
        assert description["write_ports"][1]["priority_mask"] == [True, False]
        assert [p["ce_over_srst"] for p in description["read_ports"]] == [False, False, True, False]
        samples, outputs = suite.simulate(sync, sync_vectors(), SIM_PREPARE)
        check_memory_state(samples, outputs)
        suite.run(f"read_rtlil {sync[0]}/formal.il; write_verilog {sync[0]}/formal.v; design -reset; "
                  f"read_verilog {sync[0]}/formal.v; proc; memory_map; async2sync; miter -equiv -flatten gold gate miter; "
                  "hierarchy -top miter; sat -seq 4 -set-init-zero -prove trigger 0 -verify")
        negative_write = suite.prepare("negative-write", "test_cellfuzz_memory fixture negative-write", "memory_access", gold_verilog=True)
        check_description(negative_write[1])
        assert all(not port["clock_positive"] for port in negative_write[1]["memories"][0]["write_ports"])
        samples, outputs = suite.simulate(negative_write, sync_vectors(), SIM_PREPARE)
        check_memory_state(samples, outputs, write_positive=False)
        off = suite.prepare("off", setup, "memory_access", options="-mem-access off", gold_verilog=True)
        check_description(off[1])
        assert not off[1]["items"] and not off[1]["memories"][0]["access_observed"]
        assert off[1]["analysis"] == sync[1]["analysis"] and off[1]["selection"] == sync[1]["selection"]
        samples, outputs = suite.simulate(off, sync_vectors(), SIM_PREPARE)
        check_memory_state(samples, outputs)
        excluded = suite.prepare("excluded", setup, "memory_access", selection="-nodes register", options="-mem-content query", gold_verilog=True)
        check_description(excluded[1])
        assert not excluded[1]["memories"][0]["selected"] and not excluded[1]["items"]
        for mode in ("wide", "semantics", "legacy", "write-only", "read-only", "offset", "negative", "zero-address", "async-write"):
            case = suite.prepare(mode, f"test_cellfuzz_memory fixture {mode}", "memory_access", options="-chunk-width 8 -prefix mem_dbg", gold_verilog=True)
            check_description(case[1])
            memory = case[1]["memories"][0]
            if mode == "wide":
                assert [port["packed_index"] for port in memory["read_ports"]] == [0, 2]
                assert [port["packed_index"] for port in memory["write_ports"]] == [0, 2]
                assert [port["data_width"] for port in memory["read_ports"]] == [66, 33]
                assert memory["read_ports"][0]["signals"]["address"]["source"][0] == 0
            if mode == "semantics":
                assert memory["read_ports"][1]["transparency_mask"] == [True, False]
                assert memory["read_ports"][1]["collision_x_mask"] == [False, True]
            if mode == "legacy":
                assert memory["type"] == "$mem" and memory["read_ports"][1]["signals"]["async_reset"]["port_id"] is None
            if mode == "negative":
                assert memory["start_offset"] == -4
            vectors = sync_vectors() if mode == "semantics" else generic_vectors(case[2], case[1], rng)
            samples, outputs = suite.simulate(case, vectors, SIM_PREPARE)
            if mode == "semantics":
                check_transparency(samples, outputs)
        # Memory parsing and repeated planning are exactly read-only and
        # deterministic; query and artifact errors leave the batch usable.
        command = f"cellinstrument -plan-only -manifest {work}/planned.json"
        suite.run(setup + f"; cellfilter; write_rtlil {work}/before.il; test_cellfuzz_memory invalid; "
                  f"test_cellfuzz_type_plan unchanged {command}; write_rtlil {work}/after.il; "
                  f"cellinstrument -plan-only -manifest {work}/again.json")
        assert (work / "before.il").read_bytes() == (work / "after.il").read_bytes()
        assert (work / "planned.json").read_bytes() == (work / "again.json").read_bytes()
        for option in ("-mem-access on", "-mem-access off"):
            (work / "planned.json").write_text("previous artifact\n")
            suite.run("test_cellfuzz_memory fixture negative" + f"; cellfilter; test_cellfuzz_materialize preflight {work}/planned.json "
                      f"cellinstrument -manifest {work}/planned.json {option} -mem-content query; "
                      f"cellinstrument -plan-only -manifest {work}/usable.json")
            assert (work / "planned.json").read_text() == "previous artifact\n"
        for destination in (str(work / "missing/out.json"), "/dev/full"):
            suite.run(setup + f"; cellfilter; test_cellfuzz_materialize preflight {work}/planned.json "
                      f"cellinstrument -manifest {destination}; " + command)
        assert not list(work.rglob("*.tmp.*"))
    print("CellFuzz memory access tests passed.")


if __name__ == "__main__":
    main()
