"""Stage 5: independent array values through actual exported query interfaces."""
from pathlib import Path
import random
import subprocess
import tempfile

from memory_access import (SIM_PREPARE, check_description, check_memory_state,
                           check_transparency, generic_vectors, sync_vectors)
from value_roundtrip import CONFIG, HERE, Roundtrip


def with_addresses(case, vectors):
    """Sweep queries with business inputs stationary, including both boundaries."""
    memories = [m for m in case[1]["memories"] if m["content_query"]]
    if not memories:
        return vectors
    def at(vector, step):
        return dict(vector, **{m["query"]["address_port"]: m["start_offset"] + step % m["size"] for m in memories})
    # Visit every initialized word before the first write. Then change query
    # address with stationary business inputs at every stimulus phase.
    size = max(m["size"] for m in memories)
    return ([at(vectors[0], step) for step in range(size)] +
            [at(vector, index * 2 + step) for index, vector in enumerate(vectors) for step in range(min(2, size))])


def check_query(samples, report, initial=None):
    """Integer-address, per-bit array oracle; no Mem API or query gold read.

    Writers commit in fixture priority order. Wide lanes use consecutive logical
    words; query data always names one word, independent of business read hold.
    """
    memory = report["memories"][0]
    width, offset, size = memory["width"], memory["start_offset"], memory["size"]
    state = [format(0x10 + i * 0x11, f"0{width}b") for i in range(size)] if initial is None else initial
    query = memory["query"]
    previous_clk, seen, stationary = 0, set(), False
    previous = None
    for timestamp, values in samples:
        get = lambda name: int(values["stim_" + name], 2)
        clk = get("clk")
        for index, writer in enumerate(memory["write_ports"]):
            edge = clk != previous_clk and clk == int(writer["clock_positive"])
            if writer["clock_enabled"] and not edge:
                continue
            words = writer["words"]
            start = get(f"w{index}_addr") & ~(words - 1)
            mask, data = get(f"w{index}_en"), get(f"w{index}_data")
            for lane in range(words):
                address = start + lane
                if not offset <= address < offset + size:
                    continue
                bits = list(state[address - offset])
                for bit in range(width):
                    position = lane * width + bit
                    if mask & (1 << position):
                        bits[-1-bit] = str((data >> position) & 1)
                state[address - offset] = "".join(bits)
        address = get(query["address_port"])
        expected = state[address - offset]
        assert values["a_query0"] == expected, (timestamp, address, expected, values["a_query0"])
        seen.add(address)
        business = {key: value for key, value in values.items() if key.startswith("stim_") and key != "stim_" + query["address_port"]}
        if previous and business == previous[0] and address != previous[1]:
            stationary = True
        previous = business, address
        previous_clk = clk
    assert seen == set(range(offset, offset + size))
    assert stationary or size == 1


def check_static(case):
    _, report, _ = case
    check_description(report)
    graph_names = {wire["name"].lstrip("\\") for wire in report["graph"]["wires"]}
    for memory in report["memories"]:
        assert memory["content_query"] == (memory["selected"] and report["config"]["mem_content"] == "query")
        query = memory["query"]
        if not memory["content_query"]:
            assert query is None
            continue
        assert query["address_encoding"] == "unsigned_logical" and query["asynchronous"] and query["always_enabled"]
        assert query["start_offset"] == memory["start_offset"]
        assert query["end_offset_exclusive"] == memory["end_offset_exclusive"]
        assert query["read_index"] == len(memory["read_ports"])
        assert query["packed_index"] == sum(p["words"] for p in memory["read_ports"])
        assert query["address_width"] == max(1, (memory["end_offset_exclusive"] - 1).bit_length())
        assert query["data_width"] == memory["width"]
        assert query["address_port"] not in graph_names and query["data_port"] not in graph_names
        assert all(p["name"] not in (query["address_port"], query["data_port"]) for p in report["transport"]["ports"])


