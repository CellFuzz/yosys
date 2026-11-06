"""Supplement snapshot's exact operation counts with repeatable wall/RSS data.

Run on both builds with identical counts/repeats; no timing threshold is a test.
Input is an unflattened chain, and cellfilter targets every module by default.
"""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--counts", nargs="+", type=int, default=[100, 200, 400, 800])
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert args.repeats >= 5 and all(count > 0 for count in args.counts)
    root = Path(__file__).resolve().parents[2]
    yosys = os.environ.get("YOSYS", str(root / "build/yosys"))
    result = {"yosys": yosys, "command": "read_verilog chain.v; hierarchy -top m0; cellfilter", "chains": {}}
    with tempfile.TemporaryDirectory(prefix="cellfuzz-benchmark-") as temporary:
        work = Path(temporary)
        for count in args.counts:
            source = work / f"chain{count}.v"
            source.write_text("".join(f"module m{i}(input a,output y); " +
                              (f"m{i+1} u(.a(a),.y(y));" if i+1 < count else "assign y=~a;") +
                              " endmodule\n" for i in range(count)))
            samples, rss = [], []
            for trial in range(args.repeats + 1):
                start = time.perf_counter()
                run = subprocess.run(["/usr/bin/time", "-f", "%M", "-o", str(work / "rss"), yosys,
                                      "-Q", "-T", "-q", "-p", f"read_verilog {source}; hierarchy -top m0; cellfilter"],
                                     capture_output=True, text=True, timeout=120)
                assert run.returncode == 0, run.stderr
                elapsed = time.perf_counter() - start
                if trial:
                    samples.append(elapsed)
                    rss.append(int((work / "rss").read_text()))
            result["chains"][count] = {"seconds": samples, "median_seconds": statistics.median(samples), "peak_rss_kib": rss}
            print(count, result["chains"][count], flush=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