def main():
    rng = random.Random(0x5CE11)
    with tempfile.TemporaryDirectory(prefix="cellfuzz-query-") as temporary:
        work = Path(temporary)
        suite = Roundtrip(work)
        plugin = work / "memory_access.so"
        subprocess.run([CONFIG, "--build", str(plugin), f"-I{HERE.parent.parent}", str(HERE / "memory_access.cc")],
                       check=True, timeout=120)
        suite.plugins += ["-m", str(plugin)]
        for variant, diagnostic in (("query-control", "raw query connection mismatch: RD_EN"),
                                    ("init", "raw query parameter mismatch: INIT"),
                                    ("business-mask", "raw query parameter mismatch: RD_COLLISION_X_MASK")):
            suite.run("test_cellfuzz_memory fixture sync; cellfilter; "
                      f"test_cellfuzz_materialize raw-fault {variant} {work}/fault.json", error=diagnostic)
        # Handwritten mem[5:7], INIT 11/22/33, nibble enables, synchronous
        # business read. Expected words/timing do not come from the manifest.
        nibble = suite.prepare("nibble", "read_verilog query_nibble.v; hierarchy -top query_nibble; "
                               "proc; memory_dff; memory_collect; opt_clean", "query_nibble", options="-mem-content query", gold_verilog=True)
        query = nibble[1]["memories"][0]["query"]
        vectors = [dict(clk=clk, address=address, data=0xab, enable=enable,
                        **{query["address_port"]: query_address})
                   for clk,address,enable,query_address in ((0,6,0,5),(0,6,0,6),(0,6,0,7),(0,6,1,6),
                                                           (1,6,1,6),(0,6,0,6),(1,6,0,6),(0,6,0,6))]
        samples, outputs = suite.simulate(nibble, vectors, SIM_PREPARE + " setundef -undriven -undef;")
        for timestamp, values in samples:
            tick = int(values["tick"], 2)
            expected_query = (0x11,0x22,0x33,0x22,0x2b,0x2b,0x2b,0x2b)[tick]
            assert values["a_query0"] == format(expected_query, "08b"), ("nibble query", tick, timestamp)
            if tick >= 4:
                expected_q = 0x22 if tick < 6 else 0x2b
                assert values[f"a_func{outputs.index('q')}"] == format(expected_q, "08b"), ("nibble business Q", tick)
        # Three bounded SAT steps, fixed word 6 / low nibble b, no high-nibble
        # write. This establishes only this constrained three-step example.
        suite.run(f"read_verilog {nibble[0]}/actual.v; hierarchy -top query_nibble; proc; memory_map; "
                  f"opt_clean; sat -seq 3 -set address 6 -set data 171 -set {query['address_port']} 6 "
                  "-set-at 1 enable 1 -set-at 2 enable 0 -set-at 3 enable 0 "
                  f"-prove-skip 2 -prove q 43 -prove {query['data_port']} 43 -verify")
        setup = "test_cellfuzz_memory fixture sync"
        matrix = []
        for access, content in (("on", "off"), ("off", "off"), ("off", "query"), ("on", "query")):
            case = suite.prepare(access + "-" + content, setup, "memory_access",
                                 options=f"-mem-access {access} -mem-content {content} -chunk-width 17", gold_verilog=True)
            check_static(case)
            matrix.append(case)
            samples, outputs = suite.simulate(case, with_addresses(case, sync_vectors()), SIM_PREPARE)
            check_memory_state(samples, outputs, query=case[1]["memories"][0]["query"])
            if content == "query":
                check_query(samples, case[1])
        assert all(c[1]["graph"] == matrix[0][1]["graph"] for c in (matrix[3],))
        assert matrix[0][1]["items"] == matrix[3][1]["items"]
        # Bounded functional/observation equivalence for arbitrary query input.
        case = matrix[3]
        suite.run(f"read_rtlil {case[0]}/formal.il; write_verilog {case[0]}/formal.v; design -reset; "
                  f"read_verilog {case[0]}/formal.v; proc; memory_map; async2sync; miter -equiv -flatten gold gate miter; "
                  "hierarchy -top miter; sat -seq 4 -set-init-zero -prove trigger 0 -verify")
        for mode in ("negative-write", "wide", "semantics", "legacy", "write-only", "read-only", "offset", "zero-address", "async-write", "extend", "partial-init"):
            case = suite.prepare(mode, f"test_cellfuzz_memory fixture {mode}", "memory_access",
                                 options="-mem-content query -chunk-width 8 -prefix mem_dbg", gold_verilog=True)
            check_static(case)
            vectors = sync_vectors() if mode in ("negative-write", "semantics") else generic_vectors(case[2], case[1], rng)
            if mode not in ("negative-write", "semantics"):
                # Establish data/masks/addresses before the sampling edge;
                # simultaneous clock/data LUT changes have no setup guarantee.
                vectors = [dict(vector, clk=clk) for vector in vectors[::2] for clk in (0, 1, 1, 0)]
            samples, outputs = suite.simulate(case, with_addresses(case, vectors), SIM_PREPARE)
            initial = [format(0x10 + i * 0x11, "08b") for i in range(3)] + ["x" * 8] if mode == "partial-init" else None
            check_query(samples, case[1], initial)
            if mode == "negative-write":
                check_memory_state(samples, outputs, write_positive=False, query=case[1]["memories"][0]["query"])
            if mode == "semantics":
                check_transparency(samples, outputs)
        case = suite.prepare("multiple", "test_cellfuzz_memory fixture multiple", "memory_access",
                             options="-mem-content query", gold_verilog=True)
        check_static(case)
        assert [m["name"] for m in case[1]["memories"]] == ["\\array", "\\zarray"]
        assert [m["query"]["address_port"] for m in case[1]["memories"]] == ["cf_mem_0000_addr", "cf_mem_0001_addr"]
        vectors = [dict(v, extra_addr=5 + i % 3) for i, v in enumerate(sync_vectors()[:16])]
        # memory_map leaves padding mux inputs undriven for non-power-of-two
        # ranges; make those unspecified values explicit X in this sim copy.
        samples, outputs = suite.simulate(case, with_addresses(case, vectors), SIM_PREPARE + " setundef -undriven -undef;")
        check_query(samples, case[1])
        query = case[1]["memories"][1]["query"]
        for timestamp, values in samples:
            address = int(values["stim_" + query["address_port"]], 2)
            assert values["a_query1"] == format((0x1abc, 0x1234, 0x678)[address - 5], "013b"), timestamp
        # Mem parsing treats fully X/Z words as undefined. Query emission must
        # retain the original packed INIT verbatim, even where export uses X.
        zinit = suite.prepare("z-init", "test_cellfuzz_memory fixture z-init", "memory_access",
                              options="-mem-content query", gold_verilog=True)
        check_static(zinit)
        assert "z" in zinit[1]["memories"][0]["init"]["bits"]
        suite.run("test_cellfuzz_memory fixture negative; cellfilter -nodes register; "
                  f"test_cellfuzz_materialize success {work}/excluded.json cellinstrument -mem-content query -manifest {work}/excluded.json")
        # Repeated query planning is deterministic and leaves RTLIL/batch intact.
        command = f"cellinstrument -plan-only -mem-content query -manifest {work}/planned.json"
        suite.run(setup + f"; cellfilter; write_rtlil {work}/before.il; test_cellfuzz_type_plan unchanged {command}; "
                  f"write_rtlil {work}/after.il; cellinstrument -plan-only -mem-content query -manifest {work}/again.json")
        assert (work / "before.il").read_bytes() == (work / "after.il").read_bytes()
        assert (work / "planned.json").read_bytes() == (work / "again.json").read_bytes()
        # Query-only limits and name/file failures must precede all mutation.
        for setup, options in (("test_cellfuzz_memory fixture negative", ""),
                               ("test_cellfuzz_memory fixture overflow", ""),
                               ("test_cellfuzz_memory fixture collision-addr", ""),
                               ("test_cellfuzz_memory fixture collision-data", "")):
            for access in ("on", "off"):
                (work / "old.json").write_text("previous artifact\n")
                suite.run(setup + f"; cellfilter; test_cellfuzz_materialize preflight {work}/old.json "
                          f"cellinstrument -manifest {work}/old.json -mem-access {access} -mem-content query {options}; "
                          f"cellinstrument -plan-only -manifest {work}/usable.json")
                assert (work / "old.json").read_text() == "previous artifact\n"
        for destination in (str(work / "missing/out.json"), "/dev/full"):
            suite.run("test_cellfuzz_memory fixture sync; cellfilter; "
                      f"test_cellfuzz_materialize preflight {work}/old.json cellinstrument -manifest {destination} -mem-content query; "
                      f"cellinstrument -plan-only -manifest {work}/usable.json")
        for name in ("cf_mem_0000_addr", "cf_mem_0000_data"):
            source = work / "memid.v"
            source.write_text(f"module top(input [1:0] a, output [7:0] q); reg [7:0] {name}[0:3]; "
                              f"initial {name}[0]=8'h12; assign q={name}[a]; endmodule\n")
            for mode in ("", "-plan-only"):
                (work / "old.json").write_text("previous artifact\n")
                suite.run(f"read_verilog {source}; hierarchy -top top; proc; memory_collect; "
                          f"cd top; rename {name} arrcell; cd ..; cellfilter; "
                          f"test_cellfuzz_materialize preflight {work}/old.json cellinstrument {mode} "
                          f"-manifest {work}/old.json -mem-access off -mem-content query; "
                          f"cellinstrument -plan-only -prefix safe -mem-content query -manifest {work}/usable.json")
                assert (work / "old.json").read_text() == "previous artifact\n"
        for mode in ("interrupt", "corrupt", "rename", "query-interrupt", "query-corrupt", "query-rename"):
            (work / "old.json").write_text("previous artifact\n")
            suite.run("test_cellfuzz_memory fixture sync; cellfilter; "
                      f"test_cellfuzz_materialize {mode} {work}/old.json cellinstrument -manifest {work}/old.json -mem-content query " +
                      ("-mem-access off" if mode.startswith("query-") else ""))
            assert (work / "old.json").read_text() == "previous artifact\n"
        assert not list(work.rglob("*.tmp.*"))
    print("CellFuzz memory query tests passed.")


if __name__ == "__main__":
    main()
